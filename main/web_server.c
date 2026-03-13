#include "web_server.h"

#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "WEB";

static httpd_handle_t s_server = NULL;

esp_err_t web_server_start(void)
{
	if (s_server)
	{
		return ESP_OK;
	}

	httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
	cfg.max_uri_handlers = 20;
	// Flashing over SWD runs in this handler and needs larger stack than default.
	cfg.stack_size = 12288;

	esp_err_t ret = httpd_start(&s_server, &cfg);
	if (ret != ESP_OK)
	{
		return ret;
	}

	ret = web_manual_register_routes(s_server);
	if (ret != ESP_OK)
	{
		return ret;
	}

	ret = web_scenario_register_routes(s_server);
	if (ret != ESP_OK)
	{
		return ret;
	}

	ret = web_dashboard_register_routes(s_server);
	if (ret != ESP_OK)
	{
		return ret;
	}

	ESP_LOGI(TAG, "Web server started");
	return ESP_OK;
}
