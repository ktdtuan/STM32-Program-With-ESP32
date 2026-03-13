#pragma once

#include <stdio.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

esp_err_t storage_init(void);
esp_err_t storage_list_json(char *out, size_t out_len);
esp_err_t storage_open_for_read(const char *name, FILE **out_fp);
esp_err_t storage_open_for_write(const char *name, FILE **out_fp);
esp_err_t storage_delete(const char *name);

#ifdef __cplusplus
}
#endif
