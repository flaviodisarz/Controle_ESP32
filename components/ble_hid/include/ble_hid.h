#pragma once

#include <stdint.h>
#include <stdbool.h>
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
    BLE_HID_STATE_ADVERTISING = 1,
    BLE_HID_STATE_CONNECTED = 2,
} ble_hid_state_t;

typedef enum {
    BLE_HID_EVT_ADVERTISING = 0,
    BLE_HID_EVT_CONNECTED = 1,
    BLE_HID_EVT_DISCONNECTED = 2,
} ble_hid_evt_t;

typedef void (*ble_hid_evt_cb_t)(ble_hid_evt_t evt, void* user);

typedef struct {
    ble_hid_slot_t slot;
    const char* device_name_prefix;   // ex: "Controle"
    ble_hid_evt_cb_t on_evt;          // callback pra state/led
    void* user;                       // ctx do callback
} ble_hid_cfg_t;

typedef enum {
    BLE_HID_CC_VOL_UP = 0,
    BLE_HID_CC_VOL_DOWN,
    BLE_HID_CC_MUTE,
    BLE_HID_CC_PREV,
    BLE_HID_CC_PLAY_PAUSE,
    BLE_HID_CC_NEXT,
} ble_hid_cc_t;

esp_err_t ble_hid_init(const ble_hid_cfg_t* cfg);
esp_err_t ble_hid_start(void);
esp_err_t ble_hid_stop(void);
esp_err_t ble_hid_set_slot(ble_hid_slot_t slot);

ble_hid_state_t ble_hid_get_state(void);
static inline bool ble_hid_is_connected(void) {
    return ble_hid_get_state() == BLE_HID_STATE_CONNECTED;
}

// ✅ Etapa 2.2: press + release via o teu report map (1 byte / 6 bits)
esp_err_t ble_hid_send_cc(ble_hid_cc_t key);

#ifdef __cplusplus
}
#endif
