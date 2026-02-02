idf_component_register(
    SRCS "state.c"
    INCLUDE_DIRS "include"
    REQUIRES input led battery ble_hid esp_timer driver esp_hw_support
    PRIV_REQUIRES board
)
