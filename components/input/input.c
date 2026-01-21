#include "input.h"
#include <string.h>
#include "esp_log.h"

static const char* TAG = "input";

static input_config_t s_cfg;
static input_event_cb_t s_cb = NULL;
static void* s_cb_ctx = NULL;

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
    // Etapa 1: ainda não vamos ler hardware de verdade.
    // Etapa 2/3: aqui vamos criar task de scan e chamar callback.
    ESP_LOGI(TAG, "start (stub)");
    (void)s_cb;
    (void)s_cb_ctx;
}
