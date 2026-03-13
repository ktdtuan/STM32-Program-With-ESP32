#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

esp_err_t chip_lock_apply_level(uint32_t dev_id, uint8_t lock_level);

#ifdef __cplusplus
}
#endif
