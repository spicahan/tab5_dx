// SPDX-License-Identifier: MIT
// cc -std=c11 -Wall -Wextra -Werror -Itests/lpf_sense_stubs \
//    -Itests/rf_loopback_stubs -Itab5/main/include tests/lpf_sense_test.c \
//    tab5/main/lpf_sense.c tab5/main/lpf_classify.c -o /tmp/lpf_sense_test

#include "lpf_sense.h"
#include "esp_adc/adc_cali_scheme.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    NONE, MAP_ERROR, MAP_WRONG_UNIT, MAP_WRONG_CHANNEL, CREATE_ERROR,
    CHANNEL_ERROR, CALIBRATION_ERROR, CALIBRATION_DELETE_ERROR, UNIT_DELETE_ERROR,
} failure_t;
static failure_t failure;
static unsigned reads;
static unsigned calibrations;
static unsigned unit_deletes;
static unsigned calibration_deletes;
static unsigned fail_read;
static unsigned fail_calibrate;
static int batch_raw = 1234;
static int batch_mv = 1055;

esp_err_t adc_oneshot_io_to_channel(int io, adc_unit_t *unit, adc_channel_t *channel)
{
    assert(io == 51);
    if (failure == MAP_ERROR) { return ESP_FAIL; }
    *unit = failure == MAP_WRONG_UNIT ? ADC_UNIT_1 : ADC_UNIT_2;
    *channel = failure == MAP_WRONG_CHANNEL ? ADC_CHANNEL_0 : ADC_CHANNEL_2;
    return ESP_OK;
}

esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *config,
                              adc_oneshot_unit_handle_t *handle)
{
    assert(config->unit_id == ADC_UNIT_2 && config->ulp_mode == ADC_ULP_MODE_DISABLE);
    if (failure == CREATE_ERROR) { return ESP_ERR_NO_MEM; }
    *handle = (void *)1;
    return ESP_OK;
}

esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t handle,
                                    adc_channel_t channel,
                                    const adc_oneshot_chan_cfg_t *config)
{
    assert(handle == (void *)1 && channel == ADC_CHANNEL_2);
    assert(config->atten == ADC_ATTEN_DB_12 && config->bitwidth == ADC_BITWIDTH_DEFAULT);
    return failure == CHANNEL_ERROR ? ESP_FAIL : ESP_OK;
}

esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *config,
                                              adc_cali_handle_t *handle)
{
    assert(config->unit_id == ADC_UNIT_2 && config->chan == ADC_CHANNEL_2);
    assert(config->atten == ADC_ATTEN_DB_12 && config->bitwidth == ADC_BITWIDTH_DEFAULT);
    if (failure == CALIBRATION_ERROR) { return ESP_FAIL; }
    *handle = (void *)2;
    return ESP_OK;
}

esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t handle)
{
    assert(handle == (void *)2);
    ++calibration_deletes;
    return failure == CALIBRATION_DELETE_ERROR ? ESP_FAIL : ESP_OK;
}

esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t handle)
{
    assert(handle == (void *)1);
    ++unit_deletes;
    return failure == UNIT_DELETE_ERROR ? ESP_FAIL : ESP_OK;
}

esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t handle, adc_channel_t channel, int *raw)
{
    assert(handle == (void *)1 && channel == ADC_CHANNEL_2);
    ++reads;
    if (reads == fail_read) { return ESP_ERR_TIMEOUT; }
    *raw = reads == 1U ? 0 : batch_raw; // dummy conversion must be excluded
    return ESP_OK;
}

esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t handle, int raw, int *millivolts)
{
    assert(handle == (void *)2 && raw == batch_raw);
    ++calibrations;
    if (calibrations == fail_calibrate) { return ESP_FAIL; }
    *millivolts = batch_mv;
    return ESP_OK;
}

static void reset_reads(void)
{
    reads = 0U;
    calibrations = 0U;
    fail_read = 0U;
    fail_calibrate = 0U;
    batch_raw = 1234;
    batch_mv = 1055;
}

static void check_adc_error(const lpf_sense_result_t *result)
{
    assert(result->classification == LPF_CLASS_ADC_ERROR);
    assert(result->sample_count == 0U && result->raw_mean == 0 && result->mv_mean == 0);
    assert(result->mv_min == 0 && result->mv_max == 0 && result->mv_spread == 0);
}

int main(void)
{
    lpf_sense_t sense = {0};
    lpf_sense_result_t result;
    assert(lpf_sense_init(NULL) == ESP_ERR_INVALID_ARG);
    assert(lpf_sense_deinit(NULL) == ESP_ERR_INVALID_ARG);
    assert(lpf_sense_read_batch(&sense, NULL) == ESP_ERR_INVALID_ARG);
    assert(lpf_sense_read_batch(NULL, &result) == ESP_ERR_INVALID_ARG);
    check_adc_error(&result);
    assert(lpf_sense_read_batch(&sense, &result) == ESP_ERR_INVALID_STATE);
    check_adc_error(&result);

    const struct { failure_t failure; esp_err_t error; unsigned deletes; } cases[] = {
        {MAP_ERROR, ESP_FAIL, 0}, {MAP_WRONG_UNIT, ESP_ERR_INVALID_STATE, 0},
        {MAP_WRONG_CHANNEL, ESP_ERR_INVALID_STATE, 0}, {CREATE_ERROR, ESP_ERR_NO_MEM, 0},
        {CHANNEL_ERROR, ESP_FAIL, 1}, {CALIBRATION_ERROR, ESP_FAIL, 1},
    };
    for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        failure = cases[index].failure;
        unit_deletes = 0U;
        assert(lpf_sense_init(&sense) == cases[index].error);
        assert(sense.unit == NULL && sense.calibration == NULL);
        assert(unit_deletes == cases[index].deletes);
        assert(lpf_sense_read_batch(&sense, &result) == ESP_ERR_INVALID_STATE);
        check_adc_error(&result);
    }

    failure = NONE;
    assert(lpf_sense_init(&sense) == ESP_OK);
    assert(lpf_sense_init(&sense) == ESP_ERR_INVALID_STATE);
    reset_reads();
    assert(lpf_sense_read_batch(&sense, &result) == ESP_OK);
    assert(reads == 9U && calibrations == 8U);
    assert(result.classification == LPF_CLASS_40M && result.sample_count == 8U);
    assert(result.raw_mean == 1234 && result.mv_mean == 1055 && result.mv_spread == 0);

    for (unsigned index = 1U; index <= 9U; ++index) {
        reset_reads();
        fail_read = index;
        memset(&result, 0xA5, sizeof(result));
        assert(lpf_sense_read_batch(&sense, &result) == ESP_ERR_TIMEOUT);
        assert(reads == index);
        check_adc_error(&result);
    }
    for (unsigned index = 1U; index <= 8U; ++index) {
        reset_reads();
        fail_calibrate = index;
        memset(&result, 0xA5, sizeof(result));
        assert(lpf_sense_read_batch(&sense, &result) == ESP_FAIL);
        assert(calibrations == index && reads == index + 1U);
        check_adc_error(&result);
    }
    reset_reads();
    batch_mv = 1650;
    assert(lpf_sense_read_batch(&sense, &result) == ESP_OK);
    assert(result.classification == LPF_CLASS_20M);
    reset_reads();
    batch_mv = 3300;
    assert(lpf_sense_read_batch(&sense, &result) == ESP_OK);
    assert(result.classification == LPF_CLASS_MISSING);
    reset_reads();
    batch_raw = 4095;
    assert(lpf_sense_read_batch(&sense, &result) == ESP_OK);
    assert(result.classification == LPF_CLASS_INVALID);

    calibration_deletes = unit_deletes = 0U;
    failure = CALIBRATION_DELETE_ERROR;
    assert(lpf_sense_deinit(&sense) == ESP_FAIL);
    assert(sense.calibration != NULL && sense.unit == NULL);
    assert(calibration_deletes == 1U && unit_deletes == 1U);
    failure = NONE;
    assert(lpf_sense_deinit(&sense) == ESP_OK);
    assert(sense.calibration == NULL && sense.unit == NULL);
    assert(calibration_deletes == 2U && unit_deletes == 1U);
    assert(lpf_sense_init(&sense) == ESP_OK);
    failure = UNIT_DELETE_ERROR;
    assert(lpf_sense_deinit(&sense) == ESP_FAIL);
    assert(sense.calibration == NULL && sense.unit != NULL);
    failure = NONE;
    assert(lpf_sense_deinit(&sense) == ESP_OK);
    assert(lpf_sense_deinit(&sense) == ESP_OK);
    puts("LPF ADC wrapper tests passed: exact pin/channel/calibration configuration, dummy read, failure cleanup, all read/calibration errors, no fallback");
    return 0;
}
