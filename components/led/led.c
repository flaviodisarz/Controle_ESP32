#include "led.h"
#include "esp_log.h"

static const char* TAG = "led";
static led_mode_t s_mode = LED_MODE_MEDIA;

void led_init(void)
{
    ESP_LOGI(TAG, "init (stub)");
}

void led_set_mode(led_mode_t mode)
{
    s_mode = mode;
    ESP_LOGI(TAG, "mode=%s", (s_mode == LED_MODE_MEDIA) ? "MEDIA" : "DEVICES");
}

void led_feedback_button(uint8_t btn_id)
{
    // Etapa 1: só log. Depois vira WS2812 de verdade.
    ESP_LOGI(TAG, "button %u feedback (mode=%s)",
             (unsigned)btn_id,
             (s_mode == LED_MODE_MEDIA) ? "MEDIA" : "DEVICES");
}
