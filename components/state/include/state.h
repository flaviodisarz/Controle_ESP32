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
    uint32_t long_press_ms;  // padrão para long press (ex: 700ms)
} state_config_t;

void state_init(const state_config_t* cfg);
void state_start(void);

// callback do input (rápido, sem travar)
bool state_post_input(const input_event_t* ev);

#ifdef __cplusplus
}
#endif
