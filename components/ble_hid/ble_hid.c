#include "ble_hid.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char* TAG = "ble_hid";

// Etapa 2.0: só garantir que BT foi habilitado no menuconfig.
#if !defined(CONFIG_BT_ENABLED) || (CONFIG_BT_ENABLED == 0)
#error "Bluetooth desabilitado. Habilite em: idf.py menuconfig -> Component config -> Bluetooth"
#endif

esp_err_t ble_hid_init(const ble_hid_cfg_t* cfg)
{
    const char* slot = (cfg && cfg->slot == BLE_HID_SLOT_B) ? "B" : "A";
    ESP_LOGI(TAG, "init stub (slot=%s) - BLE real entra na Etapa 2.1", slot);
    return ESP_OK;
}

void ble_hid_deinit(void)
{
    ESP_LOGI(TAG, "deinit stub");
}
