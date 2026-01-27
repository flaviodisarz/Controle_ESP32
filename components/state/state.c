#include "state.h"

#include <string.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_err.h"
#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "led.h"
#include "battery.h"
#include "pins.h"

static const char* TAG = "state";

// ===== tempos padrão =====
#define MEDIA_LIGHT_MS_DEFAULT       (30UL * 60UL * 1000UL)  // 30 min
#define MEDIA_DEEP_MS_DEFAULT        (60UL * 60UL * 1000UL)  // 60 min total
#define DEV_LIGHT_MS_DEFAULT         (10UL * 60UL * 1000UL)  // 10 min
#define DEV_LIGHT_TO_DEEP_MS_DEFAULT (5UL  * 60UL * 1000UL)  // 5 min no light

// Slot local
#define SLOT_A 0
#define SLOT_B 1

// Persistência em deep sleep
RTC_DATA_ATTR static uint8_t s_slot_rtc = SLOT_A;
RTC_DATA_ATTR static uint8_t s_mode_rtc = APP_MODE_MEDIA;

// runtime
static app_mode_t s_mode = APP_MODE_MEDIA;
static uint8_t    s_slot = SLOT_A;
static state_config_t s_cfg;

static QueueHandle_t s_q = NULL;
static esp_timer_handle_t s_idle_timer = NULL;

static bool s_suppress_btn9_once = false;

// ✅ trava geral durante transição de sleep
static volatile bool s_sleep_lock = false;

// ✅ anti “double toggle” do BTN9 (wake + evento)
static int64_t s_btn9_guard_until_us = 0;

typedef enum {
    ST_EV_INPUT = 0,
    ST_EV_IDLE_TIMEOUT = 1,
} st_ev_type_t;

typedef struct {
    st_ev_type_t type;
    input_event_t in;
} st_ev_t;

// ================== cores fixas ==================
static inline void led_blue_long(void) { led_pulse_rgb(0, 80, 255, 350); }
static inline void led_pink_long(void) { led_pulse_rgb(255, 0, 110, 350); }

// ================== tempo por modo ==================
static uint32_t idle_to_light_ms_for_mode(app_mode_t m)
{
    if (m == APP_MODE_MEDIA) {
        return (s_cfg.media_light_sleep_ms != 0) ? s_cfg.media_light_sleep_ms : MEDIA_LIGHT_MS_DEFAULT;
    }
    return DEV_LIGHT_MS_DEFAULT;
}

static uint32_t light_to_deep_ms_for_mode(app_mode_t m)
{
    if (m == APP_MODE_MEDIA) {
        uint32_t total = (s_cfg.media_deep_sleep_ms != 0) ? s_cfg.media_deep_sleep_ms : MEDIA_DEEP_MS_DEFAULT;
        uint32_t light = idle_to_light_ms_for_mode(m);
        if (total > light) return (total - light);
        return MEDIA_LIGHT_MS_DEFAULT;
    }

    if (s_cfg.dev_deep_sleep_ms != 0) return s_cfg.dev_deep_sleep_ms;
    return DEV_LIGHT_TO_DEEP_MS_DEFAULT;
}

// ================== persist ==================
static void persist_state(void)
{
    s_mode_rtc = (uint8_t)s_mode;
    s_slot_rtc = s_slot;
}

// ================== matriz wake (light sleep) ==================
static void prepare_matrix_for_light_wakeup(void)
{
    // ROWs OUTPUT LOW (assim qualquer tecla puxa COL pra LOW)
    gpio_set_direction(PIN_ROW_0, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_ROW_1, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_ROW_2, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_ROW_0, 0);
    gpio_set_level(PIN_ROW_1, 0);
    gpio_set_level(PIN_ROW_2, 0);

    // COLs INPUT PULLUP
    gpio_set_direction(PIN_COL_0, GPIO_MODE_INPUT);
    gpio_set_direction(PIN_COL_1, GPIO_MODE_INPUT);
    gpio_set_direction(PIN_COL_2, GPIO_MODE_INPUT);

    gpio_pullup_en(PIN_COL_0); gpio_pulldown_dis(PIN_COL_0);
    gpio_pullup_en(PIN_COL_1); gpio_pulldown_dis(PIN_COL_1);
    gpio_pullup_en(PIN_COL_2); gpio_pulldown_dis(PIN_COL_2);

    ESP_ERROR_CHECK(gpio_wakeup_enable(PIN_COL_0, GPIO_INTR_LOW_LEVEL));
    ESP_ERROR_CHECK(gpio_wakeup_enable(PIN_COL_1, GPIO_INTR_LOW_LEVEL));
    ESP_ERROR_CHECK(gpio_wakeup_enable(PIN_COL_2, GPIO_INTR_LOW_LEVEL));
}

// ================== BTN9 wake (light/deep) ==================
static void prepare_btn9_input_pullup(void)
{
    gpio_set_direction(PIN_BTN9, GPIO_MODE_INPUT);
    gpio_pullup_en(PIN_BTN9);
    gpio_pulldown_dis(PIN_BTN9);
}

static void enable_light_wakeup_sources(void)
{
    prepare_btn9_input_pullup();
    ESP_ERROR_CHECK(gpio_wakeup_enable(PIN_BTN9, GPIO_INTR_LOW_LEVEL));

    prepare_matrix_for_light_wakeup();

    ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());
}

static void enable_deep_wakeup_sources(void)
{
    prepare_btn9_input_pullup();

    esp_err_t err = esp_deep_sleep_enable_gpio_wakeup(PIN_BTN9, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "deep gpio wakeup falhou no pin %d: %s", (int)PIN_BTN9, esp_err_to_name(err));
    }
}

// ================== idle timer ==================
static void idle_timer_cb(void* arg)
{
    (void)arg;
    if (!s_q) return;

    st_ev_t ev = { .type = ST_EV_IDLE_TIMEOUT };
    (void)xQueueSend(s_q, &ev, 0);
}

static void restart_idle_timer(void)
{
    if (!s_idle_timer) return;

    uint32_t ms = idle_to_light_ms_for_mode(s_mode);

    (void)esp_timer_stop(s_idle_timer);
    ESP_ERROR_CHECK(esp_timer_start_once(s_idle_timer, (uint64_t)ms * 1000ULL));
}

// ================== core actions ==================
static void apply_mode_led(app_mode_t m)
{
    if (m == APP_MODE_DEVICES) {
        led_set_mode(LED_MODE_DEVICES);
        led_pink_long();
    } else {
        led_set_mode(LED_MODE_MEDIA);
        led_blue_long();
    }
}

static void media_toggle_slot(void)
{
    if (s_sleep_lock) return;

    s_slot = (s_slot == SLOT_A) ? SLOT_B : SLOT_A;
    persist_state();

    if (s_slot == SLOT_B) {
        ESP_LOGI(TAG, "Slot B Conectando");
        led_blink_rgb(0, 80, 255, 3, 60, 60);
    } else {
        ESP_LOGI(TAG, "Slot A Conectando");
        led_blink_rgb(0, 80, 255, 2, 60, 60);
    }
}

static void toggle_mode(void)
{
    if (s_sleep_lock) return;

    s_mode = (s_mode == APP_MODE_MEDIA) ? APP_MODE_DEVICES : APP_MODE_MEDIA;
    persist_state();

    ESP_LOGI(TAG, "Modo %s", (s_mode == APP_MODE_MEDIA) ? "MIDIA" : "DISPOSITIVOS");
    apply_mode_led(s_mode);

    restart_idle_timer();
}

static void suspend_input_task_now(void)
{
    TaskHandle_t h_input = xTaskGetHandle("input_task");
    if (h_input) {
        vTaskSuspend(h_input);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void enter_deep_sleep(const char* why)
{
    // ✅ trava total (não deixa reentrar)
    if (s_sleep_lock) return;
    s_sleep_lock = true;

    ESP_LOGI(TAG, "Entrando em Deep Sleep%s%s%s",
             why ? " (" : "", why ? why : "", why ? ")" : "");

    (void)esp_timer_stop(s_idle_timer);

    // para de varrer matriz / parar eventos enquanto desliga
    suspend_input_task_now();

    if (s_q) xQueueReset(s_q);

    // apaga LED (WS2812 segura o último estado se não limpar)
    led_off();
    vTaskDelay(pdMS_TO_TICKS(30));

    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    enable_deep_wakeup_sources();

    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
}

static void enter_light_sleep(const char* why)
{
    // ✅ trava (evita chamadas duplicadas)
    if (s_sleep_lock) return;
    s_sleep_lock = true;

    ESP_LOGI(TAG, "Entrando em Light Sleep%s%s%s",
             why ? " (" : "", why ? why : "", why ? ")" : "");

    (void)esp_timer_stop(s_idle_timer);

    // 🔥 BUGFIX: para o scan da matriz antes de configurar rows LOW,
    // senão o input_task coloca rows HIGH de novo e você nunca acorda pela matriz.
    suspend_input_task_now();

    if (s_q) xQueueReset(s_q);

    // opcional: apagar LED durante sleep
    led_off();

    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    enable_light_wakeup_sources();

    uint32_t light_to_deep_ms = light_to_deep_ms_for_mode(s_mode);
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)light_to_deep_ms * 1000ULL));

    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_ERROR_CHECK(esp_light_sleep_start());

    // voltou
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "Acordou do Light Sleep (cause=%d)", (int)cause);

    // retoma scan
    TaskHandle_t h_input = xTaskGetHandle("input_task");
    if (h_input) {
        vTaskResume(h_input);
    }

    // destrava
    s_sleep_lock = false;

    // Se acordou pelo timer do light -> vai pro deep
    if (cause == ESP_SLEEP_WAKEUP_TIMER) {
        enter_deep_sleep("timer do light sleep");
        return;
    }

    // volta a contar inatividade
    restart_idle_timer();
}

// ================== handlers ==================
static void handle_media_short(uint8_t id)
{
    switch (id) {
        case 1: enter_light_sleep("BTN1 curto"); break;
        case 2: ESP_LOGI(TAG, "MEDIA: VOL+"); break;
        case 3: ESP_LOGI(TAG, "MEDIA: MUTE"); break;
        case 4: ESP_LOGI(TAG, "MEDIA: PREV"); break;
        case 5: ESP_LOGI(TAG, "MEDIA: PLAY/PAUSE"); break;
        case 6: ESP_LOGI(TAG, "MEDIA: NEXT"); break;
        case 7: media_toggle_slot(); break;
        case 8: ESP_LOGI(TAG, "MEDIA: VOL-"); break;
        default: ESP_LOGW(TAG, "MEDIA: id invalido=%u", (unsigned)id); break;
    }
}

static void handle_media_long(uint8_t id, uint32_t ms)
{
    (void)ms;
    if (id == 1) enter_deep_sleep("BTN1 longo");
    else         ESP_LOGI(TAG, "MEDIA: LONG ignorado (id=%u)", (unsigned)id);
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

// ================== state task ==================
static void state_task(void* arg)
{
    (void)arg;

    st_ev_t ev;
    while (1) {
        if (xQueueReceive(s_q, &ev, portMAX_DELAY) != pdTRUE) continue;

        if (ev.type == ST_EV_IDLE_TIMEOUT) {
            enter_light_sleep("inatividade");
            continue;
        }

        const input_event_t* in = &ev.in;

        // ✅ se estiver travado entrando em sleep, ignora tudo
        if (s_sleep_lock) continue;

        // ✅ inatividade considera só botões 1..8
        if (in->id >= 1 && in->id <= 8) {
            if (in->type == INPUT_EV_DOWN || in->type == INPUT_EV_SHORT || in->type == INPUT_EV_LONG) {
                restart_idle_timer();
            }
        }

        if (in->type == INPUT_EV_SHORT) {

            if (in->id == 9) {
                int64_t now = esp_timer_get_time();

                // deep wake: ignora 1x o short
                if (s_suppress_btn9_once) {
                    s_suppress_btn9_once = false;
                    ESP_LOGI(TAG, "BTN9 (wake deep) ignorando 1x o SHORT");
                    // ainda cria um guard curtinho contra duplo
                    s_btn9_guard_until_us = now + 500000;
                    continue;
                }

                // guard anti “double toggle”
                if (now < s_btn9_guard_until_us) {
                    ESP_LOGI(TAG, "BTN9: guard (evitando toggle duplo)");
                    continue;
                }

                s_btn9_guard_until_us = now + 500000;
                toggle_mode();
                continue;
            }

            if (s_mode == APP_MODE_MEDIA) handle_media_short(in->id);
            else                          handle_devices_short(in->id);
            continue;
        }

        if (in->type == INPUT_EV_LONG) {

            if (in->id == 9) {
                ESP_LOGI(TAG, "Carga Bateria");
                continue;
            }

            if (s_mode == APP_MODE_MEDIA) handle_media_long(in->id, in->duration_ms);
            else                          handle_devices_long(in->id, in->duration_ms);
            continue;
        }
    }
}

// ================== API ==================
void state_init(const state_config_t* cfg)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.long_press_ms = 700;
    if (cfg) s_cfg = *cfg;

    s_mode = (s_mode_rtc == APP_MODE_DEVICES) ? APP_MODE_DEVICES : APP_MODE_MEDIA;
    s_slot = (s_slot_rtc == SLOT_B) ? SLOT_B : SLOT_A;

    led_init(NULL);
    battery_init();
    apply_mode_led(s_mode);

    s_q = xQueueCreate(16, sizeof(st_ev_t));
    if (!s_q) {
        ESP_LOGE(TAG, "queue create failed");
        return;
    }

    const esp_timer_create_args_t tcfg = {
        .callback = idle_timer_cb,
        .name = "idle_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&tcfg, &s_idle_timer));

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "init: mode=%s slot=%s wake_cause=%d",
             (s_mode == APP_MODE_MEDIA) ? "MEDIA" : "DEVICES",
             (s_slot == SLOT_A) ? "A" : "B",
             (int)cause);

    // ✅ Se acordou do DEEP por GPIO, IGNORA 1x o SHORT do BTN9
    if (cause == ESP_SLEEP_WAKEUP_GPIO) {
        s_suppress_btn9_once = true;
    }

    // garantias
    s_sleep_lock = false;
    s_btn9_guard_until_us = 0;

    restart_idle_timer();
}

void state_start(void)
{
    xTaskCreate(state_task, "state_task", 4096, NULL, 10, NULL);
    ESP_LOGI(TAG, "start (ok)");
}

bool state_post_input(const input_event_t* ev)
{
    if (!ev || !s_q) return false;

    st_ev_t sev = {0};
    sev.type = ST_EV_INPUT;
    sev.in = *ev;

    return (xQueueSend(s_q, &sev, 0) == pdTRUE);
}
