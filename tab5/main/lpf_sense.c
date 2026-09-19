// SPDX-License-Identifier: MIT

#include "lpf_sense.h"

#include <string.h>

#include "esp_adc/adc_cali_scheme.h"
#include "soc/soc_caps.h"

#if !ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
#error "LPF sensing requires calibrated ADC curve fitting"
#endif

_Static_assert(SOC_ADC_DIGI_MAX_BITWIDTH == 12, "LPF raw-rail checks require 12-bit ADC");

esp_err_t lpf_sense_init(lpf_sense_t *sense)
{
    if (sense == NULL) { return ESP_ERR_INVALID_ARG; }
    if (sense->unit != NULL || sense->calibration != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    adc_unit_t mapped_unit;
    adc_channel_t mapped_channel;
    esp_err_t error = adc_oneshot_io_to_channel(LPF_SENSE_GPIO, &mapped_unit, &mapped_channel);
    if (error != ESP_OK) { return error; }
    if (mapped_unit != LPF_SENSE_ADC_UNIT || mapped_channel != LPF_SENSE_ADC_CHANNEL) {
        return ESP_ERR_INVALID_STATE;
    }

    const adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = LPF_SENSE_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    error = adc_oneshot_new_unit(&unit_config, &sense->unit);
    if (error != ESP_OK) { return error; }
    const adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    error = adc_oneshot_config_channel(sense->unit, LPF_SENSE_ADC_CHANNEL, &channel_config);
    if (error != ESP_OK) { goto cleanup; }
    const adc_cali_curve_fitting_config_t calibration_config = {
        .unit_id = LPF_SENSE_ADC_UNIT,
        .chan = LPF_SENSE_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    error = adc_cali_create_scheme_curve_fitting(&calibration_config, &sense->calibration);
    if (error != ESP_OK) { goto cleanup; }
    return ESP_OK;

cleanup:
    (void)lpf_sense_deinit(sense);
    return error;
}

esp_err_t lpf_sense_deinit(lpf_sense_t *sense)
{
    if (sense == NULL) { return ESP_ERR_INVALID_ARG; }
    esp_err_t error = ESP_OK;
    if (sense->calibration != NULL) {
        error = adc_cali_delete_scheme_curve_fitting(sense->calibration);
        if (error == ESP_OK) { sense->calibration = NULL; }
    }
    if (sense->unit != NULL) {
        const esp_err_t unit_error = adc_oneshot_del_unit(sense->unit);
        if (unit_error == ESP_OK) { sense->unit = NULL; }
        if (error == ESP_OK) { error = unit_error; }
    }
    return error;
}

esp_err_t lpf_sense_read_batch(lpf_sense_t *sense, lpf_sense_result_t *result)
{
    if (result == NULL) { return ESP_ERR_INVALID_ARG; }
    memset(result, 0, sizeof(*result));
    result->classification = LPF_CLASS_ADC_ERROR;
    if (sense == NULL) { return ESP_ERR_INVALID_ARG; }
    if (sense->unit == NULL || sense->calibration == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    int discarded_raw;
    esp_err_t error = adc_oneshot_read(sense->unit, LPF_SENSE_ADC_CHANNEL, &discarded_raw);
    if (error != ESP_OK) { return error; }
    int raw[LPF_SENSE_SAMPLE_COUNT];
    int millivolts[LPF_SENSE_SAMPLE_COUNT];
    for (size_t index = 0U; index < LPF_SENSE_SAMPLE_COUNT; ++index) {
        error = adc_oneshot_read(sense->unit, LPF_SENSE_ADC_CHANNEL, &raw[index]);
        if (error != ESP_OK) { return error; }
        error = adc_cali_raw_to_voltage(sense->calibration, raw[index], &millivolts[index]);
        if (error != ESP_OK) { return error; }
    }
    (void)lpf_classify_batch(raw, millivolts, LPF_SENSE_SAMPLE_COUNT, result);
    return ESP_OK;
}
