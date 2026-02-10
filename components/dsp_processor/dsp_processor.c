

#include <stdint.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#if CONFIG_USE_DSP_PROCESSOR
#include "dsp_processor.h"
#include "dsps_biquad.h"
#include "dsps_biquad_gen.h"
#include "esp_log.h"
#include "player.h"

typedef struct ptype {
  int filtertype;
  float freq;
  float gain;
  float q;
  float *in, *out;
  float coeffs[5];
  float w[2];
} ptype_t;

typedef struct dsp_all_params_s {
  dspFlows_t active_flow;
  struct {
    float fc_1;
    float gain_1;
    float fc_3;
    float gain_3;
  } flow_params[DSP_FLOW_COUNT];
} dsp_all_params_t;

#ifdef CONFIG_USE_BIQUAD_ASM
#define BIQUAD dsps_biquad_f32_ae32
#else
#define BIQUAD dsps_biquad_f32
#endif

static const char *TAG = "dsp_proc";

#define DSP_PROCESSOR_LEN 16

// Legacy queue removed. Use paramsChangedSemaphoreHandle to notify worker of updates.
static SemaphoreHandle_t paramsChangedSemaphoreHandle = NULL;
static SemaphoreHandle_t params_mutex = NULL;

// Centralized parameter storage - one set of parameters per DSP flow
static dsp_all_params_t all_params;

static ptype_t *filter = NULL;

static double dynamic_vol = 1.0;

static bool init = false;

static float *sbuffer0 = NULL;
static float *sbufout0 = NULL;

#if CONFIG_USE_DSP_PROCESSOR
#define SNAPCAST_USE_SOFT_VOL CONFIG_SNAPCLIENT_USE_SOFT_VOL
// Default DSP flow is Stereo, but will be updated from NVS at runtime
dspFlows_t dspFlowInit = dspfStereo;
#endif

/**
 *
 */
void dsp_processor_init(void) {
  ESP_LOGD(TAG, "%s: initializing", __func__);
  init = false;
  // Initialize all_params with defaults for each flow
  memset(&all_params, 0, sizeof(dsp_all_params_t));
  all_params.active_flow = dspFlowInit;
  
  // Set defaults for dspfEQBassTreble
  all_params.flow_params[dspfEQBassTreble].fc_1 = DSP_BASS_FREQ_DEFAULT;
  all_params.flow_params[dspfEQBassTreble].gain_1 = DSP_GAIN_DEFAULT;
  all_params.flow_params[dspfEQBassTreble].fc_3 = DSP_TREBLE_FREQ_DEFAULT;
  all_params.flow_params[dspfEQBassTreble].gain_3 = DSP_GAIN_DEFAULT;
  
  // Set defaults for dspfBassBoost
  all_params.flow_params[dspfBassBoost].fc_1 = DSP_BASS_FREQ_DEFAULT;
  all_params.flow_params[dspfBassBoost].gain_1 = DSP_BASSBOOST_GAIN_DEFAULT;
  
  // Set defaults for dspfBiamp
  all_params.flow_params[dspfBiamp].fc_1 = DSP_CROSSOVER_FREQ_DEFAULT;
  all_params.flow_params[dspfBiamp].gain_1 = DSP_GAIN_DEFAULT;
  all_params.flow_params[dspfBiamp].fc_3 = DSP_CROSSOVER_FREQ_DEFAULT;
  all_params.flow_params[dspfBiamp].gain_3 = DSP_GAIN_DEFAULT;
  
  // dspfStereo has no parameters (pass-through with volume only)
  // dspf2DOT1 and dspfFunkyHonda not yet implemented

  // Note: Do not read settings here to avoid circular dependency. The
  // dsp_processor_settings component will call dsp_processor_set_params_for_flow()
  // and dsp_processor_switch_flow() during its init to apply saved settings.

  if (params_mutex == NULL) {
    params_mutex = xSemaphoreCreateMutex();
    if (params_mutex == NULL) {
      ESP_LOGW(TAG, "%s: failed to create params mutex", __func__);
    }
  }
  if (paramsChangedSemaphoreHandle == NULL) {
    paramsChangedSemaphoreHandle = xSemaphoreCreateBinary();
    if (paramsChangedSemaphoreHandle == NULL) {
      ESP_LOGW(TAG, "%s: failed to create params changed semaphore", __func__);
    } else {
      xSemaphoreTake(paramsChangedSemaphoreHandle, 10);
    }
  }
  ESP_LOGI(TAG, "%s: Initialized with flow=%d, fc_1=%.1f, gain_1=%.1f", __func__,
           all_params.active_flow, 
           all_params.flow_params[all_params.active_flow].fc_1,
           all_params.flow_params[all_params.active_flow].gain_1);

  ESP_LOGI(TAG, "%s: init done", __func__);
}

/**
 * free previously allocated memories
 */
void dsp_processor_uninit(void) {
  ESP_LOGD(TAG, "%s: uninitializing", __func__);
  if (sbuffer0) {
    free(sbuffer0);
    sbuffer0 = NULL;
  }

  if (sbufout0) {
    free(sbufout0);
    sbufout0 = NULL;
  }

  if (filter) {
    free(filter);
    filter = NULL;
  }

  if (params_mutex) {
    vSemaphoreDelete(params_mutex);
    params_mutex = NULL;
  }

  if (paramsChangedSemaphoreHandle) {
    vSemaphoreDelete(paramsChangedSemaphoreHandle);
    paramsChangedSemaphoreHandle = NULL;
  }
  init = false;

  ESP_LOGI(TAG, "%s: uninit done", __func__);
}

/**
 * Update filter parameters
 * Updates centralized storage and queues for worker thread
 */
esp_err_t dsp_processor_update_filter_params(filterParams_t *params) {
  ESP_LOGD(TAG, "%s: updating filter params", __func__);
  
  // Update centralized storage for the current flow
  dspFlows_t flow = params->dspFlow;
  if (flow >= 0 && flow < DSP_FLOW_COUNT) {  // Validate flow index
    // Acquire mutex once, update parameters and set the notification flag
    if (params_mutex && (xSemaphoreTake(params_mutex, portMAX_DELAY) == pdTRUE)) {
      all_params.active_flow = flow;
      all_params.flow_params[flow].fc_1 = params->fc_1;
      all_params.flow_params[flow].gain_1 = params->gain_1;
      all_params.flow_params[flow].fc_3 = params->fc_3;
      all_params.flow_params[flow].gain_3 = params->gain_3;
      xSemaphoreGive(paramsChangedSemaphoreHandle);
      xSemaphoreGive(params_mutex);
    } else {
      // No mutex available: best-effort update
      all_params.active_flow = flow;
      all_params.flow_params[flow].fc_1 = params->fc_1;
      all_params.flow_params[flow].gain_1 = params->gain_1;
      all_params.flow_params[flow].fc_3 = params->fc_3;
      all_params.flow_params[flow].gain_3 = params->gain_3;
      xSemaphoreGive(paramsChangedSemaphoreHandle);
    }
  }
  
  // Worker polls params_changed; we set it while holding the mutex above
  // to ensure atomic visibility. Nothing more to do here.

  return ESP_OK;
}

/**
 *
 */
static int32_t dsp_processor_gen_filter(ptype_t *filter, uint32_t cnt) {
  ESP_LOGD(TAG, "%s: generating %lu filters", __func__, (unsigned long)cnt);
  if ((filter == NULL) && (cnt > 0)) {
    return ESP_FAIL;
  }

  for (int n = 0; n < cnt; n++) {
    switch (filter[n].filtertype) {
      case HIGHSHELF:
        dsps_biquad_gen_highShelf_f32(filter[n].coeffs, filter[n].freq,
                                      filter[n].gain, filter[n].q);
        break;

      case LOWSHELF:
        dsps_biquad_gen_lowShelf_f32(filter[n].coeffs, filter[n].freq,
                                     filter[n].gain, filter[n].q);
        break;

      case LPF:
        dsps_biquad_gen_lpf_f32(filter[n].coeffs, filter[n].freq, filter[n].q);
        break;

      case HPF:
        dsps_biquad_gen_hpf_f32(filter[n].coeffs, filter[n].freq, filter[n].q);
        break;

      default:
        break;
    }
  }

  return ESP_OK;
}

/**
 *
 */
int dsp_processor_worker(void *p_pcmChnk, const void *p_scSet) {
  ESP_LOGV(TAG, "%s: processing audio chunk", __func__);
  const snapcastSetting_t *scSet = (const snapcastSetting_t *)p_scSet;
  pcm_chunk_message_t *pcmChnk = (pcm_chunk_message_t *)p_pcmChnk;
  uint32_t samplerate = scSet->sr;

  if (!pcmChnk || !pcmChnk->fragment->payload) {
    return -1;
  }

  int bits = scSet->bits;
  int ch = scSet->ch;

  if (bits == 0) {
    bits = 16;
  }

  if (ch == 0) {
    ch = 2;
  }

  if (samplerate == 0) {
    samplerate = 44100;
    ESP_LOGW(TAG, "%s: Sample rate is not set, using default: %lu", __func__, (unsigned long)samplerate);
  }

  int16_t len = pcmChnk->fragment->size / ((bits / 8) * ch);
  int16_t valint;
  uint16_t i;
  // volatile needed to ensure 32 bit access
  volatile uint32_t *audio_tmp =
      (volatile uint32_t *)(pcmChnk->fragment->payload);
  
  // Local working copy of filter parameters
  static filterParams_t currentFilterParams = {0};
  static bool paramsInitialized = false;
  
  // Initialize on first run
  if (!paramsInitialized) {
    currentFilterParams.dspFlow = all_params.active_flow;
    currentFilterParams.fc_1 = all_params.flow_params[all_params.active_flow].fc_1;
    currentFilterParams.gain_1 = all_params.flow_params[all_params.active_flow].gain_1;
    currentFilterParams.fc_3 = all_params.flow_params[all_params.active_flow].fc_3;
    currentFilterParams.gain_3 = all_params.flow_params[all_params.active_flow].gain_3;
    paramsInitialized = true;
  }

  // If parameters were changed by API, copy them from centralized storage
  if (xSemaphoreTake(paramsChangedSemaphoreHandle, 0) == pdTRUE) {
    // Copy under mutex to avoid torn reads
    if (params_mutex) {
      xSemaphoreTake(params_mutex, portMAX_DELAY);
    } else {
      ESP_LOGW(TAG, "%s: params mutex not available, proceeding without mutex", __func__);
    }
    
    dspFlows_t aflow = all_params.active_flow;
    currentFilterParams.dspFlow = aflow;
    currentFilterParams.fc_1 = all_params.flow_params[aflow].fc_1;
    currentFilterParams.gain_1 = all_params.flow_params[aflow].gain_1;
    currentFilterParams.fc_3 = all_params.flow_params[aflow].fc_3;
    currentFilterParams.gain_3 = all_params.flow_params[aflow].gain_3;
    if (params_mutex) {
      xSemaphoreGive(params_mutex);
    } else {
      ESP_LOGW(TAG, "%s: params mutex not available, proceeding without mutex", __func__);
    }

    ESP_LOGI(TAG, "Applying filter update: flow=%d", currentFilterParams.dspFlow);
    init = false;
  }

  dspFlows_t dspFlow = currentFilterParams.dspFlow;

  if (init == false) {
    uint32_t cnt = 0;

    if (filter) {
      free(filter);
      filter = NULL;
    }

    switch (dspFlow) {
      case dspfEQBassTreble: {
        cnt = 4;

        filter =
            (ptype_t *)heap_caps_malloc(sizeof(ptype_t) * cnt, MALLOC_CAP_8BIT);
        if (filter) {
          // simple EQ control of low and high frequencies (bass, treble)
          float bass_fc = currentFilterParams.fc_1 / samplerate;
          float bass_gain = currentFilterParams.gain_1;
          float treble_fc = currentFilterParams.fc_3 / samplerate;
          float treble_gain = currentFilterParams.gain_3;

          // filters for CH 0
          filter[0] = (ptype_t){LOWSHELF, bass_fc, bass_gain,       0.707,
                                NULL,     NULL,    {0, 0, 0, 0, 0}, {0, 0}};
          filter[1] = (ptype_t){HIGHSHELF, treble_fc, treble_gain,     0.707,
                                NULL,      NULL,      {0, 0, 0, 0, 0}, {0, 0}};
          // filters for CH 1
          filter[2] = (ptype_t){LOWSHELF, bass_fc, bass_gain,       0.707,
                                NULL,     NULL,    {0, 0, 0, 0, 0}, {0, 0}};
          filter[3] = (ptype_t){HIGHSHELF, treble_fc, treble_gain,     0.707,
                                NULL,      NULL,      {0, 0, 0, 0, 0}, {0, 0}};

          ESP_LOGI(TAG, "got new setting for dspfEQBassTreble");
        } else {
          ESP_LOGE(TAG, "failed to get memory for filter");
        }

        break;
      }

      case dspfStereo: {
        cnt = 0;
        break;
      }

      case dspfBassBoost: {
        cnt = 2;

        filter =
            (ptype_t *)heap_caps_malloc(sizeof(ptype_t) * cnt, MALLOC_CAP_8BIT);
        if (filter) {
          float bass_fc = currentFilterParams.fc_1 / samplerate;
          float bass_gain = currentFilterParams.gain_1;

          filter[0] = (ptype_t){LOWSHELF, bass_fc, bass_gain,       0.707,
                                NULL,     NULL,    {0, 0, 0, 0, 0}, {0, 0}};
          filter[1] = (ptype_t){LOWSHELF, bass_fc, bass_gain,       0.707,
                                NULL,     NULL,    {0, 0, 0, 0, 0}, {0, 0}};

          ESP_LOGI(TAG, "got new setting for dspfBassBoost: fc=%.1f gain=%.1f", 
                   currentFilterParams.fc_1, currentFilterParams.gain_1);
        } else {
          ESP_LOGE(TAG, "failed to get memory for filter");
        }

        break;
      }

      case dspfBiamp: {
        cnt = 4;

        filter =
            (ptype_t *)heap_caps_malloc(sizeof(ptype_t) * cnt, MALLOC_CAP_8BIT);
        if (filter) {
          float lp_fc = currentFilterParams.fc_1 / samplerate;
          float lp_gain = currentFilterParams.gain_1;
          float hp_fc = currentFilterParams.fc_3 / samplerate;
          float hp_gain = currentFilterParams.gain_3;

          filter[0] = (ptype_t){LPF,  lp_fc, lp_gain,         0.707,
                                NULL, NULL,  {0, 0, 0, 0, 0}, {0, 0}};
          filter[1] = (ptype_t){LPF,  lp_fc, lp_gain,         0.707,
                                NULL, NULL,  {0, 0, 0, 0, 0}, {0, 0}};
          filter[2] = (ptype_t){HPF,  hp_fc, hp_gain,         0.707,
                                NULL, NULL,  {0, 0, 0, 0, 0}, {0, 0}};
          filter[3] = (ptype_t){HPF,  hp_fc, hp_gain,         0.707,
                                NULL, NULL,  {0, 0, 0, 0, 0}, {0, 0}};

          ESP_LOGI(TAG, "got new setting for dspfBiamp");
        } else {
          ESP_LOGE(TAG, "failed to get memory for filter");
        }

        break;
      }

      case dspf2DOT1: {  // Process audio L + R LOW PASS FILTER
        cnt = 0;
        dspFlow = dspfStereo;

        ESP_LOGW(TAG, "dspf2DOT1, not implemented yet, using stereo instead");
      } break;

      case dspfFunkyHonda: {  // Process audio L + R LOW PASS FILTER
        cnt = 0;
        dspFlow = dspfStereo;

        ESP_LOGW(TAG,
                 "dspfFunkyHonda, not implemented yet, using stereo instead");
        break;
      }

      default: {
        break;
      }
    }

    dsp_processor_gen_filter(filter, cnt);

    init = true;
  }

  // only process data if it is valid
  if (audio_tmp) {
    sbuffer0 = (float *)heap_caps_malloc(sizeof(float) * DSP_PROCESSOR_LEN,
                                         MALLOC_CAP_8BIT);
    if (sbuffer0 == NULL) {
      ESP_LOGE(TAG, "No Memory allocated for dsp_processor sbuffer0");

      return -1;
    }

    sbufout0 = (float *)heap_caps_malloc(sizeof(float) * DSP_PROCESSOR_LEN,
                                         MALLOC_CAP_8BIT);
    if (sbufout0 == NULL) {
      ESP_LOGE(TAG, "No Memory allocated for dsp_processor sbufout0");

      free(sbuffer0);

      return -1;
    }

#if CONFIG_SNAPCLIENT_MIX_LR_TO_MONO
    if (ch == 2) {
      for (int k = 0; k < len; k += DSP_PROCESSOR_LEN) {
        volatile uint32_t *tmp = (uint32_t *)(&audio_tmp[k]);
        uint32_t max = DSP_PROCESSOR_LEN;
        uint32_t test = len - k;

        if (test < DSP_PROCESSOR_LEN) {
          max = test;
        }

        for (i = 0; i < max; i++) {
          int16_t channel0 = (int16_t)((tmp[i] & 0xFFFF0000) >> 16);
          int16_t channel1 = (int16_t)(tmp[i] & 0x0000FFFF);
          int16_t mixMono = ((int32_t)channel0 + (int32_t)channel1) / 2;

          tmp[i] = ((uint32_t)mixMono << 16) | ((uint32_t)mixMono & 0x0000FFFF);
        }
      }
    }
#endif

    switch (dspFlow) {
      case dspfEQBassTreble: {
        for (int k = 0; k < len; k += DSP_PROCESSOR_LEN) {
          volatile uint32_t *tmp = (uint32_t *)(&audio_tmp[k]);
          uint32_t max = DSP_PROCESSOR_LEN;
          uint32_t test = len - k;

          if (test < DSP_PROCESSOR_LEN) {
            max = test;
          }

          // channel 0
          for (i = 0; i < max; i++) {
            sbuffer0[i] = dynamic_vol * /*0.5 **/
                          ((float)((int16_t)(tmp[i] & 0xFFFF))) / INT16_MAX;
          }

          // BASS
          BIQUAD(sbuffer0, sbufout0, max, filter[0].coeffs, filter[0].w);
          // TREBLE
          BIQUAD(sbufout0, sbuffer0, max, filter[1].coeffs, filter[1].w);

          for (i = 0; i < max; i++) {
            valint = (int16_t)(sbuffer0[i] * INT16_MAX);
            tmp[i] =
                (volatile uint32_t)((tmp[i] & 0xFFFF0000) + (uint32_t)valint);
          }

          // channel 1
          for (i = 0; i < max; i++) {
            sbuffer0[i] = dynamic_vol * /*0.5 **/
                          ((float)((int16_t)((tmp[i] & 0xFFFF0000) >> 16))) /
                          INT16_MAX;
          }

          // BASS
          BIQUAD(sbuffer0, sbufout0, max, filter[2].coeffs, filter[2].w);
          // TREBLE
          BIQUAD(sbufout0, sbuffer0, max, filter[3].coeffs, filter[3].w);

          for (i = 0; i < max; i++) {
            valint = (int16_t)(sbuffer0[i] * INT16_MAX);
            tmp[i] = (volatile uint32_t)((tmp[i] & 0xFFFF) +
                                         ((uint32_t)valint << 16));
          }
        }

        break;
      }

      case dspfStereo: {
#if SNAPCAST_USE_SOFT_VOL
        for (int k = 0; k < len; k += DSP_PROCESSOR_LEN) {
          volatile uint32_t *tmp = (uint32_t *)(&audio_tmp[k]);
          uint32_t max = DSP_PROCESSOR_LEN;
          uint32_t test = len - k;

          if (test < DSP_PROCESSOR_LEN) {
            max = test;
          }

          // set volume
          if (dynamic_vol != 1.0) {
            for (i = 0; i < max; i++) {
              tmp[i] =
                  ((uint32_t)(dynamic_vol *
                              ((float)((int16_t)((tmp[i] & 0xFFFF0000) >> 16))))
                   << 16) +
                  (uint32_t)(dynamic_vol *
                             ((float)((int16_t)(tmp[i] & 0xFFFF))));
            }
          }
        }
#endif

        break;
      }

      case dspfBassBoost: {  // Low shelf bass boost with adjustable gain
        for (int k = 0; k < len; k += DSP_PROCESSOR_LEN) {
          volatile uint32_t *tmp = (uint32_t *)(&audio_tmp[k]);
          uint32_t max = DSP_PROCESSOR_LEN;
          uint32_t test = len - k;

          if (test < DSP_PROCESSOR_LEN) {
            max = test;
          }

          // channel 0
          for (i = 0; i < max; i++) {
            sbuffer0[i] =
                0.5 * ((float)((int16_t)(tmp[i] & 0xFFFF))) / INT16_MAX;
#if SNAPCAST_USE_SOFT_VOL
            sbuffer0[i] *= dynamic_vol;
#endif
          }
          BIQUAD(sbuffer0, sbufout0, max, filter[0].coeffs, filter[0].w);

          for (i = 0; i < max; i++) {
            valint = (int16_t)(sbufout0[i] * INT16_MAX);
            tmp[i] = (tmp[i] & 0xFFFF0000) + (uint32_t)valint;
          }

          // channel 1
          for (i = 0; i < max; i++) {
            sbuffer0[i] = 0.5 *
                          ((float)((int16_t)((tmp[i] & 0xFFFF0000) >> 16))) /
                          INT16_MAX;
#if SNAPCAST_USE_SOFT_VOL
            sbuffer0[i] *= dynamic_vol;
#endif
          }
          BIQUAD(sbuffer0, sbufout0, max, filter[1].coeffs, filter[1].w);

          for (i = 0; i < max; i++) {
            valint = (int16_t)(sbufout0[i] * INT16_MAX);
            tmp[i] = (tmp[i] & 0xFFFF) + ((uint32_t)valint << 16);
          }
        }

        break;
      }

      case dspfBiamp: {
        for (int k = 0; k < len; k += DSP_PROCESSOR_LEN) {
          volatile uint32_t *tmp = (uint32_t *)(&audio_tmp[k]);
          uint32_t max = DSP_PROCESSOR_LEN;
          uint32_t test = len - k;

          if (test < DSP_PROCESSOR_LEN) {
            max = test;
          }

          // Process audio ch0 LOW PASS FILTER
          for (i = 0; i < max; i++) {
            sbuffer0[i] =
                0.5 * ((float)((int16_t)(tmp[i] & 0xFFFF))) / INT16_MAX;
#if SNAPCAST_USE_SOFT_VOL
            sbuffer0[i] *= dynamic_vol;
#endif
          }
          BIQUAD(sbuffer0, sbufout0, max, filter[0].coeffs, filter[0].w);
          BIQUAD(sbufout0, sbuffer0, max, filter[1].coeffs, filter[1].w);

          for (i = 0; i < max; i++) {
            valint = (int16_t)(sbuffer0[i] * INT16_MAX);
            tmp[i] = (tmp[i] & 0xFFFF0000) + (uint32_t)valint;
          }

          // Process audio ch1 HIGH PASS FILTER
          for (i = 0; i < max; i++) {
            sbuffer0[i] = 0.5 *
                          ((float)((int16_t)((tmp[i] & 0xFFFF0000) >> 16))) /
                          INT16_MAX;
#if SNAPCAST_USE_SOFT_VOL
            sbuffer0[i] *= dynamic_vol;
#endif
          }
          BIQUAD(sbuffer0, sbufout0, max, filter[2].coeffs, filter[2].w);
          BIQUAD(sbufout0, sbuffer0, max, filter[3].coeffs, filter[3].w);

          for (i = 0; i < max; i++) {
            valint = (int16_t)(sbuffer0[i] * INT16_MAX);
            tmp[i] = (tmp[i] & 0xFFFF) + ((uint32_t)valint << 16);
          }
        }

        break;
      }

      case dspf2DOT1: {  // Process audio L + R LOW PASS FILTER
        /*
           BIQUAD(sbuffer2, sbuftmp0, len, bq[0].coeffs, bq[0].w);
           BIQUAD(sbuftmp0, sbufout2, len, bq[1].coeffs, bq[1].w);

           // Process audio L HIGH PASS FILTER
           BIQUAD(sbuffer0, sbuftmp0, len, bq[2].coeffs, bq[2].w);
           BIQUAD(sbuftmp0, sbufout0, len, bq[3].coeffs, bq[3].w);

           // Process audio R HIGH PASS FILTER
           BIQUAD(sbuffer1, sbuftmp0, len, bq[4].coeffs, bq[4].w);
           BIQUAD(sbuftmp0, sbufout1, len, bq[5].coeffs, bq[5].w);

           int16_t valint[5];
           for (uint16_t i = 0; i < len; i++) {
             valint[0] =
                 (muteCH[0] == 1) ? (int16_t)0 : (int16_t)(sbufout0[i] *
           INT16_MAX); valint[1] = (muteCH[1] == 1) ? (int16_t)0 :
           (int16_t)(sbufout1[i] * INT16_MAX); valint[2] = (muteCH[2] == 1) ?
           (int16_t)0 : (int16_t)(sbufout2[i] * INT16_MAX); dsp_audio[i * 4 + 0]
           = (valint[2] & 0xff); dsp_audio[i * 4 + 1] = ((valint[2] & 0xff00) >>
           8); dsp_audio[i * 4 + 2] = 0; dsp_audio[i * 4 + 3] = 0;

             dsp_audio1[i * 4 + 0] = (valint[0] & 0xff);
             dsp_audio1[i * 4 + 1] = ((valint[0] & 0xff00) >> 8);
             dsp_audio1[i * 4 + 2] = (valint[1] & 0xff);
             dsp_audio1[i * 4 + 3] = ((valint[1] & 0xff00) >> 8);
           }

           // TODO: this copy could be avoided if dsp_audio buffers are
           // allocated dynamically and pointers are exchanged after
           // audio was freed
           memcpy(audio, dsp_audio, chunk_size);

           ESP_LOGW(TAG, "Don't know what to do with dsp_audio1");
     */
        ESP_LOGW(TAG, "dspf2DOT1, not implemented yet, using stereo instead");
      } break;

      case dspfFunkyHonda: {  // Process audio L + R LOW PASS FILTER
        /*
          BIQUAD(sbuffer2, sbuftmp0, len, bq[0].coeffs, bq[0].w);
          BIQUAD(sbuftmp0, sbufout2, len, bq[1].coeffs, bq[1].w);

          // Process audio L HIGH PASS FILTER
          BIQUAD(sbuffer0, sbuftmp0, len, bq[2].coeffs, bq[2].w);
          BIQUAD(sbuftmp0, sbufout0, len, bq[3].coeffs, bq[3].w);

          // Process audio R HIGH PASS FILTER
          BIQUAD(sbuffer1, sbuftmp0, len, bq[4].coeffs, bq[4].w);
          BIQUAD(sbuftmp0, sbufout1, len, bq[5].coeffs, bq[5].w);

          uint16_t scale = 16384;  // INT16_MAX
          int16_t valint[5];
          for (uint16_t i = 0; i < len; i++) {
            valint[0] =
                (muteCH[0] == 1) ? (int16_t)0 : (int16_t)(sbufout0[i] * scale);
            valint[1] =
                (muteCH[1] == 1) ? (int16_t)0 : (int16_t)(sbufout1[i] * scale);
            valint[2] =
                (muteCH[2] == 1) ? (int16_t)0 : (int16_t)(sbufout2[i] * scale);
            valint[3] = valint[0] + valint[2];
            valint[4] = -valint[2];
            valint[5] = -valint[1] - valint[2];
            dsp_audio[i * 4 + 0] = (valint[3] & 0xff);
            dsp_audio[i * 4 + 1] = ((valint[3] & 0xff00) >> 8);
            dsp_audio[i * 4 + 2] = (valint[2] & 0xff);
            dsp_audio[i * 4 + 3] = ((valint[2] & 0xff00) >> 8);

            dsp_audio1[i * 4 + 0] = (valint[4] & 0xff);
            dsp_audio1[i * 4 + 1] = ((valint[4] & 0xff00) >> 8);
            dsp_audio1[i * 4 + 2] = (valint[5] & 0xff);
            dsp_audio1[i * 4 + 3] = ((valint[5] & 0xff00) >> 8);
          }

          // TODO: this copy could be avoided if dsp_audio buffers are
          // allocated dynamically and pointers are exchanged after
          // audio was freed
          memcpy(audio, dsp_audio, chunk_size);

          ESP_LOGW(TAG, "Don't know what to do with dsp_audio1");
          */
        ESP_LOGW(TAG,
                 "dspfFunkyHonda, not implemented yet, using stereo instead");
      } break;

      default: {
      } break;
    }

    free(sbuffer0);
    sbuffer0 = NULL;

    free(sbufout0);
    sbufout0 = NULL;
  }

  return 0;
}

// void dsp_set_xoverfreq(uint8_t freqh, uint8_t freql, uint32_t samplerate) {
//  float freq = freqh * 256 + freql;
//  //  printf("%f\n", freq);
//  float f = freq / samplerate / 2.;
//  for (int8_t n = 0; n <= 5; n++) {
//    bq[n].freq = f;
//    switch (bq[n].filtertype) {
//      case LPF:
//        //        for (uint8_t i = 0; i <= 4; i++) {
//        //          printf("%.6f ", bq[n].coeffs[i]);
//        //        }
//        //        printf("\n");
//        dsps_biquad_gen_lpf_f32(bq[n].coeffs, bq[n].freq, bq[n].q);
//        //        for (uint8_t i = 0; i <= 4; i++) {
//        //          printf("%.6f ", bq[n].coeffs[i]);
//        //        }
//        //        printf("%f \n", bq[n].freq);
//        break;
//      case HPF:
//        dsps_biquad_gen_hpf_f32(bq[n].coeffs, bq[n].freq, bq[n].q);
//        break;
//      default:
//        break;
//    }
//  }
//}

/**
 *
 */
void dsp_processor_set_volome(double volume) {
  ESP_LOGD(TAG, "%s: volume=%f", __func__, volume);
  if (volume >= 0 && volume <= 1.0) {
    ESP_LOGI(TAG, "Set volume to %f", volume);
    dynamic_vol = volume;
  }
}
/**
 * Set parameters for a specific flow (without switching to it)
 */
esp_err_t dsp_processor_set_params_for_flow(dspFlows_t flow, const filterParams_t *params) {
  ESP_LOGD(TAG, "%s: setting params for flow %d", __func__, flow);
  
  if (params == NULL || flow < 0 || flow >= DSP_FLOW_COUNT) {
    return ESP_ERR_INVALID_ARG;
  }
  
  // Update centralized storage for this specific flow
  if (params_mutex) {
    xSemaphoreTake(params_mutex, portMAX_DELAY);
  } else {
    ESP_LOGW(TAG, "%s: params mutex not available, proceeding without mutex", __func__);
  }

  all_params.flow_params[flow].fc_1 = params->fc_1;
  all_params.flow_params[flow].gain_1 = params->gain_1;
  all_params.flow_params[flow].fc_3 = params->fc_3;
  all_params.flow_params[flow].gain_3 = params->gain_3;
  if (params_mutex) {
    xSemaphoreGive(params_mutex);
  } else {
    ESP_LOGW(TAG, "%s: params mutex not available, proceeding without mutex", __func__);
  }
  
  // If this is the active flow, also update the legacy filterParams and notify worker
  if (params_mutex) {
    xSemaphoreTake(params_mutex, portMAX_DELAY);
  } else {
    ESP_LOGW(TAG, "%s: params mutex not available, proceeding without mutex", __func__);
  }
  
    bool is_active = (flow == all_params.active_flow);
  if (params_mutex) {
    xSemaphoreGive(params_mutex);
  } else {
    ESP_LOGW(TAG, "%s: params mutex not available, proceeding without mutex", __func__);
  }

  if (is_active) {
    filterParams_t temp_params;
    temp_params.dspFlow = flow;
    temp_params.fc_1 = params->fc_1;
    temp_params.gain_1 = params->gain_1;
    temp_params.fc_3 = params->fc_3;
    temp_params.gain_3 = params->gain_3;
    
    return dsp_processor_update_filter_params(&temp_params);
  }
  
  return ESP_OK;
}

/**
 * Switch to a different DSP flow
 */
esp_err_t dsp_processor_switch_flow(dspFlows_t flow) {
  if (flow < 0 || flow >= DSP_FLOW_COUNT) {
    ESP_LOGE(TAG, "%s: invalid flow %d", __func__, flow);
    return ESP_ERR_INVALID_ARG;
  }
  
  ESP_LOGI(TAG, "%s: switching from flow %d to %d", __func__, all_params.active_flow, flow);
  // Set active flow under mutex and capture params to apply
  if (params_mutex) {
    xSemaphoreTake(params_mutex, portMAX_DELAY);
  } else {
    ESP_LOGW(TAG, "%s: params mutex not available, proceeding without mutex", __func__);
  }

  all_params.active_flow = flow;
  filterParams_t params;
  params.dspFlow = flow;
  params.fc_1 = all_params.flow_params[flow].fc_1;
  params.gain_1 = all_params.flow_params[flow].gain_1;
  params.fc_3 = all_params.flow_params[flow].fc_3;
  params.gain_3 = all_params.flow_params[flow].gain_3;
  if (params_mutex) {
    xSemaphoreGive(params_mutex);
  } else {
    ESP_LOGW(TAG, "%s: params mutex not available, proceeding without mutex", __func__);
  }

  return dsp_processor_update_filter_params(&params);
}

#endif
