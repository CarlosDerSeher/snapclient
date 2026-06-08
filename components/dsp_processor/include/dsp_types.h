/**
 * @file dsp_types.h
 * @brief Shared DSP types and constants
 * 
 * This header contains types and constants shared between DSP processor
 * and DSP settings components to break circular dependencies.
 */

#ifndef __DSP_TYPES_H__
#define __DSP_TYPES_H__

#ifdef __cplusplus
extern "C" {
#endif

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

/**
 * DSP Flow types - different audio processing modes
 */
typedef enum dspFlows {
  dspfStereo,         // Pass-through with volume only
  dspfBiamp,          // Bi-amp crossover (low/high split)
  dspf2DOT1,          // 2.1 subwoofer configuration (not implemented)
  dspfFunkyHonda,     // Custom multi-way split (not implemented)
  dspfBassBoost,      // Bass boost with low shelf
  dspfEQBassTreble,   // Simple bass/treble EQ
  DSP_FLOW_COUNT      // Total number of DSP flows
} dspFlows_t;

/**
 * Filter parameters for a single DSP flow
 * Used to communicate parameter changes
 */
typedef struct filterParams_s {
  dspFlows_t dspFlow;  // Which flow these parameters belong to
  float fc_1;          // Primary frequency (bass/crossover) in Hz
  float gain_1;        // Primary gain (bass/boost) in dB
  float fc_3;          // Tertiary frequency (treble/high crossover) in Hz
  float gain_3;        // Tertiary gain (treble) in dB
} filterParams_t;

/**
 * DSP settings change event types
 */
typedef enum {
  DSP_EVENT_FLOW_CHANGED,     // Active flow was changed
  DSP_EVENT_PARAMS_CHANGED,   // Parameters for a flow were updated
} dsp_event_type_t;

/**
 * DSP settings change event
 * Used to notify subscribers when settings change
 */
typedef struct {
  dsp_event_type_t type;      // Type of change
  dspFlows_t flow;            // Affected flow
  filterParams_t params;      // New parameters (for PARAMS_CHANGED events)
} dsp_event_t;

#ifdef __cplusplus
}
#endif

#endif /* __DSP_TYPES_H__ */
