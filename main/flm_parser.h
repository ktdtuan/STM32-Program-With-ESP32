#pragma once

#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define FLM_MAX_SECTORS 16

typedef struct
{
	uint32_t size;
	uint32_t address;
} flm_sector_t;

typedef struct
{
	char source_name[64];
	char device_name[128];
	uint16_t version;
	uint16_t device_type;
	uint32_t flash_start;
	uint32_t flash_size;
	uint32_t page_size;
	uint8_t erased_value;
	uint32_t program_timeout_ms;
	uint32_t erase_timeout_ms;
	flm_sector_t sectors[FLM_MAX_SECTORS];
	uint32_t sector_count;
} flm_info_t;

esp_err_t flm_parse_from_file(FILE *fp, const char *name_hint, flm_info_t *out_info);

#ifdef __cplusplus
}
#endif
