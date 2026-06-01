/**
 * @file pcm51xx_settings.c
 * @brief PCM5122 DAC settings persistence and JSON serialization implementation
 *
 * Mirrors the tas5805m_settings architecture:
 *
 *   1. pcm51xx_settings_init() — called once at boot after pcm51xx_init().
 *      Creates a mutex, loads NVS defaults, and starts a background task
 *      that waits for the I2S clock to become stable.
 *
 *   2. pcm51xx_settings_notify_i2s_ready() — called by the audio subsystem
 *      when I2S BCK is running.  Unblocks the background task immediately.
 *      If the notification never arrives, the task applies settings after a
 *      30-second safety timeout so a hard-coded delay can never stall the
 *      system permanently.
 *
 *   3. pcm51xx_settings_apply_delayed() — executed by the background task
 *      (or explicitly by the caller).  Calls pcm51xx_dsp_start() to configure
 *      process flow 5 and zero all EQ bands, then restores every per-band gain
 *      persisted in NVS.
 */

#include "pcm51xx_settings.h"

#if defined(CONFIG_DAC_PCM51XX_EQ_SUPPORT)

#include <string.h>
#include <stdio.h>
#include <limits.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"

#include "pcm51xx.h"
#include "pcm51xx_eq_config.h"

static const char *TAG = "pcm51xx_settings";

/* -------------------------------------------------------------------------
 * Module state
 * ------------------------------------------------------------------------- */

/** Mutex protecting all NVS operations. */
static SemaphoreHandle_t s_mutex = NULL;

/** Handle of the background polling / delay task (NULL when not running). */
static TaskHandle_t s_poll_task_handle = NULL;

/** Set to true once apply_delayed() has run successfully at least once. */
static bool s_settings_applied = false;

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/** Open NVS with read/write access and the module namespace. */
static esp_err_t settings_nvs_open(nvs_handle_t *handle)
{
    return nvs_open(PCM51XX_NVS_NAMESPACE, NVS_READWRITE, handle);
}

/** Build the per-band NVS key into buf (must be at least 12 bytes). */
static void make_gain_key(int band, char *buf, size_t buf_len)
{
    snprintf(buf, buf_len, "%s%d", PCM51XX_NVS_KEY_EQ_GAIN_PREFIX, band);
}

/* -------------------------------------------------------------------------
 * Background task — waits for I2S clock notification, then applies settings
 * ------------------------------------------------------------------------- */

/**
 * Wait up to PCM51XX_SETTINGS_CLK_WAIT_MS milliseconds for the I2S-ready
 * notification sent by pcm51xx_settings_notify_i2s_ready(), then call
 * pcm51xx_settings_apply_delayed().
 */
#define PCM51XX_SETTINGS_CLK_WAIT_MS  30000

static void pcm51xx_settings_poll_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "%s: Waiting up to %d s for I2S clock", __func__,
             PCM51XX_SETTINGS_CLK_WAIT_MS / 1000);

    /* Block until the audio subsystem sends a notification or the timeout
     * expires.  Either way we apply settings — the timeout is a safety net
     * so a missed notification can never permanently defer EQ restore. */
    uint32_t notif_value = 0;
    BaseType_t notified = xTaskNotifyWait(0, ULONG_MAX, &notif_value,
                                          pdMS_TO_TICKS(PCM51XX_SETTINGS_CLK_WAIT_MS));

    if (notified == pdTRUE) {
        ESP_LOGI(TAG, "%s: I2S clock ready — applying persisted EQ settings", __func__);
    } else {
        ESP_LOGW(TAG, "%s: Timeout waiting for I2S clock — applying settings anyway", __func__);
    }

    esp_err_t ret = pcm51xx_settings_apply_delayed();
    if (ret == ESP_OK) {
        s_settings_applied = true;
    } else {
        ESP_LOGE(TAG, "%s: pcm51xx_settings_apply_delayed() failed: %s",
                 __func__, esp_err_to_name(ret));
    }

    s_poll_task_handle = NULL;
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------
 * Public API — lifecycle
 * ------------------------------------------------------------------------- */

esp_err_t pcm51xx_settings_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            ESP_LOGE(TAG, "%s: Failed to create mutex", __func__);
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "%s: PCM5122 settings manager initialised", __func__);

    /* Start the background task that waits for I2S clock then applies EQ. */
    if (!s_settings_applied && s_poll_task_handle == NULL) {
        BaseType_t ret = xTaskCreate(pcm51xx_settings_poll_task,
                                     "pcm51xx_settings",
                                     4096,
                                     NULL,
                                     tskIDLE_PRIORITY + 2,
                                     &s_poll_task_handle);
        if (ret == pdPASS) {
            ESP_LOGD(TAG, "%s: Started background settings task", __func__);
        } else {
            ESP_LOGW(TAG, "%s: Failed to start background task; call "
                     "pcm51xx_settings_apply_delayed() manually", __func__);
        }
    }

    return ESP_OK;
}

/**
 * @brief Notify the settings module that the I2S clock is now stable.
 *
 * Unblocks the background task started by pcm51xx_settings_init() so that
 * DSP initialisation and EQ restore happen immediately rather than waiting
 * for the safety timeout.  Safe to call from any context including ISR.
 */
void pcm51xx_settings_notify_i2s_ready(void)
{
    if (s_poll_task_handle != NULL) {
        xTaskNotify(s_poll_task_handle, 1, eSetBits);
    }
}

esp_err_t pcm51xx_settings_apply_delayed(void)
{
    ESP_LOGI(TAG, "%s: Applying delayed PCM5122 settings from NVS", __func__);

    /* Initialise DSP: set process flow 5 and write identity coefficients. */
    esp_err_t ret = pcm51xx_dsp_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: pcm51xx_dsp_start() failed: %s",
                 __func__, esp_err_to_name(ret));
        return ret;
    }

    /* Check whether EQ is enabled. */
    bool eq_enabled = true;
    pcm51xx_settings_load_eq_enabled(&eq_enabled);

    if (!eq_enabled) {
        ESP_LOGI(TAG, "%s: EQ is disabled; skipping gain restore", __func__);
        return ESP_OK;
    }

    /* Restore per-band gains. */
    for (int band = 0; band < PCM51XX_EQ_BANDS; band++) {
        int gain_db = 0;
        esp_err_t load_ret = pcm51xx_settings_load_eq_gain(band, &gain_db);
        if (load_ret == ESP_ERR_NVS_NOT_FOUND) {
            /* No persisted value — leave band at 0 dB (already identity). */
            ESP_LOGD(TAG, "%s: band %d: no saved gain, using 0 dB", __func__, band);
            continue;
        }
        if (load_ret != ESP_OK) {
            ESP_LOGW(TAG, "%s: band %d: NVS load error: %s",
                     __func__, band, esp_err_to_name(load_ret));
            continue;
        }
        if (gain_db == 0) {
            /* Identity — dsp_start already wrote unity coefficients. */
            continue;
        }
        esp_err_t set_ret = pcm51xx_set_eq_gain(band, gain_db);
        if (set_ret != ESP_OK) {
            ESP_LOGW(TAG, "%s: band %d: pcm51xx_set_eq_gain(%d) failed: %s",
                     __func__, band, gain_db, esp_err_to_name(set_ret));
        } else {
            ESP_LOGD(TAG, "%s: band %d (%d Hz): gain=%d dB",
                     __func__, band, (int)pcm51xx_eq_band_cfg[band].freq_hz, gain_db);
        }
    }

    ESP_LOGI(TAG, "%s: Done", __func__);
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Public API — EQ enable flag
 * ------------------------------------------------------------------------- */

esp_err_t pcm51xx_settings_save_eq_enabled(bool enabled)
{
    if (s_mutex == NULL) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t ret = settings_nvs_open(&h);
    if (ret == ESP_OK) {
        ret = nvs_set_u8(h, PCM51XX_NVS_KEY_EQ_ENABLED, enabled ? 1u : 0u);
        if (ret == ESP_OK) ret = nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(s_mutex);

    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "%s: eq_enabled=%d saved", __func__, (int)enabled);
    } else {
        ESP_LOGW(TAG, "%s: NVS write failed: %s", __func__, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t pcm51xx_settings_load_eq_enabled(bool *enabled)
{
    if (enabled == NULL) return ESP_ERR_INVALID_ARG;
    if (s_mutex == NULL) return ESP_ERR_INVALID_STATE;

    *enabled = true; /* default */

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t ret = settings_nvs_open(&h);
    if (ret == ESP_OK) {
        uint8_t val = 1u;
        ret = nvs_get_u8(h, PCM51XX_NVS_KEY_EQ_ENABLED, &val);
        if (ret == ESP_OK) {
            *enabled = (val != 0u);
        }
        nvs_close(h);
    }
    xSemaphoreGive(s_mutex);
    return ret;
}

/* -------------------------------------------------------------------------
 * Public API — per-band EQ gain
 * ------------------------------------------------------------------------- */

esp_err_t pcm51xx_settings_save_eq_gain(int band, int gain_db)
{
    if (band < 0 || band >= PCM51XX_EQ_BANDS) return ESP_ERR_INVALID_ARG;
    if (gain_db < PCM51XX_EQ_MIN_DB || gain_db > PCM51XX_EQ_MAX_DB) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mutex == NULL) return ESP_ERR_INVALID_STATE;

    char key[16];
    make_gain_key(band, key, sizeof(key));

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t ret = settings_nvs_open(&h);
    if (ret == ESP_OK) {
        ret = nvs_set_i8(h, key, (int8_t)gain_db);
        if (ret == ESP_OK) ret = nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(s_mutex);

    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "%s: band=%d gain=%d dB saved", __func__, band, gain_db);
    } else {
        ESP_LOGW(TAG, "%s: NVS write failed: %s", __func__, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t pcm51xx_settings_load_eq_gain(int band, int *gain_db)
{
    if (band < 0 || band >= PCM51XX_EQ_BANDS || gain_db == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mutex == NULL) return ESP_ERR_INVALID_STATE;

    *gain_db = 0; /* default */

    char key[16];
    make_gain_key(band, key, sizeof(key));

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t ret = settings_nvs_open(&h);
    if (ret == ESP_OK) {
        int8_t val = 0;
        ret = nvs_get_i8(h, key, &val);
        if (ret == ESP_OK) {
            *gain_db = (int)val;
        }
        nvs_close(h);
    }
    xSemaphoreGive(s_mutex);
    return ret;
}

/* -------------------------------------------------------------------------
 * Public API — JSON serialization
 * ------------------------------------------------------------------------- */

esp_err_t pcm51xx_settings_get_eq_json(char *json_out, size_t max_len)
{
    if (json_out == NULL || max_len == 0) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return ESP_ERR_NO_MEM;

    bool eq_enabled = true;
    pcm51xx_settings_load_eq_enabled(&eq_enabled);
    cJSON_AddBoolToObject(root, PCM51XX_NVS_KEY_EQ_ENABLED, eq_enabled);

    for (int band = 0; band < PCM51XX_EQ_BANDS; band++) {
        int gain_db = 0;
        pcm51xx_settings_load_eq_gain(band, &gain_db);

        char key[16];
        make_gain_key(band, key, sizeof(key));
        cJSON_AddNumberToObject(root, key, gain_db);
    }

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (str == NULL) return ESP_ERR_NO_MEM;

    if (strlen(str) >= max_len) {
        free(str);
        return ESP_ERR_NO_MEM;
    }

    strncpy(json_out, str, max_len);
    free(str);
    return ESP_OK;
}

esp_err_t pcm51xx_settings_get_eq_schema_json(char *json_out, size_t max_len)
{
    if (json_out == NULL || max_len == 0) return ESP_ERR_INVALID_ARG;

    cJSON *root   = cJSON_CreateObject();
    cJSON *params = cJSON_AddArrayToObject(root, "params");
    if (root == NULL || params == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    /* EQ enable toggle */
    cJSON *en = cJSON_CreateObject();
    cJSON_AddStringToObject(en, "key",   PCM51XX_NVS_KEY_EQ_ENABLED);
    cJSON_AddStringToObject(en, "name",  "EQ Enabled");
    cJSON_AddStringToObject(en, "type",  "bool");
    cJSON_AddItemToArray(params, en);

    /* Per-band gain sliders */
    for (int band = 0; band < PCM51XX_EQ_BANDS; band++) {
        cJSON *item = cJSON_CreateObject();

        char key[16];
        make_gain_key(band, key, sizeof(key));
        cJSON_AddStringToObject(item, "key", key);

        /* Human-readable label: frequency in Hz */
        char label[32];
        uint16_t freq = pcm51xx_eq_band_cfg[band].freq_hz;
        if (freq >= 1000u) {
            snprintf(label, sizeof(label), "%d kHz", freq / 1000);
        } else {
            snprintf(label, sizeof(label), "%d Hz", freq);
        }
        cJSON_AddStringToObject(item, "name",  label);
        cJSON_AddStringToObject(item, "type",  "slider");
        cJSON_AddNumberToObject(item, "min",   pcm51xx_eq_band_cfg[band].min_db);
        cJSON_AddNumberToObject(item, "max",   pcm51xx_eq_band_cfg[band].max_db);
        cJSON_AddNumberToObject(item, "step",  1);
        cJSON_AddNumberToObject(item, "value", 0);

        cJSON_AddItemToArray(params, item);
    }

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (str == NULL) return ESP_ERR_NO_MEM;
    if (strlen(str) >= max_len) {
        free(str);
        return ESP_ERR_NO_MEM;
    }

    strncpy(json_out, str, max_len);
    free(str);
    return ESP_OK;
}

esp_err_t pcm51xx_settings_set_eq_from_json(const char *json_in)
{
    if (json_in == NULL) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_Parse(json_in);
    if (root == NULL) {
        ESP_LOGE(TAG, "%s: JSON parse error", __func__);
        return ESP_FAIL;
    }

    esp_err_t result = ESP_OK;

    /* EQ enable flag */
    cJSON *en_item = cJSON_GetObjectItemCaseSensitive(root, PCM51XX_NVS_KEY_EQ_ENABLED);
    if (cJSON_IsBool(en_item)) {
        bool enabled = cJSON_IsTrue(en_item);
        esp_err_t ret = pcm51xx_settings_save_eq_enabled(enabled);
        if (ret != ESP_OK) result = ret;
    }

    /* Per-band gains */
    for (int band = 0; band < PCM51XX_EQ_BANDS; band++) {
        char key[16];
        make_gain_key(band, key, sizeof(key));

        cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
        if (!cJSON_IsNumber(item)) continue;

        int gain_db = (int)cJSON_GetNumberValue(item);
        /* Clamp to valid range before saving. */
        if (gain_db < PCM51XX_EQ_MIN_DB) gain_db = PCM51XX_EQ_MIN_DB;
        if (gain_db > PCM51XX_EQ_MAX_DB) gain_db = PCM51XX_EQ_MAX_DB;

        esp_err_t ret = pcm51xx_settings_save_eq_gain(band, gain_db);
        if (ret != ESP_OK) {
            result = ret;
            continue;
        }

        /* Apply immediately if DSP is already initialised. */
        if (s_settings_applied) {
            ret = pcm51xx_set_eq_gain(band, gain_db);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "%s: band %d: live gain apply failed: %s",
                         __func__, band, esp_err_to_name(ret));
                result = ret;
            }
        }
    }

    cJSON_Delete(root);
    return result;
}

#endif /* CONFIG_DAC_PCM51XX_EQ_SUPPORT */
