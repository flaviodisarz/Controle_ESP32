#include "input.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "pins.h"   // <- nosso mapa de pinos
#include "esp_rom_sys.h"

static const char* TAG = "input";

static input_config_t s_cfg;
static input_event_cb_t s_cb = NULL;
static void* s_cb_ctx = NULL;

#define BTN_COUNT 9   // 1..9 (9 é dedicado)
#define MAT_COUNT 8   // 1..8 (matriz)

typedef struct {
    bool raw;                // leitura instantânea (sem debounce)
    bool stable;             // estado debounced
    int64_t last_change_us;  // quando raw mudou
    int64_t down_us;         // quando ficou pressionado (stable)
    bool long_sent;          // já enviou LONG?
} btn_state_t;

static btn_state_t s_btn[BTN_COUNT + 1]; // indexado por id 1..9

static const gpio_num_t ROW_PINS[3] = { PIN_ROW_0, PIN_ROW_1, PIN_ROW_2 };
static const gpio_num_t COL_PINS[3] = { PIN_COL_0, PIN_COL_1, PIN_COL_2 };

static inline int64_t now_us(void) { return esp_timer_get_time(); }

static void emit(uint8_t id, input_event_type_t type, uint32_t dur_ms)
{
    if (!s_cb) return;
    input_event_t ev = { .id = id, .type = type, .duration_ms = dur_ms };
    s_cb(&ev, s_cb_ctx);
}

static void io_init(void)
{
    // Linhas: output, default HIGH
    for (int r = 0; r < 3; r++) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << ROW_PINS[r],
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = 0,
            .pull_down_en = 0,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
        gpio_set_level(ROW_PINS[r], 1);
    }

    // Colunas: input pull-up
    for (int c = 0; c < 3; c++) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << COL_PINS[c],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = 1,
            .pull_down_en = 0,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
    }

    // Botão 9 dedicado: input pull-up
    gpio_config_t b9 = {
        .pin_bit_mask = 1ULL << PIN_BTN9,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&b9);

    ESP_LOGI(TAG, "IO init ok");
}

static bool scan_matrix_pressed(uint8_t id)
{
    // id 1..8
    uint8_t idx = (uint8_t)(id - 1);
    int row = idx / 3;
    int col = idx % 3;

    // id 8 é row2 col1, ok. id 9 não entra aqui.
    // id 7 é row2 col0.
    // O id "9" da matriz não existe.
    if (id < 1 || id > 8) return false;
    if (row == 2 && col == 2) return false; // só pra garantir (não existe)

    // Ativa a linha correspondente (LOW), outras HIGH
    for (int r = 0; r < 3; r++) {
        gpio_set_level(ROW_PINS[r], (r == row) ? 0 : 1);
    }

    // Pequeno settle time (bem curto)
    // (em geral não precisa delay, mas ajuda a estabilizar)
    esp_rom_delay_us(30);


    int v = gpio_get_level(COL_PINS[col]);

    // Volta todas HIGH (reduz consumo e evita “colar” estados)
    for (int r = 0; r < 3; r++) gpio_set_level(ROW_PINS[r], 1);

    // Como colunas têm pull-up: pressionado => LOW
    return (v == 0);
}

static bool scan_btn9_pressed(void)
{
    // pull-up: pressionado => LOW
    int v = gpio_get_level((gpio_num_t)PIN_BTN9);
    return (v == 0);
}

static void update_button(uint8_t id, bool pressed_raw, int64_t t_us)
{
    btn_state_t* b = &s_btn[id];

    // detecta mudança no raw
    if (pressed_raw != b->raw) {
        b->raw = pressed_raw;
        b->last_change_us = t_us;
    }

    // debounce: só atualiza stable se raw ficou igual por debounce_ms
    int64_t stable_after_us = (int64_t)s_cfg.debounce_ms * 1000;
    if ((t_us - b->last_change_us) < stable_after_us) {
        return;
    }

    // se stable precisa mudar
    if (b->stable != b->raw) {
        b->stable = b->raw;

        if (b->stable) {
            // ficou pressionado
            b->down_us = t_us;
            b->long_sent = false;
            emit(id, INPUT_EV_DOWN, 0);
        } else {
            // soltou
            emit(id, INPUT_EV_UP, 0);

            uint32_t held_ms = (uint32_t)((t_us - b->down_us) / 1000);

            if (!b->long_sent) {
                // se não mandou LONG, então é SHORT
                emit(id, INPUT_EV_SHORT, held_ms);
            }
        }
    }

    // long press: dispara uma vez quando passa do limiar
    if (b->stable && !b->long_sent) {
        uint32_t held_ms = (uint32_t)((t_us - b->down_us) / 1000);
        if (held_ms >= s_cfg.long_press_ms) {
            b->long_sent = true;
            emit(id, INPUT_EV_LONG, held_ms);
        }
    }
}

static void scan_and_update_all(int64_t t_us)
{
    // Botões da matriz: 1..8
    for (uint8_t id = 1; id <= 8; id++) {
        bool pressed = scan_matrix_pressed(id);
        update_button(id, pressed, t_us);
    }

    // Botão dedicado: 9
    bool pressed9 = scan_btn9_pressed();
    update_button(9, pressed9, t_us);
}


static void input_task(void* arg)
{
    TickType_t last = xTaskGetTickCount();

    while (1) {
        int64_t t_us = esp_timer_get_time();

        scan_and_update_all(t_us);

        // Dá respiro pro IDLE (e deixa o scan cravado no período)
        TickType_t inc = pdMS_TO_TICKS(s_cfg.scan_period_ms);
        if (inc == 0) inc = 1;  // garante pelo menos 1 tick
        vTaskDelayUntil(&last, inc);
    }
}

void input_init(const input_config_t* cfg)
{
    if (cfg) s_cfg = *cfg;
    else {
        s_cfg.scan_period_ms = 5;
        s_cfg.debounce_ms = 25;
        s_cfg.long_press_ms = 700;
    }

    ESP_LOGI(TAG, "init: scan=%ums debounce=%ums long=%ums",
             (unsigned)s_cfg.scan_period_ms,
             (unsigned)s_cfg.debounce_ms,
             (unsigned)s_cfg.long_press_ms);
}

void input_set_callback(input_event_cb_t cb, void* user_ctx)
{
    s_cb = cb;
    s_cb_ctx = user_ctx;
}

void input_start(void)
{
    xTaskCreate(input_task, "input_task", 4096, NULL, 10, NULL);
}
