#include "web_server.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "flm_parser.h"
#include "flm_runtime.h"
#include "hex2bin.h"
#include "led_ctrl.h"
#include "stm32_programmer.h"
#include "storage.h"
#include "target_swd.h"

static const char *TAG = "WEB_DASH";

extern const uint8_t dashboard_html_start[] asm("_binary_Manual_html_start");
extern const uint8_t dashboard_html_end[] asm("_binary_Manual_html_end");

static void file_ext_lower(const char *name, char *ext, size_t ext_len)
{
	if (!ext || ext_len == 0)
	{
		return;
	}

	ext[0] = '\0';
	if (!name)
	{
		return;
	}

	const char *dot = strrchr(name, '.');
	if (!dot || dot[1] == '\0')
	{
		return;
	}

	size_t i = 0;
	for (const char *p = dot; *p && i < ext_len - 1; ++p, ++i)
	{
		ext[i] = (char)tolower((unsigned char)*p);
	}
	ext[i] = '\0';
}

static int has_allowed_upload_extension(const char *name)
{
	char ext[8] = {0};
	file_ext_lower(name, ext, sizeof(ext));
	return strcmp(ext, ".hex") == 0 || strcmp(ext, ".bin") == 0 || strcmp(ext, ".flm") == 0;
}

static int is_valid_basename(const char *name)
{
	if (!name || name[0] == '\0')
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

static int has_allowed_firmware_extension(const char *name)
{
	char ext[8] = {0};
	file_ext_lower(name, ext, sizeof(ext));
	return strcmp(ext, ".bin") == 0;
}

static int is_hex_file(const char *name)
{
	char ext[8] = {0};
	file_ext_lower(name, ext, sizeof(ext));
	return strcmp(ext, ".hex") == 0;
}

static int is_flm_file(const char *name)
{
	char ext[8] = {0};
	file_ext_lower(name, ext, sizeof(ext));
	return strcmp(ext, ".flm") == 0;
}

static esp_err_t make_bin_name_from_hex(const char *hex_name, char *bin_name, size_t bin_name_len)
{
	if (!hex_name || !bin_name || bin_name_len == 0)
	{
		return ESP_ERR_INVALID_ARG;
	}

	bin_name[0] = '\0';
	size_t len = strlen(hex_name);
	if (len < 5 || len + 1 > bin_name_len)
	{
		return ESP_ERR_INVALID_SIZE;
	}

	strncpy(bin_name, hex_name, bin_name_len - 1);
	bin_name[bin_name_len - 1] = '\0';
	char *dot = strrchr(bin_name, '.');
	if (!dot)
	{
		return ESP_ERR_INVALID_ARG;
	}
	strncpy(dot, ".bin", bin_name_len - (size_t)(dot - bin_name));
	return ESP_OK;
}

static void str_to_lower_copy(const char *src, char *dst, size_t dst_len)
{
	if (!dst || dst_len == 0)
	{
		return;
	}

	dst[0] = '\0';
	if (!src)
	{
		return;
	}

	size_t i = 0;
	for (; src[i] && i < dst_len - 1; i++)
	{
		dst[i] = (char)tolower((unsigned char)src[i]);
	}
	dst[i] = '\0';
}

static esp_err_t find_best_flm_for_family(const char *family, char *out_name, size_t out_name_len)
{
	if (!family || !out_name || out_name_len == 0)
	{
		return ESP_ERR_INVALID_ARG;
	}

	char json[2048] = {0};
	esp_err_t ret = storage_list_json(json, sizeof(json));
	if (ret != ESP_OK)
	{
		return ret;
	}

	cJSON *list = cJSON_Parse(json);
	if (!list || !cJSON_IsArray(list))
	{
		if (list)
		{
			cJSON_Delete(list);
		}
		return ESP_FAIL;
	}

	char best_name[64] = {0};
	int best_score = -1;
	char family_lc[16] = {0};
	str_to_lower_copy(family, family_lc, sizeof(family_lc));

	cJSON *it = NULL;
	cJSON_ArrayForEach(it, list)
	{
		if (!cJSON_IsString(it) || !it->valuestring)
		{
			continue;
		}

		const char *name = it->valuestring;
		if (!is_flm_file(name))
		{
			continue;
		}

		char lc[64] = {0};
		str_to_lower_copy(name, lc, sizeof(lc));

		int score = 1;
		if (strstr(lc, family_lc))
		{
			score = 10;
		}
		if (strstr(lc, "stm32") && strstr(lc, family_lc))
		{
			score = 20;
		}

		if (score > best_score)
		{
			best_score = score;
			strncpy(best_name, name, sizeof(best_name) - 1);
		}
	}

	cJSON_Delete(list);
	if (best_score < 0)
	{
		return ESP_ERR_NOT_FOUND;
	}

	strncpy(out_name, best_name, out_name_len - 1);
	return ESP_OK;
}

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t buf_len)
{
	if (!req || !buf || buf_len == 0)
	{
		return ESP_ERR_INVALID_ARG;
	}

	int total = req->content_len;
	if (total <= 0 || (size_t)total >= buf_len)
	{
		return ESP_ERR_INVALID_SIZE;
	}

	int received = 0;
	while (received < total)
	{
		int r = httpd_req_recv(req, buf + received, total - received);
		if (r <= 0)
		{
			return ESP_FAIL;
		}
		received += r;
	}

	buf[received] = '\0';
	return ESP_OK;
}

static void send_err(httpd_req_t *req, int code, const char *msg)
{
	httpd_resp_set_status(req, code == 400 ? "400 Bad Request" : "500 Internal Server Error");
	httpd_resp_set_type(req, "text/plain");
	httpd_resp_sendstr(req, msg ? msg : "error");
}

static esp_err_t handle_root(httpd_req_t *req)
{
	size_t html_len = (size_t)(dashboard_html_end - dashboard_html_start);
	httpd_resp_set_type(req, "text/html; charset=utf-8");
	return httpd_resp_send(req, (const char *)dashboard_html_start, html_len);
}

static esp_err_t handle_btn_connect(httpd_req_t *req)
{
	uint32_t idcode = 0;
	uint32_t flash_kb = 0;

	int ret = swd_read_idcode(&idcode);
	if (ret == 0)
	{
		ret = swd_start();
	}
	if (ret == 0)
	{
		(void)swd_read_flash_size_kb(&flash_kb);
	}

	char resp[96] = {0};
	snprintf(resp, sizeof(resp), "{\"chip\":\"0x%08" PRIX32 "\",\"size\":\"%" PRIu32 " KB\"}", idcode, flash_kb);
	httpd_resp_set_type(req, "application/json");
	return httpd_resp_sendstr(req, resp);
}

static esp_err_t handle_firmware_list(httpd_req_t *req)
{
	char json[1024] = {0};
	esp_err_t ret = storage_list_json(json, sizeof(json));
	if (ret != ESP_OK)
	{
		send_err(req, 500, "list failed");
		return ESP_FAIL;
	}

	httpd_resp_set_type(req, "application/json");
	return httpd_resp_sendstr(req, json);
}

static esp_err_t handle_firmware_upload(httpd_req_t *req)
{
	char query[128] = {0};
	char name[64] = {0};

	ESP_LOGI(TAG, "Upload request: content_len=%d", req->content_len);

	if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
		httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK)
	{
		ESP_LOGW(TAG, "Upload missing query name, query='%s'", query);
		send_err(req, 400, "missing name");
		return ESP_FAIL;
	}

	if (!has_allowed_upload_extension(name))
	{
		send_err(req, 400, "only .hex/.bin/.flm allowed");
		return ESP_FAIL;
	}

	if (!is_valid_basename(name))
	{
		send_err(req, 400, "invalid file name format");
		return ESP_FAIL;
	}

	if (req->content_len <= 0)
	{
		ESP_LOGW(TAG, "Upload '%s' has empty body", name);
		send_err(req, 400, "empty body");
		return ESP_FAIL;
	}

	FILE *fp = NULL;
	esp_err_t ret = storage_open_for_write(name, &fp);
	if (ret != ESP_OK || !fp)
	{
		ESP_LOGE(TAG, "Upload open failed: name='%s', err=%s", name, esp_err_to_name(ret));
		send_err(req, 500, "open failed");
		return ESP_FAIL;
	}

	char buf[512];
	int remain = req->content_len;
	int total_written = 0;
	while (remain > 0)
	{
		int chunk = remain > (int)sizeof(buf) ? (int)sizeof(buf) : remain;
		int r = httpd_req_recv(req, buf, chunk);
		if (r <= 0)
		{
			ESP_LOGE(TAG, "Upload recv failed: name='%s', remain=%d", name, remain);
			fclose(fp);
			send_err(req, 500, "recv failed");
			return ESP_FAIL;
		}

		size_t w = fwrite(buf, 1, (size_t)r, fp);
		if (w != (size_t)r)
		{
			ESP_LOGE(TAG, "Upload write failed: name='%s', got=%d, wrote=%d", name, r, (int)w);
			fclose(fp);
			send_err(req, 500, "write failed");
			return ESP_FAIL;
		}

		remain -= r;
		total_written += r;
	}

	fclose(fp);
	ESP_LOGI(TAG, "Upload completed: name='%s', bytes=%d", name, total_written);

	char stored_name[64] = {0};
	strncpy(stored_name, name, sizeof(stored_name) - 1);
	int stored_bytes = total_written;

	if (is_hex_file(name))
	{
		FILE *hex_fp = NULL;
		esp_err_t hret = storage_open_for_read(name, &hex_fp);
		if (hret != ESP_OK || !hex_fp)
		{
			ESP_LOGE(TAG, "HEX reopen failed: file='%s' err=%s", name, esp_err_to_name(hret));
			send_err(req, 500, "hex reopen failed");
			return ESP_FAIL;
		}

		char bin_name[64] = {0};
		if (make_bin_name_from_hex(name, bin_name, sizeof(bin_name)) != ESP_OK)
		{
			fclose(hex_fp);
			ESP_LOGE(TAG, "Invalid HEX filename for convert: '%s'", name);
			send_err(req, 400, "invalid hex name");
			return ESP_FAIL;
		}

		FILE *bin_fp = NULL;
		esp_err_t bret = storage_open_for_write(bin_name, &bin_fp);
		if (bret != ESP_OK || !bin_fp)
		{
			fclose(hex_fp);
			ESP_LOGE(TAG, "BIN open failed: file='%s' err=%s", bin_name, esp_err_to_name(bret));
			send_err(req, 500, "bin open failed");
			return ESP_FAIL;
		}

		uint32_t bin_size = 0;
		uint32_t base_addr = 0;
		esp_err_t cvt = hex2bin_stream(hex_fp, bin_fp, &base_addr, &bin_size);
		fclose(hex_fp);
		fclose(bin_fp);
		if (cvt != ESP_OK)
		{
			ESP_LOGE(TAG, "HEX convert failed: src='%s' dst='%s' err=%s", name, bin_name, esp_err_to_name(cvt));
			(void)storage_delete(bin_name);
			send_err(req, 400, "hex convert failed");
			return ESP_FAIL;
		}

		FILE *check_fp = NULL;
		esp_err_t ck = storage_open_for_read(bin_name, &check_fp);
		if (ck != ESP_OK || !check_fp)
		{
			ESP_LOGE(TAG, "BIN verify open failed: file='%s' err=%s", bin_name, esp_err_to_name(ck));
			(void)storage_delete(bin_name);
			send_err(req, 500, "bin write failed");
			return ESP_FAIL;
		}
		fclose(check_fp);

		esp_err_t dret = storage_delete(name);
		if (dret != ESP_OK)
		{
			ESP_LOGW(TAG, "Delete original HEX failed: file='%s' err=%s", name, esp_err_to_name(dret));
		}
		strncpy(stored_name, bin_name, sizeof(stored_name) - 1);
		stored_bytes = (int)bin_size;
		ESP_LOGI(TAG, "Upload hex converted: src='%s' dst='%s' bytes=%d base=0x%08" PRIX32,
				 name,
				 stored_name,
				 stored_bytes,
				 base_addr);
	}

	char resp[128] = {0};
	snprintf(resp, sizeof(resp), "{\"ok\":true,\"name\":\"%s\",\"bytes\":%d}", stored_name, stored_bytes);
	httpd_resp_set_type(req, "application/json");
	return httpd_resp_sendstr(req, resp);
}

static esp_err_t handle_firmware_delete(httpd_req_t *req)
{
	char body[160] = {0};
	if (read_body(req, body, sizeof(body)) != ESP_OK)
	{
		led_set_error();
		send_err(req, 400, "bad body");
		return ESP_FAIL;
	}

	cJSON *root = cJSON_Parse(body);
	if (!root)
	{
		led_set_error();
		send_err(req, 400, "bad json");
		return ESP_FAIL;
	}

	cJSON *file = cJSON_GetObjectItem(root, "file");
	if (!cJSON_IsString(file) || !file->valuestring)
	{
		cJSON_Delete(root);
		send_err(req, 400, "missing file");
		return ESP_FAIL;
	}

	esp_err_t ret = storage_delete(file->valuestring);
	cJSON_Delete(root);
	if (ret != ESP_OK)
	{
		send_err(req, 500, "delete failed");
		return ESP_FAIL;
	}

	httpd_resp_set_type(req, "application/json");
	return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_btn_flash(httpd_req_t *req)
{
	char body[256] = {0};
	if (read_body(req, body, sizeof(body)) != ESP_OK)
	{
		send_err(req, 400, "bad body");
		return ESP_FAIL;
	}

	cJSON *root = cJSON_Parse(body);
	if (!root)
	{
		send_err(req, 400, "bad json");
		return ESP_FAIL;
	}

	cJSON *fw = cJSON_GetObjectItem(root, "firmware");
	cJSON *addr = cJSON_GetObjectItem(root, "address");
	cJSON *algo = cJSON_GetObjectItem(root, "algorithm");
	if (!cJSON_IsString(fw) || !fw->valuestring || fw->valuestring[0] == '\0')
	{
		cJSON_Delete(root);
		led_set_error();
		send_err(req, 400, "missing args");
		return ESP_FAIL;
	}

	uint32_t base_addr = 0;
	int has_user_addr = 0;
	if (cJSON_IsString(addr) && addr->valuestring && addr->valuestring[0] != '\0')
	{
		char *endptr = NULL;
		base_addr = (uint32_t)strtoul(addr->valuestring, &endptr, 0);
		if (endptr == addr->valuestring || (endptr && *endptr != '\0'))
		{
			cJSON_Delete(root);
			led_set_error();
			send_err(req, 400, "invalid address");
			return ESP_FAIL;
		}
		has_user_addr = 1;
	}

	char name[64] = {0};
	strncpy(name, fw->valuestring, sizeof(name) - 1);
	char algorithm_name[64] = {0};
	if (cJSON_IsString(algo) && algo->valuestring && algo->valuestring[0] != '\0')
	{
		strncpy(algorithm_name, algo->valuestring, sizeof(algorithm_name) - 1);
	}
	cJSON_Delete(root);

	if (!has_allowed_firmware_extension(name))
	{
		led_set_error();
		send_err(req, 400, "only .bin allowed");
		return ESP_FAIL;
	}

	FILE *fp = NULL;
	esp_err_t sret = storage_open_for_read(name, &fp);
	if (sret != ESP_OK || !fp)
	{
		if (sret == ESP_ERR_INVALID_ARG || sret == ESP_ERR_INVALID_SIZE)
		{
			led_set_error();
			send_err(req, 400, "invalid firmware name");
		}
		else
		{
			led_set_error();
			send_err(req, 400, "firmware not found");
		}
		return ESP_FAIL;
	}

	FILE *flash_fp = fp;

	stm32_chip_profile_t profile = {0};
	esp_err_t pret = stm32_detect_profile(&profile, NULL);
	if (pret != ESP_OK)
	{
		if (flash_fp)
		{
			fclose(flash_fp);
		}
		led_set_error();
		send_err(req, 500, "target detect failed");
		return ESP_FAIL;
	}

	if (algorithm_name[0] == '\0')
	{
		esp_err_t s = find_best_flm_for_family(profile.family, algorithm_name, sizeof(algorithm_name));
		if (s != ESP_OK)
		{
			if (flash_fp)
			{
				fclose(flash_fp);
			}
			led_set_error();
			send_err(req, 400, "no matching .flm for chip family");
			return ESP_FAIL;
		}
	}

	if (!is_flm_file(algorithm_name))
	{
		if (flash_fp)
		{
			fclose(flash_fp);
		}
		led_set_error();
		send_err(req, 400, "algorithm must be .flm");
		return ESP_FAIL;
	}

	FILE *algo_fp = NULL;
	esp_err_t aret = storage_open_for_read(algorithm_name, &algo_fp);
	if (aret != ESP_OK || !algo_fp)
	{
		if (flash_fp)
		{
			fclose(flash_fp);
		}
		led_set_error();
		send_err(req, 400, "algorithm file not found");
		return ESP_FAIL;
	}

	flm_info_t flm = {0};
	aret = flm_parse_from_file(algo_fp, algorithm_name, &flm);
	fclose(algo_fp);
	if (aret != ESP_OK)
	{
		if (flash_fp)
		{
			fclose(flash_fp);
		}
		led_set_error();
		send_err(req, 400, "invalid .flm file");
		return ESP_FAIL;
	}

	if (!has_user_addr)
	{
		base_addr = flm.flash_start;
	}

	uint32_t firmware_size_u32 = 0;
	if (fseek(flash_fp, 0, SEEK_END) != 0)
	{
		fclose(flash_fp);
		led_set_error();
		send_err(req, 500, "firmware seek failed");
		return ESP_FAIL;
	}
	long firmware_size = ftell(flash_fp);
	if (firmware_size < 0)
	{
		fclose(flash_fp);
		led_set_error();
		send_err(req, 500, "firmware size failed");
		return ESP_FAIL;
	}
	if (fseek(flash_fp, 0, SEEK_SET) != 0)
	{
		fclose(flash_fp);
		led_set_error();
		send_err(req, 500, "firmware rewind failed");
		return ESP_FAIL;
	}
	firmware_size_u32 = (uint32_t)firmware_size;

	if (base_addr < flm.flash_start ||
		(uint64_t)base_addr + (uint64_t)firmware_size_u32 > (uint64_t)flm.flash_start + (uint64_t)flm.flash_size)
	{
		if (flash_fp)
		{
			fclose(flash_fp);
		}
		led_set_error();
		send_err(req, 400, "firmware range out of flash by .flm");
		return ESP_FAIL;
	}

	uint32_t written = 0;
	esp_err_t fret = ESP_FAIL;

	FILE *algo_run_fp = NULL;
	aret = storage_open_for_read(algorithm_name, &algo_run_fp);
	if (aret != ESP_OK || !algo_run_fp)
	{
		if (flash_fp)
		{
			fclose(flash_fp);
		}
		led_set_error();
		send_err(req, 400, "algorithm file open failed");
		return ESP_FAIL;
	}

	(void)led_blink_alternate_start(100);
	fret = flm_runtime_flash_stream(algo_run_fp, &flm, base_addr, flash_fp, firmware_size_u32, &written);
	fclose(algo_run_fp);
	(void)led_effect_stop();

	if (flash_fp)
	{
		fclose(flash_fp);
	}

	if (fret != ESP_OK)
	{
		led_set_error();
		send_err(req, 500, "flash failed");
		return ESP_FAIL;
	}
	led_set_ready();

	ESP_LOGI(TAG,
			 "Flash OK: fw='%s' algo='%s' dev_id=0x%03" PRIX32 " base=0x%08" PRIX32 " bytes=%" PRIu32,
			 name,
			 algorithm_name,
			 profile.dev_id,
			 base_addr,
			 written);
	httpd_resp_set_type(req, "application/json");
	return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_btn_erase(httpd_req_t *req)
{
	char body[256] = {0};
	if (read_body(req, body, sizeof(body)) != ESP_OK)
	{
		send_err(req, 400, "bad body");
		return ESP_FAIL;
	}

	cJSON *root = cJSON_Parse(body);
	if (!root)
	{
		send_err(req, 400, "bad json");
		return ESP_FAIL;
	}

	cJSON *algo = cJSON_GetObjectItem(root, "algorithm");

	char algorithm_name[64] = {0};
	if (cJSON_IsString(algo) && algo->valuestring && algo->valuestring[0] != '\0')
	{
		strncpy(algorithm_name, algo->valuestring, sizeof(algorithm_name) - 1);
	}

	cJSON_Delete(root);

	stm32_chip_profile_t profile = {0};
	esp_err_t pret = stm32_detect_profile(&profile, NULL);
	if (pret != ESP_OK)
	{
		led_set_error();
		send_err(req, 500, "target detect failed");
		return ESP_FAIL;
	}

	uint8_t current_lock_level = 0;
	int lock_known = 0;
	int unlocked_before_erase = 0;
	int erase_executed = 0;
	esp_err_t glret = stm32_get_lock_level(profile.dev_id, &current_lock_level);
	if (glret == ESP_OK)
	{
		lock_known = 1;
		if (current_lock_level > 0U)
		{
			esp_err_t ulret = stm32_apply_lock_level(profile.dev_id, 0);
			if (ulret != ESP_OK)
			{
				led_set_error();
				send_err(req, 500, "unlock before erase failed");
				return ESP_FAIL;
			}
			unlocked_before_erase = 1;

			ESP_LOGI(TAG,
					 "Erase request handled as unlock-only: dev_id=0x%03" PRIX32 " lock_before=%u",
					 profile.dev_id,
					 (unsigned)current_lock_level);
			led_set_ready();
			httpd_resp_set_type(req, "application/json");
			return httpd_resp_sendstr(req, "{\"ok\":true,\"mode\":\"unlock_only\"}");
		}
	}

	if (algorithm_name[0] == '\0')
	{
		esp_err_t s = find_best_flm_for_family(profile.family, algorithm_name, sizeof(algorithm_name));
		if (s != ESP_OK)
		{
			led_set_error();
			send_err(req, 400, "no matching .flm for chip family");
			return ESP_FAIL;
		}
	}

	if (!is_flm_file(algorithm_name))
	{
		led_set_error();
		send_err(req, 400, "algorithm must be .flm");
		return ESP_FAIL;
	}

	FILE *algo_fp = NULL;
	esp_err_t aret = storage_open_for_read(algorithm_name, &algo_fp);
	if (aret != ESP_OK || !algo_fp)
	{
		led_set_error();
		send_err(req, 400, "algorithm file not found");
		return ESP_FAIL;
	}

	flm_info_t flm = {0};
	aret = flm_parse_from_file(algo_fp, algorithm_name, &flm);
	fclose(algo_fp);
	if (aret != ESP_OK)
	{
		led_set_error();
		send_err(req, 400, "invalid .flm file");
		return ESP_FAIL;
	}

	FILE *algo_run_fp = NULL;
	aret = storage_open_for_read(algorithm_name, &algo_run_fp);
	if (aret != ESP_OK || !algo_run_fp)
	{
		led_set_error();
		send_err(req, 400, "algorithm file open failed");
		return ESP_FAIL;
	}

	esp_err_t rt_erase = flm_runtime_erase_chip(algo_run_fp, &flm);
	fclose(algo_run_fp);
	if (rt_erase != ESP_OK)
	{
		led_set_error();
		send_err(req, 500, "erase failed");
		return ESP_FAIL;
	}
	erase_executed = 1;

	(void)swd_target_reset_run();
	
	led_set_ready();

	ESP_LOGI(TAG,
			 "Erase OK: algo='%s' dev_id=0x%03" PRIX32 " flash_start=0x%08" PRIX32 " flash_size=%" PRIu32 " lock_known=%d lock_before=%u unlocked_before_erase=%d erase_executed=%d",
			 algorithm_name,
			 profile.dev_id,
			 flm.flash_start,
			 flm.flash_size,
			 lock_known,
			 (unsigned)current_lock_level,
			 unlocked_before_erase,
			 erase_executed);

	httpd_resp_set_type(req, "application/json");
	return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_btn_lock(httpd_req_t *req)
{
	char body[128] = {0};
	if (read_body(req, body, sizeof(body)) != ESP_OK)
	{
		send_err(req, 400, "bad body");
		return ESP_FAIL;
	}

	cJSON *root = cJSON_Parse(body);
	if (!root)
	{
		send_err(req, 400, "bad json");
		return ESP_FAIL;
	}

	cJSON *lock = cJSON_GetObjectItem(root, "lock");
	uint8_t lock_level = 0;
	if (cJSON_IsString(lock) && lock->valuestring && lock->valuestring[0] != '\0')
	{
		char *lock_end = NULL;
		unsigned long lv = strtoul(lock->valuestring, &lock_end, 10);
		if (lock_end == lock->valuestring || (lock_end && *lock_end != '\0') || lv > 2UL)
		{
			cJSON_Delete(root);
			send_err(req, 400, "invalid lock level");
			return ESP_FAIL;
		}
		lock_level = (uint8_t)lv;
	}
	else if (cJSON_IsNumber(lock))
	{
		int lv = lock->valueint;
		if (lv < 0 || lv > 2)
		{
			cJSON_Delete(root);
			send_err(req, 400, "invalid lock level");
			return ESP_FAIL;
		}
		lock_level = (uint8_t)lv;
	}
	else
	{
		cJSON_Delete(root);
		send_err(req, 400, "missing lock");
		return ESP_FAIL;
	}

	cJSON_Delete(root);

	stm32_chip_profile_t profile = {0};
	esp_err_t pret = stm32_detect_profile(&profile, NULL);
	if (pret != ESP_OK)
	{
		send_err(req, 500, "target detect failed");
		return ESP_FAIL;
	}

	esp_err_t lret = stm32_apply_lock_level(profile.dev_id, lock_level);
	if (lret != ESP_OK)
	{
		send_err(req, 500, "apply lock level failed");
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Lock level applied: dev_id=0x%03" PRIX32 " level=%u", profile.dev_id, (unsigned)lock_level);
	httpd_resp_set_type(req, "application/json");
	return httpd_resp_sendstr(req, "{\"ok\":true}");
}

esp_err_t web_manual_register_routes(httpd_handle_t server)
{
	const httpd_uri_t routes[] = {
		{.uri = "/Manual.html", .method = HTTP_GET, .handler = handle_root, .user_ctx = NULL},
		{.uri = "/connect", .method = HTTP_GET, .handler = handle_btn_connect, .user_ctx = NULL},
		{.uri = "/flash", .method = HTTP_POST, .handler = handle_btn_flash, .user_ctx = NULL},
		{.uri = "/erase", .method = HTTP_POST, .handler = handle_btn_erase, .user_ctx = NULL},
		{.uri = "/lock", .method = HTTP_POST, .handler = handle_btn_lock, .user_ctx = NULL},
		{.uri = "/list_fw", .method = HTTP_GET, .handler = handle_firmware_list, .user_ctx = NULL},
		{.uri = "/upload_fw", .method = HTTP_POST, .handler = handle_firmware_upload, .user_ctx = NULL},
		{.uri = "/delete_fw", .method = HTTP_POST, .handler = handle_firmware_delete, .user_ctx = NULL},
	};

	for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
	{
		esp_err_t ret = httpd_register_uri_handler(server, &routes[i]);
		if (ret != ESP_OK)
		{
			return ret;
		}
	}

	return ESP_OK;
}
