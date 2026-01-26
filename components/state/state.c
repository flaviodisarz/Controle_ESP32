#include "state.h"

#include <string.h>
#include "esp_log.h"

#include "led.h"
#include "battery.h"

static const char* TAG = "state";

static app_mode_t s_mode = APP_MODE_MEDIA;
static state_config_t s_cfg;

static void handle_media_short(uint8_t id) {
    switch (id) {
        case 1: ESP_LOGI(TAG, "MEDIA: LIGHT SLEEP"); break;
        case 2: ESP_LOGI(TAG, "MEDIA: VOL+"); break;
        case 3: ESP_LOGI(TAG, "MEDIA: MUTE"); break;
        case 4: ESP_LOGI(TAG, "MEDIA: PREV"); break;
        case 5: ESP_LOGI(TAG, "MEDIA: PLAY/PAUSE"); break;
        case 6: ESP_LOGI(TAG, "MEDIA: NEXT"); break;
        case 7: ESP_LOGI(TAG, "MEDIA: SLOT (fase 2)"); break;
        case 8: ESP_LOGI(TAG, "MEDIA: VOL-"); break;
        case 9: ESP_LOGI(TAG, "MEDIA: BTN9 (fase 2)"); break;
        default: ESP_LOGW(TAG, "MEDIA: id invalido=%u", (unsigned)id); break;
    }
}

static void handle_media_long(uint8_t id, uint32_t ms) {
    if (id == 1) ESP_LOGI(TAG, "MEDIA: DEEP SLEEP (long %ums)", (unsigned)ms);
    else         ESP_LOGI(TAG, "MEDIA: LONG ignorado (id=%u, %ums)", (unsigned)id, (unsigned)ms);
}

static void handle_devices_short(uint8_t id) {
    if (id >= 1 && id <= 8) ESP_LOGI(TAG, "DEVICES: Toggle Luz %u", (unsigned)id);
    else if (id == 9)       ESP_LOGI(TAG, "DEVICES: BTN9 (fase 2)");
    else                    ESP_LOGW(TAG, "DEVICES: id invalido=%u", (unsigned)id);
}

static void handle_devices_long(uint8_t id, uint32_t ms) {
    ESP_LOGI(TAG, "DEVICES: LONG ignorado (id=%u, %ums)", (unsigned)id, (unsigned)ms);
}

void state_init(const state_config_t* cfg)
{
    // defaults
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.long_press_ms = 700;

    if (cfg) s_cfg = *cfg;

    led_init(NULL);
    battery_init();

    // converte STATE->LED (sem LED conhecer state.h)
    led_set_mode((s_mode == APP_MODE_MEDIA) ? LED_MODE_MEDIA : LED_MODE_DEVICES);

    ESP_LOGI(TAG, "init: mode=%s long=%ums",
             (s_mode == APP_MODE_MEDIA) ? "MEDIA" : "DEVICES",
             (unsigned)s_cfg.long_press_ms);
}

void state_start(void)
{
    ESP_LOGI(TAG, "start (ok)");
}

bool state_post_input(const input_event_t* ev)
{
    if (!ev) return false;

    // LED: só no DOWN e só 1..8 (pra não piscar 3x no mesmo clique)
    if (ev->type == INPUT_EV_DOWN) {
        if (ev->id >= 1 && ev->id <= 8) {
            led_button_feedback(ev->id);
        }
        return true;
    }

    if (ev->type == INPUT_EV_SHORT) {
        if (s_mode == APP_MODE_MEDIA) handle_media_short(ev->id);
        else                          handle_devices_short(ev->id);
        return true;
    }

    if (ev->type == INPUT_EV_LONG) {
        if (s_mode == APP_MODE_MEDIA) handle_media_long(ev->id, ev->duration_ms);
        else                          handle_devices_long(ev->id, ev->duration_ms);
        return true;
    }

    return false;
}
