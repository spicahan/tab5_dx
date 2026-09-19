#pragma once

#include "esp_err.h"

typedef enum { ADC_UNIT_1, ADC_UNIT_2 } adc_unit_t;
typedef enum { ADC_CHANNEL_0, ADC_CHANNEL_1, ADC_CHANNEL_2 } adc_channel_t;
typedef enum { ADC_ATTEN_DB_12 = 3 } adc_atten_t;
typedef enum { ADC_BITWIDTH_DEFAULT = 0 } adc_bitwidth_t;
typedef void *adc_cali_handle_t;

esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t handle, int raw, int *millivolts);
