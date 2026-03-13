#pragma once

#include <stdio.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Convert Intel HEX stream to binary stream.
// out_base_addr: minimum absolute address found in HEX records.
// out_size: resulting contiguous binary size from min..max with gaps padded by 0xFF.
esp_err_t hex2bin_stream(FILE *hex_fp, FILE *bin_fp, uint32_t *out_base_addr, uint32_t *out_size);

// Convert Intel HEX stream to an allocated contiguous binary buffer.
// Caller must free(*out_buf) when done.
esp_err_t hex2bin_to_buffer(FILE *hex_fp, uint8_t **out_buf, uint32_t *out_base_addr, uint32_t *out_size);

#ifdef __cplusplus
}
#endif
