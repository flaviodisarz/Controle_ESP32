#include "ble_hid.h"
#include "esp_random.h"
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_ble_api.h"
#include "esp_event.h"

#include "esp_hid_common.h"
#include "esp_hidd.h"

#if !CONFIG_BT_ENABLED
#error "Bluetooth desabilitado. Habilite em menuconfig: Component config -> Bluetooth"
#endif

#if !CONFIG_BT_BLUEDROID_ENABLED
#error "Você precisa do Bluedroid habilitado (NimBLE OFF) pra usar esp_hid/esp_hidd_dev_init."
#endif

#if CONFIG_BT_NIMBLE_ENABLED
#error "NimBLE está ligado. Desligue NimBLE e use Bluedroid pra este caminho."
#endif

static const char* TAG = "ble_hid";

/* ===================== Report Map (o seu) ===================== */
static const unsigned char mediaReportMap[] = {
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01,
    0x85, 0x03,
    0x09, 0x02, 0xA1, 0x02,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x0A, 0x15, 0x01, 0x25, 0x0A, 0x75, 0x04, 0x95, 0x01, 0x81, 0x00,
    0xC0,
    0x05, 0x0C, 0x09, 0x86, 0x15, 0xFF, 0x25, 0x01, 0x75, 0x02, 0x95, 0x01, 0x81, 0x46,
    0x09, 0xE9, 0x09, 0xEA, 0x15, 0x00, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
    0x09, 0xE2, 0x09, 0x30, 0x09, 0x83, 0x09, 0x81, 0x09, 0xB0, 0x09, 0xB1, 0x09, 0xB2, 0x09, 0xB3, 0x09, 0xB4, 0x09, 0xB5, 0x09, 0xB6, 0x09, 0xB7,
    0x15, 0x01, 0x25, 0x0C, 0x75, 0x04, 0x95, 0x01, 0x81, 0x00,
    0x09, 0x80, 0xA1, 0x02,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x01, 0x25, 0x03, 0x75, 0x02, 0x81, 0x00,
    0xC0,
    0x81, 0x03,
    0xC0
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    { .data = mediaReportMap, .len = sizeof(mediaReportMap) }
};

/* ===================== Estado interno ===================== */

static ble_hid_cfg_t s_cfg;
static ble_hid_state_t s_state = BLE_HID_STATE_OFF;

static esp_hidd_dev_t* s_hid = NULL;

static char s_dev_name[32];

static bool s_adv_requested   = false;
static bool s_adv_ready       = false;
static bool s_scan_rsp_ready  = false;
static bool s_adv_stopping    = false;

static esp_timer_handle_t s_adv_retry_t = NULL;

static void set_state(ble_hid_state_t st) { s_state = st; }

static void emit_evt(ble_hid_evt_t evt)
{
    ESP_LOGI(TAG, "emit_evt=%d", (int)evt);
    if (s_cfg.on_evt) s_cfg.on_evt(evt, s_cfg.user);
    else ESP_LOGW(TAG, "on_evt NULL (STATE/LED não vai receber eventos)");
}

/* ===================== NVS safe init ===================== */

static esp_err_t nvs_safe_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

/* ===================== Event loop ===================== */

static void ensure_event_loop(void)
{
    esp_err_t err = esp_event_loop_create_default();
    if (err == ESP_ERR_INVALID_STATE) return; // já existe
    ESP_ERROR_CHECK(err);
}

/* ===================== Nome (A/B) ===================== */

static void build_name(void)
{
    const char* prefix = (s_cfg.device_name_prefix && s_cfg.device_name_prefix[0])
        ? s_cfg.device_name_prefix
        : "Controle";

    char slotc = (s_cfg.slot == BLE_HID_SLOT_B) ? 'B' : 'A';
    snprintf(s_dev_name, sizeof(s_dev_name), "%s %c", prefix, slotc);

    if (strlen(s_dev_name) > 29) s_dev_name[29] = '\0';
}

/* ===================== Slot A/B com endereço próprio ===================== */

static void gen_static_rand_addr(uint8_t out[6])
{
    // static random: dois bits mais altos do byte 5 precisam ser 1 (11xxxxxx)
    esp_fill_random(out, 6);
    out[5] = (out[5] & 0x3F) | 0xC0;
}

static esp_err_t load_or_create_addr(const char* key, uint8_t out[6])
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("blehid", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    size_t len = 6;
    err = nvs_get_blob(h, key, out, &len);

    if (err == ESP_OK && len == 6) {
        nvs_close(h);
        return ESP_OK;
    }

    // não existe: cria e salva
    gen_static_rand_addr(out);
    err = nvs_set_blob(h, key, out, 6);
    if (err == ESP_OK) err = nvs_commit(h);

    nvs_close(h);
    return err;
}

static esp_err_t apply_slot_identity(ble_hid_slot_t slot)
{
    uint8_t addr[6];
    const char* key = (slot == BLE_HID_SLOT_B) ? "addrB" : "addrA";

    esp_err_t err = load_or_create_addr(key, addr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "load_or_create_addr(%s) failed: %s", key, esp_err_to_name(err));
        return err;
    }

    // IMPORTANTÍSSIMO: usar endereço RANDOM para separar slots
    err = esp_ble_gap_set_rand_addr(addr);
    ESP_LOGI(TAG, "set_rand_addr(%c) => %s  [%02X:%02X:%02X:%02X:%02X:%02X]",
             (slot == BLE_HID_SLOT_B) ? 'B' : 'A',
             esp_err_to_name(err),
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

    return err;
}

/* ===================== Advertising (GAP) ===================== */

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min        = 0x20,
    .adv_int_max        = 0x40,
    .adv_type           = ADV_TYPE_IND,
    .own_addr_type      = BLE_ADDR_TYPE_RANDOM,   // <- já prepara pro rand addr
    .channel_map        = ADV_CHNL_ALL,
    .adv_filter_policy  = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp        = false,
    .include_name        = false,
    .include_txpower     = false,
    .min_interval        = 0,
    .max_interval        = 0,
    .appearance          = 0,
    .manufacturer_len    = 0,
    .p_manufacturer_data = NULL,
    .service_data_len    = 0,
    .p_service_data      = NULL,
    .service_uuid_len    = 0,
    .p_service_uuid      = NULL,
    .flag                = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_data_t s_scan_rsp_data = {
    .set_scan_rsp        = true,
    .include_name        = true,
    .include_txpower     = true,
    .min_interval        = 0,
    .max_interval        = 0,
    .appearance          = 0,
    .manufacturer_len    = 0,
    .p_manufacturer_data = NULL,
    .service_data_len    = 0,
    .p_service_data      = NULL,
    .service_uuid_len    = 0,
    .p_service_uuid      = NULL,
    .flag                = 0,
};

static void try_start_adv(void)
{
    ESP_LOGI(TAG, "try_start_adv req=%d adv=%d scan=%d stopping=%d",
             (int)s_adv_requested, (int)s_adv_ready, (int)s_scan_rsp_ready, (int)s_adv_stopping);

    if (!s_adv_requested) return;
    if (!s_adv_ready) return;
    if (!s_scan_rsp_ready) return;
    if (s_adv_stopping) return;

    esp_err_t err = esp_ble_gap_start_advertising(&s_adv_params);
    ESP_LOGI(TAG, "gap_start_adv => %s", esp_err_to_name(err));

    if (err == ESP_ERR_INVALID_STATE) return; // já está anunciando
    if (err != ESP_OK) ESP_LOGE(TAG, "start_advertising failed: %s", esp_err_to_name(err));
}

static void request_stop_adv(void)
{
    esp_err_t err = esp_ble_gap_stop_advertising();

    if (err == ESP_OK) {
        s_adv_stopping = true;
        ESP_LOGI(TAG, "stop_adv => OK (waiting STOP_COMPLETE)");
        return;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        s_adv_stopping = false;
        ESP_LOGW(TAG, "stop_adv => INVALID_STATE (wasn't advertising)");
        return;
    }

    s_adv_stopping = false;
    ESP_LOGW(TAG, "stop_adv => %s (ignored)", esp_err_to_name(err));
}

static void adv_retry_cb(void* arg)
{
    (void)arg;
    ESP_LOGI(TAG, "adv_retry_cb");
    s_adv_stopping = false; // paranoia saudável
    try_start_adv();
}

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {

    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        ESP_LOGI(TAG, "ADV data set");
        s_adv_ready = true;
        try_start_adv();
        break;

    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        ESP_LOGI(TAG, "SCAN RSP data set");
        s_scan_rsp_ready = true;
        try_start_adv();
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Advertising started (name='%s')", s_dev_name);
            set_state(BLE_HID_STATE_ADVERTISING);
            emit_evt(BLE_HID_EVT_ADVERTISING);
        } else {
            ESP_LOGE(TAG, "ADV start failed, status=%d", param->adv_start_cmpl.status);
        }
        break;

    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        ESP_LOGI(TAG, "Advertising stopped");
        s_adv_stopping = false;
        try_start_adv();
        break;

    default:
        break;
    }
}

/* ===================== Callback do HID device ===================== */

static void hidd_event_cb(void* handler_args, esp_event_base_t base, int32_t id, void* event_data)
{
    (void)handler_args;
    (void)base;

    esp_hidd_event_data_t* p = (esp_hidd_event_data_t*)event_data;

    switch ((esp_hidd_event_t)id) {

    case ESP_HIDD_START_EVENT:
        ESP_LOGI(TAG, "HID start status=%d", p->start.status);
        break;

    case ESP_HIDD_CONNECT_EVENT:
        ESP_LOGI(TAG, "HID connected status=%d", p->connect.status);
        set_state(BLE_HID_STATE_CONNECTED);
        emit_evt(BLE_HID_EVT_CONNECTED);
        break;

case ESP_HIDD_DISCONNECT_EVENT:
    ESP_LOGW(TAG, "HID disconnected reason=%d", p->disconnect.reason);
    emit_evt(BLE_HID_EVT_DISCONNECTED);

    set_state(BLE_HID_STATE_ADVERTISING);

    // força voltar a anunciar
    s_adv_requested = true;
    s_adv_stopping = false;

    esp_err_t err = esp_ble_gap_start_advertising(&s_adv_params);
    ESP_LOGI(TAG, "gap_start_adv (after disc) => %s", esp_err_to_name(err));

    break;

    default:
        break;
    }
}

/* ===================== BT stack init ===================== */

static esp_err_t bt_stack_init(void)
{
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret) return ret;

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) return ret;

    ret = esp_bluedroid_init();
    if (ret) return ret;

    ret = esp_bluedroid_enable();
    return ret;
}

static esp_err_t config_adv_payloads(void)
{
    esp_err_t e1 = esp_ble_gap_config_adv_data(&s_adv_data);
    ESP_LOGI(TAG, "config ADV => %s", esp_err_to_name(e1));
    if (e1 != ESP_OK) return e1;

    esp_err_t e2 = esp_ble_gap_config_adv_data(&s_scan_rsp_data);
    ESP_LOGI(TAG, "config SCAN RSP => %s", esp_err_to_name(e2));
    if (e2 != ESP_OK) return e2;

    return ESP_OK;
}

/* ===================== API pública ===================== */

esp_err_t ble_hid_init(const ble_hid_cfg_t* cfg)
{
    if (cfg) s_cfg = *cfg;

    ESP_LOGI(TAG, "init (slot=%c)", (s_cfg.slot == BLE_HID_SLOT_B) ? 'B' : 'A');

    ESP_ERROR_CHECK(nvs_safe_init());
    ensure_event_loop();

    (void)esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

    ESP_ERROR_CHECK(bt_stack_init());
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_cb));

    // timer de retry (pra reconectar sem reset)
    if (!s_adv_retry_t) {
        const esp_timer_create_args_t tcfg = {
            .callback = adv_retry_cb,
            .arg = NULL,
            .name = "adv_retry"
        };
        ESP_ERROR_CHECK(esp_timer_create(&tcfg, &s_adv_retry_t));
    }

    // identidade do slot (MAC diferente por slot)
    ESP_ERROR_CHECK(apply_slot_identity(s_cfg.slot));

    build_name();
    ESP_ERROR_CHECK(esp_ble_gap_set_device_name(s_dev_name));

    s_adv_requested = false;
    s_adv_ready = false;
    s_scan_rsp_ready = false;
    s_adv_stopping = false;
    s_hid = NULL;

    ESP_ERROR_CHECK(config_adv_payloads());

    esp_hid_device_config_t hid_cfg = {
        .vendor_id = 0xCAFE,
        .product_id = 0x0003,
        .version = 0x0100,

        .device_name = s_dev_name,
        .manufacturer_name = "Disarz",
        .serial_number = "0001",

        .report_maps = s_report_maps,
        .report_maps_len = sizeof(s_report_maps) / sizeof(s_report_maps[0]),
    };

    ESP_ERROR_CHECK(esp_hidd_dev_init(&hid_cfg, ESP_HID_TRANSPORT_BLE, hidd_event_cb, &s_hid));
    ESP_LOGI(TAG, "esp_hidd_dev_init ok, hid=%p", s_hid);

    set_state(BLE_HID_STATE_OFF);
    return ESP_OK;
}

esp_err_t ble_hid_start(void)
{
    ESP_LOGI(TAG, "start requested");
    s_adv_requested = true;
    try_start_adv();
    return ESP_OK;
}

esp_err_t ble_hid_stop(void)
{
    ESP_LOGI(TAG, "stop requested");
    s_adv_requested = false;
    request_stop_adv();
    set_state(BLE_HID_STATE_OFF);
    return ESP_OK;
}

esp_err_t ble_hid_set_slot(ble_hid_slot_t slot)
{
    s_cfg.slot = slot;

    build_name();
    ESP_LOGI(TAG, "set_slot -> %c (name='%s')",
             (slot == BLE_HID_SLOT_B) ? 'B' : 'A',
             s_dev_name);

    request_stop_adv();

    // aplica MAC novo do slot (resolve cache e separa devices)
    ESP_ERROR_CHECK(apply_slot_identity(slot));

    ESP_ERROR_CHECK(esp_ble_gap_set_device_name(s_dev_name));

    s_adv_ready = false;
    s_scan_rsp_ready = false;

    ESP_ERROR_CHECK(config_adv_payloads());

    try_start_adv();
    return ESP_OK;
}

ble_hid_state_t ble_hid_get_state(void)
{
    return s_state;
}
