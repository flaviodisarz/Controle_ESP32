#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    INPUT_EV_DOWN = 0,
    INPUT_EV_UP,
    INPUT_EV_SHORT,
    INPUT_EV_LONG,
} input_event_type_t;

typedef struct {
    uint8_t id;                 // 1..9
    input_event_type_t type;    // DOWN/UP/SHORT/LONG
    uint32_t duration_ms;       // usado em LONG
} input_event_t;

typedef void (*input_event_cb_t)(const input_event_t* ev, void* user_ctx);

typedef struct {
    uint32_t scan_period_ms;    // ex: 5ms
    uint32_t debounce_ms;       // ex: 25ms
    uint32_t long_press_ms;     // ex: 700ms
} input_config_t;

void input_init(const input_config_t* cfg);
void input_set_callback(input_event_cb_t cb, void* user_ctx);
void input_start(void);

#ifdef __cplusplus
}
#endif
