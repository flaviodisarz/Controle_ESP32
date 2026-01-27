#include "state.h"

#include <string.h>
#include <stdint.h>
#include "esp_log.h"

#include "led.h"
#include "battery.h"

static const char* TAG = "state";

static app_mode_t s_mode = APP_MODE_MEDIA;
static state_config_t s_cfg;

// Slot local (pra não depender de state.h)
#define SLOT_A 0
#define SLOT_B 1
static uint8_t s_slot = SLOT_A;

// ===================== helpers =====================

static void media_toggle_slot(void)
{
    s_slot = (s_slot == SLOT_A) ? SLOT_B : SLOT_A;

    if (s_slot == SLOT_B) {
        ESP_LOGI(TAG, "Slot B Conectando");
        led_blink_rgb(0, 80, 255, 3, 60, 60);   // 3 piscadas rápidas azul
    } else {
        ESP_LOGI(TAG, "Slot A Conectando");
        led_blink_rgb(0, 80, 255, 2, 60, 60);   // 2 piscadas rápidas azul
    }
}

static void toggle_mode(void)
{
    s_mode = (s_mode == APP_MODE_MEDIA) ? APP_MODE_DEVICES : APP_MODE_MEDIA;

    if (s_mode == APP_MODE_DEVICES) {
        ESP_LOGI(TAG, "Modo DISPOSITIVOS");
        led_set_mode(LED_MODE_DEVICES);
        led_pulse_rgb(255, 0, 110, 350);       // longo rosa
    } else {
        ESP_LOGI(TAG, "Modo MIDIA");
        led_set_mode(LED_MODE_MEDIA);
        led_pulse_rgb(0, 80, 255, 350);        // longo azul
    }
}

// ===================== handlers =====================

static void handle_media_short(uint8_t id)
{
    switch (id) {
        case 1: ESP_LOGI(TAG, "MEDIA: LIGHT SLEEP"); break;
        case 2: ESP_LOGI(TAG, "MEDIA: VOL+"); break;
        case 3: ESP_LOGI(TAG, "MEDIA: MUTE"); break;
        case 4: ESP_LOGI(TAG, "MEDIA: PREV"); break;
        case 5: ESP_LOGI(TAG, "MEDIA: PLAY/PAUSE"); break;
        case 6: ESP_LOGI(TAG, "MEDIA: NEXT"); break;
        case 7: media_toggle_slot(); break;                 // ✅ BTN7 alterna Slot A/B
        case 8: ESP_LOGI(TAG, "MEDIA: VOL-"); break;
        default: ESP_LOGW(TAG, "MEDIA: id invalido=%u", (unsigned)id); break;
    }
}

static void handle_media_long(uint8_t id, uint32_t ms)
{
    if (id == 1) ESP_LOGI(TAG, "MEDIA: DEEP SLEEP (long %ums)", (unsigned)ms);
    else         ESP_LOGI(TAG, "MEDIA: LONG ignorado (id=%u, %ums)", (unsigned)id, (unsigned)ms);
}

static void handle_devices_short(uint8_t id)
{
    if (id >= 1 && id <= 8) ESP_LOGI(TAG, "DEVICES: Toggle Luz %u", (unsigned)id);
    else                    ESP_LOGW(TAG, "DEVICES: id invalido=%u", (unsigned)id);
}

static void handle_devices_long(uint8_t id, uint32_t ms)
{
    ESP_LOGI(TAG, "DEVICES: LONG ignorado (id=%u, %ums)", (unsigned)id, (unsigned)ms);
}

// ===================== API =====================

void state_init(const state_config_t* cfg)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.long_press_ms = 700;

    if (cfg) s_cfg = *cfg;

    led_init(NULL);
    battery_init();

    led_set_mode((s_mode == APP_MODE_MEDIA) ? LED_MODE_MEDIA : LED_MODE_DEVICES);

    ESP_LOGI(TAG, "init: mode=%s long=%ums slot=%s",
             (s_mode == APP_MODE_MEDIA) ? "MEDIA" : "DEVICES",
             (unsigned)s_cfg.long_press_ms,
             (s_slot == SLOT_A) ? "A" : "B");
}

void state_start(void)
{
    ESP_LOGI(TAG, "start (ok)");
}

bool state_post_input(const input_event_t* ev)
{
    if (!ev) return false;

    // ✅ não pisca em tecla normal no DOWN (acabou a balada)
    // (o pulso de 5min fica no led.c via timer)

    if (ev->type == INPUT_EV_SHORT) {

        // ✅ BTN9 SHORT: alterna modo em qualquer estado
        if (ev->id == 9) {
            toggle_mode();
            return true;
        }

        if (s_mode == APP_MODE_MEDIA) handle_media_short(ev->id);
        else                          handle_devices_short(ev->id);
        return true;
    }

    if (ev->type == INPUT_EV_LONG) {

        // ✅ BTN9 LONG: “Carga Bateria”
        if (ev->id == 9) {
            ESP_LOGI(TAG, "Carga Bateria");
            return true;
        }

        if (s_mode == APP_MODE_MEDIA) handle_media_long(ev->id, ev->duration_ms);
        else                          handle_devices_long(ev->id, ev->duration_ms);
        return true;
    }

    return false;
}
