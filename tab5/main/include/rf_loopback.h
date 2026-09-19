// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "pcm1808_i2s.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RF_LOOPBACK_TONE_COUNT 2U

typedef struct {
    int32_t minimum;
    int32_t maximum;
    size_t rail_hits;
    double dc;
    double rms_ac;
} rf_loopback_channel_result_t;

typedef struct {
    uint32_t frequency_hz;
    double peak[PCM1808_I2S_WORDS_PER_FRAME];
    double dbfs[PCM1808_I2S_WORDS_PER_FRAME];
    double relative_phase_degrees;
} rf_loopback_tone_result_t;

typedef struct {
    uint32_t capture_ms;
    size_t frames_captured;
    size_t discard_frames;
    size_t nonzero_padding_words;
    rf_loopback_channel_result_t channels[PCM1808_I2S_WORDS_PER_FRAME];
    rf_loopback_tone_result_t tones[RF_LOOPBACK_TONE_COUNT];
} rf_loopback_result_t;

/**
 * Quiet capture for a caller-controlled PA burst. Discard 0..5000 ms, then
 * capture exactly 50, 100, 200, 250, or 1000 ms of the enabled 48 kHz stream.
 * This function emits no measurement logs, so the caller can disable its RF
 * source before printing. The underlying I2S driver may still log a malformed
 * transfer. Result is valid only on ESP_OK and is cleared on failure. No GPIOs,
 * RF clocks, or I2S channel state are changed.
 *
 * Capture duration describes sample count, NOT a hard wall-clock deadline:
 * reads can block and queued DMA samples can predate the call. The caller must
 * drain stale data and enforce any PA cutoff independently of this function.
 */
esp_err_t rf_loopback_capture(pcm1808_i2s_t *adc, uint32_t startup_discard_ms,
                              uint32_t capture_ms, rf_loopback_result_t *result);

/** Print a successful capture after the caller has made the RF source safe. */
esp_err_t rf_loopback_report(const char *label, const rf_loopback_result_t *result);

/** Logged convenience wrapper; NOT suitable for logging-free active PA bursts. */
esp_err_t rf_loopback_measure_window(pcm1808_i2s_t *adc, const char *label,
                                     uint32_t startup_discard_ms,
                                     uint32_t capture_ms);

/**
 * Drain startup_discard_ms (0..5000) of an already enabled, 48 kHz ADC stream,
 * then measure one second of stereo audio. Log signed-24 sample statistics and
 * coherent single-bin projections at 1 kHz and 2 kHz. Tone dBFS uses 2^23 as
 * full-scale peak; phase is right minus left, wrapped to [-180, 180] degrees.
 * Rail hits count exact minimum/maximum signed-24 codes, not all distortion.
 * Phase is not meaningful when either tone is buried in noise or absent.
 *
 * The caller owns all RF switching, settling policy, I2S startup, and cleanup.
 * This bounded measurement never changes GPIOs, clocks, or RX channel state.
 * Successful completion means that samples were read, not an analog PASS;
 * single-bin estimates can under-read an off-frequency tone and do not verify
 * DMA continuity, absolute RF gain, isolation, or full receiver performance.
 */
esp_err_t rf_loopback_measure(pcm1808_i2s_t *adc, const char *label,
                              uint32_t startup_discard_ms);

#ifdef __cplusplus
}
#endif
