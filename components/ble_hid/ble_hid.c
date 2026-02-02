#include "ble_hid.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_err.h"
#include "sdkconfig.h"

#include "nvs_flash.h"
#include "esp_bt.h"

// ESP HID Device (componente esp_hid)
#include "esp_hidd.h"
#include "esp_hid_common.h"

// NimBLE (GAP advertising)
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "services/gap/ble_svc_gap.h"

/* ===================== Checks de config ===================== */

#if !CONFIG_BT_ENABLED
#error "Bluetooth desabilitado. Habilite em menuconfig: Component config -> Bluetooth -> Bluetooth"
#endif

#if !CONFIG_BT_NIMBLE_ENABLED
#error "NimBLE desabilitado. Habilite em menuconfig: Component config -> Bluetooth -> NimBLE"
#endif

#if !CONFIG_BT_NIMBLE_ROLE_PERIPHERAL
#error "NimBLE Peripheral role desabilitado. Habilite: Component config -> Bluetooth -> NimBLE -> Peripheral Role"
#endif

static const char* TAG = "ble_hid";

/* ===================== Report Map (Consumer / Media) ===================== */
/* Mantive o seu, porque você já tá montando a base do consumer control. */
static const unsigned char mediaReportMap[] = {
    0x05, 0x0C,       // Usage Page (Consumer)
    0x09, 0x01,       // Usage (Consumer Control)
    0xA1, 0x01,       // Collection (Application)

    0x85, 0x03,       // Report ID (3)

    0x09, 0x02,       // Usage (Numeric Key Pad)
    0xA1, 0x02,       // Collection (Logical)
    0x05, 0x09,       // Usage Page (Button)
    0x19, 0x01,       // Usage Minimum (1)
    0x29, 0x0A,       // Usage Maximum (10)
    0x15, 0x01,       // Logical Minimum (1)
    0x25, 0x0A,       // Logical Maximum (10)
    0x75, 0x04,       // Report Size (4)
    0x95, 0x01,       // Report Count (1)
    0x81, 0x00,       // Input (Data,Array,Abs)
    0xC0,             // End Collection

    0x05, 0x0C,       // Usage Page (Consumer)
    0x09, 0x86,       // Usage (Channel)
    0x15, 0xFF,       // Logical Minimum (-1)
    0x25, 0x01,       // Logical Maximum (1)
    0x75, 0x02,       // Report Size (2)
    0x95, 0x01,       // Report Count (1)
    0x81, 0x46,       // Input (Data,Var,Rel,Null State)

    0x09, 0xE9,       // Usage (Volume Increment)
    0x09, 0xEA,       // Usage (Volume Decrement)
    0x15, 0x00,       // Logical Minimum (0)
    0x75, 0x01,       // Report Size (1)
    0x95, 0x02,       // Report Count (2)
    0x81, 0x02,       // Input (Data,Var,Abs)

    0x09, 0xE2,       // Usage (Mute)
    0x09, 0x30,       // Usage (Power)
    0x09, 0x83,       // Usage (Recall Last)
    0x09, 0x81,       // Usage (Assign Selection)
    0x09, 0xB0,       // Usage (Play)
    0x09, 0xB1,       // Usage (Pause)
    0x09, 0xB2,       // Usage (Record)
    0x09, 0xB3,       // Usage (Fast Forward)
    0x09, 0xB4,       // Usage (Rewind)
    0x09, 0xB5,       // Usage (Scan Next Track)
    0x09, 0xB6,       // Usage (Scan Previous Track)
    0x09, 0xB7,       // Usage (Stop)
    0x15, 0x01,       // Logical Minimum (1)
    0x25, 0x0C,       // Logical Maximum (12)
    0x75, 0x04,       // Report Size (4)
    0x95, 0x01,       // Report Count (1)
    0x81, 0x00,       // Input (Data,Array,Abs)

    0x09, 0x80,       // Usage (Selection)
    0xA1, 0x02,       // Collection (Logical)
    0x05, 0x09,       // Usage Page (Button)
    0x19, 0x01,       // Usage Minimum (1)
    0x29, 0x03,       // Usage Maximum (3)
    0x15, 0x01,       // Logical Minimum (1)
    0x25, 0x03,       // Logical Maximum (3)
    0x75, 0x02,       // Report Size (2)
    0x81, 0x00,       // Input (Data,Array,Abs)
    0xC0,             // End Collection

    0x81, 0x03,       // Input (Const,Var,Abs)
    0xC0              // End Collection
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    { .data = mediaReportMap, .len = sizeof(mediaReportMap) }
};

/* ===================== Estado interno ===================== */

static ble_hid_cfg_t s_cfg;
static ble_hid_state_t s_state = BLE_HID_STATE_OFF;
static esp_hidd_dev_t* s_hid = NULL;

static char s_dev_name[32];

static bool s_stack_started = false;
static bool s_adv_requested = false;

static void set_state(ble_hid_state_t st) { s_state = st; }

static void emit_evt(ble_hid_evt_t evt)
{
    if (s_cfg.on_evt) s_cfg.on_evt(evt, s_cfg.user);
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

/* ===================== Nome (A/B) ===================== */

static void build_name(void)
{
    const char* prefix = (s_cfg.device_name_prefix && s_cfg.device_name_prefix[0])
        ? s_cfg.device_name_prefix
        : "Controle";

    char slotc = (s_cfg.slot == BLE_HID_SLOT_B) ? 'B' : 'A';
    snprintf(s_dev_name, sizeof(s_dev_name), "%s %c", prefix, slotc);

    // Atualiza o GAP device name (NimBLE GAP service)
    ble_svc_gap_device_name_set(s_dev_name);
}

/* ===================== Advertising (NimBLE) ===================== */

static int gap_event_cb(struct ble_gap_event* event, void* arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "ADV complete, reason=%d", event->adv_complete.reason);
        // se ainda queremos ficar anunciando, tenta de novo
        if (s_adv_requested && s_stack_started) {
            // restart
            // (não chama aqui direto pra não ficar loop nervoso se der erro)
        }
        break;

    default:
        break;
    }

    return 0;
}

static esp_err_t adv_set_fields(uint16_t appearance, const char* name)
{
    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof(f));

    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    // Nome
    f.name = (const uint8_t*)name;
    f.name_len = (uint8_t)strlen(name);
    f.name_is_complete = 1;

    // TX power automático (opcional)
    f.tx_pwr_lvl_is_present = 1;
    f.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    // Appearance (ajuda alguns hosts)
    f.appearance_is_present = 1;
    f.appearance = appearance;

    int rc = ble_gap_adv_set_fields(&f);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields rc=%d", rc);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t adv_start(void)
{
    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto rc=%d", rc);
        return ESP_FAIL;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND; // conectável
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN; // discoverable

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                          &adv_params, gap_event_cb, NULL);

    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start rc=%d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Advertising started (name='%s')", s_dev_name);
    set_state(BLE_HID_STATE_ADVERTISING);
    emit_evt(BLE_HID_EVT_ADVERTISING);
    return ESP_OK;
}

static void adv_start_if_enabled(void)
{
    if (!s_stack_started) return;
    if (!s_adv_requested) return;

    // Atualiza fields sempre que for anunciar (nome/appearance)
    (void)adv_set_fields(ESP_HID_APPEARANCE_GENERIC, s_dev_name);
    (void)adv_start();
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
        if (p->start.status == ESP_OK) {
            s_stack_started = true;

            // O nome pode ser aplicado aqui também
            build_name();

            // Se alguém já pediu start, anuncia agora
            adv_start_if_enabled();
        }
        break;

    case ESP_HIDD_CONNECT_EVENT:
        ESP_LOGI(TAG, "HID connected status=%d", p->connect.status);
        set_state(BLE_HID_STATE_CONNECTED);
        emit_evt(BLE_HID_EVT_CONNECTED);
        break;

    case ESP_HIDD_DISCONNECT_EVENT:
        ESP_LOGW(TAG, "HID disconnected reason=%d", p->disconnect.reason);
        emit_evt(BLE_HID_EVT_DISCONNECTED);

        // volta a anunciar se ainda estiver “ligado”
        set_state(BLE_HID_STATE_ADVERTISING);
        adv_start_if_enabled();
        break;

    default:
        break;
    }
}

/* ===================== API pública ===================== */

esp_err_t ble_hid_init(const ble_hid_cfg_t* cfg)
{
    if (cfg) s_cfg = *cfg;

    ESP_LOGI(TAG, "init (slot=%c)",
             (s_cfg.slot == BLE_HID_SLOT_B) ? 'B' : 'A');

    ESP_ERROR_CHECK(nvs_safe_init());

    // BLE only: libera memória de classic (se tiver)
    (void)esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

    // Esse nome é aplicado no START_EVENT (quando stack tá pronta),
    // mas já montamos agora pra log/consistência.
    snprintf(s_dev_name, sizeof(s_dev_name), "Controle %c",
             (s_cfg.slot == BLE_HID_SLOT_B) ? 'B' : 'A');

    // Config HID
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

    s_stack_started = false;
    s_adv_requested = false;
    s_hid = NULL;

    ESP_ERROR_CHECK(esp_hidd_dev_init(&hid_cfg, ESP_HID_TRANSPORT_BLE, hidd_event_cb, &s_hid));

    set_state(BLE_HID_STATE_OFF);
    return ESP_OK;
}

esp_err_t ble_hid_start(void)
{
    ESP_LOGI(TAG, "start requested");
    s_adv_requested = true;

    // Se o stack já subiu, anuncia agora.
    adv_start_if_enabled();
    return ESP_OK;
}

esp_err_t ble_hid_stop(void)
{
    ESP_LOGI(TAG, "stop requested");
    s_adv_requested = false;

    // Para advertising se estiver rodando
    (void)ble_gap_adv_stop();

    set_state(BLE_HID_STATE_OFF);
    return ESP_OK;
}

esp_err_t ble_hid_set_slot(ble_hid_slot_t slot)
{
    s_cfg.slot = slot;

    // Atualiza nome (e advertising se estiver ativo)
    build_name();

    ESP_LOGI(TAG, "set_slot -> %c (name='%s')",
             (slot == BLE_HID_SLOT_B) ? 'B' : 'A',
             s_dev_name);

    // Se estiver anunciando, reinicia o adv pro nome novo “pegar”
    if (s_adv_requested && s_stack_started) {
        (void)ble_gap_adv_stop();
        adv_start_if_enabled();
    }

    return ESP_OK;
}

ble_hid_state_t ble_hid_get_state(void)
{
    return s_state;
}
