#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "input.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_MODE_MEDIA = 0,
    APP_MODE_DEVICES = 1,
} app_mode_t;

typedef struct {
    uint32_t long_press_ms;          // BTN1 longo (fase 1)
    uint32_t media_light_sleep_ms;   // futuro
    uint32_t media_deep_sleep_ms;    // futuro
    uint32_t dev_deep_sleep_ms;      // futuro
} state_config_t;

void state_init(const state_config_t* cfg);
void state_start(void);
bool state_post_input(const input_event_t* ev);

#ifdef __cplusplus
}
#endif
