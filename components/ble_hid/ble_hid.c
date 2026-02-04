#include "ble_hid.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"

#include "nvs_flash.h"
#include "nvs.h"

#include "esp_timer.h"
#include "esp_random.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_ble_api.h"
#include "esp_event.h"
#include "esp_gatts_api.h"

#include "esp_hid_common.h"
#include "esp_hidd.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
// Report ID que vamos usar pro Consumer Control
#define HID_RPT_ID_CC_IN   0x03
#define HID_CC_IN_RPT_LEN  1

static const unsigned char mediaReportMap[] = {
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, HID_RPT_ID_CC_IN,   //   Report ID (3)

    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)

    // 6 botões (1 bit cada)
    0x95, 0x06,        //   Report Count (6)
    0x09, 0xE9,        //   Usage (Volume Increment)
    0x09, 0xEA,        //   Usage (Volume Decrement)
    0x09, 0xE2,        //   Usage (Mute)
    0x09, 0xB6,        //   Usage (Scan Previous Track)
    0x09, 0xCD,        //   Usage (Play/Pause)
    0x09, 0xB5,        //   Usage (Scan Next Track)
    0x81, 0x02,        //   Input (Data,Var,Abs)

    // padding até fechar 1 byte
    0x95, 0x02,        //   Report Count (2)
    0x81, 0x03,        //   Input (Const,Var,Abs)

    0xC0               // End Collection
};





static void setup_security_params(void)
{
    // Bonding sem MITM (sem teclado/sem tela)
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;

    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key  = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;

    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key));

    ESP_LOGI(TAG, "security params set (BOND, IOCAP_NONE)");
}

static void request_conn_params(const esp_bd_addr_t bda)
{
    esp_ble_conn_update_params_t cp = {0};
    memcpy(cp.bda, bda, sizeof(esp_bd_addr_t));

    cp.min_int = 12;   // 15ms
    cp.max_int = 24;   // 30ms
    cp.latency = 0;

    // 🔥 aqui é o pulo do gato pro “desconectou e eu sei rápido”
    // unidade = 10ms. 400 = 4s. Se quiser mais rápido depois, testa 300 (3s).
    cp.timeout = 400;

    esp_err_t e = esp_ble_gap_update_conn_params(&cp);
    ESP_LOGI(TAG, "update_conn_params => %s (timeout=%ums)", esp_err_to_name(e), (unsigned)(cp.timeout * 10));
}



static esp_hid_raw_report_map_t s_report_maps[] = {
    { .data = mediaReportMap, .len = sizeof(mediaReportMap) }
};

/* ===================== Estado interno ===================== */

static ble_hid_cfg_t s_cfg;
static ble_hid_state_t s_state = BLE_HID_STATE_OFF;

static esp_hidd_dev_t* s_hid = NULL;
static char s_dev_name[32];

static bool s_hid_started     = false;

static bool s_adv_requested   = false;
static bool s_adv_ready       = false;
static bool s_scan_rsp_ready  = false;

static bool s_adv_running     = false;
static bool s_adv_starting    = false;
static bool s_adv_stopping    = false;

static bool s_user_stop       = false;   // evita “auto-restart” após stop()

static bool s_slot_pending = false;
static ble_hid_slot_t s_slot_pending_value = BLE_HID_SLOT_A;

// pra poder desconectar “na marra” (capturado no GATTS CONNECT)
static bool s_peer_valid = false;
static esp_bd_addr_t s_peer_bda = {0};
static esp_gatt_if_t s_peer_gatts_if = ESP_GATT_IF_NONE;
static uint16_t s_peer_conn_id = 0;

static esp_timer_handle_t s_adv_retry_t = NULL;

/* ===================== Forward decls ===================== */
static void try_start_adv(void);
static void schedule_adv_retry_ms(uint32_t ms);
static void request_stop_adv(void);

static void apply_pending_slot_if_any(void);
static esp_err_t apply_slot_identity(ble_hid_slot_t slot);

static void on_connected_once(void);
static void on_disconnected_once(uint8_t reason);

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void gatts_wrapper_cb(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);
static void hidd_event_cb(void* handler_args, esp_event_base_t base, int32_t id, void* event_data);

/* ===================== helpers ===================== */

static void set_state(ble_hid_state_t st) { s_state = st; }

static void emit_evt(ble_hid_evt_t evt)
{
    ESP_LOGI(TAG, "emit_evt=%d", (int)evt);
    if (s_cfg.on_evt) s_cfg.on_evt(evt, s_cfg.user);
    else ESP_LOGW(TAG, "on_evt NULL (STATE/LED não vai receber eventos)");
}

static void build_name(void)
{
    const char* prefix = (s_cfg.device_name_prefix && s_cfg.device_name_prefix[0])
        ? s_cfg.device_name_prefix
        : "Controle";

    char slotc = (s_cfg.slot == BLE_HID_SLOT_B) ? 'B' : 'A';
    snprintf(s_dev_name, sizeof(s_dev_name), "%s %c", prefix, slotc);

    if (strlen(s_dev_name) > 29) s_dev_name[29] = '\0';
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

static void ensure_event_loop(void)
{
    esp_err_t err = esp_event_loop_create_default();
    if (err == ESP_ERR_INVALID_STATE) return;
    ESP_ERROR_CHECK(err);
}

/* ===================== Slot A/B com endereço próprio (NVS) ===================== */

static void gen_static_rand_addr(uint8_t out[6])
{
    esp_fill_random(out, 6);
    out[5] = (out[5] & 0x3F) | 0xC0; // static random
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

    err = esp_ble_gap_set_rand_addr(addr);
    ESP_LOGI(TAG, "set_rand_addr(%c) => %s  [%02X:%02X:%02X:%02X:%02X:%02X]",
             (slot == BLE_HID_SLOT_B) ? 'B' : 'A',
             esp_err_to_name(err),
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    return err;
}

/* ===================== Advertising (GAP) ===================== */

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min        = 0x20,
    .adv_int_max        = 0x40,
    .adv_type           = ADV_TYPE_IND,
    .own_addr_type      = BLE_ADDR_TYPE_RANDOM,
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

static void schedule_adv_retry_ms(uint32_t ms)
{
    if (!s_adv_retry_t) return;
    (void)esp_timer_stop(s_adv_retry_t);
    esp_err_t e = esp_timer_start_once(s_adv_retry_t, (uint64_t)ms * 1000ULL);
    if (e != ESP_OK) ESP_LOGW(TAG, "adv_retry start_once failed: %s", esp_err_to_name(e));
}

static void try_start_adv(void)
{
    ESP_LOGI(TAG, "try_start_adv req=%d adv=%d scan=%d stop=%d run=%d start=%d hid=%d userStop=%d",
             (int)s_adv_requested, (int)s_adv_ready, (int)s_scan_rsp_ready,
             (int)s_adv_stopping, (int)s_adv_running, (int)s_adv_starting,
             (int)s_hid_started, (int)s_user_stop);

    if (s_user_stop) return;
    if (!s_adv_requested) return;
    if (!s_adv_ready) return;
    if (!s_scan_rsp_ready) return;
    if (s_adv_stopping) return;
    if (s_adv_running) return;
    if (s_adv_starting) return;
    if (!s_hid_started) return;

    esp_err_t err = esp_ble_gap_start_advertising(&s_adv_params);
    ESP_LOGI(TAG, "gap_start_adv => %s", esp_err_to_name(err));

    if (err == ESP_OK) {
        s_adv_starting = true;
        return;
    }

    if (err == ESP_ERR_INVALID_STATE) {
        // stack às vezes diz isso quando já tá anunciando / transicionando
        s_adv_running = true;
        s_adv_starting = false;
        schedule_adv_retry_ms(250);
        return;
    }

    s_adv_starting = false;
    schedule_adv_retry_ms(250);
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
        s_adv_running  = false;
        s_adv_starting = false;
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
    s_adv_stopping = false;
    try_start_adv();
}

/* ===================== Slot apply ===================== */

static void apply_pending_slot_if_any(void)
{
    if (!s_slot_pending) return;

    if (s_state == BLE_HID_STATE_CONNECTED) {
        ESP_LOGW(TAG, "slot pending but CONNECTED -> waiting disconnect");
        return;
    }

    ble_hid_slot_t slot = s_slot_pending_value;
    s_slot_pending = false;

    ESP_LOGI(TAG, "apply_pending_slot -> %c", (slot == BLE_HID_SLOT_B) ? 'B' : 'A');

    ESP_ERROR_CHECK(apply_slot_identity(slot));

    s_cfg.slot = slot;
    build_name();
    ESP_ERROR_CHECK(esp_ble_gap_set_device_name(s_dev_name));

    // reconfigura payloads pra refletir o nome novo no scan response
    s_adv_ready = false;
    s_scan_rsp_ready = false;
    ESP_ERROR_CHECK(config_adv_payloads());
}

/* ===================== GAP callback ===================== */

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
        s_adv_starting = false;
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Advertising started (name='%s')", s_dev_name);
            s_adv_running = true;
            set_state(BLE_HID_STATE_ADVERTISING);
            emit_evt(BLE_HID_EVT_ADVERTISING);
        } else {
            s_adv_running = false;
            ESP_LOGE(TAG, "ADV start failed, status=%d", param->adv_start_cmpl.status);
            schedule_adv_retry_ms(300);
        }
        break;

    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        ESP_LOGI(TAG, "Advertising stopped");
        s_adv_stopping = false;
        s_adv_running  = false;
        s_adv_starting = false;

        apply_pending_slot_if_any();
        try_start_adv();
        break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
        // “sim, pode iniciar segurança”
        ESP_LOGI(TAG, "SEC_REQ -> accept");
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;

    case ESP_GAP_BLE_NC_REQ_EVT:
        // Numeric Comparison: precisa confirmar dos dois lados
        ESP_LOGW(TAG, "NC_REQ passkey=%06lu -> auto-accept",
                (unsigned long)param->ble_security.key_notif.passkey);
        esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
        break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        ESP_LOGI(TAG, "AUTH_CMPL success=%d addr=%02X:%02X:%02X:%02X:%02X:%02X",
                param->ble_security.auth_cmpl.success,
                param->ble_security.auth_cmpl.bd_addr[0],
                param->ble_security.auth_cmpl.bd_addr[1],
                param->ble_security.auth_cmpl.bd_addr[2],
                param->ble_security.auth_cmpl.bd_addr[3],
                param->ble_security.auth_cmpl.bd_addr[4],
                param->ble_security.auth_cmpl.bd_addr[5]);
        break;

    default:
        break;
    }
}

/* ===================== GATTS wrapper (captura peer + repassa pro HID) ===================== */

static void gatts_wrapper_cb(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    // IMPORTANTÍSSIMO: repassa pro handler do esp_hidd
    esp_hidd_gatts_event_handler(event, gatts_if, param);

    switch (event) {
    case ESP_GATTS_CONNECT_EVT:
        // se já capturou o peer nessa conexão, ignora duplicados
        if (s_peer_valid) {
            // opcional: ESP_LOGD(TAG, "CONNECT duplicate ignored");
            break;
        }

        s_peer_valid = true;
        s_peer_gatts_if = gatts_if;
        s_peer_conn_id = param->connect.conn_id;
        memcpy(s_peer_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));

        ESP_LOGI(TAG, "GATTS CONNECT: conn_id=%u bda=%02X:%02X:%02X:%02X:%02X:%02X",
                (unsigned)s_peer_conn_id,
                s_peer_bda[0], s_peer_bda[1], s_peer_bda[2], s_peer_bda[3], s_peer_bda[4], s_peer_bda[5]);

        request_conn_params(param->connect.remote_bda);
        break;


    case ESP_GATTS_DISCONNECT_EVT: {
        if (!s_peer_valid) break; // ignora duplicados
        uint8_t r = (uint8_t)param->disconnect.reason;
        ESP_LOGW(TAG, "GATTS DISCONNECT reason=%u", (unsigned)r);
        s_peer_valid = false;
        on_disconnected_once(r);
        break;
    }

    default:
        break;
    }
}

/* ===================== HID device callback ===================== */

static void hidd_event_cb(void* handler_args, esp_event_base_t base, int32_t id, void* event_data)
{
    (void)handler_args;
    (void)base;

    esp_hidd_event_t evt = (esp_hidd_event_t)id;

    // Em alguns builds do IDF (o teu é um deles), o START pode vir com event_data == NULL.
    if (!event_data) {
        ESP_LOGW(TAG, "HID event %d with NULL data", (int)evt);

        // Se chegou START, o stack já “subiu” o suficiente pra gente liberar advertising.
        if (evt == ESP_HIDD_START_EVENT) {
            s_hid_started = true;
            try_start_adv();
        }
        return;
    }

    // Se vier dado, usa (mas ainda com cuidado)
    esp_hidd_event_data_t* p = (esp_hidd_event_data_t*)event_data;

    switch (evt) {

    case ESP_HIDD_START_EVENT:
        ESP_LOGI(TAG, "HID start status=%d", p->start.status);
        s_hid_started = (p->start.status == ESP_BT_STATUS_SUCCESS);
        try_start_adv();
        break;

    case ESP_HIDD_CONNECT_EVENT:
        ESP_LOGI(TAG, "HID connected status=%d", p->connect.status);
        on_connected_once();
        break;

    case ESP_HIDD_DISCONNECT_EVENT:
        ESP_LOGW(TAG, "HID disconnected reason=%d", p->disconnect.reason);
        on_disconnected_once((uint8_t)p->disconnect.reason);
        break;

    default:
        break;
    }
}

/* ===================== Once handlers ===================== */

static void on_connected_once(void)
{
    if (s_state == BLE_HID_STATE_CONNECTED) return;

    set_state(BLE_HID_STATE_CONNECTED);
    emit_evt(BLE_HID_EVT_CONNECTED);

    // ao conectar, o advertising some
    s_adv_running = false;
    s_adv_starting = false;
    s_adv_stopping = false;
}

static void on_disconnected_once(uint8_t reason)
{
    if (s_state != BLE_HID_STATE_CONNECTED) return;
    ESP_LOGW(TAG, "Disconnected (reason=%u) userStop=%d", (unsigned)reason, (int)s_user_stop);

    emit_evt(BLE_HID_EVT_DISCONNECTED);

    if (s_user_stop || !s_adv_requested) {
        set_state(BLE_HID_STATE_OFF);
        return;
    }

    set_state(BLE_HID_STATE_ADVERTISING);

    apply_pending_slot_if_any();

    // garante retorno do advertising
    s_adv_running = false;
    s_adv_starting = false;
    s_adv_stopping = false;

    schedule_adv_retry_ms(200);
    try_start_adv();
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
    setup_security_params();


    if (!s_adv_retry_t) {
        const esp_timer_create_args_t tcfg = {
            .callback = adv_retry_cb,
            .arg = NULL,
            .name = "adv_retry"
        };
        ESP_ERROR_CHECK(esp_timer_create(&tcfg, &s_adv_retry_t));
    }

    ESP_ERROR_CHECK(apply_slot_identity(s_cfg.slot));

    build_name();
    ESP_ERROR_CHECK(esp_ble_gap_set_device_name(s_dev_name));

    // reset flags
    s_hid_started = false;

    s_adv_requested = false;
    s_adv_ready = false;
    s_scan_rsp_ready = false;

    s_adv_running = false;
    s_adv_starting = false;
    s_adv_stopping = false;

    s_user_stop = false;

    s_slot_pending = false;

    s_peer_valid = false;
    memset(s_peer_bda, 0, sizeof(s_peer_bda));
    s_peer_gatts_if = ESP_GATT_IF_NONE;
    s_peer_conn_id = 0;

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

    // Aqui é a virada: wrapper nosso, mas ele repassa pro esp_hidd_gatts_event_handler
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_wrapper_cb));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(0x55));

    ESP_ERROR_CHECK(esp_hidd_dev_init(&hid_cfg, ESP_HID_TRANSPORT_BLE, hidd_event_cb, &s_hid));
    ESP_LOGI(TAG, "esp_hidd_dev_init ok, hid=%p", s_hid);

    set_state(BLE_HID_STATE_OFF);
    return ESP_OK;
}

esp_err_t ble_hid_start(void)
{
    ESP_LOGI(TAG, "start requested");

    s_user_stop = false;
    s_adv_requested = true;
    try_start_adv();
    return ESP_OK;
}

esp_err_t ble_hid_stop(void)
{
    ESP_LOGI(TAG, "stop requested");

    s_user_stop = true;
    s_adv_requested = false;

    // se estiver conectado, tenta derrubar
    if (s_state == BLE_HID_STATE_CONNECTED && s_peer_valid) {
        ESP_LOGW(TAG, "stop: disconnecting peer...");
        (void)esp_ble_gap_disconnect(s_peer_bda);
    }

    // se estiver anunciando, para
    request_stop_adv();

    set_state(BLE_HID_STATE_OFF);
    return ESP_OK;
}

esp_err_t ble_hid_set_slot(ble_hid_slot_t slot)
{
    ESP_LOGI(TAG, "set_slot requested -> %c", (slot == BLE_HID_SLOT_B) ? 'B' : 'A');

    s_slot_pending = true;
    s_slot_pending_value = slot;

    // Depois da troca, normalmente você quer voltar a anunciar
    s_adv_requested = true;

    // Se estiver conectado, precisa desconectar primeiro (senão não pode trocar addr)
    if (s_state == BLE_HID_STATE_CONNECTED && s_peer_valid) {
        ESP_LOGW(TAG, "CONNECTED: disconnecting to apply slot...");
        (void)esp_ble_gap_disconnect(s_peer_bda);
        return ESP_OK;
    }

    // Se estiver anunciando (ou tentando), para primeiro e aplica no STOP_COMPLETE
    if (s_adv_running || s_adv_starting || s_adv_stopping) {
        request_stop_adv();
        return ESP_OK;
    }

    // Se não estava nem conectado nem anunciando, aplica já
    apply_pending_slot_if_any();
    try_start_adv();
    return ESP_OK;
}

ble_hid_state_t ble_hid_get_state(void)
{
    return s_state;
}
static uint8_t cc_mask_for_key(ble_hid_cc_t key)
{
    switch (key) {
        case BLE_HID_CC_VOL_UP:      return (1u << 0);
        case BLE_HID_CC_VOL_DOWN:    return (1u << 1);
        case BLE_HID_CC_MUTE:        return (1u << 2);
        case BLE_HID_CC_PREV:        return (1u << 3);
        case BLE_HID_CC_PLAY_PAUSE:  return (1u << 4);
        case BLE_HID_CC_NEXT:        return (1u << 5);
        default: return 0;
    }
}

esp_err_t ble_hid_send_cc(ble_hid_cc_t key)
{
    if (s_state != BLE_HID_STATE_CONNECTED || !s_hid) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t m = cc_mask_for_key(key);
    if (!m) return ESP_ERR_INVALID_ARG;

    // press (1 byte)
    esp_err_t err = esp_hidd_dev_input_set(s_hid, 0, HID_RPT_ID_CC_IN, &m, HID_CC_IN_RPT_LEN);
    if (err != ESP_OK) return err;

    vTaskDelay(pdMS_TO_TICKS(15));

    // release (zera 1 byte)
    uint8_t z = 0;
    return esp_hidd_dev_input_set(s_hid, 0, HID_RPT_ID_CC_IN, &z, HID_CC_IN_RPT_LEN);
}