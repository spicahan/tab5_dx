// SPDX-License-Identifier: MIT

#include "rf_loopback.h"

#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#define LOOPBACK_SAMPLE_RATE_HZ 48000U
#define LOOPBACK_BUFFER_FRAMES  240U
#define LOOPBACK_READ_TIMEOUT_MS 1000U
#define LOOPBACK_TABLE_LENGTH   48U
#define LOOPBACK_TONE_COUNT     RF_LOOPBACK_TONE_COUNT
#define LOOPBACK_FULL_SCALE     8388608.0
#define LOOPBACK_PI             3.14159265358979323846

static const char *TAG = "rf_loopback";

typedef struct {
    int32_t minimum;
    int32_t maximum;
    size_t rail_hits;
    // Welford's online variance avoids subtracting two large DC moments.
    double mean;
    double squared_deviations;
    double cosine_sum[LOOPBACK_TONE_COUNT];
    double sine_sum[LOOPBACK_TONE_COUNT];
} channel_measurement_t;

static int32_t decode_signed24(uint32_t word)
{
    const uint32_t sample = word >> 8;
    // Avoid implementation-defined signed shifts/conversion of negative words.
    return (sample & UINT32_C(0x800000)) != 0U
               ? (int32_t)sample - INT32_C(0x1000000)
               : (int32_t)sample;
}

static esp_err_t read_frames(pcm1808_i2s_t *adc, uint32_t *buffer,
                             size_t requested, size_t *received)
{
    const esp_err_t error = pcm1808_i2s_read(adc, buffer, requested, received,
                                             LOOPBACK_READ_TIMEOUT_MS);
    if (error != ESP_OK) {
        return error;
    }
    if (*received == 0U || *received > requested) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static void accumulate(channel_measurement_t *channel, int32_t sample,
                       size_t frames_captured, double inverse_frame_count,
                       const double *cosine, const double *sine,
                       size_t table_index)
{
    if (frames_captured == 1U || sample < channel->minimum) {
        channel->minimum = sample;
    }
    if (frames_captured == 1U || sample > channel->maximum) {
        channel->maximum = sample;
    }
    if (sample == -INT32_C(8388608) || sample == INT32_C(8388607)) {
        ++channel->rail_hits;
    }

    const double value = (double)sample;
    const double difference = value - channel->mean;
    channel->mean += difference * inverse_frame_count;
    channel->squared_deviations += difference * (value - channel->mean);

    for (size_t tone = 0U; tone < LOOPBACK_TONE_COUNT; ++tone) {
        const size_t index = (table_index * (tone + 1U)) % LOOPBACK_TABLE_LENGTH;
        channel->cosine_sum[tone] += value * cosine[index];
        channel->sine_sum[tone] += value * sine[index];
    }
}

static double peak_dbfs(double peak)
{
    return peak > 0.0 ? 20.0 * log10(peak / LOOPBACK_FULL_SCALE) : -INFINITY;
}

static bool valid_capture_ms(uint32_t capture_ms)
{
    return capture_ms == 50U || capture_ms == 100U || capture_ms == 200U ||
           capture_ms == 250U || capture_ms == 1000U;
}

esp_err_t rf_loopback_capture(pcm1808_i2s_t *adc, uint32_t startup_discard_ms,
                              uint32_t capture_ms, rf_loopback_result_t *result)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    if (adc == NULL || startup_discard_ms > 5000U || !valid_capture_ms(capture_ms)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (adc->rx_channel == NULL || !adc->enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    if (adc->sample_rate_hz != LOOPBACK_SAMPLE_RATE_HZ) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t *buffer = malloc(LOOPBACK_BUFFER_FRAMES *
                              PCM1808_I2S_WORDS_PER_FRAME * sizeof(*buffer));
    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    double cosine[LOOPBACK_TABLE_LENGTH];
    double sine[LOOPBACK_TABLE_LENGTH];
    // 48 samples are one 1 kHz cycle; twice the index selects 2 kHz. Enforce
    // exact opposite halves so a constant DC input cancels in both tone bins.
    for (size_t index = 0U; index < LOOPBACK_TABLE_LENGTH / 2U; ++index) {
        const double angle = 2.0 * LOOPBACK_PI * (double)index /
                             (double)LOOPBACK_TABLE_LENGTH;
        cosine[index] = cos(angle);
        sine[index] = sin(angle);
        cosine[index + LOOPBACK_TABLE_LENGTH / 2U] = -cosine[index];
        sine[index + LOOPBACK_TABLE_LENGTH / 2U] = -sine[index];
    }

    const size_t discard_frames =
        (size_t)((uint64_t)adc->sample_rate_hz * startup_discard_ms / 1000U);
    const size_t capture_frames =
        (size_t)((uint64_t)adc->sample_rate_hz * capture_ms / 1000U);

    esp_err_t error = ESP_OK;
    size_t remaining = discard_frames;
    while (remaining > 0U) {
        const size_t requested = remaining < LOOPBACK_BUFFER_FRAMES
                                     ? remaining : LOOPBACK_BUFFER_FRAMES;
        size_t received = 0U;
        error = read_frames(adc, buffer, requested, &received);
        if (error != ESP_OK) {
            goto cleanup;
        }
        remaining -= received;
    }

    channel_measurement_t channels[PCM1808_I2S_WORDS_PER_FRAME] = {0};
    size_t frames_captured = 0U;
    size_t nonzero_padding_words = 0U;
    while (frames_captured < capture_frames) {
        remaining = capture_frames - frames_captured;
        const size_t requested = remaining < LOOPBACK_BUFFER_FRAMES
                                     ? remaining : LOOPBACK_BUFFER_FRAMES;
        size_t received = 0U;
        error = read_frames(adc, buffer, requested, &received);
        if (error != ESP_OK) {
            goto cleanup;
        }

        for (size_t frame = 0U; frame < received; ++frame) {
            const size_t table_index = frames_captured % LOOPBACK_TABLE_LENGTH;
            ++frames_captured;
            const double inverse_frame_count = 1.0 / (double)frames_captured;
            for (size_t channel = 0U; channel < PCM1808_I2S_WORDS_PER_FRAME;
                 ++channel) {
                const uint32_t word =
                    buffer[frame * PCM1808_I2S_WORDS_PER_FRAME + channel];
                nonzero_padding_words += (word & UINT32_C(0xFF)) != 0U;
                accumulate(&channels[channel], decode_signed24(word),
                           frames_captured, inverse_frame_count,
                           cosine, sine, table_index);
            }
        }
    }

    result->capture_ms = capture_ms;
    result->frames_captured = frames_captured;
    result->discard_frames = discard_frames;
    result->nonzero_padding_words = nonzero_padding_words;
    for (size_t channel = 0U; channel < PCM1808_I2S_WORDS_PER_FRAME; ++channel) {
        const channel_measurement_t *measurement = &channels[channel];
        result->channels[channel] = (rf_loopback_channel_result_t) {
            .minimum = measurement->minimum,
            .maximum = measurement->maximum,
            .rail_hits = measurement->rail_hits,
            .dc = measurement->mean,
            .rms_ac = sqrt(fmax(0.0, measurement->squared_deviations /
                                     (double)frames_captured)),
        };
    }

    for (size_t tone = 0U; tone < LOOPBACK_TONE_COUNT; ++tone) {
        double phase[PCM1808_I2S_WORDS_PER_FRAME];
        rf_loopback_tone_result_t *tone_result = &result->tones[tone];
        tone_result->frequency_hz = (uint32_t)((tone + 1U) * 1000U);
        for (size_t channel = 0U; channel < PCM1808_I2S_WORDS_PER_FRAME; ++channel) {
            const channel_measurement_t *measurement = &channels[channel];
            tone_result->peak[channel] =
                2.0 * hypot(measurement->cosine_sum[tone],
                             measurement->sine_sum[tone]) / (double)frames_captured;
            tone_result->dbfs[channel] = peak_dbfs(tone_result->peak[channel]);
            phase[channel] = atan2(-measurement->sine_sum[tone],
                                    measurement->cosine_sum[tone]);
        }
        double relative_phase = (phase[1] - phase[0]) * 180.0 / LOOPBACK_PI;
        if (relative_phase > 180.0) {
            relative_phase -= 360.0;
        } else if (relative_phase < -180.0) {
            relative_phase += 360.0;
        }
        tone_result->relative_phase_degrees = relative_phase;
    }

cleanup:
    free(buffer);
    return error;
}

esp_err_t rf_loopback_report(const char *label, const rf_loopback_result_t *result)
{
    if (label == NULL || label[0] == '\0' || result == NULL ||
        !valid_capture_ms(result->capture_ms) ||
        result->frames_captured != (size_t)result->capture_ms * 48U) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "RF LOOPBACK label=%s frames=%u discard_frames=%u padding_nonzero=%u",
             label, (unsigned)result->frames_captured, (unsigned)result->discard_frames,
             (unsigned)result->nonzero_padding_words);
    for (size_t channel = 0U; channel < PCM1808_I2S_WORDS_PER_FRAME; ++channel) {
        const rf_loopback_channel_result_t *measurement = &result->channels[channel];
        ESP_LOGI(TAG, "RF LOOPBACK %s min=%" PRId32 " max=%" PRId32
                      " dc=%.3f rms_ac=%.3f rail_hits=%u",
                 channel == 0U ? "L" : "R", measurement->minimum,
                 measurement->maximum, measurement->dc, measurement->rms_ac,
                 (unsigned)measurement->rail_hits);
    }
    for (size_t tone = 0U; tone < LOOPBACK_TONE_COUNT; ++tone) {
        const rf_loopback_tone_result_t *measurement = &result->tones[tone];
        ESP_LOGI(TAG, "RF LOOPBACK tone=%u Hz L_peak=%.3f L_dBFS=%.2f"
                      " R_peak=%.3f R_dBFS=%.2f phase_R-L=%.2f deg",
                 (unsigned)measurement->frequency_hz,
                 measurement->peak[0], measurement->dbfs[0],
                 measurement->peak[1], measurement->dbfs[1],
                 measurement->relative_phase_degrees);
    }
    ESP_LOGI(TAG, "RF LOOPBACK measurement complete; not an analog PASS;"
                  " tone peak reference=2^23, phase meaningful only above noise");
    if (result->nonzero_padding_words > 0U || result->channels[0].rail_hits > 0U ||
        result->channels[1].rail_hits > 0U) {
        ESP_LOGW(TAG, "loopback sample warning: nonzero padding or rail hits");
    }
    return ESP_OK;
}

esp_err_t rf_loopback_measure_window(pcm1808_i2s_t *adc, const char *label,
                                     uint32_t startup_discard_ms,
                                     uint32_t capture_ms)
{
    if (adc == NULL || label == NULL || label[0] == '\0' ||
        startup_discard_ms > 5000U || !valid_capture_ms(capture_ms)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (adc->rx_channel == NULL || !adc->enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    if (adc->sample_rate_hz != LOOPBACK_SAMPLE_RATE_HZ) {
        ESP_LOGE(TAG, "loopback measurement requires Fs=48000 Hz");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "RF LOOPBACK begin label=%s discard_ms=%" PRIu32
                  " capture_ms=%" PRIu32 " Fs=%" PRIu32,
             label, startup_discard_ms, capture_ms, adc->sample_rate_hz);
    rf_loopback_result_t result;
    const esp_err_t error = rf_loopback_capture(adc, startup_discard_ms,
                                               capture_ms, &result);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "loopback capture failed: %s", esp_err_to_name(error));
        return error;
    }
    return rf_loopback_report(label, &result);
}

esp_err_t rf_loopback_measure(pcm1808_i2s_t *adc, const char *label,
                              uint32_t startup_discard_ms)
{
    return rf_loopback_measure_window(adc, label, startup_discard_ms, 1000U);
}
