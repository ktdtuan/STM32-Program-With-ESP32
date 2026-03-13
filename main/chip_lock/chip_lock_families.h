#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t chip_lock_apply_level_g0(uint8_t lock_level);
esp_err_t chip_lock_apply_level_f0(uint8_t lock_level);
esp_err_t chip_lock_apply_level_f1(uint8_t lock_level);
