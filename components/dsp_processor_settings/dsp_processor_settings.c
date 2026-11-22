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
#include <stdlib.h>
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

/*
 * DSP capabilities for the web UI ("GET /capabilities?tab=dsp")
 *
 * The flows and their parameters are described by the const tables below;
 * dsp_settings_write_json() turns them into JSON (format in
 * dsp_processor_settings.h) and adds the current value of every parameter.
 * The JSON is written in small pieces, one flow header or one parameter at a
 * time, so no heap is needed (a cJSON tree for this took ~260 allocations and
 * ~15 KB of heap while the response was sent).
 *
 * To show another flow or parameter in the web UI, add a row to the tables.
 * Strings are copied into the JSON as they are: they must not contain '"' or
 * '\'.
 */

/* One adjustable parameter of a flow, e.g. the bass frequency */
typedef struct {
	const char *key;   // filterParams_t field: "fc_1", "gain_1", "fc_3" or "gain_3"
	const char *name;  // label in the web UI
	const char *unit;
	int min, max, def, step;
} dsp_param_desc_t;

/* One DSP flow and the parameters the web UI can change */
typedef struct {
	dspFlows_t flow;
	const char *id;    // enum name, used by the web UI
	const char *name;
	const char *description;
	const dsp_param_desc_t *params;
	size_t num_params;
} dsp_flow_desc_t;

#define DSP_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static const dsp_param_desc_t eq_params[] = {
	{.key = "fc_1", .name = "Bass Frequency", .unit = "Hz",
	 .min = DSP_BASS_FREQ_MIN, .max = DSP_BASS_FREQ_MAX,
	 .def = DSP_EQ_BASS_FREQ_DEFAULT, .step = DSP_BASS_FREQ_STEP},
	{.key = "gain_1", .name = "Bass Gain", .unit = "dB",
	 .min = DSP_GAIN_MIN, .max = DSP_GAIN_MAX,
	 .def = DSP_EQ_BASS_GAIN_DEFAULT, .step = DSP_GAIN_STEP},
	{.key = "fc_3", .name = "Treble Frequency", .unit = "Hz",
	 .min = DSP_TREBLE_FREQ_MIN, .max = DSP_TREBLE_FREQ_MAX,
	 .def = DSP_EQ_TREBLE_FREQ_DEFAULT, .step = DSP_TREBLE_FREQ_STEP},
	{.key = "gain_3", .name = "Treble Gain", .unit = "dB",
	 .min = DSP_GAIN_MIN, .max = DSP_GAIN_MAX,
	 .def = DSP_EQ_TREBLE_GAIN_DEFAULT, .step = DSP_GAIN_STEP},
};

static const dsp_param_desc_t boost_params[] = {
	{.key = "fc_1", .name = "Bass Frequency", .unit = "Hz",
	 .min = DSP_BASS_FREQ_MIN, .max = DSP_BASS_FREQ_MAX,
	 .def = DSP_BASS_FREQ_DEFAULT, .step = DSP_BASS_FREQ_STEP},
	{.key = "gain_1", .name = "Bass Gain", .unit = "dB",
	 .min = DSP_BASSBOOST_GAIN_MIN, .max = DSP_BASSBOOST_GAIN_MAX,
	 .def = DSP_BASSBOOST_GAIN_DEFAULT, .step = DSP_BASSBOOST_GAIN_STEP},
};

static const dsp_param_desc_t biamp_params[] = {
	{.key = "fc_1", .name = "Low-Pass Frequency", .unit = "Hz",
	 .min = DSP_CROSSOVER_FREQ_MIN, .max = DSP_CROSSOVER_FREQ_MAX,
	 .def = DSP_CROSSOVER_FREQ_DEFAULT, .step = DSP_CROSSOVER_FREQ_STEP},
	{.key = "gain_1", .name = "Low-Pass Gain", .unit = "dB",
	 .min = DSP_GAIN_MIN, .max = DSP_GAIN_MAX,
	 .def = DSP_GAIN_DEFAULT, .step = DSP_GAIN_STEP},
	{.key = "fc_3", .name = "High-Pass Frequency", .unit = "Hz",
	 .min = DSP_CROSSOVER_FREQ_MIN, .max = DSP_CROSSOVER_FREQ_MAX,
	 .def = DSP_CROSSOVER_FREQ_DEFAULT, .step = DSP_CROSSOVER_FREQ_STEP},
	{.key = "gain_3", .name = "High-Pass Gain", .unit = "dB",
	 .min = DSP_GAIN_MIN, .max = DSP_GAIN_MAX,
	 .def = DSP_GAIN_DEFAULT, .step = DSP_GAIN_STEP},
};

/* Flows shown in the web UI, in display order */
static const dsp_flow_desc_t ui_flows[] = {
	{.flow = dspfStereo, .id = "dspfStereo",
	 .name = "Stereo Pass-Through",
	 .description = "No DSP processing, optional soft volume"},
	{.flow = dspfEQBassTreble, .id = "dspfEQBassTreble",
	 .name = "Bass & Treble EQ",
	 .description = "Simple 2-band equalizer with bass and treble controls",
	 .params = eq_params, .num_params = DSP_ARRAY_SIZE(eq_params)},
	{.flow = dspfBassBoost, .id = "dspfBassBoost",
	 .name = "Bass Boost",
	 .description = "Adjustable bass enhancement",
	 .params = boost_params, .num_params = DSP_ARRAY_SIZE(boost_params)},
	{.flow = dspfBiamp, .id = "dspfBiamp",
	 .name = "Bi-Amp Crossover",
	 .description = "Channel 0: Low-pass, Channel 1: High-pass",
	 .params = biamp_params, .num_params = DSP_ARRAY_SIZE(biamp_params)},
};

/* Current value of the parameter named by key */
static float param_current(const filterParams_t *p, const char *key) {
	if (strcmp(key, "fc_1") == 0) return p->fc_1;
	if (strcmp(key, "gain_1") == 0) return p->gain_1;
	if (strcmp(key, "fc_3") == 0) return p->fc_3;
	return p->gain_3;
}

esp_err_t dsp_settings_write_json(dsp_json_write_fn write, void *ctx) {
	if (!write) {
		return ESP_ERR_INVALID_ARG;
	}

	// holds one piece: a parameter is ~140 B, the longest flow header ~150 B
	char buf[192];
	int n;
	esp_err_t err;

	// Default to Stereo pass-through if nothing is stored
	dspFlows_t active_flow = dspfStereo;
	(void)dsp_settings_load_active_flow(&active_flow);

	n = snprintf(buf, sizeof(buf),
				 "{"
				 "\"active_flow\":%d,"
				 "\"flows\":[",
				 (int)active_flow);
	if ((err = write(ctx, buf, n)) != ESP_OK) return err;

	for (size_t f = 0; f < DSP_ARRAY_SIZE(ui_flows); f++) {
		const dsp_flow_desc_t *fl = &ui_flows[f];
		filterParams_t cur = {0};
		if (fl->num_params) {
			dsp_settings_get_flow_params(fl->flow, &cur);
		}

		// flow object, up to the start of its parameter array
		n = snprintf(buf, sizeof(buf),
					 "%s{"                      // comma between flows
					 "\"id\":\"%s\","
					 "\"name\":\"%s\","
					 "\"description\":\"%s\","
					 "\"enum_value\":%d,"
					 "\"parameters\":[",
					 f ? "," : "",
					 fl->id,
					 fl->name,
					 fl->description,
					 (int)fl->flow);
		if (n < 0 || n >= (int)sizeof(buf)) return ESP_ERR_INVALID_SIZE;
		if ((err = write(ctx, buf, n)) != ESP_OK) return err;

		for (size_t i = 0; i < fl->num_params; i++) {
			const dsp_param_desc_t *p = &fl->params[i];

			// one complete parameter object
			n = snprintf(buf, sizeof(buf),
						 "%s{"                  // comma between parameters
						 "\"key\":\"%s\","
						 "\"name\":\"%s\","
						 "\"unit\":\"%s\","
						 "\"min\":%d,"
						 "\"max\":%d,"
						 "\"default\":%d,"
						 "\"step\":%d,"
						 "\"current\":%d"
						 "}",
						 i ? "," : "",
						 p->key,
						 p->name,
						 p->unit,
						 p->min,
						 p->max,
						 p->def,
						 p->step,
						 (int)param_current(&cur, p->key));
			if (n < 0 || n >= (int)sizeof(buf)) return ESP_ERR_INVALID_SIZE;
			if ((err = write(ctx, buf, n)) != ESP_OK) return err;
		}

		// close "parameters" and the flow object
		if ((err = write(ctx, "]}", 2)) != ESP_OK) return err;
	}

	// close "flows" and the root object
	return write(ctx, "]}", 2);
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
		params->fc_1 = (flow == dspfEQBassTreble)
						   ? DSP_EQ_BASS_FREQ_DEFAULT
						   : ((flow == dspfBiamp) ? DSP_CROSSOVER_FREQ_DEFAULT
												  : DSP_BASS_FREQ_DEFAULT);
	}

	if (dsp_settings_load_flow_param(flow, "gain_1", &v) == ESP_OK) {
		params->gain_1 = (float)v;
	} else {
		params->gain_1 =
			(flow == dspfEQBassTreble)
				? DSP_EQ_BASS_GAIN_DEFAULT
				: ((flow == dspfBassBoost) ? DSP_BASSBOOST_GAIN_DEFAULT
										 : DSP_GAIN_DEFAULT);
	}

	if (dsp_settings_load_flow_param(flow, "fc_3", &v) == ESP_OK) {
		params->fc_3 = (float)v;
	} else {
		params->fc_3 = (flow == dspfEQBassTreble)
						   ? DSP_EQ_TREBLE_FREQ_DEFAULT
						   : ((flow == dspfBiamp) ? DSP_CROSSOVER_FREQ_DEFAULT
												  : DSP_TREBLE_FREQ_DEFAULT);
	}

	if (dsp_settings_load_flow_param(flow, "gain_3", &v) == ESP_OK) {
		params->gain_3 = (float)v;
	} else {
		params->gain_3 =
			(flow == dspfEQBassTreble)
				? DSP_EQ_TREBLE_GAIN_DEFAULT
				: DSP_GAIN_DEFAULT;
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

	ESP_LOGI(TAG, "Setting params for flow %d: fc_1=%.1f gain_1=%.1f fc_3=%.1f gain_3=%.1f", 
			 flow, params->fc_1, params->gain_1, params->fc_3, params->gain_3);

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
