#include "battery.h"
#include "esp_log.h"

static const char* TAG = "battery";

void battery_init(void)
{
    ESP_LOGI(TAG, "init (stub)");
}

uint8_t battery_get_percent(void)
{
    // Etapa 1: valor fixo só pra testar lógica/LED depois
    return 80;
}
