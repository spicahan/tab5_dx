// SPDX-License-Identifier: MIT
// Run from the repository root (output may be placed in any temporary path):
// cc -std=c11 -Wall -Wextra -Werror -Itests/rf_loopback_stubs -Itab5/main/include \
//    tests/rf_loopback_host_test.c tab5/main/rf_loopback.c -lm -o /tmp/rf_loopback_test

#include "rf_loopback.h"

#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TEST_PI 3.14159265358979323846

typedef enum { TONE_1K, TONE_2K, CONSTANT, SILENCE, RAILS } test_signal_t;
static test_signal_t signal_kind;
static size_t sample_index;
static size_t fail_at_frame;
static unsigned read_behavior;
static int add_padding;
static char output[16384];
static size_t output_used;

void rf_loopback_test_log(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const int count = vsnprintf(output + output_used, sizeof(output) - output_used,
                                format, arguments);
    va_end(arguments);
    assert(count >= 0 && (size_t)count + 1U < sizeof(output) - output_used);
    output_used += (size_t)count;
    output[output_used++] = '\n';
    output[output_used] = '\0';
}

const char *esp_err_to_name(esp_err_t error)
{
    (void)error;
    return "HOST_TEST_ERROR";
}

esp_err_t pcm1808_i2s_read(pcm1808_i2s_t *adc, uint32_t *words,
                           size_t capacity, size_t *received, uint32_t timeout)
{
    (void)adc;
    assert(timeout == 1000U);
    if (fail_at_frame != 0U && sample_index >= fail_at_frame) {
        return ESP_ERR_TIMEOUT;
    }
    if (read_behavior != 0U) {
        *received = read_behavior == 1U ? 0U : capacity + 1U;
        return ESP_OK;
    }
    // Exercise partial successful reads, independent of the 48-sample period.
    *received = capacity < 137U ? capacity : 137U;
    for (size_t frame = 0U; frame < *received; ++frame, ++sample_index) {
        int32_t samples[2] = {0};
        if (signal_kind == TONE_1K || signal_kind == TONE_2K) {
            const unsigned harmonic = signal_kind == TONE_1K ? 1U : 2U;
            const double angle = 2.0 * TEST_PI * harmonic *
                                 (double)(sample_index % 48U) / 48.0;
            samples[0] = (int32_t)lround(1000000.0 * cos(angle)) + 2000000;
            samples[1] = (int32_t)lround(500000.0 * cos(angle + TEST_PI / 2.0)) - 2000000;
        } else if (signal_kind == CONSTANT) {
            samples[0] = -1;
            samples[1] = 8000000;
        } else if (signal_kind == RAILS) {
            samples[0] = (sample_index & 1U) != 0U ? 8388607 : -8388608;
            samples[1] = -8388608;
        }
        for (size_t channel = 0U; channel < 2U; ++channel) {
            words[2U * frame + channel] = ((uint32_t)samples[channel] << 8) |
                                          (add_padding ? 1U : 0U);
        }
    }
    return ESP_OK;
}

static void reset_test(test_signal_t signal)
{
    signal_kind = signal;
    sample_index = 0U;
    fail_at_frame = 0U;
    read_behavior = 0U;
    add_padding = 0;
    output_used = 0U;
    output[0] = '\0';
}

static void check_tone(unsigned frequency, double left_expected,
                       double right_expected, double phase_expected)
{
    char prefix[64];
    (void)snprintf(prefix, sizeof(prefix), "RF LOOPBACK tone=%u Hz", frequency);
    const char *line = strstr(output, prefix);
    assert(line != NULL);
    unsigned actual_frequency;
    double left, left_dbfs, right, right_dbfs, phase;
    assert(sscanf(line,
                  "RF LOOPBACK tone=%u Hz L_peak=%lf L_dBFS=%lf R_peak=%lf R_dBFS=%lf phase_R-L=%lf deg",
                  &actual_frequency, &left, &left_dbfs, &right, &right_dbfs,
                  &phase) == 6);
    assert(actual_frequency == frequency);
    assert(fabs(left - left_expected) < 1.0);
    assert(fabs(right - right_expected) < 1.0);
    if (left_expected > 0.0 && right_expected > 0.0) {
        assert(fabs(left_dbfs - 20.0 * log10(left_expected / 8388608.0)) < 0.01);
        assert(fabs(right_dbfs - 20.0 * log10(right_expected / 8388608.0)) < 0.01);
        assert(fabs(phase - phase_expected) < 0.01);
    }
}

static void check_channel(char channel, int32_t minimum, int32_t maximum,
                          double dc_expected, double rms_expected, unsigned rails)
{
    char prefix[40];
    (void)snprintf(prefix, sizeof(prefix), "RF LOOPBACK %c min=", channel);
    const char *line = strstr(output, prefix);
    assert(line != NULL);
    char actual_channel;
    int actual_minimum, actual_maximum;
    unsigned actual_rails;
    double dc, rms;
    assert(sscanf(line, "RF LOOPBACK %c min=%d max=%d dc=%lf rms_ac=%lf rail_hits=%u",
                  &actual_channel, &actual_minimum, &actual_maximum,
                  &dc, &rms, &actual_rails) == 6);
    assert(actual_channel == channel && actual_minimum == minimum && actual_maximum == maximum);
    assert(fabs(dc - dc_expected) < 0.01);
    assert(fabs(rms - rms_expected) < 1.0);
    assert(actual_rails == rails);
}

static void check_empty_result(const rf_loopback_result_t *result)
{
    const unsigned char *bytes = (const unsigned char *)result;
    for (size_t index = 0U; index < sizeof(*result); ++index) {
        assert(bytes[index] == 0U);
    }
    assert(output_used == 0U);
}

static void test_bounded_capture(pcm1808_i2s_t *adc)
{
    const uint32_t capture_windows[] = {50U, 100U, 200U, 250U, 1000U};
    rf_loopback_result_t result;
    for (size_t window = 0U; window < sizeof(capture_windows) / sizeof(capture_windows[0]); ++window) {
        const uint32_t capture_ms = capture_windows[window];
        const size_t capture_frames = (size_t)capture_ms * 48U;
        for (size_t harmonic = 0U; harmonic < 2U; ++harmonic) {
            reset_test(harmonic == 0U ? TONE_1K : TONE_2K);
            assert(rf_loopback_capture(adc, 1000U, capture_ms, &result) == ESP_OK);
            assert(output_used == 0U);
            assert(sample_index == 48000U + capture_frames);
            assert(result.capture_ms == capture_ms);
            assert(result.frames_captured == capture_frames);
            assert(result.discard_frames == 48000U);
            assert(result.nonzero_padding_words == 0U);
            assert(result.channels[0].rail_hits == 0U);
            assert(result.channels[1].rail_hits == 0U);
            assert(fabs(result.channels[0].dc - 2000000.0) < 0.01);
            assert(fabs(result.channels[1].dc + 2000000.0) < 0.01);
            assert(fabs(result.channels[0].rms_ac - 1000000.0 / sqrt(2.0)) < 1.0);
            assert(fabs(result.channels[1].rms_ac - 500000.0 / sqrt(2.0)) < 1.0);
            const rf_loopback_tone_result_t *tone = &result.tones[harmonic];
            assert(tone->frequency_hz == (harmonic + 1U) * 1000U);
            assert(fabs(tone->peak[0] - 1000000.0) < 1.0);
            assert(fabs(tone->peak[1] - 500000.0) < 1.0);
            assert(fabs(tone->dbfs[0] - 20.0 * log10(1000000.0 / 8388608.0)) < 0.01);
            assert(fabs(tone->dbfs[1] - 20.0 * log10(500000.0 / 8388608.0)) < 0.01);
            assert(fabs(tone->relative_phase_degrees - 90.0) < 0.01);
            assert(result.tones[1U - harmonic].peak[0] < 1.0);
            assert(result.tones[1U - harmonic].peak[1] < 1.0);

            // Reporting is a separate action and cannot add sample reads.
            assert(rf_loopback_report("BOUNDED", &result) == ESP_OK);
            assert(sample_index == 48000U + capture_frames);
            check_tone((unsigned)((harmonic + 1U) * 1000U),
                       1000000.0, 500000.0, 90.0);
        }

        reset_test(RAILS);
        add_padding = 1;
        assert(rf_loopback_capture(adc, 0U, capture_ms, &result) == ESP_OK);
        assert(output_used == 0U);
        assert(result.nonzero_padding_words == capture_frames * 2U);
        assert(result.channels[0].rail_hits == capture_frames);
        assert(result.channels[1].rail_hits == capture_frames);
        assert(rf_loopback_report("BOUNDED_RAILS", &result) == ESP_OK);
        assert(strstr(output, "sample warning") != NULL);

        reset_test(CONSTANT);
        assert(rf_loopback_measure_window(adc, "BOUNDED_DC", 250U, capture_ms) == ESP_OK);
        assert(sample_index == 12000U + capture_frames);
        check_channel('L', -1, -1, -1.0, 0.0, 0U);
        check_channel('R', 8000000, 8000000, 8000000.0, 0.0, 0U);
        check_tone(1000U, 0.0, 0.0, 0.0);
        check_tone(2000U, 0.0, 0.0, 0.0);
    }

    reset_test(SILENCE);
    memset(&result, 0xA5, sizeof(result));
    assert(rf_loopback_capture(NULL, 0U, 100U, &result) == ESP_ERR_INVALID_ARG);
    check_empty_result(&result);
    assert(rf_loopback_capture(adc, 0U, 100U, NULL) == ESP_ERR_INVALID_ARG);
    assert(rf_loopback_capture(adc, 5001U, 100U, &result) == ESP_ERR_INVALID_ARG);
    check_empty_result(&result);
    const uint32_t invalid_windows[] = {0U, 1U, 99U, 101U, 249U, 251U, 999U, 1001U, UINT32_MAX};
    for (size_t index = 0U; index < sizeof(invalid_windows) / sizeof(invalid_windows[0]); ++index) {
        assert(rf_loopback_capture(adc, 0U, invalid_windows[index], &result) == ESP_ERR_INVALID_ARG);
        check_empty_result(&result);
    }
    adc->sample_rate_hz = 44100U;
    assert(rf_loopback_capture(adc, 0U, 100U, &result) == ESP_ERR_INVALID_ARG);
    check_empty_result(&result);
    adc->sample_rate_hz = 48000U;
    adc->enabled = false;
    assert(rf_loopback_capture(adc, 0U, 100U, &result) == ESP_ERR_INVALID_STATE);
    check_empty_result(&result);
    adc->enabled = true;
    assert(rf_loopback_report(NULL, &result) == ESP_ERR_INVALID_ARG);
    assert(rf_loopback_report("", &result) == ESP_ERR_INVALID_ARG);
    assert(rf_loopback_report("BAD", NULL) == ESP_ERR_INVALID_ARG);
    assert(rf_loopback_report("BAD", &result) == ESP_ERR_INVALID_ARG);
    assert(output_used == 0U);

    for (unsigned behavior = 1U; behavior <= 2U; ++behavior) {
        read_behavior = behavior;
        assert(rf_loopback_capture(adc, 0U, 100U, &result) == ESP_ERR_INVALID_SIZE);
        check_empty_result(&result);
    }
    reset_test(SILENCE);
    fail_at_frame = 137U;
    assert(rf_loopback_capture(adc, 1000U, 100U, &result) == ESP_ERR_TIMEOUT);
    check_empty_result(&result);
    reset_test(SILENCE);
    fail_at_frame = 48000U + 137U;
    assert(rf_loopback_capture(adc, 1000U, 100U, &result) == ESP_ERR_TIMEOUT);
    check_empty_result(&result);
    assert(rf_loopback_report("FAILED_CAPTURE", &result) == ESP_ERR_INVALID_ARG);
    assert(output_used == 0U);
}

int main(void)
{
    pcm1808_i2s_t adc = {
        .rx_channel = (void *)1,
        .sample_rate_hz = 48000U,
        .enabled = true,
    };
    reset_test(TONE_1K);
    assert(rf_loopback_measure(&adc, "HOST_1K", 1000U) == ESP_OK);
    assert(sample_index == 96000U);
    assert(strstr(output, "frames=48000 discard_frames=48000 padding_nonzero=0") != NULL);
    check_tone(1000U, 1000000.0, 500000.0, 90.0);
    check_tone(2000U, 0.0, 0.0, 0.0);
    check_channel('L', 1000000, 3000000, 2000000.0, 1000000.0 / sqrt(2.0), 0U);
    check_channel('R', -2500000, -1500000, -2000000.0, 500000.0 / sqrt(2.0), 0U);

    reset_test(TONE_2K);
    assert(rf_loopback_measure(&adc, "HOST_2K", 250U) == ESP_OK);
    assert(sample_index == 60000U);
    check_tone(1000U, 0.0, 0.0, 0.0);
    check_tone(2000U, 1000000.0, 500000.0, 90.0);

    reset_test(CONSTANT);
    assert(rf_loopback_measure(&adc, "HOST_DC", 0U) == ESP_OK);
    check_channel('L', -1, -1, -1.0, 0.0, 0U);
    check_channel('R', 8000000, 8000000, 8000000.0, 0.0, 0U);
    check_tone(1000U, 0.0, 0.0, 0.0);
    check_tone(2000U, 0.0, 0.0, 0.0);

    reset_test(SILENCE);
    assert(rf_loopback_measure(&adc, "HOST_SILENCE", 0U) == ESP_OK);
    check_tone(1000U, 0.0, 0.0, 0.0);
    check_tone(2000U, 0.0, 0.0, 0.0);

    reset_test(RAILS);
    add_padding = 1;
    assert(rf_loopback_measure(&adc, "HOST_RAILS", 0U) == ESP_OK);
    check_channel('L', -8388608, 8388607, -0.5, 8388607.5, 48000U);
    check_channel('R', -8388608, -8388608, -8388608.0, 0.0, 48000U);
    assert(strstr(output, "padding_nonzero=96000") != NULL);
    assert(strstr(output, "sample warning") != NULL);

    reset_test(SILENCE);
    assert(rf_loopback_measure(NULL, "BAD", 1000U) == ESP_ERR_INVALID_ARG);
    assert(rf_loopback_measure(&adc, NULL, 1000U) == ESP_ERR_INVALID_ARG);
    assert(rf_loopback_measure(&adc, "", 1000U) == ESP_ERR_INVALID_ARG);
    assert(rf_loopback_measure(&adc, "BAD", 5001U) == ESP_ERR_INVALID_ARG);
    adc.sample_rate_hz = 44100U;
    assert(rf_loopback_measure(&adc, "BAD", 1000U) == ESP_ERR_INVALID_ARG);
    adc.sample_rate_hz = 48000U;
    adc.enabled = false;
    assert(rf_loopback_measure(&adc, "BAD", 1000U) == ESP_ERR_INVALID_STATE);
    adc.enabled = true;
    for (unsigned behavior = 1U; behavior <= 2U; ++behavior) {
        read_behavior = behavior;
        assert(rf_loopback_measure(&adc, "BAD_READ", 1000U) == ESP_ERR_INVALID_SIZE);
    }
    reset_test(SILENCE);
    fail_at_frame = 137U;
    assert(rf_loopback_measure(&adc, "DISCARD_TIMEOUT", 1000U) == ESP_ERR_TIMEOUT);
    reset_test(SILENCE);
    fail_at_frame = 48000U + 137U;
    assert(rf_loopback_measure(&adc, "CAPTURE_TIMEOUT", 1000U) == ESP_ERR_TIMEOUT);

    test_bounded_capture(&adc);
    puts("rf_loopback host tests passed: 50/100/200/250/1000 ms quiet capture and deferred report, quadrature 1/2 kHz, DC, silence, clipping, padding, invalid arguments, partial reads, and errors");
    return 0;
}
