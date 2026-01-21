#include "state.h"
#include "input.h"
#include "esp_log.h"

static void on_input_event(const input_event_t* ev, void* ctx)
{
    (void)ctx;
    state_post_input(ev);
}

void app_main(void)
{
    ESP_LOGI("main", "Controle ESP32-C3 - Fase 1 (Etapa 2)");

    state_config_t scfg = {
        .long_press_ms = 700,
    };

    input_config_t icfg = {
        .scan_period_ms = 5,
        .debounce_ms = 25,
        .long_press_ms = 700,
    };

    state_init(&scfg);

    input_init(&icfg);
    input_set_callback(on_input_event, NULL);
    input_start();

    state_start();
}
