// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

#include "pcm1808_i2s.h"

#ifdef __cplusplus
extern "C" {
#endif

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
