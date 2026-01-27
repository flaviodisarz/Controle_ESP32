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
    uint32_t flash_ms;            // flash curto
    uint32_t pulse_on_ms;         // pulso 5min: tempo ligado
    uint32_t pulse_off_ms;        // pulso 5min: tempo desligado
    uint32_t pulse_period_ms;     // 5 min = 300000
} led_config_t;

void led_init(const led_config_t* cfg);
void led_set_mode(led_mode_t mode);

// utilitários (BTN7/BTN9 / avisos)
void led_pulse_rgb(uint8_t r, uint8_t g, uint8_t b, uint32_t on_ms);
void led_blink_rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t times, uint32_t on_ms, uint32_t off_ms);

// apaga imediatamente (pra entrar em sleep sem deixar LED aceso)
void led_off(void);

// legado/debug (não usamos agora)
void led_button_feedback(uint8_t id);

#ifdef __cplusplus
}
#endif
