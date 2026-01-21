#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void battery_init(void);
uint8_t battery_get_percent(void);

#ifdef __cplusplus
}
#endif
