/**
 * @file pcm51xx_settings.c
 * @brief PCM5122 DAC settings persistence and JSON serialization implementation
 *
 * Mirrors the tas5805m_settings architecture:
 *
 *   -- pcm51xx_settings_init() — called once at boot after pcm51xx_init().
 *      Creates a mutex, loads NVS defaults, initialises the DSP with identity
 *      BQ coefficients, restores the persisted process flow and per-band gains.
 *
 */

#include "pcm51xx_settings.h"

#if defined(CONFIG_DAC_PCM51XX_EQ_SUPPORT)

#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

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

/** Set to true once init has applied settings successfully at least once. */
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

    ESP_LOGI(TAG, "%s: Applying PCM5122 settings from NVS", __func__);

    /* Load the persisted process flow. Default to flow 1 (bypass). */
    uint8_t flow = PCM51XX_PROC_FLOW_1;
    pcm51xx_settings_load_process_flow(&flow);

    esp_err_t ret = pcm51xx_set_process_flow(flow);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: pcm51xx_set_process_flow(%d) failed: %s",
                    __func__, (int)flow, esp_err_to_name(ret));
        return ret;
    }

    /* Restore per-band gains from NVS. */
    for (int band = 0; band < PCM51XX_EQ_BANDS; band++) {
        int gain_db = 0;
        esp_err_t load_ret = pcm51xx_settings_load_eq_gain(band, &gain_db);
        if (load_ret == ESP_ERR_NVS_NOT_FOUND) {
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

    s_settings_applied = true;
    ESP_LOGI(TAG, "%s: Done (flow=%d)", __func__, (int)flow);
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Public API — process flow selection
 * ------------------------------------------------------------------------- */

esp_err_t pcm51xx_settings_save_process_flow(uint8_t flow)
{
    if (s_mutex == NULL) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t ret = settings_nvs_open(&h);
    if (ret == ESP_OK) {
        ret = nvs_set_u8(h, PCM51XX_NVS_KEY_PROC_FLOW, flow);
        if (ret == ESP_OK) ret = nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(s_mutex);

    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "%s: proc_flow=%d saved", __func__, (int)flow);
    } else {
        ESP_LOGW(TAG, "%s: NVS write failed: %s", __func__, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t pcm51xx_settings_load_process_flow(uint8_t *flow)
{
    if (flow == NULL) return ESP_ERR_INVALID_ARG;
    if (s_mutex == NULL) return ESP_ERR_INVALID_STATE;

    *flow = PCM51XX_PROC_FLOW_5; /* default: parametric EQ active */

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t ret = settings_nvs_open(&h);
    if (ret == ESP_OK) {
        uint8_t val = PCM51XX_PROC_FLOW_5;
        ret = nvs_get_u8(h, PCM51XX_NVS_KEY_PROC_FLOW, &val);
        if (ret == ESP_OK) {
            *flow = val;
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

    uint8_t flow = PCM51XX_PROC_FLOW_5;
    pcm51xx_settings_load_process_flow(&flow);
    cJSON_AddNumberToObject(root, PCM51XX_NVS_KEY_PROC_FLOW, (int)flow);

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

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;

    cJSON *groups = cJSON_AddArrayToObject(root, "groups");
    if (!groups) { cJSON_Delete(root); return ESP_ERR_NO_MEM; }

    /* ---- Group 1: Process Flow selector ---- */
    {
        uint8_t cur_flow = PCM51XX_PROC_FLOW_5;
        pcm51xx_settings_load_process_flow(&cur_flow);

        cJSON *grp = cJSON_CreateObject();
        cJSON_AddStringToObject(grp, "name", "Process Flow");
        cJSON_AddStringToObject(grp, "description",
            "DSP signal processing path. Flow 5 activates the 6-band parametric EQ (c10–c39).");

        cJSON *params = cJSON_CreateArray();
        cJSON *param  = cJSON_CreateObject();
        cJSON_AddStringToObject(param, "key",  PCM51XX_NVS_KEY_PROC_FLOW);
        cJSON_AddStringToObject(param, "name", "Process Flow");
        cJSON_AddStringToObject(param, "type", "enum");
        cJSON_AddNumberToObject(param, "current", (int)cur_flow);

        cJSON *values = cJSON_CreateArray();
        const struct { int v; const char *n; } flow_opts[] = {
            { PCM51XX_PROC_FLOW_1, "Flow 1: FIR interpolation (EQ off)" },
            { PCM51XX_PROC_FLOW_2, "Flow 2: Low-latency IIR (EQ off)" },
            { PCM51XX_PROC_FLOW_3, "Flow 3: High-attenuation FIR (EQ off)" },
            { PCM51XX_PROC_FLOW_5, "Flow 5: Parametric EQ (EQ on)" },
            { PCM51XX_PROC_FLOW_7, "Flow 7: Ringing-less FIR (EQ off)" },
        };
        for (int i = 0; i < (int)(sizeof(flow_opts)/sizeof(flow_opts[0])); i++) {
            cJSON *opt = cJSON_CreateObject();
            cJSON_AddNumberToObject(opt, "value", flow_opts[i].v);
            cJSON_AddStringToObject(opt, "name",  flow_opts[i].n);
            cJSON_AddItemToArray(values, opt);
        }
        cJSON_AddItemToObject(param, "values", values);
        cJSON_AddItemToArray(params, param);
        cJSON_AddItemToObject(grp, "parameters", params);
        cJSON_AddItemToArray(groups, grp);
    }

    /* ---- Group 2: Per-band gain sliders (eq-bands layout) ---- */
    {
        cJSON *grp = cJSON_CreateObject();
        cJSON_AddStringToObject(grp, "name", "EQ Bands");
        cJSON_AddStringToObject(grp, "description",
            "6-band parametric equalizer. Active when Process Flow 5 is selected.");
        cJSON_AddStringToObject(grp, "layout", "eq-bands");

        cJSON *params = cJSON_CreateArray();
        for (int band = 0; band < PCM51XX_EQ_BANDS; band++) {
            int cur_gain = 0;
            pcm51xx_settings_load_eq_gain(band, &cur_gain);

            cJSON *item = cJSON_CreateObject();
            char key[16];
            make_gain_key(band, key, sizeof(key));
            cJSON_AddStringToObject(item, "key", key);

            char label[32];
            uint16_t freq = pcm51xx_eq_band_cfg[band].freq_hz;
            if (freq >= 1000u) {
                snprintf(label, sizeof(label), "%d kHz", freq / 1000);
            } else {
                snprintf(label, sizeof(label), "%d Hz", freq);
            }
            cJSON_AddStringToObject(item, "name",    label);
            cJSON_AddStringToObject(item, "type",    "range");
            cJSON_AddStringToObject(item, "unit",    "dB");
            cJSON_AddNumberToObject(item, "min",     pcm51xx_eq_band_cfg[band].min_db);
            cJSON_AddNumberToObject(item, "max",     pcm51xx_eq_band_cfg[band].max_db);
            cJSON_AddNumberToObject(item, "step",    1);
            cJSON_AddNumberToObject(item, "default", 0);
            cJSON_AddNumberToObject(item, "current", cur_gain);
            cJSON_AddItemToArray(params, item);
        }
        cJSON_AddItemToObject(grp, "parameters", params);
        cJSON_AddItemToArray(groups, grp);
    }

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (str == NULL) return ESP_ERR_NO_MEM;
    if (strlen(str) >= max_len) { free(str); return ESP_ERR_NO_MEM; }

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

    /* Process flow selection */
    cJSON *flow_item = cJSON_GetObjectItemCaseSensitive(root, PCM51XX_NVS_KEY_PROC_FLOW);
    if (cJSON_IsNumber(flow_item)) {
        uint8_t flow = (uint8_t)(int)cJSON_GetNumberValue(flow_item);
        esp_err_t ret = pcm51xx_settings_save_process_flow(flow);
        if (ret != ESP_OK) result = ret;

        /* Apply immediately if settings have been applied once. */
        if (s_settings_applied) {
            ret = pcm51xx_set_process_flow(flow);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "%s: live flow apply failed: %s",
                         __func__, esp_err_to_name(ret));
                result = ret;
            }
        }
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

        /* Apply immediately if settings have been applied once. */
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