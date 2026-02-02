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
    BLE_HID_STATE_ADVERTISING,
    BLE_HID_STATE_CONNECTED,
} ble_hid_state_t;

typedef struct {
    ble_hid_slot_t slot;
} ble_hid_cfg_t;

// Etapa 2.0: stub (só pra compilar com BT habilitado)
esp_err_t ble_hid_init(const ble_hid_cfg_t* cfg);
void      ble_hid_deinit(void);

#ifdef __cplusplus
}
#endif
