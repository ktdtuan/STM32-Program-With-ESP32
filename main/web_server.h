#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C"
{
#endif

esp_err_t web_server_start(void);
esp_err_t web_dashboard_register_routes(httpd_handle_t server);
esp_err_t web_scenario_register_routes(httpd_handle_t server);

// Route registration functions are split by feature area.
esp_err_t web_dashboard_register_routes(httpd_handle_t server);
esp_err_t web_scenario_register_routes(httpd_handle_t server);
esp_err_t web_manual_register_routes(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
