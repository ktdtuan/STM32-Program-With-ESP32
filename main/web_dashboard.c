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

static const char *TAG = "WEB_DASHBOARD";

extern const uint8_t dashboard_page_start[] asm("_binary_Dashboard_html_start");
extern const uint8_t dashboard_page_end[] asm("_binary_Dashboard_html_end");

static void send_err(httpd_req_t *req, int code, const char *msg)
{
	httpd_resp_set_status(req, code == 400 ? "400 Bad Request" : "500 Internal Server Error");
	httpd_resp_set_type(req, "text/plain");
	httpd_resp_sendstr(req, msg ? msg : "error");
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

static int has_json_extension(const char *name)
{
	if (!name)
	{
		return 0;
	}

	size_t len = strlen(name);
	if (len < 5)
	{
		return 0;
	}

	const char *ext = name + (len - 5);
	return tolower((unsigned char)ext[0]) == '.' &&
			   tolower((unsigned char)ext[1]) == 'j' &&
			   tolower((unsigned char)ext[2]) == 's' &&
			   tolower((unsigned char)ext[3]) == 'o' &&
			   tolower((unsigned char)ext[4]) == 'n';
}

static esp_err_t normalize_scenario_filename(const char *input, char *out, size_t out_len)
{
	if (!input || !out || out_len == 0)
	{
		return ESP_ERR_INVALID_ARG;
	}

	out[0] = '\0';

	char temp[96] = {0};
	size_t n = strnlen(input, sizeof(temp) - 1);
	if (n == 0)
	{
		return ESP_ERR_INVALID_ARG;
	}
	memcpy(temp, input, n);
	temp[n] = '\0';

	while (n > 0 && (temp[n - 1] == ' ' || temp[n - 1] == '\t' || temp[n - 1] == '\r' || temp[n - 1] == '\n'))
	{
		temp[n - 1] = '\0';
		n--;
	}

	if (!is_valid_basename(temp))
	{
		return ESP_ERR_INVALID_ARG;
	}

	if (!has_json_extension(temp))
	{
		int w = snprintf(out, out_len, "%s.json", temp);
		if (w <= 0 || (size_t)w >= out_len)
		{
			return ESP_ERR_INVALID_SIZE;
		}
		return ESP_OK;
	}

	if (strlen(temp) + 1 > out_len)
	{
		return ESP_ERR_INVALID_SIZE;
	}
	strncpy(out, temp, out_len - 1);
	out[out_len - 1] = '\0';
	return ESP_OK;
}

static int has_allowed_firmware_extension(const char *name)
{
	if (!name)
	{
		return 0;
	}

	const char *dot = strrchr(name, '.');
	if (!dot)
	{
		return 0;
	}

	char ext[8] = {0};
	size_t i = 0;
	for (const char *p = dot; *p && i < sizeof(ext) - 1; ++p, ++i)
	{
		ext[i] = (char)tolower((unsigned char)*p);
	}

	return strcmp(ext, ".hex") == 0 || strcmp(ext, ".bin") == 0;
}

static int is_hex_firmware(const char *name)
{
	if (!name)
	{
		return 0;
	}

	const char *dot = strrchr(name, '.');
	if (!dot)
	{
		return 0;
	}

	char ext[8] = {0};
	size_t i = 0;
	for (const char *p = dot; *p && i < sizeof(ext) - 1; ++p, ++i)
	{
		ext[i] = (char)tolower((unsigned char)*p);
	}

	return strcmp(ext, ".hex") == 0;
}

static int is_flm_file(const char *name)
{
	if (!name)
	{
		return 0;
	}

	const char *dot = strrchr(name, '.');
	if (!dot)
	{
		return 0;
	}

	char ext[8] = {0};
	size_t i = 0;
	for (const char *p = dot; *p && i < sizeof(ext) - 1; ++p, ++i)
	{
		ext[i] = (char)tolower((unsigned char)*p);
	}

	return strcmp(ext, ".flm") == 0;
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

static esp_err_t read_text_file_alloc(const char *name, char **out_text)
{
	if (!name || !out_text)
	{
		return ESP_ERR_INVALID_ARG;
	}
	*out_text = NULL;

	FILE *fp = NULL;
	esp_err_t ret = storage_open_for_read(name, &fp);
	if (ret != ESP_OK || !fp)
	{
		return ret;
	}

	if (fseek(fp, 0, SEEK_END) != 0)
	{
		fclose(fp);
		return ESP_FAIL;
	}
	long sz = ftell(fp);
	if (sz < 0 || sz > 8192)
	{
		fclose(fp);
		return ESP_ERR_INVALID_SIZE;
	}
	if (fseek(fp, 0, SEEK_SET) != 0)
	{
		fclose(fp);
		return ESP_FAIL;
	}

	char *buf = (char *)malloc((size_t)sz + 1U);
	if (!buf)
	{
		fclose(fp);
		return ESP_ERR_NO_MEM;
	}

	size_t r = fread(buf, 1, (size_t)sz, fp);
	fclose(fp);
	if (r != (size_t)sz)
	{
		free(buf);
		return ESP_FAIL;
	}

	buf[r] = '\0';
	*out_text = buf;
	return ESP_OK;
}

static void add_step_result(cJSON *steps, const char *step, int ok, const char *message)
{
	if (!steps)
	{
		return;
	}

	cJSON *item = cJSON_CreateObject();
	if (!item)
	{
		return;
	}

	cJSON_AddStringToObject(item, "step", step ? step : "unknown");
	cJSON_AddBoolToObject(item, "ok", ok ? 1 : 0);
	cJSON_AddStringToObject(item, "message", message ? message : "");
	cJSON_AddItemToArray(steps, item);
}

static esp_err_t respond_run_status(httpd_req_t *req,
									const char *scenario_name,
									int ok,
									cJSON *steps,
									const char *message)
{
	cJSON *resp = cJSON_CreateObject();
	if (!resp)
	{
		send_err(req, 500, "oom");
		return ESP_FAIL;
	}

	cJSON_AddBoolToObject(resp, "ok", ok ? 1 : 0);
	cJSON_AddStringToObject(resp, "scenario", scenario_name ? scenario_name : "");
	cJSON_AddStringToObject(resp, "message", message ? message : (ok ? "completed" : "failed"));
	cJSON_AddItemToObject(resp, "steps", steps ? steps : cJSON_CreateArray());

	char *payload = cJSON_PrintUnformatted(resp);
	cJSON_Delete(resp);
	if (!payload)
	{
		send_err(req, 500, "json encode failed");
		return ESP_FAIL;
	}

	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, payload);
	free(payload);
	return ESP_OK;
}

static esp_err_t handle_dashboard_page(httpd_req_t *req)
{
	size_t html_len = (size_t)(dashboard_page_end - dashboard_page_start);
	httpd_resp_set_type(req, "text/html; charset=utf-8");
	return httpd_resp_send(req, (const char *)dashboard_page_start, html_len);
}

typedef struct
{
	char filename[96];
	char firmware_name[64];
	char algorithm_name[64];
	uint32_t base_addr;
	uint8_t desired_lock_level;

	stm32_chip_profile_t profile;

	char *scenario_text;
	cJSON *doc;

	FILE *flash_fp;
	uint8_t *hex_buf;
	uint32_t firmware_size_u32;

	flm_info_t flm;
	uint32_t written;
} dashboard_run_ctx_t;

static void dashboard_run_ctx_init(dashboard_run_ctx_t *ctx)
{
	if (!ctx)
	{
		return;
	}
	memset(ctx, 0, sizeof(*ctx));
	ctx->base_addr = 0x08000000;
}

static void dashboard_run_ctx_cleanup(dashboard_run_ctx_t *ctx)
{
	if (!ctx)
	{
		return;
	}

	if (ctx->flash_fp)
	{
		fclose(ctx->flash_fp);
		ctx->flash_fp = NULL;
	}

	if (ctx->hex_buf)
	{
		free(ctx->hex_buf);
		ctx->hex_buf = NULL;
	}

	if (ctx->doc)
	{
		cJSON_Delete(ctx->doc);
		ctx->doc = NULL;
	}

	if (ctx->scenario_text)
	{
		free(ctx->scenario_text);
		ctx->scenario_text = NULL;
	}
}

static esp_err_t step_parse_request_and_json(httpd_req_t *req, cJSON *steps, dashboard_run_ctx_t *ctx)
{
	char body[256] = {0};
	if (read_body(req, body, sizeof(body)) != ESP_OK)
	{
		add_step_result(steps, "request", 0, "bad body");
		return ESP_FAIL;
	}

	cJSON *root = cJSON_Parse(body);
	if (!root)
	{
		add_step_result(steps, "request", 0, "bad json");
		return ESP_FAIL;
	}

	cJSON *scenario = cJSON_GetObjectItem(root, "scenario");
	if (!cJSON_IsString(scenario) || !scenario->valuestring || scenario->valuestring[0] == '\0')
	{
		cJSON_Delete(root);
		add_step_result(steps, "request", 0, "missing scenario");
		return ESP_FAIL;
	}

	if (normalize_scenario_filename(scenario->valuestring, ctx->filename, sizeof(ctx->filename)) != ESP_OK)
	{
		cJSON_Delete(root);
		add_step_result(steps, "request", 0, "invalid scenario name");
		return ESP_FAIL;
	}
	cJSON_Delete(root);

	esp_err_t rret = read_text_file_alloc(ctx->filename, &ctx->scenario_text);
	if (rret != ESP_OK || !ctx->scenario_text)
	{
		add_step_result(steps, "load_scenario", 0, "scenario file not found");
		return ESP_FAIL;
	}

	ctx->doc = cJSON_Parse(ctx->scenario_text);
	if (!ctx->doc)
	{
		add_step_result(steps, "load_scenario", 0, "scenario json invalid");
		return ESP_FAIL;
	}

	cJSON *fw = cJSON_GetObjectItem(ctx->doc, "firmware");
	cJSON *addr = cJSON_GetObjectItem(ctx->doc, "address");
	cJSON *dlm = cJSON_GetObjectItem(ctx->doc, "dlm");
	cJSON *lock = cJSON_GetObjectItem(ctx->doc, "lock");

	if (!cJSON_IsString(fw) || !fw->valuestring || fw->valuestring[0] == '\0')
	{
		add_step_result(steps, "load_scenario", 0, "missing firmware in scenario");
		return ESP_FAIL;
	}

	strncpy(ctx->firmware_name, fw->valuestring, sizeof(ctx->firmware_name) - 1);
	if (!has_allowed_firmware_extension(ctx->firmware_name))
	{
		add_step_result(steps, "load_scenario", 0, "firmware must be .hex or .bin");
		return ESP_FAIL;
	}

	if (cJSON_IsString(addr) && addr->valuestring && addr->valuestring[0] != '\0')
	{
		char *endptr = NULL;
		ctx->base_addr = (uint32_t)strtoul(addr->valuestring, &endptr, 0);
		if (endptr == addr->valuestring || (endptr && *endptr != '\0'))
		{
			add_step_result(steps, "load_scenario", 0, "invalid address in scenario");
			return ESP_FAIL;
		}
	}

	if (cJSON_IsString(dlm) && dlm->valuestring && dlm->valuestring[0] != '\0')
	{
		strncpy(ctx->algorithm_name, dlm->valuestring, sizeof(ctx->algorithm_name) - 1);
	}

	ctx->desired_lock_level = 0;
	if (cJSON_IsString(lock) && lock->valuestring)
	{
		ctx->desired_lock_level = (uint8_t)strtoul(lock->valuestring, NULL, 10);
	}
	else if (cJSON_IsNumber(lock))
	{
		int lv = lock->valueint;
		if (lv < 0)
		{
			lv = 0;
		}
		if (lv > 2)
		{
			lv = 2;
		}
		ctx->desired_lock_level = (uint8_t)lv;
	}

	add_step_result(steps, "load_scenario", 1, "scenario loaded");
	return ESP_OK;
}

static esp_err_t step_connect_target(cJSON *steps, dashboard_run_ctx_t *ctx)
{
	(void)ctx;
	uint32_t idcode = 0;
	int cret = swd_read_idcode(&idcode);
	if (cret == 0)
	{
		cret = swd_start();
	}
	if (cret != 0)
	{
		add_step_result(steps, "connect", 0, "cannot connect target");
		return ESP_FAIL;
	}

	add_step_result(steps, "connect", 1, "target connected");
	return ESP_OK;
}

static esp_err_t step_unlock_if_needed(cJSON *steps, dashboard_run_ctx_t *ctx)
{
	esp_err_t pret = stm32_detect_profile(&ctx->profile, NULL);
	if (pret != ESP_OK)
	{
		add_step_result(steps, "check_lock", 0, "target detect failed");
		return ESP_FAIL;
	}

	uint8_t current_lock_level = 0;
	esp_err_t glret = stm32_get_lock_level(ctx->profile.dev_id, &current_lock_level);
	if (glret != ESP_OK)
	{
		add_step_result(steps, "check_lock", 0, "read lock level failed");
		return ESP_FAIL;
	}

	if (current_lock_level == 0)
	{
		add_step_result(steps, "check_lock", 1, "already unlocked, skip");
		add_step_result(steps, "reconnect_after_unlock", 1, "skip (no unlock)");
		return ESP_OK;
	}

	esp_err_t ulret = stm32_apply_lock_level(ctx->profile.dev_id, 0);
	if (ulret != ESP_OK)
	{
		add_step_result(steps, "check_lock", 0, "unlock failed");
		return ESP_FAIL;
	}
	add_step_result(steps, "check_lock", 1, "target unlocked before flash");

	// OB reload after unlock can drop SWD access temporarily; reconnect before flash.
	(void)swd_target_reset_run();
	vTaskDelay(pdMS_TO_TICKS(120));

	int reconnect_ok = 0;
	for (int i = 0; i < 6; ++i)
	{
		uint32_t reconnect_idcode = 0;
		if (swd_read_idcode(&reconnect_idcode) == 0 && swd_start() == 0)
		{
			reconnect_ok = 1;
			break;
		}
		vTaskDelay(pdMS_TO_TICKS(50));
	}

	if (!reconnect_ok)
	{
		add_step_result(steps, "reconnect_after_unlock", 0, "reconnect failed after unlock");
		return ESP_FAIL;
	}

	pret = stm32_detect_profile(&ctx->profile, NULL);
	if (pret != ESP_OK)
	{
		add_step_result(steps, "reconnect_after_unlock", 0, "target detect failed after unlock");
		return ESP_FAIL;
	}

	uint8_t verify_lock = 0xFFU;
	esp_err_t vret = stm32_get_lock_level(ctx->profile.dev_id, &verify_lock);
	if (vret != ESP_OK || verify_lock != 0U)
	{
		add_step_result(steps, "reconnect_after_unlock", 0, "unlock verify failed");
		return ESP_FAIL;
	}

	add_step_result(steps, "reconnect_after_unlock", 1, "target reconnected after unlock");
	return ESP_OK;
}

static esp_err_t step_prepare_flash(cJSON *steps, dashboard_run_ctx_t *ctx)
{
	FILE *fw_fp = NULL;
	esp_err_t fret = storage_open_for_read(ctx->firmware_name, &fw_fp);
	if (fret != ESP_OK || !fw_fp)
	{
		add_step_result(steps, "flash", 0, "firmware file not found");
		return ESP_FAIL;
	}

	ctx->flash_fp = fw_fp;
	uint32_t hex_size = 0;
	uint32_t hex_base_addr = 0;
	if (is_hex_firmware(ctx->firmware_name))
	{
		esp_err_t cvt = hex2bin_to_buffer(ctx->flash_fp, &ctx->hex_buf, &hex_base_addr, &hex_size);
		fclose(ctx->flash_fp);
		ctx->flash_fp = NULL;
		if (cvt != ESP_OK)
		{
			add_step_result(steps, "flash", 0, "hex convert failed");
			return ESP_FAIL;
		}
		if (hex_base_addr != 0)
		{
			ctx->base_addr = hex_base_addr;
		}
	}

	if (ctx->algorithm_name[0] == '\0')
	{
		esp_err_t s = find_best_flm_for_family(ctx->profile.family, ctx->algorithm_name, sizeof(ctx->algorithm_name));
		if (s != ESP_OK)
		{
			add_step_result(steps, "flash", 0, "no matching .flm for chip family");
			return ESP_FAIL;
		}
	}

	if (!is_flm_file(ctx->algorithm_name))
	{
		add_step_result(steps, "flash", 0, "algorithm must be .flm");
		return ESP_FAIL;
	}

	FILE *algo_fp = NULL;
	esp_err_t aret = storage_open_for_read(ctx->algorithm_name, &algo_fp);
	if (aret != ESP_OK || !algo_fp)
	{
		add_step_result(steps, "flash", 0, "algorithm file not found");
		return ESP_FAIL;
	}

	aret = flm_parse_from_file(algo_fp, ctx->algorithm_name, &ctx->flm);
	fclose(algo_fp);
	if (aret != ESP_OK)
	{
		add_step_result(steps, "flash", 0, "invalid .flm file");
		return ESP_FAIL;
	}

	if (ctx->hex_buf)
	{
		ctx->firmware_size_u32 = hex_size;
	}
	else
	{
		if (fseek(ctx->flash_fp, 0, SEEK_END) != 0)
		{
			add_step_result(steps, "flash", 0, "firmware seek failed");
			return ESP_FAIL;
		}
		long firmware_size = ftell(ctx->flash_fp);
		if (firmware_size < 0)
		{
			add_step_result(steps, "flash", 0, "firmware size failed");
			return ESP_FAIL;
		}
		if (fseek(ctx->flash_fp, 0, SEEK_SET) != 0)
		{
			add_step_result(steps, "flash", 0, "firmware rewind failed");
			return ESP_FAIL;
		}
		ctx->firmware_size_u32 = (uint32_t)firmware_size;
	}

	if (ctx->base_addr < ctx->flm.flash_start ||
		(uint64_t)ctx->base_addr + (uint64_t)ctx->firmware_size_u32 > (uint64_t)ctx->flm.flash_start + (uint64_t)ctx->flm.flash_size)
	{
		add_step_result(steps, "flash", 0, "firmware range out of flash by .flm");
		return ESP_FAIL;
	}

	return ESP_OK;
}

static esp_err_t step_flash_program(cJSON *steps, dashboard_run_ctx_t *ctx)
{
	FILE *algo_run_fp = NULL;
	esp_err_t aret = storage_open_for_read(ctx->algorithm_name, &algo_run_fp);
	if (aret != ESP_OK || !algo_run_fp)
	{
		add_step_result(steps, "flash", 0, "algorithm open failed");
		return ESP_FAIL;
	}

	esp_err_t run_ret = ESP_FAIL;
	(void)led_blink_alternate_start(100);
	if (ctx->hex_buf)
	{
		run_ret = flm_runtime_flash_buffer(algo_run_fp, &ctx->flm, ctx->base_addr, ctx->hex_buf, ctx->firmware_size_u32, &ctx->written);
	}
	else
	{
		run_ret = flm_runtime_flash_stream(algo_run_fp, &ctx->flm, ctx->base_addr, ctx->flash_fp, ctx->firmware_size_u32, &ctx->written);
	}
	fclose(algo_run_fp);
	(void)led_effect_stop();

	if (run_ret != ESP_OK)
	{
		(void)led_set_level(true, false);
		add_step_result(steps, "flash", 0, "flash failed");
		return ESP_FAIL;
	}
	(void)led_set_level(false, true);

	char flash_msg[160] = {0};
	snprintf(flash_msg,
			 sizeof(flash_msg),
			 "flash done: fw='%.32s' algo='%.32s' base=0x%08" PRIX32 " bytes=%" PRIu32,
			 ctx->firmware_name,
			 ctx->algorithm_name,
			 ctx->base_addr,
			 ctx->written);
	add_step_result(steps, "flash", 1, flash_msg);
	return ESP_OK;
}

static esp_err_t step_apply_lock(cJSON *steps, dashboard_run_ctx_t *ctx)
{
	esp_err_t lret = stm32_apply_lock_level(ctx->profile.dev_id, ctx->desired_lock_level);
	if (lret != ESP_OK)
	{
		add_step_result(steps, "lock", 0, "apply lock level failed");
		return ESP_FAIL;
	}

	char lock_msg[80] = {0};
	snprintf(lock_msg, sizeof(lock_msg), "lock level applied: %u", (unsigned)ctx->desired_lock_level);
	add_step_result(steps, "lock", 1, lock_msg);
	return ESP_OK;
}

static esp_err_t handle_dashboard_run(httpd_req_t *req)
{
	dashboard_run_ctx_t ctx;
	dashboard_run_ctx_init(&ctx);

	cJSON *steps = cJSON_CreateArray();
	if (!steps)
	{
		send_err(req, 500, "oom");
		return ESP_FAIL;
	}

	if (step_parse_request_and_json(req, steps, &ctx) != ESP_OK)
	{
		led_set_error();
		dashboard_run_ctx_cleanup(&ctx);
		return respond_run_status(req, ctx.filename, 0, steps, "Scenario request failed");
	}

	if (step_connect_target(steps, &ctx) != ESP_OK)
	{
		led_set_error();
		dashboard_run_ctx_cleanup(&ctx);
		return respond_run_status(req, ctx.filename, 0, steps, "Scenario execution failed");
	}

	if (step_unlock_if_needed(steps, &ctx) != ESP_OK)
	{
		led_set_error();
		dashboard_run_ctx_cleanup(&ctx);
		return respond_run_status(req, ctx.filename, 0, steps, "Scenario execution failed");
	}

	if (step_prepare_flash(steps, &ctx) != ESP_OK)
	{
		led_set_error();
		dashboard_run_ctx_cleanup(&ctx);
		return respond_run_status(req, ctx.filename, 0, steps, "Scenario execution failed");
	}

	if (step_flash_program(steps, &ctx) != ESP_OK)
	{
		led_set_error();
		dashboard_run_ctx_cleanup(&ctx);
		return respond_run_status(req, ctx.filename, 0, steps, "Scenario execution failed");
	}

	if (step_apply_lock(steps, &ctx) != ESP_OK)
	{
		led_set_error();
		dashboard_run_ctx_cleanup(&ctx);
		return respond_run_status(req, ctx.filename, 0, steps, "Scenario execution failed");
	}

	(void)swd_target_reset_run();

	ESP_LOGI(TAG,
			 "Scenario run OK: scenario='%s' fw='%s' algo='%s' base=0x%08" PRIX32 " bytes=%" PRIu32 " lock=%u",
			 ctx.filename,
			 ctx.firmware_name,
			 ctx.algorithm_name,
			 ctx.base_addr,
			 ctx.written,
			 (unsigned)ctx.desired_lock_level);

	dashboard_run_ctx_cleanup(&ctx);
	return respond_run_status(req, ctx.filename, 1, steps, "Scenario completed");
}

esp_err_t web_dashboard_register_routes(httpd_handle_t server)
{
	const httpd_uri_t routes[] = {
		{.uri = "/", .method = HTTP_GET, .handler = handle_dashboard_page, .user_ctx = NULL},
		{.uri = "/Dashboard.html", .method = HTTP_GET, .handler = handle_dashboard_page, .user_ctx = NULL},
		{.uri = "/dashboard_run", .method = HTTP_POST, .handler = handle_dashboard_run, .user_ctx = NULL},
	};

	for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
	{
		esp_err_t ret = httpd_register_uri_handler(server, &routes[i]);
		if (ret != ESP_OK)
		{
			ESP_LOGE(TAG, "register route failed: %s", routes[i].uri);
			return ret;
		}
	}

	ESP_LOGI(TAG, "Dashboard routes registered");
	return ESP_OK;
}
