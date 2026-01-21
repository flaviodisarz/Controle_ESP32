#include "state.h"
#include "esp_log.h"

void app_main(void)
{
    ESP_LOGI("main", "Controle ESP32-C3 - Fase 1 (Etapa 1)");

    state_config_t cfg = {
        .long_press_ms = 700,
    };

    state_init(&cfg);
    state_start();
}
