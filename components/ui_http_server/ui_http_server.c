/* HTTP Server Example

		 This example code is in the Public Domain (or CC0 licensed, at your
   option.)

		 Unless required by applicable law or agreed to in writing, this
		 software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
		 CONDITIONS OF ANY KIND, either express or implied.
*/

#include "ui_http_server.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <time.h>

#include "cJSON.h"
#include "esp_chip_info.h"
#include "dsp_processor_settings.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif_ip_addr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "network_interface.h"
#include "settings_manager.h"

#if CONFIG_DAC_TAS5805M
#include "tas5805m_settings.h"
#endif

static const char *TAG = "UI_HTTP";
static const time_t STATUS_VALID_TIME_THRESHOLD_UNIX = 1767225600;  // 2026-01-01 00:00:00 UTC

static QueueHandle_t xQueueHttp = NULL;
static TaskHandle_t taskHandle = NULL;
static httpd_handle_t server = NULL;

extern struct netconn *lwipNetconn;
extern const char *VERSION_STRING;
extern uint32_t connection_get_last_snapserver_connect_uptime_sec(void);
extern const char *connection_get_last_snapserver_host(void);
extern uint16_t connection_get_last_snapserver_port(void);

// External references to embedded files
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t index_js_start[] asm("_binary_index_js_start");
extern const uint8_t index_js_end[] asm("_binary_index_js_end");
extern const uint8_t settings_ui_js_start[] asm("_binary_settings_ui_js_start");
extern const uint8_t settings_ui_js_end[] asm("_binary_settings_ui_js_end");
extern const uint8_t styles_css_start[] asm("_binary_styles_css_start");
extern const uint8_t styles_css_end[] asm("_binary_styles_css_end");
extern const uint8_t general_settings_html_start[] asm("_binary_general_settings_html_start");
extern const uint8_t general_settings_html_end[] asm("_binary_general_settings_html_end");
extern const uint8_t dsp_settings_html_start[] asm("_binary_dsp_settings_html_start");
extern const uint8_t dsp_settings_html_end[] asm("_binary_dsp_settings_html_end");
extern const uint8_t dac_settings_html_start[] asm("_binary_dac_settings_html_start");
extern const uint8_t dac_settings_html_end[] asm("_binary_dac_settings_html_end");
extern const uint8_t eq_settings_html_start[] asm("_binary_eq_settings_html_start");
extern const uint8_t eq_settings_html_end[] asm("_binary_eq_settings_html_end");
extern const uint8_t favicon_ico_start[] asm("_binary_favicon_ico_start");
extern const uint8_t favicon_ico_end[] asm("_binary_favicon_ico_end");

// Structure to map URI paths to embedded files
typedef struct {
	const char *uri;
	const uint8_t *data_start;
	const uint8_t *data_end;
	const char *content_type;
} embedded_file_t;

static const embedded_file_t embedded_files[] = {
	{"/", index_html_start, index_html_end, "text/html; charset=utf-8"},
	{"/index.html", index_html_start, index_html_end, "text/html; charset=utf-8"},
	{"/index.js", index_js_start, index_js_end, "application/javascript; charset=utf-8"},
	{"/settings-ui.js", settings_ui_js_start, settings_ui_js_end, "application/javascript; charset=utf-8"},
	{"/styles.css", styles_css_start, styles_css_end, "text/css; charset=utf-8"},
	{"/general-settings.html", general_settings_html_start, general_settings_html_end, "text/html; charset=utf-8"},
	{"/dsp-settings.html", dsp_settings_html_start, dsp_settings_html_end, "text/html; charset=utf-8"},
	{"/dac-settings.html", dac_settings_html_start, dac_settings_html_end, "text/html; charset=utf-8"},
	{"/eq-settings.html", eq_settings_html_start, eq_settings_html_end, "text/html; charset=utf-8"},
	{"/favicon.ico", favicon_ico_start, favicon_ico_end, "image/x-icon"},
};

static void format_mac_address(char *out, size_t out_size, const uint8_t *mac) {
	if ((out == NULL) || (out_size < 18) || (mac == NULL)) {
		return;
	}

	snprintf(out, out_size, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
			 mac[2], mac[3], mac[4], mac[5]);
}

static esp_err_t build_status_json(char *json_out, size_t max_len) {
	cJSON *root = NULL;
	esp_netif_t *sta_netif = NULL;
	esp_netif_t *eth_netif = NULL;
	esp_netif_t *active_netif = NULL;
	esp_netif_ip_info_t ip_info = {0};
	wifi_ap_record_t ap_info = {0};
	esp_chip_info_t chip_info = {0};
	uint8_t mac[6] = {0};
	char mac_str[18] = {0};
	char hostname[64] = {0};
	char ip_str[16] = "Unavailable";
	char wifi_ssid[sizeof(ap_info.ssid) + 1] = "Disconnected";
	char configured_host[128] = {0};
	char snapserver_target[160] = "Not configured";
	char snapserver_host[128] = {0};
	char current_time_str[64] = "Not synchronized";
	char *json_string = NULL;
	bool mdns_enabled = true;
	bool time_synced = false;
	bool wifi_connected = false;
	bool snapserver_connected = (lwipNetconn != NULL);
	int32_t configured_port = 0;
	int wifi_rssi_dbm = 0;
	uint16_t snapserver_port = connection_get_last_snapserver_port();
	time_t now = 0;

	if ((json_out == NULL) || (max_len == 0)) {
		return ESP_ERR_INVALID_ARG;
	}

	root = cJSON_CreateObject();
	if (root == NULL) {
		return ESP_ERR_NO_MEM;
	}

	settings_get_hostname(hostname, sizeof(hostname));
	settings_get_mdns_enabled(&mdns_enabled);
	settings_get_server_host(configured_host, sizeof(configured_host));
	settings_get_server_port(&configured_port);
	esp_read_mac(mac, ESP_MAC_WIFI_STA);
	format_mac_address(mac_str, sizeof(mac_str), mac);
	esp_chip_info(&chip_info);

	sta_netif = network_get_netif_from_desc(NETWORK_INTERFACE_DESC_STA);
	eth_netif = network_get_netif_from_desc(NETWORK_INTERFACE_DESC_ETH);

	if ((eth_netif != NULL) && network_is_netif_up(eth_netif)) {
		active_netif = eth_netif;
	} else if ((sta_netif != NULL) && network_is_netif_up(sta_netif)) {
		active_netif = sta_netif;
	}

	if ((active_netif != NULL) &&
		(esp_netif_get_ip_info(active_netif, &ip_info) == ESP_OK)) {
		snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
	}

	if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
		snprintf(wifi_ssid, sizeof(wifi_ssid), "%s", (const char *)ap_info.ssid);
		wifi_rssi_dbm = ap_info.rssi;
		wifi_connected = true;
	}

	if (mdns_enabled) {
		snprintf(snapserver_target, sizeof(snapserver_target),
				 "_snapcast._tcp.local");
	} else if ((configured_host[0] != '\0') && (configured_port > 0)) {
		snprintf(snapserver_target, sizeof(snapserver_target), "%s:%" PRId32,
				 configured_host, configured_port);
	} else if (configured_host[0] != '\0') {
		snprintf(snapserver_target, sizeof(snapserver_target), "%s",
				 configured_host);
	}

	if ((connection_get_last_snapserver_host() != NULL) &&
		(connection_get_last_snapserver_host()[0] != '\0')) {
		strlcpy(snapserver_host, connection_get_last_snapserver_host(),
				sizeof(snapserver_host));
	} else if (configured_host[0] != '\0') {
		strlcpy(snapserver_host, configured_host, sizeof(snapserver_host));
	} else {
		strlcpy(snapserver_host, snapserver_target, sizeof(snapserver_host));
	}

	if ((snapserver_port == 0) && (configured_port > 0)) {
		snapserver_port = (uint16_t)configured_port;
	}

	time(&now);
	if (now >= STATUS_VALID_TIME_THRESHOLD_UNIX) {
		struct tm timeinfo = {0};
		if (localtime_r(&now, &timeinfo) != NULL) {
			if (strftime(current_time_str, sizeof(current_time_str),
						 "%Y-%m-%d %H:%M:%S %Z", &timeinfo) > 0) {
				time_synced = true;
			}
		}
	}

	cJSON_AddStringToObject(root, "app_version",
							(VERSION_STRING != NULL) ? VERSION_STRING : "unknown");
	cJSON_AddStringToObject(root, "target", CONFIG_IDF_TARGET);
	cJSON_AddNumberToObject(root, "chip_cores", chip_info.cores);
	cJSON_AddNumberToObject(root, "chip_revision", chip_info.revision);
	cJSON_AddStringToObject(root, "mac", mac_str);
	cJSON_AddStringToObject(root, "hostname",
							hostname[0] ? hostname : "esp32-snapclient");
	cJSON_AddBoolToObject(root, "time_synced", time_synced);
	cJSON_AddStringToObject(root, "current_time", current_time_str);
	cJSON_AddNumberToObject(root, "unix_time", (double)now);
	cJSON_AddNumberToObject(root, "uptime_sec",
							(double)(esp_timer_get_time() / 1000000ULL));
	cJSON_AddNumberToObject(root, "task_count", uxTaskGetNumberOfTasks());
	cJSON_AddNumberToObject(root, "free_heap_bytes", esp_get_free_heap_size());
	cJSON_AddNumberToObject(root, "min_free_heap_bytes",
							esp_get_minimum_free_heap_size());
	cJSON_AddNumberToObject(root, "largest_free_block_bytes",
							heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
	cJSON_AddNumberToObject(root, "free_internal_heap_bytes",
							heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
													 MALLOC_CAP_8BIT));
	cJSON_AddNumberToObject(root, "free_internal_dma_heap_bytes",
							heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
													 MALLOC_CAP_DMA));
#if CONFIG_SPIRAM
	cJSON_AddNumberToObject(root, "free_psram_bytes",
							heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
	cJSON_AddNumberToObject(root, "total_psram_bytes",
							heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
#else
	cJSON_AddNumberToObject(root, "free_psram_bytes", 0);
	cJSON_AddNumberToObject(root, "total_psram_bytes", 0);
#endif
	cJSON_AddNumberToObject(root, "last_snapserver_reconnect_uptime_sec",
							connection_get_last_snapserver_connect_uptime_sec());
	cJSON_AddStringToObject(root, "active_interface",
							(active_netif == eth_netif)
								? "ethernet"
								: ((active_netif == sta_netif) ? "wifi" : "offline"));
	cJSON_AddStringToObject(root, "ip_address", ip_str);
	cJSON_AddBoolToObject(root, "wifi_connected", wifi_connected);
	cJSON_AddStringToObject(root, "wifi_ssid", wifi_ssid);
	if (wifi_connected) {
		cJSON_AddNumberToObject(root, "wifi_rssi_dbm", wifi_rssi_dbm);
	} else {
		cJSON_AddNullToObject(root, "wifi_rssi_dbm");
	}
	cJSON_AddBoolToObject(root, "snapserver_connected", snapserver_connected);
	cJSON_AddStringToObject(root, "snapserver_state",
							snapserver_connected ? "Connected" : "Disconnected");
	cJSON_AddStringToObject(root, "discovery_mode",
							mdns_enabled ? "mDNS" : "Static");
	cJSON_AddStringToObject(root, "snapserver_target", snapserver_target);
	cJSON_AddStringToObject(root, "snapserver_host", snapserver_host);
	cJSON_AddNumberToObject(root, "snapserver_port", snapserver_port);
	cJSON_AddStringToObject(root, "configured_server_host",
							configured_host[0] ? configured_host : "");
	cJSON_AddNumberToObject(root, "configured_server_port", configured_port);

	json_string = cJSON_PrintUnformatted(root);
	if (json_string == NULL) {
		cJSON_Delete(root);
		return ESP_ERR_NO_MEM;
	}

	if (strlen(json_string) >= max_len) {
		free(json_string);
		cJSON_Delete(root);
		return ESP_ERR_NO_MEM;
	}

	strlcpy(json_out, json_string, max_len);
	free(json_string);
	cJSON_Delete(root);

	return ESP_OK;
}

#if CONFIG_HTTP_AUTH_ENABLE
static esp_err_t build_expected_auth_header(char *encoded,
											size_t encoded_size) {
	char credentials[128];
	size_t output_len = 0;
	int credentials_len =
		snprintf(credentials, sizeof(credentials), "%s:%s",
				 CONFIG_HTTP_AUTH_USERNAME, CONFIG_HTTP_AUTH_PASSWORD);

	if ((credentials_len < 0) || (credentials_len >= sizeof(credentials))) {
		ESP_LOGE(TAG, "%s: HTTP auth credentials are too long", __func__);
		return ESP_FAIL;
	}

	if (mbedtls_base64_encode((unsigned char *)encoded, encoded_size,
							  &output_len,
							  (const unsigned char *)credentials,
							  credentials_len) != 0) {
		ESP_LOGE(TAG, "%s: failed to encode HTTP auth credentials", __func__);
		return ESP_FAIL;
	}

	if (output_len >= encoded_size) {
		return ESP_FAIL;
	}

	encoded[output_len] = '\0';
	return ESP_OK;
}

static esp_err_t send_auth_challenge(httpd_req_t *req) {
	httpd_resp_set_status(req, "401 Unauthorized");
	httpd_resp_set_hdr(req, "WWW-Authenticate",
					   "Basic realm=\"snapclient\"");
	httpd_resp_sendstr(req, "Unauthorized");
	return ESP_ERR_INVALID_STATE;
}

static esp_err_t require_http_auth(httpd_req_t *req) {
	char header_value[192];
	char expected_value[192];
	int header_len = httpd_req_get_hdr_value_len(req, "Authorization");

	if (build_expected_auth_header(expected_value, sizeof(expected_value)) !=
		ESP_OK) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"Auth setup failed");
		return ESP_FAIL;
	}

	if ((header_len <= 0) || (header_len >= sizeof(header_value))) {
		return send_auth_challenge(req);
	}

	if (httpd_req_get_hdr_value_str(req, "Authorization", header_value,
									sizeof(header_value)) != ESP_OK) {
		return send_auth_challenge(req);
	}

	if (strncmp(header_value, "Basic ", strlen("Basic ")) != 0) {
		return send_auth_challenge(req);
	}

	if (strcmp(header_value + strlen("Basic "), expected_value) != 0) {
		return send_auth_challenge(req);
	}

	return ESP_OK;
}
#else
static esp_err_t require_http_auth(httpd_req_t *req) {
	(void)req;
	return ESP_OK;
}
#endif

/**
 * Simple URL decode function
 * Decodes %XX hex sequences and + as space
 */
static void url_decode(char *dst, const char *src, size_t dst_size) {
	size_t dst_idx = 0;
	size_t src_idx = 0;

	while (src[src_idx] != '\0' && dst_idx < dst_size - 1) {
		if (src[src_idx] == '%' && src[src_idx + 1] != '\0' &&
			src[src_idx + 2] != '\0') {
			// Decode %XX
			char hex[3] = {src[src_idx + 1], src[src_idx + 2], '\0'};
			dst[dst_idx++] = (char)strtol(hex, NULL, 16);
			src_idx += 3;
		} else if (src[src_idx] == '+') {
			// Convert + to space
			dst[dst_idx++] = ' ';
			src_idx++;
		} else {
			dst[dst_idx++] = src[src_idx++];
		}
	}
	dst[dst_idx] = '\0';
}

/**
 * Find key value in parameter string
 */
static int find_key_value(char *key, char *parameter, char *value) {
	ESP_LOGD(TAG, "%s: key=%s", __func__, key);
	// char * addr1;
	char *addr1 = strstr(parameter, key);
	if (addr1 == NULL)
		return 0;
	ESP_LOGD(TAG, "%s: addr1=%s", __func__, addr1);

	char *addr2 = addr1 + strlen(key);
	ESP_LOGD(TAG, "%s: addr2=[%s]", __func__, addr2);

	char *addr3 = strstr(addr2, "&");
	ESP_LOGD(TAG, "%s: addr3=%p", __func__, addr3);
	if (addr3 == NULL) {
		strcpy(value, addr2);
	} else {
		int length = addr3 - addr2;
		ESP_LOGD(TAG, "%s: addr2=%p addr3=%p length=%d", __func__, addr2, addr3,
				 length);
		strncpy(value, addr2, length);
		value[length] = 0;
	}
	ESP_LOGD(TAG, "%s: key=[%s] value=[%s]", __func__, key, value);
	return strlen(value);
}

/**
 * Set CORS headers to allow cross-origin requests
 * This enables local development with ?backend parameter
 */
static void set_cors_headers(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Methods",
					   "GET, POST, DELETE, OPTIONS");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
	httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
}

/**
 * HTTP get handler - serves index.html from embedded files
 */
static esp_err_t root_get_handler(httpd_req_t *req) {
	ESP_LOGD(TAG, "%s: uri=%s", __func__, req->uri);

	if (require_http_auth(req) != ESP_OK) {
		return ESP_OK;
	}

	set_cors_headers(req);
	httpd_resp_set_type(req, "text/html; charset=utf-8");

	/* Send index.html from embedded data (subtract 1 for null terminator) */
	size_t index_size = index_html_end - index_html_start - 1;
	httpd_resp_send(req, (const char *)index_html_start, index_size);

	return ESP_OK;
}

/*
 * HTTP post handler
 * Expects a single parameter change in the query string:
 * /post?param=NAME&value=INT
 */
static esp_err_t root_post_handler(httpd_req_t *req) {
	ESP_LOGD(TAG, "%s: uri=%s", __func__, req->uri);
	URL_t urlBuf;
	int ret = -1;
	char param[16] = {0};
	char valstr[64] = {0}; // Increased size for hostname

	if (require_http_auth(req) != ESP_OK) {
		return ESP_OK;
	}

	set_cors_headers(req);

	memset(&urlBuf, 0, sizeof(URL_t));

	if (find_key_value("param=", (char *)req->uri, param) &&
		find_key_value("value=", (char *)req->uri, valstr)) {

		// Special handling for hostname (string parameter)
		if (strcmp(param, "hostname") == 0) {
			// URL decode the hostname value
			char decoded_hostname[64] = {0};
			url_decode(decoded_hostname, valstr, sizeof(decoded_hostname));

			ESP_LOGI(TAG, "%s: Setting hostname to: %s", __func__,
					 decoded_hostname);

			if (settings_set_hostname(decoded_hostname) == ESP_OK) {
				httpd_resp_set_status(req, "200 OK");
				httpd_resp_sendstr(req, "ok");
			} else {
				httpd_resp_set_status(req, "400 Bad Request");
				httpd_resp_sendstr(req, "Invalid hostname");
			}
			return ESP_OK;
		}

		// Special handling for snapserver host (string parameter)
		if (strcmp(param, "snapserver_host") == 0) {
			char decoded_host[128] = {0};
			url_decode(decoded_host, valstr, sizeof(decoded_host));
			ESP_LOGI(TAG, "%s: Setting snapserver_host to: %s", __func__,
					 decoded_host);
			if (settings_set_server_host(decoded_host) == ESP_OK) {
				httpd_resp_set_status(req, "200 OK");
				httpd_resp_sendstr(req, "ok");
			} else {
				httpd_resp_set_status(req, "500 Internal Server Error");
				httpd_resp_sendstr(req, "error");
			}
			return ESP_OK;
		}

		// Special handling for snapserver_use_mdns (boolean/integer)
		if (strcmp(param, "snapserver_use_mdns") == 0) {
			long v = strtol(valstr, NULL, 10);
			ESP_LOGI(TAG, "%s: Setting snapserver_use_mdns to: %ld", __func__,
					 v);

			if (settings_set_mdns_enabled(v != 0) == ESP_OK) {
				httpd_resp_set_status(req, "200 OK");
				httpd_resp_sendstr(req, "ok");
			} else {
				httpd_resp_set_status(req, "500 Internal Server Error");
				httpd_resp_sendstr(req, "error");
			}
			return ESP_OK;
		}

		// Special handling for snapserver_port (integer)
		if (strcmp(param, "snapserver_port") == 0) {
			long v = strtol(valstr, NULL, 10);
			ESP_LOGI(TAG, "%s: Setting snapserver_port to: %ld", __func__, v);
			if (settings_set_server_port((int32_t)v) == ESP_OK) {
				httpd_resp_set_status(req, "200 OK");
				httpd_resp_sendstr(req, "ok");
			} else {
				httpd_resp_set_status(req, "500 Internal Server Error");
				httpd_resp_sendstr(req, "error");
			}
			return ESP_OK;
		}

		// Parse integer value; strtol skips leading whitespace
		long v = strtol(valstr, NULL, 10);
		urlBuf.int_value = (int32_t)v;
		snprintf(urlBuf.key, sizeof(urlBuf.key), "%s", param);
		ret = 0;
		ESP_LOGD(TAG, "%s: Received param=%s value=%ld", __func__, urlBuf.key,
			 (long)urlBuf.int_value);
	} else {
		ESP_LOGD(TAG, "%s: Invalid post: expected param=NAME&value=INT in URI",
				 __func__);
	}

	if (ret >= 0) {
		// Send to http_server_task with timeout to prevent handler from
		// blocking indefinitely
		if (xQueueSend(xQueueHttp, &urlBuf, pdMS_TO_TICKS(1000)) != pdPASS) {
			ESP_LOGE(TAG, "%s: xQueueSend Fail (queue full or timeout)",
					 __func__);
			httpd_resp_set_status(req, "503 Service Unavailable");
			httpd_resp_sendstr(req, "Queue full, try again");
			return ESP_OK;
		}
	}

	httpd_resp_set_status(req, "200 OK");
	httpd_resp_sendstr(req, "ok");
	return ESP_OK;
}

/*
 * HTTP DELETE handler
 * Clears a parameter from NVS: /delete?param=NAME
 */
static esp_err_t root_delete_handler(httpd_req_t *req) {
	ESP_LOGD(TAG, "%s: uri=%s", __func__, req->uri);
	char param[32] = {0};

	if (require_http_auth(req) != ESP_OK) {
		return ESP_OK;
	}

	set_cors_headers(req);

	if (!find_key_value("param=", (char *)req->uri, param)) {
		ESP_LOGD(TAG, "%s: Invalid delete: expected param=NAME in URI",
				 __func__);
		httpd_resp_set_status(req, "400 Bad Request");
		httpd_resp_sendstr(req, "Missing param");
		return ESP_OK;
	}

	// Handle hostname clear
	if (strcmp(param, "hostname") == 0) {
		ESP_LOGI(TAG, "%s: Clearing hostname from NVS", __func__);
		if (settings_clear_hostname() == ESP_OK) {
			httpd_resp_set_status(req, "200 OK");
			httpd_resp_sendstr(req, "ok");
		} else {
			httpd_resp_set_status(req, "500 Internal Server Error");
			httpd_resp_sendstr(req, "error");
		}
		return ESP_OK;
	}

	// Handle snapserver_use_mdns clear
	if (strcmp(param, "snapserver_use_mdns") == 0) {
		ESP_LOGI(TAG, "%s: Clearing snapserver_use_mdns from NVS", __func__);
		if (settings_clear_mdns_enabled() == ESP_OK) {
			httpd_resp_set_status(req, "200 OK");
			httpd_resp_sendstr(req, "ok");
		} else {
			httpd_resp_set_status(req, "500 Internal Server Error");
			httpd_resp_sendstr(req, "error");
		}
		return ESP_OK;
	}

	// Handle snapserver_host clear
	if (strcmp(param, "snapserver_host") == 0) {
		ESP_LOGI(TAG, "%s: Clearing snapserver_host from NVS", __func__);
		if (settings_clear_server_host() == ESP_OK) {
			httpd_resp_set_status(req, "200 OK");
			httpd_resp_sendstr(req, "ok");
		} else {
			httpd_resp_set_status(req, "500 Internal Server Error");
			httpd_resp_sendstr(req, "error");
		}
		return ESP_OK;
	}

	// Handle snapserver_port clear
	if (strcmp(param, "snapserver_port") == 0) {
		ESP_LOGI(TAG, "%s: Clearing snapserver_port from NVS", __func__);
		if (settings_clear_server_port() == ESP_OK) {
			httpd_resp_set_status(req, "200 OK");
			httpd_resp_sendstr(req, "ok");
		} else {
			httpd_resp_set_status(req, "500 Internal Server Error");
			httpd_resp_sendstr(req, "error");
		}
		return ESP_OK;
	}

	// Unknown parameter
	httpd_resp_set_status(req, "400 Bad Request");
	httpd_resp_sendstr(req, "Unknown parameter");
	return ESP_OK;
}

/*
 * GET parameter handler
 * Returns current parameter value: /get?param=NAME
 * Response format: plain text integer value
 *
 * This reads from the DSP processor's centralized storage for the active flow
 */
static esp_err_t get_param_handler(httpd_req_t *req) {
	ESP_LOGD(TAG, "%s: uri=%s", __func__, req->uri);
	char param[16] = {0};

	if (require_http_auth(req) != ESP_OK) {
		return ESP_OK;
	}

	set_cors_headers(req);

	if (find_key_value("param=", (char *)req->uri, param)) {
		// Special handling for hostname (string parameter)
		if (strcmp(param, "hostname") == 0) {
			char hostname[64] = {0};
			if (settings_get_hostname(hostname, sizeof(hostname)) == ESP_OK) {
				httpd_resp_set_status(req, "200 OK");
				httpd_resp_set_type(req, "text/plain");
				httpd_resp_sendstr(req, hostname);
				ESP_LOGD(TAG, "%s: hostname=%s", __func__, hostname);
			} else {
				httpd_resp_set_status(req, "500 Internal Server Error");
				httpd_resp_sendstr(req, "error");
			}
			return ESP_OK;
		}

		if (strcmp(param, "snapserver_use_mdns") == 0) {
			bool enabled = true;
			if (settings_get_mdns_enabled(&enabled) == ESP_OK) {
				char resp[8];
				snprintf(resp, sizeof(resp), "%d", enabled ? 1 : 0);
				httpd_resp_set_status(req, "200 OK");
				httpd_resp_set_type(req, "text/plain");
				httpd_resp_sendstr(req, resp);
				ESP_LOGD(TAG, "%s: snapserver_use_mdns=%d", __func__,
						 enabled ? 1 : 0);
			} else {
				httpd_resp_set_status(req, "200 OK");
				httpd_resp_set_type(req, "text/plain");
				httpd_resp_sendstr(req, "1");
				ESP_LOGD(
					TAG,
					"%s: snapserver_use_mdns not found, returning default 1",
					__func__);
			}
			return ESP_OK;
		}

		if (strcmp(param, "snapserver_host") == 0) {
			char host[128] = {0};
			if (settings_get_server_host(host, sizeof(host)) == ESP_OK) {
				httpd_resp_set_status(req, "200 OK");
				httpd_resp_set_type(req, "text/plain");
				httpd_resp_sendstr(req, host);
				ESP_LOGD(TAG, "%s: snapserver_host=%s", __func__, host);
			} else {
				httpd_resp_set_status(req, "200 OK");
				httpd_resp_set_type(req, "text/plain");
				httpd_resp_sendstr(req, "");
				ESP_LOGD(TAG, "%s: snapserver_host not found, returning empty",
						 __func__);
			}
			return ESP_OK;
		}

		if (strcmp(param, "snapserver_port") == 0) {
			int32_t port = 0;
			if (settings_get_server_port(&port) == ESP_OK && port != 0) {
				char resp[16];
				snprintf(resp, sizeof(resp), "%d", (int)port);
				httpd_resp_set_status(req, "200 OK");
				httpd_resp_set_type(req, "text/plain");
				httpd_resp_sendstr(req, resp);
				ESP_LOGD(TAG, "%s: snapserver_port=%d", __func__, (int)port);
			} else {
				httpd_resp_set_status(req, "200 OK");
				httpd_resp_set_type(req, "text/plain");
				httpd_resp_sendstr(req, "");
				ESP_LOGD(TAG, "%s: snapserver_port not found, returning empty",
						 __func__);
			}
			return ESP_OK;
	}

#if CONFIG_USE_DSP_PROCESSOR
	// Get current flow from settings
	dspFlows_t current_flow = dsp_settings_get_active_flow();

	// Get parameters for current flow
	filterParams_t params;
	if (dsp_settings_get_flow_params(current_flow, &params) == ESP_OK) {
		int32_t value = 0;			// Map parameter name to value
			if (strcmp(param, "fc_1") == 0) {
				value = (int32_t)params.fc_1;
			} else if (strcmp(param, "gain_1") == 0) {
				value = (int32_t)params.gain_1;
			} else if (strcmp(param, "fc_3") == 0) {
				value = (int32_t)params.fc_3;
			} else if (strcmp(param, "gain_3") == 0) {
				value = (int32_t)params.gain_3;
			} else {
				httpd_resp_set_status(req, "400 Bad Request");
				httpd_resp_sendstr(req, "Unknown parameter");
				return ESP_OK;
			}

			char response[32];
			snprintf(response, sizeof(response), "%d", (int)value);
			httpd_resp_set_status(req, "200 OK");
			httpd_resp_set_type(req, "text/plain");
			httpd_resp_sendstr(req, response);
			ESP_LOGD(TAG, "%s: flow=%d %s=%d", __func__, current_flow, param,
					 (int)value);
		} else {
			httpd_resp_set_status(req, "500 Internal Server Error");
			httpd_resp_sendstr(req, "0");
		}
#else
		// Fallback: load from NVS using dsp_settings
		dspFlows_t current_flow = dspfStereo;
		if (dsp_settings_load_active_flow(&current_flow) != ESP_OK) {
			current_flow = dspfStereo; // default
		}

		int32_t value = 0;
		if (dsp_settings_load_flow_param(current_flow, param, &value) ==
			ESP_OK) {
			char response[32];
			snprintf(response, sizeof(response), "%d", (int)value);
			httpd_resp_set_status(req, "200 OK");
			httpd_resp_set_type(req, "text/plain");
			httpd_resp_sendstr(req, response);
			ESP_LOGD(TAG, "%s: flow=%d %s=%d", __func__, current_flow, param,
					 (int)value);
		} else {
			httpd_resp_set_status(req, "404 Not Found");
			httpd_resp_sendstr(req, "0");
			ESP_LOGD(TAG, "%s: flow=%d %s not found, returning 0", __func__,
					 current_flow, param);
		}
#endif
	} else {
		httpd_resp_set_status(req, "400 Bad Request");
		httpd_resp_sendstr(req, "error");
	}
	return ESP_OK;
}

/*
 * GET capabilities handler
 * Returns settings based on the 'tab' parameter: /capabilities?tab=general,
 * /capabilities?tab=status or /capabilities?tab=dsp
 *
 * Response for tab=general:
 * - hostname, mdns_enabled, server_host, server_port
 *
 * Response for tab=status:
 * - runtime device/network/snapserver status
 *
 * Response for tab=dsp (if DSP enabled):
 * - active_flow and all flow parameters
 */
static esp_err_t get_capabilities_handler(httpd_req_t *req) {
	ESP_LOGD(TAG, "%s: uri=%s", __func__, req->uri);

	if (require_http_auth(req) != ESP_OK) {
		return ESP_OK;
	}

	set_cors_headers(req);

	// Parse tab parameter
	char tab[16] = {0};
	if (!find_key_value("tab=", (char *)req->uri, tab)) {
		// No tab specified, return error
		ESP_LOGW(TAG, "%s: Missing 'tab' parameter", __func__);
		httpd_resp_set_status(req, "400 Bad Request");
		httpd_resp_sendstr(req, "{\"error\": \"Missing 'tab' parameter. Use "
								"?tab=general, ?tab=status or ?tab=dsp\"}");
		return ESP_OK;
	}

	ESP_LOGI(TAG, "%s: Requested tab: %s", __func__, tab);

	if (strcmp(tab, "general") == 0) {
		// Return general settings
		char general_json[512] = {0};
		esp_err_t ret = settings_get_json(general_json, sizeof(general_json));

		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "%s: Failed to get general settings JSON: %s",
					 __func__, esp_err_to_name(ret));
			httpd_resp_set_status(req, "500 Internal Server Error");
			httpd_resp_sendstr(
				req, "{\"error\": \"Failed to retrieve general settings\"}");
			return ESP_OK;
		}

		httpd_resp_set_status(req, "200 OK");
		httpd_resp_set_type(req, "application/json");
		httpd_resp_sendstr(req, general_json);

	} else if (strcmp(tab, "status") == 0) {
		char *status_json = (char *)calloc(1, 2048);
		if (status_json == NULL) {
			httpd_resp_set_status(req, "500 Internal Server Error");
			httpd_resp_sendstr(req, "{\"error\": \"Memory allocation failed\"}");
			return ESP_OK;
		}

		esp_err_t ret = build_status_json(status_json, 2048);

		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "%s: Failed to get status JSON: %s", __func__,
					 esp_err_to_name(ret));
			free(status_json);
			httpd_resp_set_status(req, "500 Internal Server Error");
			httpd_resp_sendstr(req,
							   "{\"error\": \"Failed to retrieve device status\"}");
			return ESP_OK;
		}

		httpd_resp_set_status(req, "200 OK");
		httpd_resp_set_type(req, "application/json");
		httpd_resp_sendstr(req, status_json);
		free(status_json);

	} else if (strcmp(tab, "dsp") == 0) {
#if CONFIG_USE_DSP_PROCESSOR
		// Return DSP settings - allocate larger buffer for schema + values
		char *dsp_json = (char *)malloc(4096);
		if (!dsp_json) {
			ESP_LOGE(TAG, "%s: Failed to allocate memory for DSP JSON",
					 __func__);
			httpd_resp_set_status(req, "500 Internal Server Error");
			httpd_resp_sendstr(req,
							   "{\"error\": \"Memory allocation failed\"}");
			return ESP_OK;
		}

		esp_err_t ret = dsp_settings_get_json(dsp_json, 4096);

		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "%s: Failed to get DSP settings JSON: %s", __func__,
					 esp_err_to_name(ret));
			free(dsp_json);
			httpd_resp_set_status(req, "500 Internal Server Error");
			httpd_resp_sendstr(
				req, "{\"error\": \"Failed to retrieve DSP settings\"}");
			return ESP_OK;
		}

		httpd_resp_set_status(req, "200 OK");
		httpd_resp_set_type(req, "application/json");
		httpd_resp_sendstr(req, dsp_json);
		free(dsp_json);
#else
		// DSP not enabled
		httpd_resp_set_status(req, "200 OK");
		httpd_resp_set_type(req, "application/json");
		httpd_resp_sendstr(req, "{\"dsp_enabled\": false}");
#endif

	} else {
		// Unknown tab
		ESP_LOGW(TAG, "%s: Unknown tab: %s", __func__, tab);
		httpd_resp_set_status(req, "400 Bad Request");
		httpd_resp_sendstr(
			req,
			"{\"error\": \"Unknown tab. Use ?tab=general, ?tab=status or ?tab=dsp\"}");
	}

	return ESP_OK;
}

/*
 * favicon get handler
 * Returns 404 since we don't have a favicon
 */
static esp_err_t favicon_get_handler(httpd_req_t *req) {
	ESP_LOGD(TAG, "%s: uri=%s", __func__, req->uri);

	if (require_http_auth(req) != ESP_OK) {
		return ESP_OK;
	}

	set_cors_headers(req);
	httpd_resp_set_status(req, "404 Not Found");
	httpd_resp_set_type(req, "text/plain");
	httpd_resp_sendstr(req, "No favicon available");
	return ESP_OK;
}

/* Restart handler: responds OK and schedules a restart shortly after */
static void restart_task(void *pv) {
	// give HTTP stack time to finish sending response
	vTaskDelay(pdMS_TO_TICKS(200));
	ESP_LOGI(TAG, "restart_task: calling esp_restart()");
	esp_restart();
	vTaskDelete(NULL);
}

static esp_err_t restart_post_handler(httpd_req_t *req) {
	ESP_LOGD(TAG, "%s: uri=%s", __func__, req->uri);

	if (require_http_auth(req) != ESP_OK) {
		return ESP_OK;
	}

	set_cors_headers(req);

	// Send immediate response before restarting
	httpd_resp_set_status(req, "200 OK");
	httpd_resp_sendstr(req, "restarting");

	// Spawn a task that will restart the chip after a short delay
	BaseType_t ok = xTaskCreate(restart_task, "restart_task", 2048, NULL, 5, NULL);
	if (ok != pdPASS) {
		ESP_LOGW(TAG, "%s: Failed to create restart task", __func__);
	}

	return ESP_OK;
}

/*
 * GET /api/dac/settings handler
 * Returns current TAS5805M DAC settings as JSON
 */
static esp_err_t get_dac_settings_handler(httpd_req_t *req) {
  ESP_LOGD(TAG, "%s: uri=%s", __func__, req->uri);

  if (require_http_auth(req) != ESP_OK) {
    return ESP_OK;
  }

  set_cors_headers(req);
  
#if CONFIG_DAC_TAS5805M
  char *dac_json = (char *)malloc(1024);
  if (!dac_json) {
    ESP_LOGE(TAG, "%s: Failed to allocate memory for DAC JSON", __func__);
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"error\": \"Memory allocation failed\"}");
    return ESP_OK;
  }
  
  esp_err_t ret = tas5805m_settings_get_dac_json(dac_json, 1024);
  
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "%s: Failed to get DAC settings JSON: %s", __func__, esp_err_to_name(ret));
    free(dac_json);
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"error\": \"Failed to retrieve DAC settings\"}");
    return ESP_OK;
  }
  
  httpd_resp_set_status(req, "200 OK");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, dac_json);
  free(dac_json);
  
  return ESP_OK;
#else
  httpd_resp_set_status(req, "404 Not Found");
  httpd_resp_sendstr(req, "{\"error\": \"TAS5805M not configured\"}");
  return ESP_OK;
#endif
}

/*
 * GET /api/dac/schema handler
 * Returns TAS5805M DAC settings schema as JSON
 */
static esp_err_t get_dac_schema_handler(httpd_req_t *req) {
  ESP_LOGD(TAG, "%s: uri=%s", __func__, req->uri);

  if (require_http_auth(req) != ESP_OK) {
    return ESP_OK;
  }

  set_cors_headers(req);
  
#if CONFIG_DAC_TAS5805M
		/* Allocate schema buffer size conditionally: large buffer only if EQ support enabled */
	#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)
		/* EQ adds many parameters to the schema; increase buffer slightly to avoid overflow */
		const size_t schema_buf_size = 36 * 1024; /* 12 KiB */
	#else
		const size_t schema_buf_size = 3 * 1024; /* 3 KiB */
	#endif

		char *schema_json = (char *)malloc(schema_buf_size);
		if (!schema_json) {
			ESP_LOGE(TAG, "%s: Failed to allocate memory for DAC schema JSON (size=%zu)", __func__, schema_buf_size);
			httpd_resp_set_status(req, "500 Internal Server Error");
			httpd_resp_sendstr(req, "{\"error\": \"Memory allocation failed\"}");
			return ESP_OK;
		}

		/* Use DAC-only schema generator to avoid including EQ parameters */
		esp_err_t ret = tas5805m_settings_get_dac_schema_json(schema_json, schema_buf_size);
  
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "%s: Failed to get DAC schema JSON: %s", __func__, esp_err_to_name(ret));
    free(schema_json);
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"error\": \"Failed to retrieve DAC schema\"}");
    return ESP_OK;
  }
  
  httpd_resp_set_status(req, "200 OK");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, schema_json);
  free(schema_json);
  
  return ESP_OK;
#else
  httpd_resp_set_status(req, "404 Not Found");
  httpd_resp_sendstr(req, "{\"error\": \"TAS5805M not configured\"}");
  return ESP_OK;
#endif
}

/*
 * POST /api/dac/settings handler
 * Updates TAS5805M DAC settings from JSON
 */
static esp_err_t post_dac_settings_handler(httpd_req_t *req) {
  ESP_LOGD(TAG, "%s: uri=%s", __func__, req->uri);

  if (require_http_auth(req) != ESP_OK) {
    return ESP_OK;
  }

  set_cors_headers(req);
  
#if CONFIG_DAC_TAS5805M
  // Allocate buffer for request body
  char *buf = (char *)malloc(req->content_len + 1);
  if (!buf) {
    ESP_LOGE(TAG, "%s: Failed to allocate buffer for request body", __func__);
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"error\": \"Memory allocation failed\"}");
    return ESP_OK;
  }
  
  // Read request body
  int ret = httpd_req_recv(req, buf, req->content_len);
  if (ret <= 0) {
    free(buf);
    if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
      httpd_resp_set_status(req, "408 Request Timeout");
      httpd_resp_sendstr(req, "{\"error\": \"Request timeout\"}");
    } else {
      httpd_resp_set_status(req, "500 Internal Server Error");
      httpd_resp_sendstr(req, "{\"error\": \"Failed to read request body\"}");
    }
    return ESP_OK;
  }
  buf[ret] = '\0';
  
  ESP_LOGI(TAG, "%s: Received JSON: %s", __func__, buf);
  
  // Update DAC-only settings
  esp_err_t err = tas5805m_settings_set_dac_from_json(buf);
  free(buf);
  
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "%s: Failed to update DAC settings: %s", __func__, esp_err_to_name(err));
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"error\": \"Failed to update DAC settings\"}");
    return ESP_OK;
  }
  
  httpd_resp_set_status(req, "200 OK");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"success\": true}");
  
  return ESP_OK;
#else
  httpd_resp_set_status(req, "404 Not Found");
  httpd_resp_sendstr(req, "{\"error\": \"TAS5805M not configured\"}");
  return ESP_OK;
#endif
}

/*
 * GET /api/eq/settings handler
 * Returns current TAS5805M EQ settings as JSON
 */
static esp_err_t get_eq_settings_handler(httpd_req_t *req) {
  ESP_LOGD(TAG, "%s: uri=%s", __func__, req->uri);

  if (require_http_auth(req) != ESP_OK) {
    return ESP_OK;
  }

  set_cors_headers(req);
  
#if CONFIG_DAC_TAS5805M
  char *eq_json = (char *)malloc(16 * 1024); // 16KB for EQ settings
  if (!eq_json) {
    ESP_LOGE(TAG, "%s: Failed to allocate memory for EQ JSON", __func__);
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"error\": \"Memory allocation failed\"}");
    return ESP_OK;
  }
  
  esp_err_t ret = tas5805m_settings_get_eq_json(eq_json, 16 * 1024);
  
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "%s: Failed to get EQ settings JSON: %s", __func__, esp_err_to_name(ret));
    free(eq_json);
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"error\": \"Failed to retrieve EQ settings\"}");
    return ESP_OK;
  }
  
  httpd_resp_set_status(req, "200 OK");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, eq_json);
  free(eq_json);
  
  return ESP_OK;
#else
  httpd_resp_set_status(req, "404 Not Found");
  httpd_resp_sendstr(req, "{\"error\": \"TAS5805M not configured\"}");
  return ESP_OK;
#endif
}

/*
 * GET /api/eq/schema handler
 * Returns TAS5805M EQ settings schema as JSON
 */
static esp_err_t get_eq_schema_handler(httpd_req_t *req) {
  ESP_LOGD(TAG, "%s: uri=%s", __func__, req->uri);

  if (require_http_auth(req) != ESP_OK) {
    return ESP_OK;
  }

  set_cors_headers(req);
  
#if CONFIG_DAC_TAS5805M
  const size_t schema_buf_size = 64 * 1024; // 64KB for EQ schema
  
  char *schema_json = (char *)malloc(schema_buf_size);
  if (!schema_json) {
    ESP_LOGE(TAG, "%s: Failed to allocate memory for EQ schema JSON (size=%zu)", __func__, schema_buf_size);
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"error\": \"Memory allocation failed\"}");
    return ESP_OK;
  }

  esp_err_t ret = tas5805m_settings_get_eq_schema_json(schema_json, schema_buf_size);
  
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "%s: Failed to get EQ schema JSON: %s", __func__, esp_err_to_name(ret));
    free(schema_json);
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"error\": \"Failed to retrieve EQ schema\"}");
    return ESP_OK;
  }
  
  httpd_resp_set_status(req, "200 OK");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, schema_json);
  free(schema_json);
  
  return ESP_OK;
#else
  httpd_resp_set_status(req, "404 Not Found");
  httpd_resp_sendstr(req, "{\"error\": \"TAS5805M not configured\"}");
  return ESP_OK;
#endif
}

/*
 * POST /api/eq/settings handler
 * Updates TAS5805M EQ settings from JSON
 */
static esp_err_t post_eq_settings_handler(httpd_req_t *req) {
  ESP_LOGD(TAG, "%s: uri=%s", __func__, req->uri);

  if (require_http_auth(req) != ESP_OK) {
    return ESP_OK;
  }

  set_cors_headers(req);
  
#if CONFIG_DAC_TAS5805M
  // Allocate buffer for request body
  char *buf = (char *)malloc(req->content_len + 1);
  if (!buf) {
    ESP_LOGE(TAG, "%s: Failed to allocate buffer for request body", __func__);
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"error\": \"Memory allocation failed\"}");
    return ESP_OK;
  }
  
  // Read request body
  int ret = httpd_req_recv(req, buf, req->content_len);
  if (ret <= 0) {
    free(buf);
    if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
      httpd_resp_set_status(req, "408 Request Timeout");
      httpd_resp_sendstr(req, "{\"error\": \"Request timeout\"}");
    } else {
      httpd_resp_set_status(req, "500 Internal Server Error");
      httpd_resp_sendstr(req, "{\"error\": \"Failed to read request body\"}");
    }
    return ESP_OK;
  }
  buf[ret] = '\0';
  
  ESP_LOGI(TAG, "%s: Received JSON: %s", __func__, buf);
  
  // Update EQ settings
  esp_err_t err = tas5805m_settings_set_eq_from_json(buf);
  free(buf);
  
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "%s: Failed to update EQ settings: %s", __func__, esp_err_to_name(err));
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"error\": \"Failed to update EQ settings\"}");
    return ESP_OK;
  }
  
  httpd_resp_set_status(req, "200 OK");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"success\": true}");
  
  return ESP_OK;
#else
  httpd_resp_set_status(req, "404 Not Found");
  httpd_resp_sendstr(req, "{\"error\": \"TAS5805M not configured\"}");
  return ESP_OK;
#endif
}

/*
 * Static file handler
 * Serves files from embedded flash memory
 */
static esp_err_t static_file_handler(httpd_req_t *req) {
	ESP_LOGD(TAG, "%s: uri=%s", __func__, req->uri);

	if (require_http_auth(req) != ESP_OK) {
		return ESP_OK;
	}

	set_cors_headers(req);

	// Search for the requested file in embedded files
	for (size_t i = 0; i < sizeof(embedded_files) / sizeof(embedded_file_t); i++) {
		if (strcmp(req->uri, embedded_files[i].uri) == 0) {
			// Found the file
			size_t file_size = embedded_files[i].data_end - embedded_files[i].data_start;
			
			// EMBED_TXTFILES adds a null terminator, but we shouldn't send it
			// Only subtract for text files (not binary like favicon)
			if (strstr(embedded_files[i].content_type, "text/") != NULL ||
			    strstr(embedded_files[i].content_type, "application/javascript") != NULL) {
				file_size--;
			}
			
			ESP_LOGD(TAG, "%s: Serving %s (%d bytes)", __func__, req->uri, file_size);
			
			httpd_resp_set_type(req, embedded_files[i].content_type);
			httpd_resp_send(req, (const char *)embedded_files[i].data_start, file_size);
			
			return ESP_OK;
		}
	}

	// File not found
	ESP_LOGW(TAG, "%s: File not found: %s", __func__, req->uri);
	httpd_resp_set_status(req, "404 Not Found");
	httpd_resp_sendstr(req, "File not found");
	return ESP_OK;
}

/*
 * OPTIONS handler for CORS preflight requests
 */
static esp_err_t options_handler(httpd_req_t *req) {
	ESP_LOGD(TAG, "%s: uri=%s", __func__, req->uri);
	set_cors_headers(req);
	httpd_resp_set_status(req, "204 No Content");
	httpd_resp_send(req, NULL, 0);
	return ESP_OK;
}

/**
 */
esp_err_t stop_server(void) {
	ESP_LOGD(TAG, "%s", __func__);
	if (server) {
		httpd_stop(server);
		server = NULL;
	}

	return ESP_OK;
}

/*
 * Function to start the web server
 */
esp_err_t start_server(const char *base_path, int port) {
	ESP_LOGD(TAG, "%s: base_path=%s port=%d", __func__, base_path, port);
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.server_port = port;
	config.stack_size = 8192;
	config.max_req_hdr_len = 2048;
	config.max_uri_len = 1024;
	config.max_open_sockets = 7;
	config.max_uri_handlers = 64;
	config.lru_purge_enable = true; // Enable LRU socket purging

	/* Enable wildcard URI matching for static file handler */
	config.uri_match_fn = httpd_uri_match_wildcard;

	ESP_LOGI(TAG, "%s: Starting HTTP Server on port: '%d'", __func__,
			 config.server_port);
	if (httpd_start(&server, &config) != ESP_OK) {
		ESP_LOGE(TAG, "%s: Failed to start file server!", __func__);
		return ESP_FAIL;
	}

	/* URI handler for get */
	httpd_uri_t _root_get_handler = {
		.uri = "/",
		.method = HTTP_GET,
		.handler = root_get_handler,
		//.user_ctx  = server_data	// Pass server data as context
	};
	httpd_register_uri_handler(server, &_root_get_handler);

	/* URI handler for post */
	httpd_uri_t _root_post_handler = {
		.uri = "/post",
		.method = HTTP_POST,
		.handler = root_post_handler,
		//.user_ctx  = server_data	// Pass server data as context
	};
	httpd_register_uri_handler(server, &_root_post_handler);

	/* URI handler for delete */
	httpd_uri_t _root_delete_handler = {
		.uri = "/delete",
		.method = HTTP_DELETE,
		.handler = root_delete_handler,
	};
	httpd_register_uri_handler(server, &_root_delete_handler);

	/* URI handler for get parameter */
	httpd_uri_t _get_param_handler = {
		.uri = "/get",
		.method = HTTP_GET,
		.handler = get_param_handler,
	};
	httpd_register_uri_handler(server, &_get_param_handler);

	/* URI handler for capabilities */
	httpd_uri_t _get_capabilities_handler = {
		.uri = "/capabilities",
		.method = HTTP_GET,
		.handler = get_capabilities_handler,
	};
	httpd_register_uri_handler(server, &_get_capabilities_handler);

	/* URI handler for favicon.ico */
	httpd_uri_t _favicon_get_handler = {
		.uri = "/favicon.ico",
		.method = HTTP_GET,
		.handler = favicon_get_handler,
		//.user_ctx  = server_data	// Pass server data as context
	};
	httpd_register_uri_handler(server, &_favicon_get_handler);

	/* URI handler for restart (POST) */
	httpd_uri_t _restart_post_handler = {
		.uri = "/restart",
		.method = HTTP_POST,
		.handler = restart_post_handler,
	};
	httpd_register_uri_handler(server, &_restart_post_handler);

	/* URI handler for OPTIONS (CORS preflight) - specific endpoints */
	httpd_uri_t _options_post_handler = {
		.uri = "/post",
		.method = HTTP_OPTIONS,
		.handler = options_handler,
	};
	httpd_register_uri_handler(server, &_options_post_handler);

	httpd_uri_t _options_get_handler = {
		.uri = "/get",
		.method = HTTP_OPTIONS,
		.handler = options_handler,
	};
	httpd_register_uri_handler(server, &_options_get_handler);

	httpd_uri_t _options_delete_handler = {
		.uri = "/delete",
		.method = HTTP_OPTIONS,
		.handler = options_handler,
	};
	httpd_register_uri_handler(server, &_options_delete_handler);

	httpd_uri_t _options_capabilities_handler = {
		.uri = "/capabilities",
		.method = HTTP_OPTIONS,
		.handler = options_handler,
	};
	httpd_register_uri_handler(server, &_options_capabilities_handler);

	httpd_uri_t _options_restart_handler = {
		.uri = "/restart",
		.method = HTTP_OPTIONS,
		.handler = options_handler,
	};
	httpd_register_uri_handler(server, &_options_restart_handler);

#if CONFIG_DAC_TAS5805M
	/* URI handlers for DAC settings API */
	httpd_uri_t _get_dac_settings_handler = {
		.uri = "/api/dac/settings",
		.method = HTTP_GET,
		.handler = get_dac_settings_handler,
	};
	httpd_register_uri_handler(server, &_get_dac_settings_handler);

	httpd_uri_t _get_dac_schema_handler = {
		.uri = "/api/dac/schema",
		.method = HTTP_GET,
		.handler = get_dac_schema_handler,
	};
	httpd_register_uri_handler(server, &_get_dac_schema_handler);

	httpd_uri_t _post_dac_settings_handler = {
		.uri = "/api/dac/settings",
		.method = HTTP_POST,
		.handler = post_dac_settings_handler,
	};
	httpd_register_uri_handler(server, &_post_dac_settings_handler);

	/* OPTIONS handlers for CORS preflight - DAC endpoints */
	httpd_uri_t _options_dac_settings_handler = {
		.uri = "/api/dac/settings",
		.method = HTTP_OPTIONS,
		.handler = options_handler,
	};
	httpd_register_uri_handler(server, &_options_dac_settings_handler);

	httpd_uri_t _options_dac_schema_handler = {
		.uri = "/api/dac/schema",
		.method = HTTP_OPTIONS,
		.handler = options_handler,
	};
	httpd_register_uri_handler(server, &_options_dac_schema_handler);

	/* URI handlers for EQ settings API */
	httpd_uri_t _get_eq_settings_handler = {
		.uri = "/api/eq/settings",
		.method = HTTP_GET,
		.handler = get_eq_settings_handler,
	};
	httpd_register_uri_handler(server, &_get_eq_settings_handler);

	httpd_uri_t _get_eq_schema_handler = {
		.uri = "/api/eq/schema",
		.method = HTTP_GET,
		.handler = get_eq_schema_handler,
	};
	httpd_register_uri_handler(server, &_get_eq_schema_handler);

	httpd_uri_t _post_eq_settings_handler = {
		.uri = "/api/eq/settings",
		.method = HTTP_POST,
		.handler = post_eq_settings_handler,
	};
	httpd_register_uri_handler(server, &_post_eq_settings_handler);

	/* OPTIONS handlers for CORS preflight - EQ endpoints */
	httpd_uri_t _options_eq_settings_handler = {
		.uri = "/api/eq/settings",
		.method = HTTP_OPTIONS,
		.handler = options_handler,
	};
	httpd_register_uri_handler(server, &_options_eq_settings_handler);

	httpd_uri_t _options_eq_schema_handler = {
		.uri = "/api/eq/schema",
		.method = HTTP_OPTIONS,
		.handler = options_handler,
	};
	httpd_register_uri_handler(server, &_options_eq_schema_handler);
#endif /* CONFIG_DAC_TAS5805M */

	/* URI handler for static files (catch-all, must be last) */
	httpd_uri_t _static_file_handler = {
		.uri = "/*",
		.method = HTTP_GET,
		.handler = static_file_handler,
	};
	esp_err_t ret = httpd_register_uri_handler(server, &_static_file_handler);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "%s: Failed to register static file handler: %s",
				 __func__, esp_err_to_name(ret));
	} else {
		ESP_LOGI(TAG, "%s: Static file handler registered for /*", __func__);
	}

	return ESP_OK;
}

/**
 * HTTP Server task - manages DSP parameters with flow-specific storage
 */
static void http_server_task(void *pvParameters) {
	ESP_LOGD(TAG, "%s: started", __func__);
	// Start Server
	ESP_ERROR_CHECK(start_server("/html", CONFIG_WEB_PORT));

	// Ensure mdns setting has a default (true) on first boot - handled by
	// settings_manager
	bool tmp_mdns = true;
	if (settings_get_mdns_enabled(&tmp_mdns) == ESP_OK) {
		ESP_LOGD(TAG, "%s: mdns setting loaded: %d", __func__,
				 tmp_mdns ? 1 : 0);
	}

	// DSP processor already loads parameters from NVS in dsp_processor_init()
	// Just get the current active flow and parameters from DSP processor
	dspFlows_t active_flow = dspfStereo; // default
	filterParams_t current_params;
	memset(&current_params, 0, sizeof(filterParams_t));

#if CONFIG_USE_DSP_PROCESSOR
	active_flow = dsp_settings_get_active_flow();
	dsp_settings_get_flow_params(active_flow, &current_params);
	ESP_LOGI(TAG, "%s: Current flow %d with fc_1=%.1f gain_1=%.1f", __func__,
		 active_flow, current_params.fc_1, current_params.gain_1);
#else
	current_params.dspFlow = active_flow;
#endif

	URL_t urlBuf;
	while (1) {
		// Waiting for post
		if (xQueueReceive(xQueueHttp, &urlBuf, portMAX_DELAY) == pdTRUE) {
			ESP_LOGI(TAG, "%s: received update: %s = %ld", __func__, urlBuf.key,
					 (long)urlBuf.int_value);

		// Handle flow change specially
		if (strcmp(urlBuf.key, "dspFlow") == 0) {
			dspFlows_t new_flow = (dspFlows_t)urlBuf.int_value;

#if CONFIG_USE_DSP_PROCESSOR
			// Switch to new flow (loads its stored parameters and notifies subscribers)
			dsp_settings_switch_active_flow(new_flow);
			// Update our tracked active flow
			active_flow = new_flow;
			// Get the parameters for the new flow
			dsp_settings_get_flow_params(new_flow, &current_params);
			ESP_LOGI(TAG, "%s: Switched to flow %d", __func__, new_flow);
#else
			active_flow = new_flow;
			current_params.dspFlow = new_flow;
#endif
			continue;
		}

		// Handle parameter updates for current flow
		bool param_recognized = false;

		if (strcmp(urlBuf.key, "fc_1") == 0) {
			current_params.fc_1 = (float)urlBuf.int_value;
			param_recognized = true;
		} else if (strcmp(urlBuf.key, "gain_1") == 0) {
			current_params.gain_1 = (float)urlBuf.int_value;
			param_recognized = true;
		} else if (strcmp(urlBuf.key, "fc_3") == 0) {
			current_params.fc_3 = (float)urlBuf.int_value;
			param_recognized = true;
		} else if (strcmp(urlBuf.key, "gain_3") == 0) {
			current_params.gain_3 = (float)urlBuf.int_value;
			param_recognized = true;
		}

		if (!param_recognized) {
			ESP_LOGW(TAG, "%s: Unknown param '%s' received, ignoring",
					 __func__, urlBuf.key);
			continue;
		}

#if CONFIG_USE_DSP_PROCESSOR
		// Always read active flow fresh from NVS to ensure we save to the correct flow
		// (the UI may have changed the flow before this task's cached value was updated)
		dspFlows_t save_flow = dsp_settings_get_active_flow();
		
		// Fetch the current params for THIS flow fresh from NVS
		// This prevents overwriting other params with stale cached values
		filterParams_t save_params;
		dsp_settings_get_flow_params(save_flow, &save_params);
		
		// Update only the changed parameter
		if (strcmp(urlBuf.key, "fc_1") == 0) {
			save_params.fc_1 = (float)urlBuf.int_value;
		} else if (strcmp(urlBuf.key, "gain_1") == 0) {
			save_params.gain_1 = (float)urlBuf.int_value;
		} else if (strcmp(urlBuf.key, "fc_3") == 0) {
			save_params.fc_3 = (float)urlBuf.int_value;
		} else if (strcmp(urlBuf.key, "gain_3") == 0) {
			save_params.gain_3 = (float)urlBuf.int_value;
		}
		
		// Update our cached params if this is the current flow
		if (save_flow == active_flow) {
			current_params = save_params;
		}
		
		dsp_settings_set_flow_params(save_flow, &save_params);
		ESP_LOGI(TAG, "%s: Saved %s = %ld to flow %d", __func__, urlBuf.key,
				 (long)urlBuf.int_value, save_flow);
#else
		// Persist parameter using dsp_settings (values are stored as int32_t)
		dspFlows_t save_flow = dsp_settings_get_active_flow();
		if (dsp_settings_save_flow_param(save_flow, urlBuf.key,
										 urlBuf.int_value) != ESP_OK) {
			ESP_LOGW(TAG, "%s: Failed to persist param '%s' to NVS",
					 __func__, urlBuf.key);
		} else {
			ESP_LOGD(TAG, "%s: Saved %s = %ld to NVS", __func__, urlBuf.key,
					 (long)urlBuf.int_value);
		}
#endif
		}
	}
	// Never reach here
	ESP_LOGI(TAG, "%s: finish", __func__);
	vTaskDelete(NULL);
}

/**
 *
 */
void init_http_server_task(void) {
	ESP_LOGD(TAG, "%s: initializing", __func__);

	// No SPIFFS mounting needed - files are embedded in flash

	// Create Queue
	if (!xQueueHttp) {
		xQueueHttp = xQueueCreate(10, sizeof(URL_t));
		configASSERT(xQueueHttp);
	}

	if (taskHandle) {
		stop_server();
		vTaskDelete(taskHandle);
		taskHandle = NULL;
	}

	// Stack size can be reduced from 512*8 since we're not using file I/O
	xTaskCreatePinnedToCore(http_server_task, "HTTP", 512 * 6, NULL, 2,
							&taskHandle, tskNO_AFFINITY);
}
