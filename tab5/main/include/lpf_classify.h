// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LPF_SENSE_SAMPLE_COUNT 8U
#define LPF_SENSE_40M_MIN_MV 900
#define LPF_SENSE_40M_MAX_MV 1200
#define LPF_SENSE_20M_MIN_MV 1450
#define LPF_SENSE_20M_MAX_MV 1850
#define LPF_SENSE_MISSING_MIN_MV 2800
#define LPF_SENSE_MAX_SPREAD_MV 80
#define LPF_SENSE_RAW_MAX 4095

typedef enum {
    LPF_CLASS_UNKNOWN = 0,
    LPF_CLASS_40M,
    LPF_CLASS_20M,
    LPF_CLASS_MISSING,
    LPF_CLASS_INVALID,
    LPF_CLASS_ADC_ERROR,
} lpf_classification_t;

typedef struct {
    lpf_classification_t classification;
    size_t sample_count;
    int raw_mean;
    int mv_mean;
    int mv_min;
    int mv_max;
    int mv_spread;
} lpf_sense_result_t;

/**
 * Classify one complete eight-sample batch of calibrated millivolt readings.
 * Every sample must be in the SAME recognized range, all raw codes must be
 * strictly between the 12-bit rails, and max-min must be <=80 mV. Averages are
 * diagnostic only and never rescue an outlier, mixed band, missing calibration
 * or rail code. Any malformed/incomplete batch is INVALID, not an LPF match.
 * This pure function has no timing policy: the caller must enforce startup
 * settling, consecutive valid batches, freshness and PA cutoff independently.
 */
lpf_classification_t lpf_classify_batch(const int *raw, const int *millivolts,
                                        size_t sample_count,
                                        lpf_sense_result_t *result);

const char *lpf_classification_name(lpf_classification_t classification);

#ifdef __cplusplus
}
#endif
