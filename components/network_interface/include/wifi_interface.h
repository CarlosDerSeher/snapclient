#ifndef _WIFI_INTERFACE_H_
#define _WIFI_INTERFACE_H_

#include <stdbool.h>

#include "esp_netif.h"
#include "esp_wifi_types.h"

// use wifi provisioning
#define ENABLE_WIFI_PROVISIONING CONFIG_ENABLE_WIFI_PROVISIONING

/* Hardcoded WiFi configuration that you can set via
   'make menuconfig'.
*/
#if !ENABLE_WIFI_PROVISIONING
#define WIFI_SSID CONFIG_WIFI_SSID
#define WIFI_PASSWORD CONFIG_WIFI_PASSWORD
#endif

#define WIFI_MAXIMUM_RETRY CONFIG_WIFI_MAXIMUM_RETRY

bool wifi_get_ip(esp_netif_ip_info_t *ip);
bool wifi_get_ap_record(wifi_ap_record_t *ap);
/* true while associated over Wi-Fi with a power save mode for idle (not
 * suppressed for an Ethernet takeover, CONFIG_WIFI_PS_NONE_MODE not set) */
bool wifi_idle_power_save(void);
void wifi_start(void);

#endif /* _WIFI_INTERFACE_H_ */
