#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_MODE_MEDIA = 0,
    LED_MODE_DEVICES = 1,
} led_mode_t;

typedef struct {
    uint8_t  brightness;         // 0..255
    uint32_t flash_ms;            // flash curto no botão
    uint32_t pulse_on_ms;         // piscada 5min: tempo ligado
    uint32_t pulse_off_ms;        // piscada 5min: tempo desligado
    uint32_t pulse_period_ms;     // 5 min = 300000
} led_config_t;

void led_init(const led_config_t* cfg);
void led_set_mode(led_mode_t mode);

// Chame quando receber DOWN de id 1..8
void led_button_feedback(uint8_t id);

#ifdef __cplusplus
}
#endif
