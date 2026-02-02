#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BLE_HID_SLOT_A = 0,
    BLE_HID_SLOT_B = 1,
} ble_hid_slot_t;

typedef enum {
    BLE_HID_STATE_OFF = 0,
    BLE_HID_STATE_ADVERTISING,
    BLE_HID_STATE_CONNECTED,
} ble_hid_state_t;

typedef enum {
    BLE_HID_EVT_ADVERTISING = 0,
    BLE_HID_EVT_CONNECTED,
    BLE_HID_EVT_DISCONNECTED,
} ble_hid_evt_t;

typedef void (*ble_hid_evt_cb_t)(ble_hid_evt_t evt, void* user);

typedef struct {
    ble_hid_slot_t slot;
    const char* device_name_prefix;   // ex: "Controle"
    ble_hid_evt_cb_t on_evt;
    void* user;
} ble_hid_cfg_t;

esp_err_t       ble_hid_init(const ble_hid_cfg_t* cfg);
esp_err_t       ble_hid_start(void);
esp_err_t       ble_hid_stop(void);
esp_err_t       ble_hid_set_slot(ble_hid_slot_t slot);
ble_hid_state_t ble_hid_get_state(void);

#ifdef __cplusplus
}
#endif
