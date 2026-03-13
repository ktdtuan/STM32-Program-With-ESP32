#include "wifi_manager.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "WIFI_MGR";

// WiFi configuration storage
static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ap_netif = NULL;
static bool sta_connected = false;
static bool ap_enabled = false;

/* WiFi Event Handler */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
	if (event_base != WIFI_EVENT)
	{
		return;
	}

	switch (event_id)
	{
	case WIFI_EVENT_STA_START:
		ESP_LOGI(TAG, "WiFi STA started, connecting...");
		esp_wifi_connect();
		break;
	case WIFI_EVENT_STA_DISCONNECTED:
		ESP_LOGI(TAG, "WiFi STA disconnected, retrying...");
		sta_connected = false;
		esp_wifi_connect();
		break;
	case WIFI_EVENT_AP_STACONNECTED:
		wifi_event_ap_staconnected_t *event_connected = (wifi_event_ap_staconnected_t *)event_data;
		ESP_LOGI(TAG, "Station " MACSTR " joined AP, AID=%d", MAC2STR(event_connected->mac), event_connected->aid);
		break;
	case WIFI_EVENT_AP_STADISCONNECTED:
		wifi_event_ap_stadisconnected_t *event_disconnected = (wifi_event_ap_stadisconnected_t *)event_data;
		ESP_LOGI(TAG, "Station " MACSTR " left AP, AID=%d", MAC2STR(event_disconnected->mac), event_disconnected->aid);
		break;
	case WIFI_EVENT_AP_START:
		ESP_LOGI(TAG, "WiFi AP started");
		ap_enabled = true;
		break;
	case WIFI_EVENT_AP_STOP:
		ESP_LOGI(TAG, "WiFi AP stopped");
		ap_enabled = false;
		break;
	default:
		break;
	}
}

/* IP Event Handler */
static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
	if (event_id == IP_EVENT_STA_GOT_IP)
	{
		ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
		ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&event->ip_info.ip));
		ESP_LOGI(TAG, "Gateway: " IPSTR, IP2STR(&event->ip_info.gw));
		ESP_LOGI(TAG, "Netmask: " IPSTR, IP2STR(&event->ip_info.netmask));
		sta_connected = true;
	}
}

/* Initialize WiFi manager with default configuration */
esp_err_t wifi_manager_init(void)
{
	esp_err_t ret;

	// Initialize network interface
	ret = esp_netif_init();
	if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
		return ret;

	// Create default event loop
	ret = esp_event_loop_create_default();
	if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
		return ret;

	// Create network interfaces for STA and AP
	sta_netif = esp_netif_create_default_wifi_sta();
	ap_netif = esp_netif_create_default_wifi_ap();

	// Initialize WiFi
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ret = esp_wifi_init(&cfg);
	if (ret != ESP_OK)
		return ret;

	// Register event handlers
	ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
	if (ret != ESP_OK)
		return ret;

	ret = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL, NULL);
	if (ret != ESP_OK)
		return ret;

	ESP_LOGI(TAG, "WiFi Manager initialized");
	return ESP_OK;
}

/* Configure WiFi STA mode */
esp_err_t wifi_manager_configure_sta(const WiFiSTA_config_t *sta_config)
{
	if (!sta_config)
	{
		return ESP_ERR_INVALID_ARG;
	}

	esp_err_t ret = ESP_OK;
	wifi_config_t wifi_config = {0};

	// Copy STA configuration
	strncpy((char *)wifi_config.sta.ssid, sta_config->ssid, 31);
	strncpy((char *)wifi_config.sta.password, sta_config->password, 63);
	wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

	// Configure static IP if DHCP is disabled
	if (!sta_config->dhcp_enabled)
	{
		esp_netif_dhcpc_stop(sta_netif);

		esp_netif_ip_info_t ip_info;
		ip_info.ip.addr = ESP_IP4TOADDR(sta_config->static_ip[0], sta_config->static_ip[1], sta_config->static_ip[2],
										sta_config->static_ip[3]);
		ip_info.gw.addr = ESP_IP4TOADDR(sta_config->gateway[0], sta_config->gateway[1], sta_config->gateway[2],
										sta_config->gateway[3]);
		ip_info.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);

		ret = esp_netif_set_ip_info(sta_netif, &ip_info);
		if (ret != ESP_OK)
		{
			ESP_LOGE(TAG, "Failed to set static IP");
			return ret;
		}

		// Set DNS server
		esp_netif_dns_info_t dns_info;
		dns_info.ip.u_addr.ip4.addr =
			ESP_IP4TOADDR(sta_config->dns[0], sta_config->dns[1], sta_config->dns[2], sta_config->dns[3]);
		dns_info.ip.type = IPADDR_TYPE_V4;
		esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns_info);

		ESP_LOGI(TAG, "STA Static IP: %d.%d.%d.%d", sta_config->static_ip[0], sta_config->static_ip[1],
				 sta_config->static_ip[2], sta_config->static_ip[3]);
	}
	else
	{
		// Enable DHCP
		esp_netif_dhcpc_start(sta_netif);
		ESP_LOGI(TAG, "STA DHCP enabled");
	}

	// Get current mode
	wifi_mode_t current_mode = WIFI_MODE_NULL;
	esp_wifi_get_mode(&current_mode);

	// Stop WiFi if running to change mode safely
	bool wifi_was_started = (current_mode != WIFI_MODE_NULL);
	if (wifi_was_started)
	{
		esp_wifi_stop();
	}

	// Set new mode (STA or STA+AP)
	if (current_mode == WIFI_MODE_AP)
	{
		ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
	}
	else
	{
		ret = esp_wifi_set_mode(WIFI_MODE_STA);
	}
	if (ret != ESP_OK)
		return ret;

	// Apply STA configuration
	ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
	if (ret != ESP_OK)
		return ret;

	// Start WiFi
	ret = esp_wifi_start();

	ESP_LOGI(TAG, "WiFi STA configured. SSID: %s, Password: %s", sta_config->ssid, sta_config->password);
	return ret;
}

/* Configure WiFi AP mode */
esp_err_t wifi_manager_configure_ap(const WiFiAP_config_t *ap_config)
{
	if (!ap_config)
	{
		return ESP_ERR_INVALID_ARG;
	}

	esp_err_t ret = ESP_OK;
	wifi_config_t wifi_config = {0};

	// Get MAC address
	uint8_t mac[6];
	esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
	
	// Create SSID with MAC address (4 last bytes)
	// Format: "Watering-AABBCCDD"
	char ssid_with_mac[32];
	snprintf(ssid_with_mac, sizeof(ssid_with_mac), "%s-%02X%02X", ap_config->ssid,  mac[4], mac[5]);
	
	// Copy AP configuration with MAC appended
	strncpy((char *)wifi_config.ap.ssid, ssid_with_mac, 31);
	wifi_config.ap.ssid_len = strlen(ssid_with_mac);
	strncpy((char *)wifi_config.ap.password, ap_config->password, 63);
	wifi_config.ap.channel = ap_config->channel;
	wifi_config.ap.max_connection = 4;
	wifi_config.ap.authmode = (strlen(ap_config->password) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_WPA2_PSK;

	// Get current mode
	wifi_mode_t current_mode = WIFI_MODE_NULL;
	esp_wifi_get_mode(&current_mode);

	// Stop WiFi if running to change mode safely
	bool wifi_was_started = (current_mode != WIFI_MODE_NULL);
	if (wifi_was_started)
	{
		esp_wifi_stop();
	}

	// Set new mode (AP or STA+AP)
	if (current_mode == WIFI_MODE_STA)
	{
		ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
	}
	else
	{
		ret = esp_wifi_set_mode(WIFI_MODE_AP);
	}
	if (ret != ESP_OK)
		return ret;

	// Apply AP configuration
	ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
	if (ret != ESP_OK)
		return ret;

	// Start WiFi
	ret = esp_wifi_start();
	if (ret != ESP_OK)
		return ret;

	ESP_LOGI(TAG, "WiFi AP configured. SSID: %s, Channel: %d", ssid_with_mac, ap_config->channel);
	return ESP_OK;
}

/* Apply complete WiFi configuration */
esp_err_t wifi_manager_apply_config(const WiFi_config_t *config)
{
	if (!config)
	{
		return ESP_ERR_INVALID_ARG;
	}

	esp_err_t ret = ESP_OK;

	// Configure AP first
	if (strlen(config->ap.ssid) > 0)
	{
		ret = wifi_manager_configure_ap(&config->ap);
		if (ret != ESP_OK)
		{
			ESP_LOGE(TAG, "Failed to configure AP");
			return ret;
		}
	}

	// Configure STA if SSID is provided
	if (strlen(config->sta.ssid) > 0)
	{
		ret = wifi_manager_configure_sta(&config->sta);
		if (ret != ESP_OK)
		{
			ESP_LOGE(TAG, "Failed to configure STA");
			return ret;
		}
	}

	ESP_LOGI(TAG, "WiFi configuration applied successfully");
	return ESP_OK;
}

/* Get WiFi IP address */
const char* wifi_manager_get_ip(void)
{
	static char ip_str[16];
	esp_netif_ip_info_t ip_info;

	if (ap_netif && esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK)
	{
		snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
		return ip_str;
	}

	return "192.168.4.1";
}

/* Check if STA is connected */
bool wifi_manager_is_sta_connected(void) { return sta_connected; }

/* Check if AP is enabled */
bool wifi_manager_is_ap_enabled(void) { return ap_enabled; }

/* Disable WiFi verbose logs (only show warnings/errors) */
void wifi_manager_disable_logs(void)
{
	esp_log_level_set("wifi", ESP_LOG_WARN);
	esp_log_level_set("wifi_init", ESP_LOG_WARN);
	esp_log_level_set("phy_init", ESP_LOG_WARN);
	// esp_log_level_set("WIFI_MGR", ESP_LOG_WARN);
	ESP_LOGI(TAG, "WiFi verbose logs disabled (Warn/Error only)");
}

/* Enable WiFi verbose logs (show all info) */
void wifi_manager_enable_logs(void)
{
	esp_log_level_set("wifi", ESP_LOG_INFO);
	esp_log_level_set("wifi_init", ESP_LOG_INFO);
	esp_log_level_set("phy_init", ESP_LOG_INFO);
	esp_log_level_set("WIFI_MGR", ESP_LOG_INFO);
	ESP_LOGI(TAG, "WiFi verbose logs enabled");
}
