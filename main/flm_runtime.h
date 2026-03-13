#pragma once

#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#include "flm_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t flm_runtime_flash_buffer(FILE *algo_fp,
                                   const flm_info_t *info,
                                   uint32_t base_addr,
                                   const uint8_t *firmware,
                                   uint32_t firmware_size,
                                   uint32_t *out_written_bytes);

esp_err_t flm_runtime_flash_stream(FILE *algo_fp,
                                   const flm_info_t *info,
                                   uint32_t base_addr,
                                   FILE *firmware_fp,
                                   uint32_t firmware_size,
                                   uint32_t *out_written_bytes);

esp_err_t flm_runtime_erase_chip(FILE *algo_fp, const flm_info_t *info);

#ifdef __cplusplus
}
#endif
