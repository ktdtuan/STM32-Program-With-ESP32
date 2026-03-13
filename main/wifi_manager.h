#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include "lwip/ip_addr.h"

// WiFi configuration structures
typedef struct
{
	char ssid[32];
	char password[64];
	bool dhcp_enabled;
	uint8_t static_ip[4];
	uint8_t gateway[4];
	uint8_t dns[4];
} WiFiSTA_config_t;

typedef struct
{
	char ssid[26];
	char password[64];
	uint8_t channel;
} WiFiAP_config_t;

typedef struct
{
	WiFiSTA_config_t sta;
	WiFiAP_config_t ap;
} WiFi_config_t;

// WiFi manager API
esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_apply_config(const WiFi_config_t *config);

// Convenience functions
esp_err_t wifi_manager_configure_sta(const WiFiSTA_config_t *sta_config);
esp_err_t wifi_manager_configure_ap(const WiFiAP_config_t *ap_config);

// Status functions
const char* wifi_manager_get_ip(void);
bool wifi_manager_is_sta_connected(void);
bool wifi_manager_is_ap_enabled(void);

// Log control
void wifi_manager_disable_logs(void);
void wifi_manager_enable_logs(void);

#endif // WIFI_MANAGER_H
