// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SI5351_DEFAULT_I2C_ADDRESS 0x60U

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t i2c_device;
    uint8_t address;
    uint32_t reference_hz;
    uint8_t reference_load_register;
} si5351_t;

typedef enum {
    SI5351_REFERENCE_EXTERNAL_CLOCK = 0x12,
    SI5351_REFERENCE_CRYSTAL_6PF = 0x52,
    SI5351_REFERENCE_CRYSTAL_8PF = 0x92,
    SI5351_REFERENCE_CRYSTAL_10PF = 0xD2,
} si5351_reference_mode_t;

#define SI5351_OUTPUT_CLK0 (1U << 0)
#define SI5351_OUTPUT_CLK1 (1U << 1)

/** Attach an Si5351A to an already-created ESP-IDF I2C master bus. */
esp_err_t si5351_init(si5351_t *device, i2c_master_bus_handle_t bus,
                      uint8_t address, uint32_t reference_hz,
                      si5351_reference_mode_t reference_mode,
                      uint32_t i2c_frequency_hz);

/** Remove the I2C device handle created by si5351_init(). */
esp_err_t si5351_deinit(si5351_t *device);

/** Perform an address-only probe. */
esp_err_t si5351_probe(const si5351_t *device, int timeout_ms);

/** Basic register access. Reads deliberately transfer one byte at a time. */
esp_err_t si5351_write_register(si5351_t *device, uint8_t reg, uint8_t value);
esp_err_t si5351_write_registers(si5351_t *device, uint8_t start_reg,
                                 const uint8_t *values, size_t length);
esp_err_t si5351_read_register(si5351_t *device, uint8_t reg, uint8_t *value);

/** Wait for reference/device initialization (and loss-of-signal) to clear. */
esp_err_t si5351_wait_ready(si5351_t *device, uint32_t timeout_ms);

/**
 * Configure the 40 m DXFT8 clock plan.
 *
 * PLLA and PLLB run at 112 * rf_hz. CLK0 is the transmit/PA clock at RF
 * (integer divide by 112), and CLK1 is the receive QSD clock at 4*RF (integer
 * divide by 28). The function finishes in RX state: CLK1 on and CLK0 off.
 */
esp_err_t si5351_configure_40m_clock_plan(si5351_t *device, uint32_t rf_hz,
                                          bool verify);

/**
 * Configure independent RX and source clocks for a BS170-absent bench test.
 *
 * CLK0 uses PLLA=112*source_frequency_hz and integer MS0=112; CLK1 uses
 * PLLB=112*rx_frequency_hz and integer MS1=28, giving the QSD clock at 4*RX.
 * Both input frequencies must be within 5,357,143..8,035,714 Hz. At nominal
 * 26 MHz reference, RX=7,074,000 and source=7,075,000 or 7,076,000 Hz give
 * exact nominal 1 kHz or 2 kHz offsets. Absolute accuracy follows the reference.
 *
 * Disables all outputs before configuring, clears spread spectrum and both
 * phase offsets, checks both PLLs, and finishes with CLK1 ONLY enabled. The
 * caller may then enable CLK0 alongside CLK1 for an A/B leakage measurement.
 * On failure, requests all outputs disabled (best effort if I2C has failed).
 * This function does not control board power or TX/RX GPIOs and is NOT a
 * permission or interlock for operating a populated power amplifier.
 */
esp_err_t si5351_configure_rx_loopback(si5351_t *device,
                                       uint32_t rx_frequency_hz,
                                       uint32_t source_frequency_hz,
                                       bool verify);

/**
 * Generate a continuous CLK0 carrier for the 20 m scope bring-up.
 *
 * Uses PLLA with MS0=56 and R0=1. Supported frequencies are 10,714,286 through
 * 16,071,428 Hz; 14,075,000 Hz uses PLLA=788.2 MHz. The reference configured by
 * si5351_init() is honored (the RF board uses a nominal 26 MHz external clock
 * into XA). Frequency accuracy therefore follows that physical reference.
 *
 * Disables all outputs while programming, powers down unused drivers, disables
 * spread spectrum, verifies PLLA lock, then enables CLK0 only at 8 mA drive.
 * The caller must separately establish the RF board's power and TX/RX state.
 */
esp_err_t si5351_configure_clk0_carrier(si5351_t *device,
                                        uint32_t frequency_hz, bool verify);

/** Enable a logical set of CLK0..CLK7 outputs; zero disables every output. */
esp_err_t si5351_set_enabled_outputs(si5351_t *device,
                                     uint8_t enabled_outputs, bool verify);

#ifdef __cplusplus
}
#endif
