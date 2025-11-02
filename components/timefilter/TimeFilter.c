// Copyright 2025 Resonate Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "TimeFilter.h"
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include "esp_log.h"


int TIMEFILTER_Init(sTimeFilter_t *timeFilter, double process_std_dev, double drift_process_std_dev, double forget_factor, 
  double adaptive_cutoff, uint8_t min_samples) {
  if (timeFilter) {
    timeFilter->process_variance_ = process_std_dev * process_std_dev;
    timeFilter->drift_process_variance_ = drift_process_std_dev * drift_process_std_dev;
    timeFilter->forget_variance_factor_ = forget_factor * forget_factor;
    timeFilter->adaptive_forgetting_cutoff_ = adaptive_cutoff;
    timeFilter->min_samples_for_forgetting_ = min_samples;
    TIMEFILTER_Reset(timeFilter);
    return 0;
  }
  return -1;
}


void TIMEFILTER_Insert(sTimeFilter_t *timeFilter, int64_t measurement, int64_t max_error, int64_t time_added) {
  if (time_added <= timeFilter->last_update_) {
    // Skip non-monotonic timestamps (duplicates or out-of-order packets)
    // This protects against division by zero and backwards time progression
    ESP_LOGE("TF", "time ERROR, old %lld, new %lld",timeFilter->last_update_, time_added);
    return;
  }

  const double dt = time_added - timeFilter->last_update_;
  const double dt_squared = dt * dt;
  timeFilter->last_update_ = time_added;

  const double update_std_dev = max_error;
  const double measurement_variance = update_std_dev * update_std_dev;

  // Filter initialization: First measurement establishes offset baseline
  if (timeFilter->count_ <= 0) {
    ++timeFilter->count_;

    timeFilter->offset_ = measurement;
    timeFilter->offset_covariance_ = measurement_variance;
    timeFilter->drift_ = 0;  // No drift information available yet

    return;
  }

  // Second measurement: Initial drift estimation from finite differences
  if (timeFilter->count_ == 1) {
    ++timeFilter->count_;

    timeFilter->drift_ = (measurement - timeFilter->offset_) / dt;
    timeFilter->offset_ = measurement;

    // Drift variance estimated from propagation of offset uncertainties
    timeFilter->drift_covariance_ = (timeFilter->offset_covariance_ + measurement_variance) / dt_squared;
    timeFilter->offset_covariance_ = measurement_variance;

    return;
  }
  //ESP_LOGI("TF", "Count %u",timeFilter->count_);

  /*** Kalman Prediction Step ***/
  // State prediction: x_k|k-1 = F * x_k-1|k-1
  double offset = timeFilter->offset_ + timeFilter->drift_ * dt;

  // Covariance prediction: P_k|k-1 = F * P_k-1|k-1 * F^T + Q
  // State transition matrix F = [1, dt; 0, 1]

  // Process noise for both offset and drift (full random walk model)
  // We assume clock jitter (offset noise) and wander (drift noise) are independent processes
  const double drift_process_variance = dt * timeFilter->drift_process_variance_;
  double new_drift_covariance = timeFilter->drift_covariance_ + drift_process_variance;

  double new_offset_drift_covariance = timeFilter->offset_drift_covariance_ + timeFilter->drift_covariance_ * dt;

  const double offset_process_variance = dt * timeFilter->process_variance_;
  double new_offset_covariance = timeFilter->offset_covariance_ + 2 * timeFilter->offset_drift_covariance_ * dt +
                                 timeFilter->drift_covariance_ * dt_squared + offset_process_variance;

  /*** Innovation and Adaptive Forgetting ***/
  const double residual = measurement - offset;  // Innovation: y_k = z_k - H * x_k|k-1
  const double max_residual_cutoff = max_error * timeFilter->adaptive_forgetting_cutoff_;

  if (timeFilter->count_ < timeFilter->min_samples_for_forgetting_) {
    // Build sufficient history before enabling adaptive forgetting
    ++timeFilter->count_;
  } else if (fabs(residual) > max_residual_cutoff) {
    // Large prediction error detected - likely network disruption or clock adjustment
    // Apply forgetting factor to increase Kalman gain and accelerate convergence
    new_drift_covariance *= timeFilter->forget_variance_factor_;
    new_offset_drift_covariance *= timeFilter->forget_variance_factor_;
    new_offset_covariance *= timeFilter->forget_variance_factor_;
  }

  /*** Kalman Update Step ***/
  // Innovation covariance: S = H * P * H^T + R, where H = [1, 0]
  const double uncertainty = 1.0 / (new_offset_covariance + measurement_variance);

  // Kalman gain: K = P * H^T * S^(-1)
  const double offset_gain = new_offset_covariance * uncertainty;
  const double drift_gain = new_offset_drift_covariance * uncertainty;

  // State update: x_k|k = x_k|k-1 + K * y_k
  timeFilter->offset_ = offset + offset_gain * residual;
  timeFilter->drift_ += drift_gain * residual;

  // Covariance update: P_k|k = (I - K*H) * P_k|k-1
  // Using simplified form to ensure numerical stability
  timeFilter->drift_covariance_ = new_drift_covariance - drift_gain * new_offset_drift_covariance;
  timeFilter->offset_drift_covariance_ = new_offset_drift_covariance - drift_gain * new_offset_covariance;
  timeFilter->offset_covariance_ = new_offset_covariance - offset_gain * new_offset_covariance;
}

int64_t TIMEFILTER_get_offset(sTimeFilter_t *timeFilter, int64_t client_time) {
  // Transform: T_server = T_client + offset + drift * (T_client - T_last_update)
  // Compute instantaneous offset accounting for linear drift:
  // offset(t) = offset_base + drift * (t - t_last_update)

  double dt = client_time - timeFilter->last_update_;
  int64_t offset = round(timeFilter->offset_ + timeFilter->drift_ * dt);
  return offset;
}

void TIMEFILTER_Reset(sTimeFilter_t *timeFilter) {
  timeFilter->count_ = 0;
  timeFilter->offset_ = 0.0;
  timeFilter->drift_ = 0.0;
  timeFilter->offset_covariance_ = INFINITY;
  timeFilter->offset_drift_covariance_ = 0.0;
  timeFilter->drift_covariance_ = 0.0;
  timeFilter->last_update_ = 0;
}

uint32_t TIMEFILTER_isFull(sTimeFilter_t *timeFilter, uint32_t n) {
  if (timeFilter->count_>= n) {
    return 1;
  } else {
    return 0;
  }
}