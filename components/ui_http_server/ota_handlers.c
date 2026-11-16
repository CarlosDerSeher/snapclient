#include "ota_handlers.h"

#if CONFIG_SNAPCLIENT_WEB_OTA

#include <string.h>
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "OTA_HTTP";

extern void sc_stop_snapclient(void);

#define OTA_HTTP_CHUNK_SIZE 4096

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
 * POST /api/ota/upload
 * Receives a raw firmware binary, writes it to the inactive OTA partition,
 * sets it as the next boot partition, and restarts.
 */
static esp_err_t ota_upload_handler(httpd_req_t *req) {
	ESP_LOGD(TAG, "%s: uri=%s content_len=%d", __func__, req->uri, req->content_len);

	set_cors_headers(req);
	httpd_resp_set_type(req, "application/json");

	if (req->content_len == 0) {
		httpd_resp_set_status(req, "400 Bad Request");
		httpd_resp_sendstr(req, "{\"error\": \"No firmware data\"}");
		return ESP_OK;
	}

	ESP_LOGI(TAG, "%s: OTA upload started, %d bytes", __func__, req->content_len);

	sc_stop_snapclient();
	vTaskDelay(pdMS_TO_TICKS(500));

	const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
	if (!update_partition) {
		httpd_resp_set_status(req, "500 Internal Server Error");
		httpd_resp_sendstr(req, "{\"error\": \"No OTA partition available\"}");
		return ESP_OK;
	}

	esp_ota_handle_t ota_handle;
	esp_err_t err = esp_ota_begin(update_partition, req->content_len, &ota_handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "%s: esp_ota_begin failed: %s", __func__, esp_err_to_name(err));
		httpd_resp_set_status(req, "500 Internal Server Error");
		httpd_resp_sendstr(req, "{\"error\": \"OTA begin failed\"}");
		return ESP_OK;
	}

	char *buf = (char *)malloc(OTA_HTTP_CHUNK_SIZE);
	if (!buf) {
		esp_ota_abort(ota_handle);
		httpd_resp_set_status(req, "500 Internal Server Error");
		httpd_resp_sendstr(req, "{\"error\": \"Memory allocation failed\"}");
		return ESP_OK;
	}

	int remaining = req->content_len;
	int received = 0;
	int last_pct10 = -1;

	while (remaining > 0) {
		int to_read = (remaining < OTA_HTTP_CHUNK_SIZE) ? remaining : OTA_HTTP_CHUNK_SIZE;
		int ret = httpd_req_recv(req, buf, to_read);

		if (ret <= 0) {
			free(buf);
			esp_ota_abort(ota_handle);
			if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
				httpd_resp_set_status(req, "408 Request Timeout");
				httpd_resp_sendstr(req, "{\"error\": \"Upload timed out\"}");
			} else {
				httpd_resp_set_status(req, "500 Internal Server Error");
				httpd_resp_sendstr(req, "{\"error\": \"Receive failed\"}");
			}
			return ESP_OK;
		}

		err = esp_ota_write(ota_handle, buf, ret);
		if (err != ESP_OK) {
			free(buf);
			esp_ota_abort(ota_handle);
			ESP_LOGE(TAG, "%s: esp_ota_write failed: %s", __func__, esp_err_to_name(err));
			httpd_resp_set_status(req, "500 Internal Server Error");
			httpd_resp_sendstr(req, "{\"error\": \"OTA write failed\"}");
			return ESP_OK;
		}

		received += ret;
		remaining -= ret;

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
		httpd_resp_set_status(req, "500 Internal Server Error");
		httpd_resp_sendstr(req, "{\"error\": \"OTA validation failed — invalid firmware?\"}");
		return ESP_OK;
	}

	err = esp_ota_set_boot_partition(update_partition);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "%s: esp_ota_set_boot_partition failed: %s", __func__, esp_err_to_name(err));
		httpd_resp_set_status(req, "500 Internal Server Error");
		httpd_resp_sendstr(req, "{\"error\": \"Failed to set boot partition\"}");
		return ESP_OK;
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
