#include "input.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"

#include "pins.h"  // mapa de pinos (Seeed XIAO ESP32-C3)

static const char *TAG = "input";

static input_config_t   s_cfg;
static input_event_cb_t s_cb     = NULL;
static void            *s_cb_ctx = NULL;

#define BTN_COUNT 9   // ids 1..9 (9 é dedicado)
#define ROWS 3
#define COLS 3

typedef struct {
    bool    raw;                // leitura instantânea (sem debounce)
    bool    stable;             // estado debounced
    int64_t last_change_us;     // quando raw mudou
    int64_t down_us;            // quando ficou pressionado (stable)
    bool    long_sent;          // já enviou LONG?
} btn_state_t;

static btn_state_t s_btn[BTN_COUNT + 1]; // indexado por id 1..9

static const gpio_num_t ROW_PINS[ROWS] = { PIN_ROW_0, PIN_ROW_1, PIN_ROW_2 };
static const gpio_num_t COL_PINS[COLS] = { PIN_COL_0, PIN_COL_1, PIN_COL_2 };

/**
 * Mapa da matriz -> ID
 * layout desejado:
 *   [1] [2] [3]
 *   [4] [5] [6]
 *   [7] [8] [ ]
 */
static const uint8_t ID_MAP[ROWS][COLS] = {
    { 1, 2, 3 },
    { 4, 5, 6 },
    { 7, 8, 0 } // vazio
};

// 1 = anti-ghost (se detectar >1 tecla pressionada no mesmo scan, ignora matriz no ciclo)
#ifndef INPUT_ANTI_GHOST
#define INPUT_ANTI_GHOST 1
#endif

// 1 = imprime logs de pressões com r/c (use só pra mapear, depois desligue)
#ifndef INPUT_DEBUG_MATRIX
#define INPUT_DEBUG_MATRIX 1
#endif

static void emit(uint8_t id, input_event_type_t type, uint32_t dur_ms)
{
    if (!s_cb) return;
    input_event_t ev = { .id = id, .type = type, .duration_ms = dur_ms };
    s_cb(&ev, s_cb_ctx);
}

static void set_all_rows_high(void)
{
    for (int r = 0; r < ROWS; r++) {
        gpio_set_level(ROW_PINS[r], 1);
    }
}

static void io_init(void)
{
    gpio_config_t cfg = {0};

    // --- ROWS: output, default HIGH ---
    cfg.intr_type = GPIO_INTR_DISABLE;
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = 0;
    cfg.pull_down_en = 0;
    cfg.pin_bit_mask =
        (1ULL << PIN_ROW_0) |
        (1ULL << PIN_ROW_1) |
        (1ULL << PIN_ROW_2);
    ESP_ERROR_CHECK(gpio_config(&cfg));
    set_all_rows_high();

    // --- COLS: input with PULL-UP ---
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = 1;
    cfg.pull_down_en = 0;
    cfg.pin_bit_mask =
        (1ULL << PIN_COL_0) |
        (1ULL << PIN_COL_1) |
        (1ULL << PIN_COL_2);
    ESP_ERROR_CHECK(gpio_config(&cfg));

    // --- BTN9: input with PULL-UP ---
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = 1;
    cfg.pull_down_en = 0;
    cfg.pin_bit_mask = (1ULL << PIN_BTN9);
    ESP_ERROR_CHECK(gpio_config(&cfg));

    // --- BAT ADC: input, NO pulls ---
    gpio_set_pull_mode((gpio_num_t)PIN_BAT_ADC, GPIO_FLOATING);

    ESP_LOGI(TAG, "IO init ok (rows/cols + btn9 + bat adc)");
}

static inline bool read_col_pressed(int c)
{
    // pull-up: pressionado => LOW
    return (gpio_get_level(COL_PINS[c]) == 0);
}

static inline bool scan_btn9_pressed(void)
{
    return (gpio_get_level((gpio_num_t)PIN_BTN9) == 0);
}

static void update_button(uint8_t id, bool pressed_raw, int64_t t_us)
{
    btn_state_t* b = &s_btn[id];

    if (pressed_raw != b->raw) {
        b->raw = pressed_raw;
        b->last_change_us = t_us;
    }

    int64_t stable_after_us = (int64_t)s_cfg.debounce_ms * 1000;
    if ((t_us - b->last_change_us) < stable_after_us) {
        return;
    }

    if (b->stable != b->raw) {
        b->stable = b->raw;

        if (b->stable) {
            b->down_us = t_us;
            b->long_sent = false;
            emit(id, INPUT_EV_DOWN, 0);
        } else {
            emit(id, INPUT_EV_UP, 0);

            uint32_t held_ms = (uint32_t)((t_us - b->down_us) / 1000);
            if (!b->long_sent) {
                emit(id, INPUT_EV_SHORT, held_ms);
            }
        }
    }

    if (b->stable && !b->long_sent) {
        uint32_t held_ms = (uint32_t)((t_us - b->down_us) / 1000);
        if (held_ms >= s_cfg.long_press_ms) {
            b->long_sent = true;
            emit(id, INPUT_EV_LONG, held_ms);
        }
    }
}

static void scan_matrix_and_update(int64_t t_us)
{
    bool raw_now[BTN_COUNT + 1] = {0};
    int pressed_count = 0;

    for (int r = 0; r < ROWS; r++) {

        // ativa apenas a linha r (LOW), outras HIGH
        for (int rr = 0; rr < ROWS; rr++) {
            gpio_set_level(ROW_PINS[rr], (rr == r) ? 0 : 1);
        }

        // settle
        esp_rom_delay_us(30);

        for (int c = 0; c < COLS; c++) {
            uint8_t id = ID_MAP[r][c];
            if (id == 0) continue;

            bool pressed = read_col_pressed(c);
            raw_now[id] = pressed;
            if (pressed) pressed_count++;

#if INPUT_DEBUG_MATRIX
            // loga só quando virou pressionado (borda raw)
            if (pressed && !s_btn[id].raw) {
                ESP_LOGI(TAG, "DOWN raw r=%d c=%d (rowGPIO=%d colGPIO=%d) => id=%d",
                         r, c, (int)ROW_PINS[r], (int)COL_PINS[c], id);
            }
#endif

        }
    }

    set_all_rows_high();

#if INPUT_ANTI_GHOST
    if (pressed_count > 1) {
        // Anti-ghost esperto: não aceita leitura nova da matriz nesse ciclo,
        // mas também NÃO força "UP" falso em quem já estava pressionado.
        for (uint8_t id = 1; id <= 8; id++) {
            update_button(id, s_btn[id].raw, t_us);
        }
        return;
    }
#endif

    for (uint8_t id = 1; id <= 8; id++) {
        update_button(id, raw_now[id], t_us);
    }
}

static void prime_states(void)
{
    int64_t t_us = esp_timer_get_time();

    // inicia com estado atual (evita "DOWN" fantasma no boot)
    bool raw_now[BTN_COUNT + 1] = {0};
    int pressed_count = 0;

    for (int r = 0; r < ROWS; r++) {
        for (int rr = 0; rr < ROWS; rr++) {
            gpio_set_level(ROW_PINS[rr], (rr == r) ? 0 : 1);
        }
        esp_rom_delay_us(30);

        for (int c = 0; c < COLS; c++) {
            uint8_t id = ID_MAP[r][c];
            if (id == 0) continue;

            bool pressed = read_col_pressed(c);
            raw_now[id] = pressed;
            if (pressed) pressed_count++;
        }
    }
    set_all_rows_high();

#if INPUT_ANTI_GHOST
    if (pressed_count > 1) {
        memset(raw_now, 0, sizeof(raw_now));
    }
#endif
    for (uint8_t id = 1; id <= 8; id++) {
        s_btn[id].raw = raw_now[id];
        s_btn[id].stable = raw_now[id];
        s_btn[id].last_change_us = t_us;
        s_btn[id].down_us = t_us;
        s_btn[id].long_sent = false;
    }

    bool p9 = scan_btn9_pressed();
    s_btn[9].raw = p9;
    s_btn[9].stable = p9;
    s_btn[9].last_change_us = t_us;
    s_btn[9].down_us = t_us;
    s_btn[9].long_sent = false;
}

static void input_task(void* arg)
{
    (void)arg;

    TickType_t last = xTaskGetTickCount();

    while (1) {
        int64_t t_us = esp_timer_get_time();

        scan_matrix_and_update(t_us);

        // botão dedicado 9
        bool pressed9 = scan_btn9_pressed();
        update_button(9, pressed9, t_us);

        TickType_t inc = pdMS_TO_TICKS(s_cfg.scan_period_ms);
        if (inc == 0) inc = 1;
        vTaskDelayUntil(&last, inc);
    }
}

void input_init(const input_config_t* cfg)
{
    if (cfg) {
        s_cfg = *cfg;
    } else {
        s_cfg.scan_period_ms = 5;
        s_cfg.debounce_ms    = 25;
        s_cfg.long_press_ms  = 700;
    }

    if (s_cfg.scan_period_ms == 0) s_cfg.scan_period_ms = 1;
    if (s_cfg.debounce_ms > 200)   s_cfg.debounce_ms = 200;

    ESP_LOGI(TAG, "init: scan=%ums debounce=%ums long=%ums antiGhost=%d debug=%d",
             (unsigned)s_cfg.scan_period_ms,
             (unsigned)s_cfg.debounce_ms,
             (unsigned)s_cfg.long_press_ms,
             (int)INPUT_ANTI_GHOST,
             (int)INPUT_DEBUG_MATRIX);
}

void input_set_callback(input_event_cb_t cb, void* user_ctx)
{
    s_cb = cb;
    s_cb_ctx = user_ctx;
}

void input_start(void)
{
    io_init();
    memset(s_btn, 0, sizeof(s_btn));
    prime_states();

    xTaskCreate(input_task, "input_task", 4096, NULL, 10, NULL);
    ESP_LOGI(TAG, "task started");
}
