#ifndef _DSP_PROCESSOR_H_
#define _DSP_PROCESSOR_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"

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

#ifdef __cplusplus
}
#endif

#endif /* _DSP_PROCESSOR_H_  */
