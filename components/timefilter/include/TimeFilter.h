#ifndef TIMEFILTER_H_
#define TIMEFILTER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct {
  int64_t last_update_;
  uint8_t count_;
  double offset_;
  double drift_;
  double offset_covariance_;
  double offset_drift_covariance_;
  double drift_covariance_;
  double process_variance_;
  double drift_process_variance_;
  double forget_variance_factor_;
  double adaptive_forgetting_cutoff_;
  uint8_t min_samples_for_forgetting_;
} sTimeFilter_t;

int TIMEFILTER_Init(sTimeFilter_t *timeFilter, double process_std_dev, double drift_process_std_dev, double forget_factor, 
  double adaptive_cutoff, uint8_t min_samples);
void TIMEFILTER_Insert(sTimeFilter_t *timeFilter, int64_t measurement, int64_t max_error, int64_t time_added);
int64_t TIMEFILTER_get_offset(sTimeFilter_t *timeFilter, int64_t client_time);
void TIMEFILTER_Reset(sTimeFilter_t *timeFilter);
uint32_t TIMEFILTER_isFull(sTimeFilter_t *timeFilter, uint32_t n);


#ifdef __cplusplus
}
#endif

#endif  // TIMEFILTER_H_
