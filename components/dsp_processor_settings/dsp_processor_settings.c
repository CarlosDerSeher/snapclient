/**
 * @file dsp_processor_settings.c
 * @brief DSP settings persistence and JSON serialization implementation
 */

#include "dsp_processor_settings.h"

#include "cJSON.h"
#include "dsp_processor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "dsp_settings";
static const char *NVS_NAMESPACE = "dsp_settings";
static const char *NVS_KEY_ACTIVE_FLOW = "active_flow";

// Mutex for thread-safe NVS access
static SemaphoreHandle_t dsp_settings_mutex = NULL;


/**
 * Generate flow-specific NVS key
 * Format: "flow_<id>_<param>" (e.g., "flow_5_fc_1")
 */
static void make_flow_key(char *out_key, size_t out_size, dspFlows_t flow,
						  const char *param) {
	if (!out_key || out_size == 0 || !param) {
		return;
	}

	snprintf(out_key, out_size, "flow_%d_%s", (int)flow, param);
}

esp_err_t dsp_settings_init(void) {
	if (dsp_settings_mutex == NULL) {
		dsp_settings_mutex = xSemaphoreCreateMutex();
		if (dsp_settings_mutex == NULL) {
			ESP_LOGE(TAG, "%s: Failed to create mutex", __func__);
			return ESP_ERR_NO_MEM;
		}
	}

	ESP_LOGI(TAG, "%s: DSP settings manager initialized", __func__);
	// Restore DSP parameters into dsp_processor so the processor has the
	// persisted configuration after both modules are initialized.
	// Note: caller (main) must initialize dsp_processor before calling
	// dsp_settings_init() so these calls succeed.
	dspFlows_t active = dspfStereo;
	if (dsp_settings_load_active_flow(&active) == ESP_OK) {
		ESP_LOGI(TAG, "%s: Restoring DSP active flow %d", __func__, active);

		// For each flow, load stored params and apply to dsp_processor
		for (int f = 0; f < DSP_FLOW_COUNT; f++) {
			filterParams_t params;
			memset(&params, 0, sizeof(params));
			if (dsp_settings_get_flow_params((dspFlows_t)f, &params) == ESP_OK) {
				esp_err_t e = dsp_processor_set_params_for_flow((dspFlows_t)f, &params);
				if (e != ESP_OK) {
					ESP_LOGW(TAG, "%s: Failed to apply params for flow %d: %s", __func__, f, esp_err_to_name(e));
				} else {
					ESP_LOGD(TAG, "%s: Restored params for flow %d: fc_1=%.2f gain_1=%.2f fc_3=%.2f gain_3=%.2f", 
						__func__, f, params.fc_1, params.gain_1, params.fc_3, params.gain_3);
				}
			} else {
				ESP_LOGD(TAG, "%s: No stored params for flow %d", __func__, f);
			}
		}
	}
	
	// Finally, instruct dsp_processor to switch to the active flow
	esp_err_t se = dsp_processor_switch_flow(active);
	if (se != ESP_OK) {
		ESP_LOGW(TAG, "%s: dsp_processor_switch_flow failed: %s", __func__, esp_err_to_name(se));
	}
	return ESP_OK;
}

esp_err_t dsp_settings_save_active_flow(dspFlows_t flow) {
	if (flow < 0 || flow >= DSP_FLOW_COUNT) {
		ESP_LOGE(TAG, "%s: invalid flow %d", __func__, flow);
		return ESP_ERR_INVALID_ARG;
	}

	ESP_LOGD(TAG, "%s: flow=%d", __func__, (int)flow);

	if (!dsp_settings_mutex) {
		ESP_LOGE(TAG, "%s: Not initialized", __func__);
		return ESP_ERR_INVALID_STATE;
	}

	if (xSemaphoreTake(dsp_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
		ESP_LOGE(TAG, "%s: Failed to acquire mutex (timeout)", __func__);
		return ESP_ERR_TIMEOUT;
	}

	nvs_handle_t h;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "%s: Failed to open NVS: %s", __func__,
				 esp_err_to_name(err));
		xSemaphoreGive(dsp_settings_mutex);
		return err;
	}

	err = nvs_set_i32(h, NVS_KEY_ACTIVE_FLOW, (int32_t)flow);
	if (err == ESP_OK) {
		err = nvs_commit(h);
	}

	nvs_close(h);
	xSemaphoreGive(dsp_settings_mutex);

	if (err == ESP_OK) {
		ESP_LOGI(TAG, "%s: Active flow saved: %d", __func__, (int)flow);
	} else {
		ESP_LOGE(TAG, "%s: Failed to save active flow: %s", __func__,
				 esp_err_to_name(err));
	}

	return err;
}

esp_err_t dsp_settings_load_active_flow(dspFlows_t *flow) {
	ESP_LOGD(TAG, "%s: entered", __func__);

	if (!flow)
		return ESP_ERR_INVALID_ARG;
	if (!dsp_settings_mutex)
		return ESP_ERR_INVALID_STATE;

	if (xSemaphoreTake(dsp_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
		return ESP_ERR_TIMEOUT;
	}

	nvs_handle_t h;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
	if (err == ESP_OK) {
		int32_t v = 0;
		err = nvs_get_i32(h, NVS_KEY_ACTIVE_FLOW, &v);
		nvs_close(h);
		if (err == ESP_OK) {
			*flow = (dspFlows_t)v;
			ESP_LOGD(TAG, "%s: Active flow from NVS: %d", __func__, (int)*flow);
		} else if (err != ESP_ERR_NVS_NOT_FOUND) {
			ESP_LOGW(TAG, "%s: NVS read error: %s", __func__,
					 esp_err_to_name(err));
		}
	}

	xSemaphoreGive(dsp_settings_mutex);
	return err;
}

esp_err_t dsp_settings_save_flow_param(dspFlows_t flow, const char *param_name,
									   int32_t value) {
	ESP_LOGD(TAG, "%s: flow=%d param=%s value=%d", __func__, (int)flow,
			 param_name, (int)value);

	if (!param_name)
		return ESP_ERR_INVALID_ARG;
	if (!dsp_settings_mutex)
		return ESP_ERR_INVALID_STATE;

	char key[32];
	make_flow_key(key, sizeof(key), flow, param_name);

	if (xSemaphoreTake(dsp_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
		return ESP_ERR_TIMEOUT;
	}

	nvs_handle_t h;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
	if (err != ESP_OK) {
		xSemaphoreGive(dsp_settings_mutex);
		return err;
	}

	err = nvs_set_i32(h, key, value);
	if (err == ESP_OK) {
		err = nvs_commit(h);
	}

	nvs_close(h);
	xSemaphoreGive(dsp_settings_mutex);

	if (err == ESP_OK) {
		ESP_LOGD(TAG, "%s: Saved %s=%d", __func__, key, (int)value);
	} else {
		ESP_LOGE(TAG, "%s: Failed to save %s: %s", __func__, key,
				 esp_err_to_name(err));
	}

	return err;
}

esp_err_t dsp_settings_load_flow_param(dspFlows_t flow, const char *param_name,
									   int32_t *value) {
	ESP_LOGV(TAG, "%s: flow=%d param=%s", __func__, (int)flow, param_name);

	if (!param_name || !value)
		return ESP_ERR_INVALID_ARG;
	if (!dsp_settings_mutex)
		return ESP_ERR_INVALID_STATE;

	if (flow < 0 || flow >= DSP_FLOW_COUNT) {
		ESP_LOGE(TAG, "%s: invalid flow %d", __func__, flow);
		return ESP_ERR_INVALID_ARG;
	}

	char key[32];
	make_flow_key(key, sizeof(key), flow, param_name);

	if (xSemaphoreTake(dsp_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
		return ESP_ERR_TIMEOUT;
	}

	nvs_handle_t h;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
	if (err == ESP_OK) {
		int32_t v = 0;
		err = nvs_get_i32(h, key, &v);
		nvs_close(h);
		if (err == ESP_OK) {
			*value = v;
			ESP_LOGD(TAG, "%s: Loaded %s=%d", __func__, key, (int)*value);
		} else if (err != ESP_ERR_NVS_NOT_FOUND) {
			ESP_LOGW(TAG, "%s: NVS read error for %s: %s", __func__, key,
					 esp_err_to_name(err));
		}
	}

	xSemaphoreGive(dsp_settings_mutex);
	return err;
}

esp_err_t dsp_settings_get_json(char *json_out, size_t max_len) {
	ESP_LOGI(TAG, "%s: Start - buffer=%p size=%zu", __func__, json_out,
			 max_len);

	if (!json_out || max_len == 0) {
		ESP_LOGE(TAG, "%s: Invalid arguments", __func__);
		return ESP_ERR_INVALID_ARG;
	}

	cJSON *root = cJSON_CreateObject();
	if (!root) {
		ESP_LOGE(TAG, "%s: Failed to create JSON object", __func__);
		return ESP_ERR_NO_MEM;
	}

	// Add active flow. Default to Stereo pass-through if nothing is stored
	// Attempt to load stored value; if not present, keep the default.
	dspFlows_t active_flow = dspfStereo; // default
	(void)dsp_settings_load_active_flow(&active_flow);
	cJSON_AddNumberToObject(root, "active_flow", (int)active_flow);

	// Add flow schema with current values
	cJSON *schema = cJSON_CreateArray();
	if (!schema) {
		cJSON_Delete(root);
		return ESP_ERR_NO_MEM;
	}

	// Flow: dspfStereo (0)
	cJSON *stereo = cJSON_CreateObject();
	cJSON_AddStringToObject(stereo, "id", "dspfStereo");
	cJSON_AddStringToObject(stereo, "name", "Stereo Pass-Through");
	cJSON_AddStringToObject(stereo, "description",
							"No DSP processing, optional soft volume");
	cJSON_AddNumberToObject(stereo, "enum_value", 0);
	cJSON_AddItemToObject(stereo, "parameters", cJSON_CreateArray());
	cJSON_AddItemToArray(schema, stereo);

	// Flow: dspfEQBassTreble (5)
	cJSON *eq = cJSON_CreateObject();
	cJSON_AddStringToObject(eq, "id", "dspfEQBassTreble");
	cJSON_AddStringToObject(eq, "name", "Bass & Treble EQ");
	cJSON_AddStringToObject(
		eq, "description",
		"Simple 2-band equalizer with bass and treble controls");
	cJSON_AddNumberToObject(eq, "enum_value", 5);
	cJSON *eq_params = cJSON_CreateArray();

	// Bass frequency
	cJSON *p1 = cJSON_CreateObject();
	cJSON_AddStringToObject(p1, "key", "fc_1");
	cJSON_AddStringToObject(p1, "name", "Bass Frequency");
	cJSON_AddStringToObject(p1, "unit", "Hz");
	cJSON_AddNumberToObject(p1, "min", (int)DSP_BASS_FREQ_MIN);
	cJSON_AddNumberToObject(p1, "max", (int)DSP_BASS_FREQ_MAX);
	cJSON_AddNumberToObject(p1, "default", (int)DSP_BASS_FREQ_DEFAULT);
	cJSON_AddNumberToObject(p1, "step", (int)DSP_BASS_FREQ_STEP);
	int32_t val_fc1 = 100;
	dsp_settings_load_flow_param(dspfEQBassTreble, "fc_1", &val_fc1);
	cJSON_AddNumberToObject(p1, "current", val_fc1);
	cJSON_AddItemToArray(eq_params, p1);

	// Bass gain
	cJSON *p2 = cJSON_CreateObject();
	cJSON_AddStringToObject(p2, "key", "gain_1");
	cJSON_AddStringToObject(p2, "name", "Bass Gain");
	cJSON_AddStringToObject(p2, "unit", "dB");
	cJSON_AddNumberToObject(p2, "min", (int)DSP_GAIN_MIN);
	cJSON_AddNumberToObject(p2, "max", (int)DSP_GAIN_MAX);
	cJSON_AddNumberToObject(p2, "default", (int)DSP_GAIN_DEFAULT);
	cJSON_AddNumberToObject(p2, "step", (int)DSP_GAIN_STEP);
	int32_t val_g1 = 0;
	dsp_settings_load_flow_param(dspfEQBassTreble, "gain_1", &val_g1);
	cJSON_AddNumberToObject(p2, "current", val_g1);
	cJSON_AddItemToArray(eq_params, p2);

	// Treble frequency
	cJSON *p3 = cJSON_CreateObject();
	cJSON_AddStringToObject(p3, "key", "fc_3");
	cJSON_AddStringToObject(p3, "name", "Treble Frequency");
	cJSON_AddStringToObject(p3, "unit", "Hz");
	cJSON_AddNumberToObject(p3, "min", (int)DSP_TREBLE_FREQ_MIN);
	cJSON_AddNumberToObject(p3, "max", (int)DSP_TREBLE_FREQ_MAX);
	cJSON_AddNumberToObject(p3, "default", (int)DSP_TREBLE_FREQ_DEFAULT);
	cJSON_AddNumberToObject(p3, "step", (int)DSP_TREBLE_FREQ_STEP);
	int32_t val_fc3 = 8000;
	dsp_settings_load_flow_param(dspfEQBassTreble, "fc_3", &val_fc3);
	cJSON_AddNumberToObject(p3, "current", val_fc3);
	cJSON_AddItemToArray(eq_params, p3);

	// Treble gain
	cJSON *p4 = cJSON_CreateObject();
	cJSON_AddStringToObject(p4, "key", "gain_3");
	cJSON_AddStringToObject(p4, "name", "Treble Gain");
	cJSON_AddStringToObject(p4, "unit", "dB");
	cJSON_AddNumberToObject(p4, "min", (int)DSP_GAIN_MIN);
	cJSON_AddNumberToObject(p4, "max", (int)DSP_GAIN_MAX);
	cJSON_AddNumberToObject(p4, "default", (int)DSP_GAIN_DEFAULT);
	cJSON_AddNumberToObject(p4, "step", (int)DSP_GAIN_STEP);
	int32_t val_g3 = 0;
	dsp_settings_load_flow_param(dspfEQBassTreble, "gain_3", &val_g3);
	cJSON_AddNumberToObject(p4, "current", val_g3);
	cJSON_AddItemToArray(eq_params, p4);

	cJSON_AddItemToObject(eq, "parameters", eq_params);
	cJSON_AddItemToArray(schema, eq);

	// Flow: dspfBassBoost (4)
	cJSON *boost = cJSON_CreateObject();
	cJSON_AddStringToObject(boost, "id", "dspfBassBoost");
	cJSON_AddStringToObject(boost, "name", "Bass Boost");
	cJSON_AddStringToObject(boost, "description",
							"Adjustable bass enhancement");
	cJSON_AddNumberToObject(boost, "enum_value", 4);
	cJSON *boost_params = cJSON_CreateArray();

	cJSON *bp1 = cJSON_CreateObject();
	cJSON_AddStringToObject(bp1, "key", "fc_1");
	cJSON_AddStringToObject(bp1, "name", "Bass Frequency");
	cJSON_AddStringToObject(bp1, "unit", "Hz");
	cJSON_AddNumberToObject(bp1, "min", (int)DSP_BASS_FREQ_MIN);
	cJSON_AddNumberToObject(bp1, "max", (int)DSP_BASS_FREQ_MAX);
	cJSON_AddNumberToObject(bp1, "default", (int)DSP_BASS_FREQ_DEFAULT);
	cJSON_AddNumberToObject(bp1, "step", (int)DSP_BASS_FREQ_STEP);
	int32_t val_b_fc1 = 100;
	dsp_settings_load_flow_param(dspfBassBoost, "fc_1", &val_b_fc1);
	cJSON_AddNumberToObject(bp1, "current", val_b_fc1);
	cJSON_AddItemToArray(boost_params, bp1);

	cJSON *bp2 = cJSON_CreateObject();
	cJSON_AddStringToObject(bp2, "key", "gain_1");
	cJSON_AddStringToObject(bp2, "name", "Bass Gain");
	cJSON_AddStringToObject(bp2, "unit", "dB");
	cJSON_AddNumberToObject(bp2, "min", (int)DSP_BASSBOOST_GAIN_MIN);
	cJSON_AddNumberToObject(bp2, "max", (int)DSP_BASSBOOST_GAIN_MAX);
	cJSON_AddNumberToObject(bp2, "default", (int)DSP_BASSBOOST_GAIN_DEFAULT);
	cJSON_AddNumberToObject(bp2, "step", (int)DSP_BASSBOOST_GAIN_STEP);
	int32_t val_b_g1 = 12;
	dsp_settings_load_flow_param(dspfBassBoost, "gain_1", &val_b_g1);
	cJSON_AddNumberToObject(bp2, "current", val_b_g1);
	cJSON_AddItemToArray(boost_params, bp2);

	cJSON_AddItemToObject(boost, "parameters", boost_params);
	cJSON_AddItemToArray(schema, boost);

	// Flow: dspfBiamp (1)
	cJSON *biamp = cJSON_CreateObject();
	cJSON_AddStringToObject(biamp, "id", "dspfBiamp");
	cJSON_AddStringToObject(biamp, "name", "Bi-Amp Crossover");
	cJSON_AddStringToObject(biamp, "description",
							"Channel 0: Low-pass, Channel 1: High-pass");
	cJSON_AddNumberToObject(biamp, "enum_value", 1);
	cJSON *biamp_params = cJSON_CreateArray();

	cJSON *bip1 = cJSON_CreateObject();
	cJSON_AddStringToObject(bip1, "key", "fc_1");
	cJSON_AddStringToObject(bip1, "name", "Low-Pass Frequency");
	cJSON_AddStringToObject(bip1, "unit", "Hz");
	cJSON_AddNumberToObject(bip1, "min", (int)DSP_CROSSOVER_FREQ_MIN);
	cJSON_AddNumberToObject(bip1, "max", (int)DSP_CROSSOVER_FREQ_MAX);
	cJSON_AddNumberToObject(bip1, "default", (int)DSP_CROSSOVER_FREQ_DEFAULT);
	cJSON_AddNumberToObject(bip1, "step", (int)DSP_CROSSOVER_FREQ_STEP);
	int32_t val_bi_fc1 = 200;
	dsp_settings_load_flow_param(dspfBiamp, "fc_1", &val_bi_fc1);
	cJSON_AddNumberToObject(bip1, "current", val_bi_fc1);
	cJSON_AddItemToArray(biamp_params, bip1);

	cJSON *bip2 = cJSON_CreateObject();
	cJSON_AddStringToObject(bip2, "key", "gain_1");
	cJSON_AddStringToObject(bip2, "name", "Low-Pass Gain");
	cJSON_AddStringToObject(bip2, "unit", "dB");
	cJSON_AddNumberToObject(bip2, "min", (int)DSP_GAIN_MIN);
	cJSON_AddNumberToObject(bip2, "max", (int)DSP_GAIN_MAX);
	cJSON_AddNumberToObject(bip2, "default", (int)DSP_GAIN_DEFAULT);
	cJSON_AddNumberToObject(bip2, "step", (int)DSP_GAIN_STEP);
	int32_t val_bi_g1 = 0;
	dsp_settings_load_flow_param(dspfBiamp, "gain_1", &val_bi_g1);
	cJSON_AddNumberToObject(bip2, "current", val_bi_g1);
	cJSON_AddItemToArray(biamp_params, bip2);

	cJSON *bip3 = cJSON_CreateObject();
	cJSON_AddStringToObject(bip3, "key", "fc_3");
	cJSON_AddStringToObject(bip3, "name", "High-Pass Frequency");
	cJSON_AddStringToObject(bip3, "unit", "Hz");
	cJSON_AddNumberToObject(bip3, "min", (int)DSP_CROSSOVER_FREQ_MIN);
	cJSON_AddNumberToObject(bip3, "max", (int)DSP_CROSSOVER_FREQ_MAX);
	cJSON_AddNumberToObject(bip3, "default", (int)DSP_CROSSOVER_FREQ_DEFAULT);
	cJSON_AddNumberToObject(bip3, "step", (int)DSP_CROSSOVER_FREQ_STEP);
	int32_t val_bi_fc3 = 200;
	dsp_settings_load_flow_param(dspfBiamp, "fc_3", &val_bi_fc3);
	cJSON_AddNumberToObject(bip3, "current", val_bi_fc3);
	cJSON_AddItemToArray(biamp_params, bip3);

	cJSON *bip4 = cJSON_CreateObject();
	cJSON_AddStringToObject(bip4, "key", "gain_3");
	cJSON_AddStringToObject(bip4, "name", "High-Pass Gain");
	cJSON_AddStringToObject(bip4, "unit", "dB");
	cJSON_AddNumberToObject(bip4, "min", (int)DSP_GAIN_MIN);
	cJSON_AddNumberToObject(bip4, "max", (int)DSP_GAIN_MAX);
	cJSON_AddNumberToObject(bip4, "default", (int)DSP_GAIN_DEFAULT);
	cJSON_AddNumberToObject(bip4, "step", (int)DSP_GAIN_STEP);
	int32_t val_bi_g3 = 0;
	dsp_settings_load_flow_param(dspfBiamp, "gain_3", &val_bi_g3);
	cJSON_AddNumberToObject(bip4, "current", val_bi_g3);
	cJSON_AddItemToArray(biamp_params, bip4);

	cJSON_AddItemToObject(biamp, "parameters", biamp_params);
	cJSON_AddItemToArray(schema, biamp);

	cJSON_AddItemToObject(root, "flows", schema);

	// Render to string
	char *json_str = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);

	if (!json_str) {
		ESP_LOGE(TAG, "%s: Failed to render JSON", __func__);
		return ESP_ERR_NO_MEM;
	}

	size_t json_len = strlen(json_str);
	ESP_LOGI(TAG, "%s: Generated JSON size: %zu bytes (buffer size: %zu)",
			 __func__, json_len, max_len);

	if (json_len >= max_len) {
		ESP_LOGE(TAG, "%s: JSON too large for buffer (%zu >= %zu)", __func__,
				 json_len, max_len);
		cJSON_free(json_str);
		return ESP_ERR_INVALID_SIZE;
	}

	strncpy(json_out, json_str, max_len - 1);
	json_out[max_len - 1] = '\0';
	cJSON_free(json_str);

	ESP_LOGV(TAG, "%s: JSON generated: %s", __func__, json_out);
	return ESP_OK;
}

esp_err_t dsp_settings_set_from_json(const char *json_in) {
	ESP_LOGD(TAG, "%s: json=%s", __func__, json_in);

	if (!json_in)
		return ESP_ERR_INVALID_ARG;

	cJSON *root = cJSON_Parse(json_in);
	if (!root) {
		ESP_LOGE(TAG, "%s: Failed to parse JSON", __func__);
		return ESP_ERR_INVALID_ARG;
	}

	esp_err_t err = ESP_OK;

	// Update active flow if present
	cJSON *active_flow = cJSON_GetObjectItem(root, "active_flow");
	if (cJSON_IsNumber(active_flow)) {
		err = dsp_settings_save_active_flow((dspFlows_t)active_flow->valueint);
		if (err != ESP_OK) {
			ESP_LOGW(TAG, "%s: Failed to save active_flow", __func__);
		}
	}

	// Iterate through all items and save flow parameters
	// Expecting keys like "flow_5_fc_1", "flow_5_gain_1", etc.
	cJSON *item = NULL;
	cJSON_ArrayForEach(item, root) {
		if (cJSON_IsNumber(item) && item->string) {
			// Parse key format: "flow_X_param"
			if (strncmp(item->string, "flow_", 5) == 0) {
				int flow_id;
				char param_name[16];
				if (sscanf(item->string, "flow_%d_%15s", &flow_id,
						   param_name) == 2) {
					esp_err_t save_err = dsp_settings_save_flow_param(
						(dspFlows_t)flow_id, param_name, item->valueint);
					if (save_err != ESP_OK) {
						ESP_LOGW(TAG, "%s: Failed to save %s", __func__,
								 item->string);
						err = save_err;
					}
				}
			}
		}
	}

	cJSON_Delete(root);
	return err;
}

/**
 * Get current active flow
 */
dspFlows_t dsp_settings_get_active_flow(void) {
	dspFlows_t flow = dspfStereo;
	if (dsp_settings_load_active_flow(&flow) == ESP_OK) {
		return flow;
	}
	return flow;
}

/**
 * Get parameters for a specific flow
 */
esp_err_t dsp_settings_get_flow_params(dspFlows_t flow,
									   filterParams_t *params) {
	if (!params || flow < 0 || flow >= DSP_FLOW_COUNT) {
		return ESP_ERR_INVALID_ARG;
	}

	params->dspFlow = flow;

	int32_t v = 0;
	if (dsp_settings_load_flow_param(flow, "fc_1", &v) == ESP_OK) {
		params->fc_1 = (float)v;
	} else {
		params->fc_1 = DSP_BASS_FREQ_DEFAULT;
	}

	if (dsp_settings_load_flow_param(flow, "gain_1", &v) == ESP_OK) {
		params->gain_1 = (float)v;
	} else {
		params->gain_1 = DSP_GAIN_DEFAULT;
	}

	if (dsp_settings_load_flow_param(flow, "fc_3", &v) == ESP_OK) {
		params->fc_3 = (float)v;
	} else {
		params->fc_3 = DSP_TREBLE_FREQ_DEFAULT;
	}

	if (dsp_settings_load_flow_param(flow, "gain_3", &v) == ESP_OK) {
		params->gain_3 = (float)v;
	} else {
		params->gain_3 = DSP_GAIN_DEFAULT;
	}

	return ESP_OK;
}

/**
 * Set parameters for a specific flow and notify subscribers
 */
esp_err_t dsp_settings_set_flow_params(dspFlows_t flow,
									   const filterParams_t *params) {
	if (!params || flow < 0 || flow >= DSP_FLOW_COUNT) {
		return ESP_ERR_INVALID_ARG;
	}

	ESP_LOGI(TAG, "Setting params for flow %d: fc_1=%.1f gain_1=%.1f", flow,
			 params->fc_1, params->gain_1);

	// Save to NVS
	esp_err_t err = ESP_OK;
	err |= dsp_settings_save_flow_param(flow, "fc_1", (int32_t)params->fc_1);
	err |=
		dsp_settings_save_flow_param(flow, "gain_1", (int32_t)params->gain_1);
	err |= dsp_settings_save_flow_param(flow, "fc_3", (int32_t)params->fc_3);
	err |=
		dsp_settings_save_flow_param(flow, "gain_3", (int32_t)params->gain_3);
	if (err == ESP_OK) {
		// If the flow we just saved is currently active, apply it to the
		// DSP processor. Read active flow from NVS on demand.
		dspFlows_t active = dspfStereo;
		if (dsp_settings_load_active_flow(&active) == ESP_OK && active == flow) {
			esp_err_t e = dsp_processor_set_params_for_flow(flow, params);
			if (e == ESP_OK) {
				ESP_LOGD(TAG, "Applied params to DSP processor for active flow");
			} else {
				ESP_LOGW(TAG, "Failed to apply params to DSP processor: %s",
						 esp_err_to_name(e));
			}
		}
	}

	return err;
}

/**
 * Switch active flow and notify subscribers
 */
esp_err_t dsp_settings_switch_active_flow(dspFlows_t flow) {
	if (flow < 0 || flow >= DSP_FLOW_COUNT) {
		return ESP_ERR_INVALID_ARG;
	}

	ESP_LOGI(TAG, "Switching active flow to %d", flow);

	// Save to NVS
	esp_err_t err = dsp_settings_save_active_flow(flow);
	if (err == ESP_OK) {
		// Get parameters for the new flow and apply to DSP processor
		filterParams_t params;
		if (dsp_settings_get_flow_params(flow, &params) == ESP_OK) {
			esp_err_t e = dsp_processor_set_params_for_flow(flow, &params);
			if (e != ESP_OK) {
				ESP_LOGW(TAG, "Failed to set params for flow on DSP processor: %s",
						 esp_err_to_name(e));
			}
		} else {
			ESP_LOGW(TAG, "Failed to load params for flow %d", flow);
		}

		// Now instruct processor to switch to the new flow
		esp_err_t e = dsp_processor_switch_flow(flow);
		if (e == ESP_OK) {
			ESP_LOGD(TAG, "DSP processor switched to flow %d", flow);
		} else {
			ESP_LOGW(TAG, "DSP processor failed to switch flow: %s",
					 esp_err_to_name(e));
		}
	}

	return err;
}
