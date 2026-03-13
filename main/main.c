#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "target_swd.h"
#include "storage.h"
#include "web_server.h"
#include "wifi_manager.h"
#include "led_ctrl.h"

static const char *TAG = "MAIN";

WiFi_config_t wifi_config = {
	// .sta =
	// 	{
	// 		.ssid = "RD",
	// 		.password = "LLrd2025",
	// 		.dhcp_enabled = true,
	// 		.static_ip = {192, 168, 95, 28},
	// 		.gateway = {192, 168, 95, 1},
	// 		.dns = {8, 8, 8, 8},
	// 	},
	.ap =
		{
			.ssid = "STM32_Flash_Tool",
			.password = "",
			.channel = 1,
		},
};

void app_main(void)
{
	ESP_LOGI(TAG, "==== ESP32 STM32 FLASH TOOL START ====");

	// Initialize NVS
	esp_err_t ret;
	ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	// Khoi tao SWD pin de su dung cho flash route.
	swd_init();
	ESP_LOGI(TAG, "SWD init done. SWDIO=GPIO%d, SWCLK=GPIO%d", SWDIO_GPIO, SWCLK_GPIO);

	if (storage_init() != ESP_OK)
	{
		ESP_LOGE(TAG, "Storage init failed");
	}

	if (led_ctrl_init() == ESP_OK)
	{
		(void)led_set_level(false, true);
	}
	
	wifi_manager_disable_logs();
	ESP_ERROR_CHECK(wifi_manager_init());
	ESP_ERROR_CHECK(wifi_manager_apply_config(&wifi_config));

	if (web_server_start() != ESP_OK)
	{
		ESP_LOGE(TAG, "Web server start failed");
	}

	while (1)
	{
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}
