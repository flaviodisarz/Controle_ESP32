#include "state.h"
#include "led.h"
#include "battery.h"
#include "esp_log.h"

static const char* TAG = "state";

static app_mode_t s_mode = APP_MODE_MEDIA;
static state_config_t s_cfg;

void state_init(const state_config_t* cfg)
{
    if (cfg) s_cfg = *cfg;
    else s_cfg.long_press_ms = 700;

    led_init();
    battery_init();
    led_set_mode(LED_MODE_MEDIA);

    ESP_LOGI(TAG, "init: mode=MEDIA long=%ums", (unsigned)s_cfg.long_press_ms);
}

void state_start(void)
{
    ESP_LOGI(TAG, "start (stub)");
}

bool state_post_input(const input_event_t* ev)
{
    if (!ev) return false;

    // Etapa 1: só logs e feedback simples
    ESP_LOGI(TAG, "ev: btn=%u type=%d dur=%ums mode=%s",
             (unsigned)ev->id, (int)ev->type, (unsigned)ev->duration_ms,
             (s_mode == APP_MODE_MEDIA) ? "MEDIA" : "DEVICES");

    if (ev->id >= 1 && ev->id <= 8) {
        led_feedback_button(ev->id);
    }

    return true;
}
