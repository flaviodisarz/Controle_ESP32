#include "led.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"

#include "led_strip.h"
#include "pins.h"  // precisa ter o pino do WS2812 aqui
#define PIN_LED_WS2812 PIN_LED_DATA
static const char* TAG = "led";

// ========= Ajuste aqui se teu pins.h usar outro nome =========
// Eu vou assumir que você tem algo como PIN_LED_WS2812 = 10.
// Se no seu pins.h for PIN_WS2812 ou PIN_LED, troca aqui:
#ifndef PIN_LED_WS2812
// #define PIN_LED_WS2812 PIN_WS2812
// #define PIN_LED_WS2812 10
#endif
// =============================================================

typedef enum {
    LED_CMD_FLASH = 0,
    LED_CMD_PULSE = 1,
} led_cmd_type_t;

typedef struct {
    led_cmd_type_t type;
    uint8_t r, g, b;
    uint32_t on_ms;
    uint32_t off_ms;
} led_cmd_t;

static led_config_t s_cfg;
static led_mode_t s_mode = LED_MODE_MEDIA;

static led_strip_handle_t s_strip = NULL;
static QueueHandle_t      s_q     = NULL;
static esp_timer_handle_t s_timer = NULL;

static inline uint8_t scale(uint8_t v)
{
    // brilho global simples (0..255)
    // (v * brightness) / 255
    return (uint8_t)(((uint16_t)v * (uint16_t)s_cfg.brightness) / 255);
}
static void mode_color(led_mode_t mode, uint8_t* r, uint8_t* g, uint8_t* b)
{
    // MEDIA = azul, DEVICES = rosa
if (mode == LED_MODE_MEDIA) {
        *r = 0;   *g = 80;  *b = 255;   // azulzinho vivo
    } else {
        *r = 255; *g = 0;   *b = 110;   // rosa
    }
}

static void strip_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) return;

    r = scale(r);
    g = scale(g);
    b = scale(b);

    // led_strip_set_pixel(strip, index, red, green, blue)
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

static void strip_off(void)
{
    if (!s_strip) return;
    led_strip_clear(s_strip);
    led_strip_refresh(s_strip);
}

static void led_task(void* arg)
{
    (void)arg;

    led_cmd_t cmd;
    strip_off();

    while (1) {
        if (xQueueReceive(s_q, &cmd, portMAX_DELAY) == pdTRUE) {

            switch (cmd.type) {
                case LED_CMD_FLASH:
                case LED_CMD_PULSE:
                default:
                    strip_set_rgb(cmd.r, cmd.g, cmd.b);
                    vTaskDelay(pdMS_TO_TICKS(cmd.on_ms));

                    strip_off();
                    vTaskDelay(pdMS_TO_TICKS(cmd.off_ms));
                    break;
            }
        }
    }
}

static void timer_cb(void* arg)
{
    (void)arg;

    if (!s_q) return;

    led_cmd_t cmd = {0};
    cmd.type = LED_CMD_PULSE;
    mode_color(s_mode, &cmd.r, &cmd.g, &cmd.b);
    cmd.on_ms  = s_cfg.pulse_on_ms;
    cmd.off_ms = s_cfg.pulse_off_ms;

    // callback do esp_timer roda numa task, então pode usar xQueueSend (sem bloquear)
    (void)xQueueSend(s_q, &cmd, 0);
}

void led_init(const led_config_t* cfg)
{
    // defaults
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.brightness       = 24;        // baixo pra bateria (ajuste depois)
    s_cfg.flash_ms         = 60;        // flash curto
    s_cfg.pulse_on_ms      = 140;       // piscada média
    s_cfg.pulse_off_ms     = 60;
    s_cfg.pulse_period_ms  = 300000;    // 5 min

    if (cfg) s_cfg = *cfg;

    ESP_LOGI(TAG, "init: bright=%u flash=%ums pulse=%ums/%ums period=%ums",
             (unsigned)s_cfg.brightness,
             (unsigned)s_cfg.flash_ms,
             (unsigned)s_cfg.pulse_on_ms,
             (unsigned)s_cfg.pulse_off_ms,
             (unsigned)s_cfg.pulse_period_ms);

    // fila de comandos do LED
    s_q = xQueueCreate(8, sizeof(led_cmd_t));
    if (!s_q) {
        ESP_LOGE(TAG, "queue create failed");
        return;
    }

    // cria o driver do WS2812 via RMT (led_strip)
    led_strip_config_t strip_config = {
        .strip_gpio_num = PIN_LED_WS2812,
        .max_leds = 1,
        .led_pixel_format = LED_STRIP_PIXEL_FORMAT_GRB, // WS2812 costuma ser GRB
        .led_model = LED_STRIP_LED_MODEL_WS2812,
        .flags.invert_out = 0,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .mem_block_symbols = 64,
        .flags.with_dma = 0,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(err));
        return;
    }

    // task do LED (engine)
    xTaskCreate(led_task, "led_task", 4096, NULL, 9, NULL);

    // timer 5 min (pisca “vida”)
    const esp_timer_create_args_t tcfg = {
        .callback = timer_cb,
        .name = "led_pulse_5min"
    };
    ESP_ERROR_CHECK(esp_timer_create(&tcfg, &s_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_timer, (uint64_t)s_cfg.pulse_period_ms * 1000ULL));
}

void led_set_mode(led_mode_t mode)
{
    s_mode = mode;
    ESP_LOGI(TAG, "mode=%s", (mode == APP_MODE_MEDIA) ? "MEDIA" : "DEVICES");
}

void led_button_feedback(uint8_t id)
{
    if (!s_q) return;

    // Etapa 6: só 1..8
    if (id < 1 || id > 8) return;

    led_cmd_t cmd = {0};
    cmd.type = LED_CMD_FLASH;
    mode_color(s_mode, &cmd.r, &cmd.g, &cmd.b);
    cmd.on_ms  = s_cfg.flash_ms;
    cmd.off_ms = 0;

    (void)xQueueSend(s_q, &cmd, 0);
}
