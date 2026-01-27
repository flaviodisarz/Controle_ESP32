#include "led.h"

#include <string.h>

#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"

#include "led_strip.h"
#include "pins.h"

#define PIN_LED_WS2812 PIN_LED_DATA
static const char* TAG = "led";

typedef enum {
    LED_CMD_PULSE = 0,
    LED_CMD_BLINK = 1,
} led_cmd_type_t;

typedef struct {
    led_cmd_type_t type;
    uint8_t r, g, b;
    uint8_t times;        // BLINK
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
    return (uint8_t)(((uint16_t)v * (uint16_t)s_cfg.brightness) / 255);
}

static void mode_color(led_mode_t mode, uint8_t* r, uint8_t* g, uint8_t* b)
{
    if (mode == LED_MODE_MEDIA) { *r = 0;   *g = 80;  *b = 255; }
    else                       { *r = 255; *g = 0;   *b = 110; }
}

static void strip_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) return;

    r = scale(r); g = scale(g); b = scale(b);
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

static void strip_off(void)
{
    if (!s_strip) return;
    led_strip_clear(s_strip);
    led_strip_refresh(s_strip);
}

static bool enqueue_cmd(const led_cmd_t* cmd, bool drop_oldest_if_full)
{
    if (!s_q) return false;

    if (xQueueSend(s_q, cmd, 0) == pdTRUE) return true;

    if (!drop_oldest_if_full) return false;

    led_cmd_t junk;
    (void)xQueueReceive(s_q, &junk, 0);
    return (xQueueSend(s_q, cmd, 0) == pdTRUE);
}

static void led_task(void* arg)
{
    (void)arg;

    led_cmd_t cmd;
    strip_off();

    while (1) {
        if (xQueueReceive(s_q, &cmd, portMAX_DELAY) == pdTRUE) {

            if (cmd.type == LED_CMD_PULSE) {
                strip_set_rgb(cmd.r, cmd.g, cmd.b);
                vTaskDelay(pdMS_TO_TICKS(cmd.on_ms));
                strip_off();
                if (cmd.off_ms > 0) vTaskDelay(pdMS_TO_TICKS(cmd.off_ms));
                continue;
            }

            // BLINK
            uint8_t n = (cmd.times == 0) ? 1 : cmd.times;
            for (uint8_t i = 0; i < n; i++) {
                strip_set_rgb(cmd.r, cmd.g, cmd.b);
                vTaskDelay(pdMS_TO_TICKS(cmd.on_ms));
                strip_off();
                if (cmd.off_ms > 0) vTaskDelay(pdMS_TO_TICKS(cmd.off_ms));
            }
        }
    }
}

static void timer_cb(void* arg)
{
    (void)arg;
    if (!s_q) return;

    // ✅ heartbeat NÃO entra se tiver coisa pendente
    if (uxQueueMessagesWaiting(s_q) > 0) return;

    led_cmd_t cmd = {0};
    cmd.type = LED_CMD_PULSE;
    mode_color(s_mode, &cmd.r, &cmd.g, &cmd.b);
    cmd.on_ms  = s_cfg.pulse_on_ms;
    cmd.off_ms = s_cfg.pulse_off_ms;

    (void)enqueue_cmd(&cmd, false);
}

void led_init(const led_config_t* cfg)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.brightness       = 24;
    s_cfg.flash_ms         = 60;
    s_cfg.pulse_on_ms      = 140;
    s_cfg.pulse_off_ms     = 60;
    s_cfg.pulse_period_ms  = 300000;

    if (cfg) s_cfg = *cfg;

    ESP_LOGI(TAG, "init: bright=%u flash=%ums pulse=%ums/%ums period=%ums",
             (unsigned)s_cfg.brightness,
             (unsigned)s_cfg.flash_ms,
             (unsigned)s_cfg.pulse_on_ms,
             (unsigned)s_cfg.pulse_off_ms,
             (unsigned)s_cfg.pulse_period_ms);

    s_q = xQueueCreate(16, sizeof(led_cmd_t));
    if (!s_q) {
        ESP_LOGE(TAG, "queue create failed");
        return;
    }

    led_strip_config_t strip_config = {
        .strip_gpio_num = PIN_LED_WS2812,
        .max_leds = 1,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = 0,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags.with_dma = 0,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(err));
        return;
    }

    xTaskCreate(led_task, "led_task", 4096, NULL, 9, NULL);

    const esp_timer_create_args_t tcfg = {
        .callback = timer_cb,
        .name = "led_heartbeat"
    };
    ESP_ERROR_CHECK(esp_timer_create(&tcfg, &s_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_timer, (uint64_t)s_cfg.pulse_period_ms * 1000ULL));
}

void led_set_mode(led_mode_t mode)
{
    s_mode = mode;
    ESP_LOGI(TAG, "mode=%s", (mode == LED_MODE_MEDIA) ? "MEDIA" : "DEVICES");
}

void led_pulse_rgb(uint8_t r, uint8_t g, uint8_t b, uint32_t on_ms)
{
    led_cmd_t cmd = {0};
    cmd.type  = LED_CMD_PULSE;
    cmd.r     = r;
    cmd.g     = g;
    cmd.b     = b;
    cmd.on_ms = on_ms;
    cmd.off_ms = 0;

    (void)enqueue_cmd(&cmd, true);
}

void led_blink_rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t times, uint32_t on_ms, uint32_t off_ms)
{
    if (times == 0) return;

    led_cmd_t cmd = {0};
    cmd.type  = LED_CMD_BLINK;
    cmd.r     = r;
    cmd.g     = g;
    cmd.b     = b;
    cmd.times = times;
    cmd.on_ms = on_ms;
    cmd.off_ms = off_ms;

    (void)enqueue_cmd(&cmd, true);
}

void led_off(void)
{
    if (s_q) xQueueReset(s_q);
    strip_off();
}

void led_button_feedback(uint8_t id)
{
    (void)id;
}
