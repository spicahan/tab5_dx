// SPDX-License-Identifier: MIT

#include "lpf_classify.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static lpf_classification_t classify_mv(int millivolts)
{
    if (millivolts >= LPF_SENSE_40M_MIN_MV && millivolts <= LPF_SENSE_40M_MAX_MV) {
        return LPF_CLASS_40M;
    }
    if (millivolts >= LPF_SENSE_20M_MIN_MV && millivolts <= LPF_SENSE_20M_MAX_MV) {
        return LPF_CLASS_20M;
    }
    if (millivolts >= LPF_SENSE_MISSING_MIN_MV) {
        return LPF_CLASS_MISSING;
    }
    return LPF_CLASS_INVALID;
}

lpf_classification_t lpf_classify_batch(const int *raw, const int *millivolts,
                                        size_t sample_count,
                                        lpf_sense_result_t *result)
{
    if (result == NULL) {
        return LPF_CLASS_INVALID;
    }
    memset(result, 0, sizeof(*result));
    result->classification = LPF_CLASS_INVALID;
    if (raw == NULL || millivolts == NULL || sample_count != LPF_SENSE_SAMPLE_COUNT) {
        return result->classification;
    }

    result->sample_count = sample_count;
    result->mv_min = millivolts[0];
    result->mv_max = millivolts[0];
    const lpf_classification_t candidate = classify_mv(millivolts[0]);
    bool valid = candidate != LPF_CLASS_INVALID;
    int64_t raw_sum = 0;
    int64_t mv_sum = 0;
    for (size_t index = 0U; index < sample_count; ++index) {
        raw_sum += raw[index];
        mv_sum += millivolts[index];
        if (millivolts[index] < result->mv_min) { result->mv_min = millivolts[index]; }
        if (millivolts[index] > result->mv_max) { result->mv_max = millivolts[index]; }
        if (raw[index] <= 0 || raw[index] >= LPF_SENSE_RAW_MAX ||
            classify_mv(millivolts[index]) != candidate) {
            valid = false;
        }
    }
    result->raw_mean = (int)(raw_sum / (int64_t)sample_count);
    result->mv_mean = (int)(mv_sum / (int64_t)sample_count);
    const int64_t spread = (int64_t)result->mv_max - (int64_t)result->mv_min;
    result->mv_spread = spread > INT_MAX ? INT_MAX : (int)spread;
    if (valid && spread <= LPF_SENSE_MAX_SPREAD_MV) {
        result->classification = candidate;
    }
    return result->classification;
}

const char *lpf_classification_name(lpf_classification_t classification)
{
    switch (classification) {
    case LPF_CLASS_UNKNOWN: return "unknown";
    case LPF_CLASS_40M: return "40m";
    case LPF_CLASS_20M: return "20m";
    case LPF_CLASS_MISSING: return "missing";
    case LPF_CLASS_INVALID: return "invalid";
    case LPF_CLASS_ADC_ERROR: return "ADC error";
    default: return "unknown";
    }
}
