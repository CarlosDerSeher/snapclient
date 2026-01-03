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

#include "dsp_processor_settings.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "settings_manager.h"

static const char *TAG = "UI_HTTP";

static QueueHandle_t xQueueHttp = NULL;
static TaskHandle_t taskHandle = NULL;
static httpd_handle_t server = NULL;

// External references to embedded files
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t index_js_start[] asm("_binary_index_js_start");
extern const uint8_t index_js_end[] asm("_binary_index_js_end");
extern const uint8_t styles_css_start[] asm("_binary_styles_css_start");
extern const uint8_t styles_css_end[] asm("_binary_styles_css_end");
extern const uint8_t general_settings_html_start[] asm("_binary_general_settings_html_start");
extern const uint8_t general_settings_html_end[] asm("_binary_general_settings_html_end");
extern const uint8_t dsp_settings_html_start[] asm("_binary_dsp_settings_html_start");
extern const uint8_t dsp_settings_html_end[] asm("_binary_dsp_settings_html_end");
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
	{"/styles.css", styles_css_start, styles_css_end, "text/css; charset=utf-8"},
	{"/general-settings.html", general_settings_html_start, general_settings_html_end, "text/html; charset=utf-8"},
	{"/dsp-settings.html", dsp_settings_html_start, dsp_settings_html_end, "text/html; charset=utf-8"},
	{"/favicon.ico", favicon_ico_start, favicon_ico_end, "image/x-icon"},
};

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
 * Returns settings based on the 'tab' parameter: /capabilities?tab=general or
 * /capabilities?tab=dsp
 *
 * Response for tab=general:
 * - hostname, mdns_enabled, server_host, server_port
 *
 * Response for tab=dsp (if DSP enabled):
 * - active_flow and all flow parameters
 */
static esp_err_t get_capabilities_handler(httpd_req_t *req) {
	ESP_LOGD(TAG, "%s: uri=%s", __func__, req->uri);

	set_cors_headers(req);

	// Parse tab parameter
	char tab[16] = {0};
	if (!find_key_value("tab=", (char *)req->uri, tab)) {
		// No tab specified, return error
		ESP_LOGW(TAG, "%s: Missing 'tab' parameter", __func__);
		httpd_resp_set_status(req, "400 Bad Request");
		httpd_resp_sendstr(req, "{\"error\": \"Missing 'tab' parameter. Use "
								"?tab=general or ?tab=dsp\"}");
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
			req, "{\"error\": \"Unknown tab. Use ?tab=general or ?tab=dsp\"}");
	}

	return ESP_OK;
}

/*
 * favicon get handler
 * Returns 404 since we don't have a favicon
 */
static esp_err_t favicon_get_handler(httpd_req_t *req) {
	ESP_LOGD(TAG, "%s: uri=%s", __func__, req->uri);
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
 * Static file handler
 * Serves files from embedded flash memory
 */
static esp_err_t static_file_handler(httpd_req_t *req) {
	ESP_LOGD(TAG, "%s: uri=%s", __func__, req->uri);

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
	config.max_open_sockets =
		7; // Increased from 2 to handle concurrent requests better
	config.max_uri_handlers =
		16; // Increased from default (8) to accommodate all handlers
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
			// Get the parameters for the new flow
			dsp_settings_get_flow_params(new_flow, &current_params);
      ESP_LOGI(TAG, "%s: Switched to flow %d", __func__, new_flow);
#else
			current_params.dspFlow = new_flow;
#endif

			continue;
		}			// Handle parameter updates for current flow
			bool param_recognized = false;
			dspFlows_t current_flow = current_params.dspFlow;

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
	// Update settings and notify subscribers (includes NVS persistence)
	dsp_settings_set_flow_params(current_flow, &current_params);
	ESP_LOGD(TAG, "%s: Updated %s = %ld", __func__, urlBuf.key,
		 (long)urlBuf.int_value);
#else
		// Persist parameter using dsp_settings (values are stored as
		// int32_t)
		if (dsp_settings_save_flow_param(current_flow, urlBuf.key,
										 urlBuf.int_value) != ESP_OK) {
			ESP_LOGW(TAG, "%s: Failed to persist param '%s' to NVS",
					 __func__, urlBuf.key);
			} else {
			ESP_LOGD(TAG, "%s: Saved %s = %ld to NVS", __func__, urlBuf.key,
					 (long)urlBuf.int_value);
		}
#endif
	}
}	// Never reach here
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
