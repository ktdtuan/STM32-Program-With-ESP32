#include "storage.h"

#include <ctype.h>
#include <dirent.h>
#include <string.h>

#include "esp_littlefs.h"
#include "esp_log.h"

#define STORAGE_BASE_PATH "/littlefs"
#define STORAGE_MAX_PATH 128

static const char *TAG = "STORAGE";

static int storage_is_valid_name(const char *name)
{
	if (!name || !name[0])
	{
		return 0;
	}

	for (const char *p = name; *p; ++p)
	{
		char c = *p;
		if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-'))
		{
			return 0;
		}
	}
	return 1;
}

static esp_err_t storage_build_path(const char *name, char *out, size_t out_len)
{
	if (!storage_is_valid_name(name) || !out || out_len == 0)
	{
		return ESP_ERR_INVALID_ARG;
	}

	int n = snprintf(out, out_len, "%s/%s", STORAGE_BASE_PATH, name);
	if (n <= 0 || (size_t)n >= out_len)
	{
		return ESP_ERR_INVALID_SIZE;
	}

	return ESP_OK;
}

esp_err_t storage_init(void)
{
	esp_vfs_littlefs_conf_t conf = {
		.base_path = STORAGE_BASE_PATH,
		.partition_label = NULL,
		.format_if_mount_failed = true,
		.dont_mount = false,
	};

	esp_err_t ret = esp_vfs_littlefs_register(&conf);
	if (ret != ESP_OK)
	{
		ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(ret));
		return ret;
	}

	size_t total = 0;
	size_t used = 0;
	ret = esp_littlefs_info(conf.partition_label, &total, &used);
	if (ret == ESP_OK)
	{
		ESP_LOGI(TAG, "LittleFS mounted: total=%u, used=%u", (unsigned int)total, (unsigned int)used);
	}

	return ESP_OK;
}

esp_err_t storage_open_for_read(const char *name, FILE **out_fp)
{
	if (!out_fp)
	{
		return ESP_ERR_INVALID_ARG;
	}

	char path[STORAGE_MAX_PATH] = {0};
	esp_err_t ret = storage_build_path(name, path, sizeof(path));
	if (ret != ESP_OK)
	{
		return ret;
	}

	FILE *fp = fopen(path, "rb");
	if (!fp)
	{
		return ESP_ERR_NOT_FOUND;
	}

	*out_fp = fp;
	return ESP_OK;
}

esp_err_t storage_open_for_write(const char *name, FILE **out_fp)
{
	if (!out_fp)
	{
		return ESP_ERR_INVALID_ARG;
	}

	char path[STORAGE_MAX_PATH] = {0};
	esp_err_t ret = storage_build_path(name, path, sizeof(path));
	if (ret != ESP_OK)
	{
		return ret;
	}

	FILE *fp = fopen(path, "wb");
	if (!fp)
	{
		return ESP_FAIL;
	}

	*out_fp = fp;
	return ESP_OK;
}

esp_err_t storage_delete(const char *name)
{
	char path[STORAGE_MAX_PATH] = {0};
	esp_err_t ret = storage_build_path(name, path, sizeof(path));
	if (ret != ESP_OK)
	{
		return ret;
	}

	if (remove(path) != 0)
	{
		return ESP_FAIL;
	}

	return ESP_OK;
}

esp_err_t storage_list_json(char *out, size_t out_len)
{
	if (!out || out_len < 3)
	{
		return ESP_ERR_INVALID_SIZE;
	}

	DIR *dir = opendir(STORAGE_BASE_PATH);
	if (!dir)
	{
		return ESP_FAIL;
	}

	size_t pos = 0;
	out[pos++] = '[';
	out[pos] = '\0';

	struct dirent *ent = NULL;
	int first = 1;
	while ((ent = readdir(dir)) != NULL)
	{
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
		{
			continue;
		}

		int n = snprintf(out + pos, out_len - pos, "%s\"%s\"", first ? "" : ",", ent->d_name);
		if (n <= 0 || (size_t)n >= out_len - pos)
		{
			closedir(dir);
			return ESP_ERR_INVALID_SIZE;
		}

		pos += (size_t)n;
		first = 0;
	}

	if (pos + 2 > out_len)
	{
		closedir(dir);
		return ESP_ERR_INVALID_SIZE;
	}

	out[pos++] = ']';
	out[pos] = '\0';

	closedir(dir);
	return ESP_OK;
}
