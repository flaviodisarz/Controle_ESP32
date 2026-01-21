idf_component_register(
    SRCS "state.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_timer input led battery
)
