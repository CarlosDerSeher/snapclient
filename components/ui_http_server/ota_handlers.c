#include "ota_handlers.h"

#if CONFIG_SNAPCLIENT_WEB_OTA

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_auth.h"

static const char *TAG = "OTA_HTTP";

// from main.c
typedef enum { STOPPED = 0, IDLE, PLAYING, PAUSED } snapclient_state_t;
extern void sc_start_snapclient(void);
extern void sc_stop_snapclient(void);
extern snapclient_state_t sc_get_snapclient_state(void);

#define OTA_HTTP_CHUNK_SIZE 4096
// consecutive receive timeouts (5 s each) before the upload is given up;
// httpd serves all clients from one task, so do not wait forever
#define OTA_HTTP_MAX_TIMEOUTS 6

static void set_cors_headers(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Methods",
	                   "GET, POST, DELETE, OPTIONS");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
	httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
}

static esp_err_t options_handler(httpd_req_t *req) {
	set_cors_headers(req);
	httpd_resp_set_status(req, "204 No Content");
	httpd_resp_send(req, NULL, 0);
	return ESP_OK;
}

static void restart_task(void *pv) {
	vTaskDelay(pdMS_TO_TICKS(200));
	esp_restart();
	vTaskDelete(NULL);
}

/*
 * Stop snapclient so it does not compete for RAM and network while the
 * image is written. Returns true if it was running (and must be resumed if
 * the update fails).
 */
static bool stop_snapclient(void) {
	if (sc_get_snapclient_state() == STOPPED) {
		return false;
	}

	sc_stop_snapclient();
	for (int i = 0; i < 200; i++) {  // up to 10 s
		if (sc_get_snapclient_state() == STOPPED) {
			return true;
		}
		vTaskDelay(pdMS_TO_TICKS(50));
	}

	// writing the image does not depend on it, so carry on
	ESP_LOGW(TAG, "%s: snapclient did not stop within 10 s", __func__);
	return true;
}

/*
 * Send an error response and resume playback if it was stopped for the
 * update.
 */
static esp_err_t ota_fail(httpd_req_t *req, bool resume, const char *status,
						  const char *json) {
	httpd_resp_set_status(req, status);
	httpd_resp_sendstr(req, json);
	if (resume) {
		sc_start_snapclient();
	}
	return ESP_OK;
}

/*
 * POST /api/ota/upload
 * Receives a raw firmware binary, writes it to the inactive OTA partition,
 * sets it as the next boot partition, and restarts.
 */
static esp_err_t ota_upload_handler(httpd_req_t *req) {
	ESP_LOGD(TAG, "%s: uri=%s content_len=%d", __func__, req->uri, req->content_len);

	if (require_http_auth(req) != ESP_OK) {
		return ESP_OK;
	}

	set_cors_headers(req);
	httpd_resp_set_type(req, "application/json");

	// Browsers send Origin with every POST, curl doesn't. Without auth, a
	// script on any web page could otherwise flash the device.
	if (httpd_req_get_hdr_value_len(req, "Origin") > 0) {
		char origin[128], host[96];
		const char *o;
		if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK ||
			!(o = strstr(origin, "://")) ||
			httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK ||
			strcasecmp(o + 3, host) != 0) {
			ESP_LOGW(TAG, "%s: cross-origin upload rejected", __func__);
			return ota_fail(req, false, "403 Forbidden",
							"{\"error\": \"Cross-origin upload rejected\"}");
		}
	}

	if (req->content_len == 0) {
		return ota_fail(req, false, "400 Bad Request",
						"{\"error\": \"No firmware data\"}");
	}

	const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
	if (!update_partition) {
		return ota_fail(req, false, "500 Internal Server Error",
						"{\"error\": \"No OTA partition available\"}");
	}

	if (req->content_len > update_partition->size) {
		ESP_LOGE(TAG, "%s: image (%d bytes) larger than partition (%lu bytes)",
				 __func__, req->content_len, update_partition->size);
		return ota_fail(req, false, "413 Payload Too Large",
						"{\"error\": \"Firmware larger than OTA partition\"}");
	}

	ESP_LOGI(TAG, "%s: OTA upload started, %d bytes", __func__, req->content_len);

	bool resume = stop_snapclient();

	// erase sector by sector while writing: erasing the whole image up front
	// blocks long enough to trigger the task watchdog
	esp_ota_handle_t ota_handle;
	esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES,
								  &ota_handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "%s: esp_ota_begin failed: %s", __func__, esp_err_to_name(err));
		return ota_fail(req, resume, "500 Internal Server Error",
						"{\"error\": \"OTA begin failed\"}");
	}

	char *buf = (char *)malloc(OTA_HTTP_CHUNK_SIZE);
	if (!buf) {
		esp_ota_abort(ota_handle);
		return ota_fail(req, resume, "500 Internal Server Error",
						"{\"error\": \"Memory allocation failed\"}");
	}

	int remaining = req->content_len;
	int received = 0;
	int last_pct10 = -1;
	int timeouts = 0;

	while (remaining > 0) {
		int to_read = (remaining < OTA_HTTP_CHUNK_SIZE) ? remaining : OTA_HTTP_CHUNK_SIZE;
		int ret = httpd_req_recv(req, buf, to_read);

		// ride out short Wi-Fi stalls
		if (ret == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < OTA_HTTP_MAX_TIMEOUTS) {
			ESP_LOGW(TAG, "%s: receive timed out after %d bytes, waiting (%d/%d)",
					 __func__, received, timeouts, OTA_HTTP_MAX_TIMEOUTS - 1);
			continue;
		}

		if (ret <= 0) {
			free(buf);
			esp_ota_abort(ota_handle);
			ESP_LOGE(TAG, "%s: receive failed after %d bytes (%d)", __func__,
					 received, ret);
			if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
				return ota_fail(req, resume, "408 Request Timeout",
								"{\"error\": \"Upload timed out\"}");
			}
			return ota_fail(req, resume, "500 Internal Server Error",
							"{\"error\": \"Receive failed\"}");
		}

		err = esp_ota_write(ota_handle, buf, ret);
		if (err != ESP_OK) {
			free(buf);
			esp_ota_abort(ota_handle);
			ESP_LOGE(TAG, "%s: esp_ota_write failed: %s", __func__, esp_err_to_name(err));
			return ota_fail(req, resume, "500 Internal Server Error",
							"{\"error\": \"OTA write failed\"}");
		}

		timeouts = 0;
		received += ret;
		remaining -= ret;

		// every flash write parks the other CPU in its IPC task; pause now and
		// then so its idle task runs and the task watchdog stays quiet
		if ((received % (8 * OTA_HTTP_CHUNK_SIZE)) < ret) {
			vTaskDelay(pdMS_TO_TICKS(10));
		}

		int pct10 = received * 10 / req->content_len;
		if (pct10 != last_pct10) {
			last_pct10 = pct10;
			ESP_LOGI(TAG, "%s: OTA progress: %d%%", __func__, pct10 * 10);
		}
	}

	free(buf);

	err = esp_ota_end(ota_handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "%s: esp_ota_end failed: %s", __func__, esp_err_to_name(err));
		return ota_fail(req, resume, "500 Internal Server Error",
						"{\"error\": \"OTA validation failed — invalid firmware?\"}");
	}

	err = esp_ota_set_boot_partition(update_partition);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "%s: esp_ota_set_boot_partition failed: %s", __func__, esp_err_to_name(err));
		return ota_fail(req, resume, "500 Internal Server Error",
						"{\"error\": \"Failed to set boot partition\"}");
	}

	ESP_LOGI(TAG, "%s: OTA complete (%d bytes), restarting...", __func__, received);

	httpd_resp_set_status(req, "200 OK");
	httpd_resp_set_hdr(req, "Connection", "close");
	httpd_resp_sendstr(req, "{\"success\": true}");

	xTaskCreate(restart_task, "ota_restart", 2048, NULL, 5, NULL);

	return ESP_OK;
}

/*
 * GET /api/ota/status
 * Returns running firmware description so the frontend can confirm a
 * successful OTA by comparing SHA256 before and after the upload.
 */
static esp_err_t get_ota_status_handler(httpd_req_t *req) {
	ESP_LOGD(TAG, "%s: uri=%s", __func__, req->uri);

	if (require_http_auth(req) != ESP_OK) {
		return ESP_OK;
	}

	set_cors_headers(req);
	httpd_resp_set_type(req, "application/json");

	const esp_app_desc_t *desc = esp_app_get_description();

	char sha256[65] = {0};
	for (int i = 0; i < 32; i++) {
		snprintf(sha256 + i * 2, 3, "%02x", desc->app_elf_sha256[i]);
	}

	char json[256];
	snprintf(json, sizeof(json),
		"{\"version\":\"%s\",\"project_name\":\"%s\","
		"\"compile_date\":\"%s\",\"compile_time\":\"%s\","
		"\"idf_version\":\"%s\",\"sha256\":\"%s\"}",
		desc->version, desc->project_name,
		desc->date, desc->time,
		desc->idf_ver, sha256);

	httpd_resp_sendstr(req, json);
	return ESP_OK;
}

void ota_register_handlers(httpd_handle_t server) {
	httpd_uri_t upload = {
		.uri     = "/api/ota/upload",
		.method  = HTTP_POST,
		.handler = ota_upload_handler,
	};
	httpd_register_uri_handler(server, &upload);

	httpd_uri_t upload_options = {
		.uri     = "/api/ota/upload",
		.method  = HTTP_OPTIONS,
		.handler = options_handler,
	};
	httpd_register_uri_handler(server, &upload_options);

	httpd_uri_t status = {
		.uri     = "/api/ota/status",
		.method  = HTTP_GET,
		.handler = get_ota_status_handler,
	};
	httpd_register_uri_handler(server, &status);
}

#endif /* CONFIG_SNAPCLIENT_WEB_OTA */
