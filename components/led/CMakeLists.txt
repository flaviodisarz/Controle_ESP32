idf_component_register(
    SRCS "led.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer led_strip
    PRIV_REQUIRES board
)
