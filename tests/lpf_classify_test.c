// SPDX-License-Identifier: MIT
// cc -std=c11 -Wall -Wextra -Werror -Itab5/main/include \
//    tests/lpf_classify_test.c tab5/main/lpf_classify.c -o /tmp/lpf_classify_test

#include "lpf_classify.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static int raw[LPF_SENSE_SAMPLE_COUNT];
static int millivolts[LPF_SENSE_SAMPLE_COUNT];

static void fill(int raw_value, int mv_value)
{
    for (size_t index = 0U; index < LPF_SENSE_SAMPLE_COUNT; ++index) {
        raw[index] = raw_value;
        millivolts[index] = mv_value;
    }
}

static lpf_sense_result_t classify(lpf_classification_t expected)
{
    lpf_sense_result_t result;
    memset(&result, 0xA5, sizeof(result));
    assert(lpf_classify_batch(raw, millivolts, LPF_SENSE_SAMPLE_COUNT, &result) == expected);
    assert(result.classification == expected);
    assert(result.sample_count == LPF_SENSE_SAMPLE_COUNT);
    return result;
}

int main(void)
{
    const struct { int millivolts; lpf_classification_t classification; } boundaries[] = {
        {-1, LPF_CLASS_INVALID}, {0, LPF_CLASS_INVALID}, {899, LPF_CLASS_INVALID},
        {900, LPF_CLASS_40M}, {1055, LPF_CLASS_40M}, {1200, LPF_CLASS_40M},
        {1201, LPF_CLASS_INVALID}, {1449, LPF_CLASS_INVALID}, {1450, LPF_CLASS_20M},
        {1650, LPF_CLASS_20M}, {1850, LPF_CLASS_20M}, {1851, LPF_CLASS_INVALID},
        {2799, LPF_CLASS_INVALID}, {2800, LPF_CLASS_MISSING}, {3300, LPF_CLASS_MISSING},
    };
    for (size_t index = 0U; index < sizeof(boundaries) / sizeof(boundaries[0]); ++index) {
        fill(1234, boundaries[index].millivolts);
        const lpf_sense_result_t result = classify(boundaries[index].classification);
        assert(result.raw_mean == 1234);
        assert(result.mv_mean == boundaries[index].millivolts);
        assert(result.mv_min == result.mv_max && result.mv_spread == 0);
    }

    fill(1000, 1000);
    millivolts[7] = 1080;
    raw[7] = 1080;
    lpf_sense_result_t result = classify(LPF_CLASS_40M);
    assert(result.mv_mean == 1010 && result.raw_mean == 1010);
    assert(result.mv_min == 1000 && result.mv_max == 1080 && result.mv_spread == 80);
    millivolts[7] = 1081;
    (void)classify(LPF_CLASS_INVALID);

    // An average inside 40 m must not hide even one out-of-range sample.
    fill(1234, 1000);
    millivolts[7] = 899;
    result = classify(LPF_CLASS_INVALID);
    assert(result.mv_mean >= 900 && result.mv_mean <= 1200);
    fill(1234, 1199);
    millivolts[0] = 1201;
    result = classify(LPF_CLASS_INVALID);
    assert(result.mv_spread == 2 && result.mv_mean == 1199);
    fill(1234, 1000);
    millivolts[7] = 1650;
    result = classify(LPF_CLASS_INVALID);
    assert(result.mv_mean >= 900 && result.mv_mean <= 1200);

    // Test both raw ADC rails and malformed/out-of-range raw codes independently
    // of calibrated mV, including one outlier hidden inside otherwise good data.
    const int invalid_raw[] = {INT_MIN, -1, 0, 4095, 4096, INT_MAX};
    for (size_t index = 0U; index < sizeof(invalid_raw) / sizeof(invalid_raw[0]); ++index) {
        fill(1234, 1055);
        raw[5] = invalid_raw[index];
        (void)classify(LPF_CLASS_INVALID);
    }
    fill(1, 1055);
    (void)classify(LPF_CLASS_40M);
    fill(4094, 1055);
    (void)classify(LPF_CLASS_40M);
    fill(4095, 3300);
    (void)classify(LPF_CLASS_INVALID); // saturated missing input is never valid

    fill(1234, 1650);
    millivolts[7] = 1731;
    (void)classify(LPF_CLASS_INVALID);
    fill(3500, 2800);
    millivolts[7] = 2880;
    (void)classify(LPF_CLASS_MISSING);
    millivolts[7] = 2881;
    (void)classify(LPF_CLASS_INVALID);

    fill(INT_MAX, INT_MAX);
    raw[0] = INT_MIN;
    millivolts[0] = INT_MIN;
    result = classify(LPF_CLASS_INVALID);
    assert(result.mv_spread == INT_MAX); // stats must not overflow either

    fill(1234, 1055);
    assert(lpf_classify_batch(raw, millivolts, 8U, NULL) == LPF_CLASS_INVALID);
    assert(lpf_classify_batch(NULL, millivolts, 8U, &result) == LPF_CLASS_INVALID);
    assert(result.sample_count == 0U);
    assert(lpf_classify_batch(raw, NULL, 8U, &result) == LPF_CLASS_INVALID);
    assert(lpf_classify_batch(raw, millivolts, 0U, &result) == LPF_CLASS_INVALID);
    assert(lpf_classify_batch(raw, millivolts, 7U, &result) == LPF_CLASS_INVALID);
    assert(lpf_classify_batch(raw, millivolts, 9U, &result) == LPF_CLASS_INVALID);
    assert(strcmp(lpf_classification_name(LPF_CLASS_UNKNOWN), "unknown") == 0);
    assert(strcmp(lpf_classification_name(LPF_CLASS_ADC_ERROR), "ADC error") == 0);
    assert(strcmp(lpf_classification_name(LPF_CLASS_40M), "40m") == 0);
    assert(strcmp(lpf_classification_name(LPF_CLASS_20M), "20m") == 0);
    assert(strcmp(lpf_classification_name(LPF_CLASS_MISSING), "missing") == 0);
    assert(strcmp(lpf_classification_name(LPF_CLASS_INVALID), "invalid") == 0);
    assert(strcmp(lpf_classification_name((lpf_classification_t)99), "unknown") == 0);
    puts("LPF classifier tests passed: boundaries, wrong/missing bands, raw rails, outliers, spread, overflow, incomplete batches");
    return 0;
}
