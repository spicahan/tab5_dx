// SPDX-License-Identifier: MIT

#pragma once

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "lpf_classify.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LPF_SENSE_GPIO 51
#define LPF_SENSE_ADC_UNIT ADC_UNIT_2
#define LPF_SENSE_ADC_CHANNEL ADC_CHANNEL_2

typedef struct {
    adc_oneshot_unit_handle_t unit;
    adc_cali_handle_t calibration;
} lpf_sense_t;

/**
 * Initialize a zeroed object for GPIO51 / ADC2 channel2 at 12 dB attenuation.
 * Runtime pin mapping and per-channel curve-fitting calibration are mandatory;
 * no raw-code threshold or uncalibrated fallback is provided. Caller supplies
 * at least 50 ms input settling before qualifying readings. Source impedance
 * and hardware filtering still need physical validation against a meter.
 * Owns ADC2; init/read/deinit must be serialized by the caller, not used in ISR.
 */
esp_err_t lpf_sense_init(lpf_sense_t *sense);
esp_err_t lpf_sense_deinit(lpf_sense_t *sense);

/**
 * Discard one ADC conversion, then calibrate and classify eight samples.
 * No sleeps, logging, tasks, GPIO power control or caller timeout are included.
 * ADC driver/conversion errors propagate and result becomes ADC_ERROR; an
 * electrically invalid/missing/wrong-band batch returns ESP_OK with the
 * corresponding non-40m classification. Only LPF_CLASS_40M is a 40m candidate,
 * not permission to transmit; freshness/debounce/cutoff remain caller policy.
 */
esp_err_t lpf_sense_read_batch(lpf_sense_t *sense, lpf_sense_result_t *result);

#ifdef __cplusplus
}
#endif
