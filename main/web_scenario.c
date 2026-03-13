#include "web_server.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "storage.h"

static const char *TAG = "WEB_SCN";

extern const uint8_t scenario_html_start[] asm("_binary_Scenario_html_start");
extern const uint8_t scenario_html_end[] asm("_binary_Scenario_html_end");

static void send_err(httpd_req_t *req, int code, const char *msg)
{
	httpd_resp_set_status(req, code == 400 ? "400 Bad Request" : "500 Internal Server Error");
	httpd_resp_set_type(req, "text/plain");
	httpd_resp_sendstr(req, msg ? msg : "error");
}

static esp_err_t read_body_alloc(httpd_req_t *req, char **out_buf, size_t *out_len)
{
	if (!req || !out_buf)
	{
		return ESP_ERR_INVALID_ARG;
	}

	*out_buf = NULL;
	if (out_len)
	{
		*out_len = 0;
	}

	int total = req->content_len;
	if (total <= 0 || total > 8192)
	{
		return ESP_ERR_INVALID_SIZE;
	}

	char *buf = (char *)malloc((size_t)total + 1U);
	if (!buf)
	{
		return ESP_ERR_NO_MEM;
	}

	int received = 0;
	while (received < total)
	{
		int r = httpd_req_recv(req, buf + received, total - received);
		if (r <= 0)
		{
			free(buf);
			return ESP_FAIL;
		}
		received += r;
	}

	buf[received] = '\0';
	*out_buf = buf;
	if (out_len)
	{
		*out_len = (size_t)received;
	}
	return ESP_OK;
}

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

static int is_json_file(const char *name)
{
	char ext[8] = {0};
	file_ext_lower(name, ext, sizeof(ext));
	return strcmp(ext, ".json") == 0;
}

static int is_hex_color(const char *value)
{
	if (!value || strlen(value) != 7 || value[0] != '#')
	{
		return 0;
	}

	for (int i = 1; i < 7; ++i)
	{
		if (!isxdigit((unsigned char)value[i]))
		{
			return 0;
		}
	}

	return 1;
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

	if (!is_json_file(temp))
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

static esp_err_t write_text_file(const char *name, const char *text, size_t text_len)
{
	FILE *fp = NULL;
	esp_err_t ret = storage_open_for_write(name, &fp);
	if (ret != ESP_OK || !fp)
	{
		return ESP_FAIL;
	}

	size_t w = fwrite(text, 1, text_len, fp);
	fclose(fp);
	if (w != text_len)
	{
		return ESP_FAIL;
	}

	FILE *check = NULL;
	ret = storage_open_for_read(name, &check);
	if (ret != ESP_OK || !check)
	{
		return ESP_FAIL;
	}
	fclose(check);
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

static esp_err_t handle_scenario_page(httpd_req_t *req)
{
	size_t html_len = (size_t)(scenario_html_end - scenario_html_start);
	httpd_resp_set_type(req, "text/html; charset=utf-8");
	return httpd_resp_send(req, (const char *)scenario_html_start, html_len);
}

static esp_err_t handle_list_scenario(httpd_req_t *req)
{
	char raw[2048] = {0};
	esp_err_t ret = storage_list_json(raw, sizeof(raw));
	if (ret != ESP_OK)
	{
		send_err(req, 500, "list failed");
		return ESP_FAIL;
	}

	cJSON *arr = cJSON_Parse(raw);
	if (!arr || !cJSON_IsArray(arr))
	{
		if (arr)
		{
			cJSON_Delete(arr);
		}
		send_err(req, 500, "parse list failed");
		return ESP_FAIL;
	}

	cJSON *filtered = cJSON_CreateArray();
	if (!filtered)
	{
		cJSON_Delete(arr);
		send_err(req, 500, "oom");
		return ESP_FAIL;
	}

	cJSON *it = NULL;
	cJSON_ArrayForEach(it, arr)
	{
		if (!cJSON_IsString(it) || !it->valuestring)
		{
			continue;
		}
		if (!is_json_file(it->valuestring))
		{
			continue;
		}
		cJSON_AddItemToArray(filtered, cJSON_CreateString(it->valuestring));
	}

	char *resp = cJSON_PrintUnformatted(filtered);
	cJSON_Delete(filtered);
	cJSON_Delete(arr);
	if (!resp)
	{
		send_err(req, 500, "print failed");
		return ESP_FAIL;
	}

	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, resp);
	free(resp);
	return ESP_OK;
}

static esp_err_t handle_scenario_get(httpd_req_t *req)
{
	char query[160] = {0};
	char name_in[96] = {0};
	char name[96] = {0};
	if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
		httpd_query_key_value(query, "name", name_in, sizeof(name_in)) != ESP_OK)
	{
		send_err(req, 400, "missing name");
		return ESP_FAIL;
	}

	if (normalize_scenario_filename(name_in, name, sizeof(name)) != ESP_OK)
	{
		send_err(req, 400, "invalid name");
		return ESP_FAIL;
	}

	char *json = NULL;
	esp_err_t ret = read_text_file_alloc(name, &json);
	if (ret != ESP_OK || !json)
	{
		send_err(req, 400, "scenario not found");
		return ESP_FAIL;
	}

	cJSON *doc = cJSON_Parse(json);
	if (!doc)
	{
		free(json);
		send_err(req, 500, "scenario json invalid");
		return ESP_FAIL;
	}
	cJSON_Delete(doc);

	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, json);
	free(json);
	return ESP_OK;
}

static esp_err_t handle_scenario_save(httpd_req_t *req)
{
	char *body = NULL;
	size_t body_len = 0;
	if (read_body_alloc(req, &body, &body_len) != ESP_OK)
	{
		send_err(req, 400, "bad body");
		return ESP_FAIL;
	}

	cJSON *in = cJSON_Parse(body);
	if (!in)
	{
		free(body);
		send_err(req, 400, "bad json");
		return ESP_FAIL;
	}

	cJSON *name = cJSON_GetObjectItem(in, "name");
	if (!cJSON_IsString(name) || !name->valuestring || name->valuestring[0] == '\0')
	{
		cJSON_Delete(in);
		free(body);
		send_err(req, 400, "missing name");
		return ESP_FAIL;
	}

	char filename[96] = {0};
	if (normalize_scenario_filename(name->valuestring, filename, sizeof(filename)) != ESP_OK)
	{
		cJSON_Delete(in);
		free(body);
		send_err(req, 400, "invalid name");
		return ESP_FAIL;
	}

	cJSON *out = cJSON_CreateObject();
	if (!out)
	{
		cJSON_Delete(in);
		free(body);
		send_err(req, 500, "oom");
		return ESP_FAIL;
	}

	cJSON *firmware = cJSON_GetObjectItem(in, "firmware");
	cJSON *address = cJSON_GetObjectItem(in, "address");
	cJSON *dlm = cJSON_GetObjectItem(in, "dlm");
	cJSON *color = cJSON_GetObjectItem(in, "color");
	cJSON *lock = cJSON_GetObjectItem(in, "lock");

	cJSON_AddStringToObject(out, "firmware", cJSON_IsString(firmware) && firmware->valuestring ? firmware->valuestring : "");
	cJSON_AddStringToObject(out, "address", cJSON_IsString(address) && address->valuestring ? address->valuestring : "0x08000000");
	cJSON_AddStringToObject(out, "dlm", cJSON_IsString(dlm) && dlm->valuestring ? dlm->valuestring : "");
	cJSON_AddStringToObject(out, "color", (cJSON_IsString(color) && color->valuestring && is_hex_color(color->valuestring)) ? color->valuestring : "#38bdf8");

	int lock_level = 0;
	if (cJSON_IsString(lock) && lock->valuestring)
	{
		lock_level = atoi(lock->valuestring);
	}
	else if (cJSON_IsNumber(lock))
	{
		lock_level = lock->valueint;
	}
	if (lock_level < 0)
	{
		lock_level = 0;
	}
	if (lock_level > 2)
	{
		lock_level = 2;
	}
	cJSON_AddNumberToObject(out, "lock", lock_level);

	char *json = cJSON_PrintUnformatted(out);
	cJSON_Delete(out);
	cJSON_Delete(in);
	free(body);
	if (!json)
	{
		send_err(req, 500, "json encode failed");
		return ESP_FAIL;
	}

	cJSON *check = cJSON_Parse(json);
	if (!check)
	{
		free(json);
		send_err(req, 500, "json validate failed");
		return ESP_FAIL;
	}
	cJSON_Delete(check);

	esp_err_t wret = write_text_file(filename, json, strlen(json));
	free(json);
	if (wret != ESP_OK)
	{
		send_err(req, 500, "save failed");
		return ESP_FAIL;
	}

	char resp[200] = {0};
	snprintf(resp, sizeof(resp), "{\"ok\":true,\"file\":\"%s\",\"persisted\":true}", filename);
	httpd_resp_set_type(req, "application/json");
	return httpd_resp_sendstr(req, resp);
}

static esp_err_t handle_scenario_delete(httpd_req_t *req)
{
	char *body = NULL;
	size_t body_len = 0;
	if (read_body_alloc(req, &body, &body_len) != ESP_OK)
	{
		send_err(req, 400, "bad body");
		return ESP_FAIL;
	}

	cJSON *root = cJSON_Parse(body);
	free(body);
	if (!root)
	{
		send_err(req, 400, "bad json");
		return ESP_FAIL;
	}

	cJSON *file = cJSON_GetObjectItem(root, "file");
	if (!cJSON_IsString(file) || !file->valuestring)
	{
		file = cJSON_GetObjectItem(root, "name");
	}

	if (!cJSON_IsString(file) || !file->valuestring)
	{
		cJSON_Delete(root);
		send_err(req, 400, "missing file");
		return ESP_FAIL;
	}

	char filename[96] = {0};
	if (normalize_scenario_filename(file->valuestring, filename, sizeof(filename)) != ESP_OK)
	{
		cJSON_Delete(root);
		send_err(req, 400, "invalid file");
		return ESP_FAIL;
	}
	cJSON_Delete(root);

	esp_err_t ret = storage_delete(filename);
	if (ret != ESP_OK)
	{
		send_err(req, 500, "delete failed");
		return ESP_FAIL;
	}

	httpd_resp_set_type(req, "application/json");
	return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_scenario_upload(httpd_req_t *req)
{
	char query[160] = {0};
	char name_in[96] = {0};
	char filename[96] = {0};

	if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
		httpd_query_key_value(query, "name", name_in, sizeof(name_in)) != ESP_OK)
	{
		send_err(req, 400, "missing name");
		return ESP_FAIL;
	}

	if (normalize_scenario_filename(name_in, filename, sizeof(filename)) != ESP_OK || !is_json_file(filename))
	{
		send_err(req, 400, "only .json allowed");
		return ESP_FAIL;
	}

	char *body = NULL;
	size_t body_len = 0;
	if (read_body_alloc(req, &body, &body_len) != ESP_OK)
	{
		send_err(req, 400, "bad body");
		return ESP_FAIL;
	}

	cJSON *doc = cJSON_ParseWithLength(body, body_len);
	if (!doc)
	{
		free(body);
		send_err(req, 400, "invalid json file");
		return ESP_FAIL;
	}
	cJSON_Delete(doc);

	esp_err_t ret = write_text_file(filename, body, body_len);
	free(body);
	if (ret != ESP_OK)
	{
		send_err(req, 500, "upload failed");
		return ESP_FAIL;
	}

	char resp[220] = {0};
	snprintf(resp, sizeof(resp), "{\"ok\":true,\"name\":\"%s\",\"bytes\":%u,\"persisted\":true}", filename, (unsigned int)body_len);
	httpd_resp_set_type(req, "application/json");
	return httpd_resp_sendstr(req, resp);
}

esp_err_t web_scenario_register_routes(httpd_handle_t server)
{
	const httpd_uri_t routes[] = {
		{.uri = "/Scenario.html", .method = HTTP_GET, .handler = handle_scenario_page, .user_ctx = NULL},
		{.uri = "/list_scenario", .method = HTTP_GET, .handler = handle_list_scenario, .user_ctx = NULL},
		{.uri = "/scenario_get", .method = HTTP_GET, .handler = handle_scenario_get, .user_ctx = NULL},
		{.uri = "/scenario_save", .method = HTTP_POST, .handler = handle_scenario_save, .user_ctx = NULL},
		{.uri = "/scenario_delete", .method = HTTP_POST, .handler = handle_scenario_delete, .user_ctx = NULL},
		{.uri = "/upload_scenario", .method = HTTP_POST, .handler = handle_scenario_upload, .user_ctx = NULL},
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

	ESP_LOGI(TAG, "Scenario routes registered");
	return ESP_OK;
}
