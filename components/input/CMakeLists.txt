idf_component_register(
    SRCS "input.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_timer driver board
)
