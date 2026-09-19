#pragma once

#include "esp_adc/adc_cali.h"

#define ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED 1
typedef struct {
    adc_unit_t unit_id;
    adc_channel_t chan;
    adc_atten_t atten;
    adc_bitwidth_t bitwidth;
} adc_cali_curve_fitting_config_t;

esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *config,
                                              adc_cali_handle_t *handle);
esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t handle);
