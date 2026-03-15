#ifndef _DSP_PROCESSOR_H_
#define _DSP_PROCESSOR_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include "dsp_types.h"
#include "freertos/FreeRTOS.h"

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


void dsp_processor_init(void);
void dsp_processor_uninit(void);
int dsp_processor_worker(void *pcmChnk, const void *scSet);
esp_err_t dsp_processor_update_filter_params(filterParams_t *params);

void dsp_processor_set_volome(double volume);

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

#ifdef __cplusplus
}
#endif

#endif /* _DSP_PROCESSOR_H_  */
