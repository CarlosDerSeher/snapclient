#ifndef _DSP_PROCESSOR_H_
#define _DSP_PROCESSOR_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"

/**
 * DSP Parameter Limits - Configurable at compile time
 * 
 * These defines control the min/max/default values for all DSP parameters
 * exposed in the UI. Modify these values before compilation to set appropriate
 * limits for your audio system.
 * 
 * All frequency values are in Hz
 * All gain values are in dB
 */

#define DSP_BASS_FREQ_MIN        30.0f
#define DSP_BASS_FREQ_MAX       500.0f
#define DSP_BASS_FREQ_DEFAULT   150.0f
#define DSP_BASS_FREQ_STEP        5.0f

#define DSP_TREBLE_FREQ_MIN      2000.0f
#define DSP_TREBLE_FREQ_MAX     16000.0f
#define DSP_TREBLE_FREQ_DEFAULT  8000.0f
#define DSP_TREBLE_FREQ_STEP      100.0f

#define DSP_GAIN_MIN            -15.0f
#define DSP_GAIN_MAX             15.0f
#define DSP_GAIN_DEFAULT          0.0f
#define DSP_GAIN_STEP             1.0f

#define DSP_BASSBOOST_GAIN_MIN     -18.0f
#define DSP_BASSBOOST_GAIN_MAX      18.0f
#define DSP_BASSBOOST_GAIN_DEFAULT   9.0f
#define DSP_BASSBOOST_GAIN_STEP      1.0f

#define DSP_CROSSOVER_FREQ_MIN        80.0f
#define DSP_CROSSOVER_FREQ_MAX      3000.0f
#define DSP_CROSSOVER_FREQ_DEFAULT   500.0f
#define DSP_CROSSOVER_FREQ_STEP       10.0f

typedef enum dspFlows {
  dspfStereo,
  dspfBiamp,
  dspf2DOT1,
  dspfFunkyHonda,
  dspfBassBoost,
  dspfEQBassTreble,
} dspFlows_t;

enum filtertypes {
  LPF,
  HPF,
  BPF,
  BPF0DB,
  NOTCH,
  ALLPASS360,
  ALLPASS180,
  PEAKINGEQ,
  LOWSHELF,
  HIGHSHELF
};

// Each audio processor node consist of a data struct holding the
// required weights and states for processing an automomous processing
// function. The high level parameters is maintained in the structure
// as well
// Process node
typedef struct ptype {
  int filtertype;
  float freq;
  float gain;
  float q;
  float *in, *out;
  float coeffs[5];
  float w[2];
} ptype_t;

// used to dynamically change used filters and their parameters
typedef struct filterParams_s {
  dspFlows_t dspFlow;
  float fc_1;
  float gain_1;
  float fc_2;
  float gain_2;
  float fc_3;
  float gain_3;
} filterParams_t;

/**
 * Centralized parameter storage for all DSP flows
 * Each DSP flow has its own isolated parameter set to prevent collisions
 * when switching between flows. This ensures that changing "gain" in one
 * flow doesn't affect "gain" in another flow.
 */
typedef struct dsp_all_params_s {
  dspFlows_t active_flow;  // Currently active DSP flow
  
  // Parameters for each DSP flow (indexed by dspFlows_t enum)
  struct {
    float fc_1;    // Primary frequency (bass/crossover)
    float gain_1;  // Primary gain (bass/boost)
    float fc_2;    // Secondary frequency
    float gain_2;  // Secondary gain
    float fc_3;    // Tertiary frequency (treble/high crossover)
    float gain_3;  // Tertiary gain (treble)
  } flow_params[6];  // One entry per dspFlows_t enum value
} dsp_all_params_t;

// TODO: this is unused, remove???
// Process flow
typedef struct pnode {
  ptype_t process;
  struct pnode *next;
} pnode_t;

void dsp_processor_init(void);
void dsp_processor_uninit(void);
int dsp_processor_worker(void *pcmChnk, const void *scSet);
esp_err_t dsp_processor_update_filter_params(filterParams_t *params);
void dsp_processor_set_volome(double volume);

/**
 * Get current DSP flow
 */
dspFlows_t dsp_processor_get_current_flow(void);

/**
 * Get all parameters (centralized storage for all flows)
 * Returns pointer to internal storage - do not modify directly
 */
const dsp_all_params_t* dsp_processor_get_all_params(void);

/**
 * Get parameters for a specific flow
 * @param flow The DSP flow to get parameters for
 * @param params Output parameter structure
 * @return ESP_OK on success
 */
esp_err_t dsp_processor_get_params_for_flow(dspFlows_t flow, filterParams_t *params);

/**
 * Set parameters for a specific flow (without switching to it)
 * This allows updating parameters for a flow that's not currently active
 * @param flow The DSP flow to set parameters for
 * @param params The parameters to set
 * @return ESP_OK on success
 */
esp_err_t dsp_processor_set_params_for_flow(dspFlows_t flow, const filterParams_t *params);

/**
 * Switch to a different DSP flow
 * This activates the flow and applies its stored parameters
 * @param flow The DSP flow to switch to
 * @return ESP_OK on success
 */
esp_err_t dsp_processor_switch_flow(dspFlows_t flow);

/**
 * Get DSP capabilities as JSON string.
 * Returns a dynamically allocated string that must be freed by caller.
 * Returns NULL on error.
 */
char* dsp_processor_get_capabilities_json(void);

#ifdef __cplusplus
}
#endif

#endif /* _DSP_PROCESSOR_H_  */
