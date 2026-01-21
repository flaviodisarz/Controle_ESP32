#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_MODE_MEDIA = 0,
    LED_MODE_DEVICES = 1,
} led_mode_t;

void led_init(void);
void led_set_mode(led_mode_t mode);

// feedback simples (Fase 1): pisca curto quando qualquer botão (1..8) é acionado
void led_feedback_button(uint8_t btn_id);

#ifdef __cplusplus
}
#endif
