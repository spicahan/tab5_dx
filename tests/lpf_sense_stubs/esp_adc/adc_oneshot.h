#pragma once

#include "esp_adc/adc_cali.h"

typedef void *adc_oneshot_unit_handle_t;
typedef enum { ADC_ULP_MODE_DISABLE } adc_ulp_mode_t;
typedef struct {
    adc_unit_t unit_id;
    adc_ulp_mode_t ulp_mode;
} adc_oneshot_unit_init_cfg_t;
typedef struct {
    adc_atten_t atten;
    adc_bitwidth_t bitwidth;
} adc_oneshot_chan_cfg_t;

esp_err_t adc_oneshot_io_to_channel(int io, adc_unit_t *unit, adc_channel_t *channel);
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *config,
                              adc_oneshot_unit_handle_t *handle);
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t handle,
                                    adc_channel_t channel,
                                    const adc_oneshot_chan_cfg_t *config);
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t handle);
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t handle, adc_channel_t channel, int *raw);
