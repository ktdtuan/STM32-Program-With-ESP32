#pragma once

#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#include "flm_parser.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct
{
	uint32_t dev_id;
	const char *family;
	int supports_direct_flash;
} stm32_chip_profile_t;

esp_err_t stm32_detect_profile(stm32_chip_profile_t *out_profile, uint32_t *out_dbgmcu_idcode);

esp_err_t stm32_flash_firmware_stream(FILE *firmware_fp,
										uint32_t base_addr,
										uint32_t max_bytes,
										uint32_t *out_written_bytes);

esp_err_t stm32_flash_firmware_buffer(const uint8_t *firmware,
						  uint32_t firmware_size,
						  uint32_t base_addr,
						  uint32_t max_bytes,
						  uint32_t *out_written_bytes);

const char *stm32_family_token_from_devid(uint32_t dev_id);

esp_err_t stm32_apply_lock_level(uint32_t dev_id, uint8_t lock_level);
esp_err_t stm32_get_lock_level(uint32_t dev_id, uint8_t *out_lock_level);

#ifdef __cplusplus
}
#endif
