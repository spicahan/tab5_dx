// SPDX-License-Identifier: MIT

#include "sdkconfig.h"
#if CONFIG_DXFT8_PA_TEST
#include "pa_test.h"
void app_main(void)
{
    pa_test_run();
}
#else

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "pcm1808_i2s.h"
#include "rf_loopback.h"
#include "si5351.h"

#if CONFIG_DXFT8_TAB5_I2C_INTERNAL_PULLUPS
#define TAB5_I2C_INTERNAL_PULLUPS true
#else
#define TAB5_I2C_INTERNAL_PULLUPS false
#endif

#if CONFIG_DXFT8_SI5351_EXTERNAL_REFERENCE
#define TAB5_SI5351_REFERENCE_MODE SI5351_REFERENCE_EXTERNAL_CLOCK
#define TAB5_SI5351_REFERENCE_DESCRIPTION "external clock on XA (0 pF)"
#elif CONFIG_DXFT8_SI5351_CRYSTAL_6PF
#define TAB5_SI5351_REFERENCE_MODE SI5351_REFERENCE_CRYSTAL_6PF
#define TAB5_SI5351_REFERENCE_DESCRIPTION "passive crystal (6 pF)"
#elif CONFIG_DXFT8_SI5351_CRYSTAL_8PF
#define TAB5_SI5351_REFERENCE_MODE SI5351_REFERENCE_CRYSTAL_8PF
#define TAB5_SI5351_REFERENCE_DESCRIPTION "passive crystal (8 pF)"
#else
#define TAB5_SI5351_REFERENCE_MODE SI5351_REFERENCE_CRYSTAL_10PF
#define TAB5_SI5351_REFERENCE_DESCRIPTION "passive crystal (10 pF)"
#endif

#define TEST_RF_HZ          7074000U
#define SCOPE_CLK0_HZ       14075000U
#define DEVICE_READY_TIMEOUT_MS 2000U

static const char *TAG = "tab5_bringup";

#if CONFIG_DXFT8_REAL_RF_BOARD
#define RF_POWER_GPIO GPIO_NUM_48
#define RF_RXSW_GPIO GPIO_NUM_47

static esp_err_t enable_rf_board_power(void)
{
    // Preload the output latches before enabling the GPIO output drivers.
    // Firmware cannot guarantee these levels during reset: hardware biasing
    // remains necessary on a finished transceiver.
    esp_err_t error = gpio_set_level(RF_POWER_GPIO, 0);
    if (error != ESP_OK) {
        return error;
    }
    error = gpio_set_level(RF_RXSW_GPIO, 1);
    if (error != ESP_OK) {
        return error;
    }
    const gpio_config_t control_pins = {
        .pin_bit_mask = (1ULL << RF_POWER_GPIO) | (1ULL << RF_RXSW_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    error = gpio_config(&control_pins);
    if (error != ESP_OK) {
        return error;
    }
    // Give a previous powered run time to discharge before starting again.
    vTaskDelay(pdMS_TO_TICKS(100));
    error = gpio_set_level(RF_POWER_GPIO, 1);
    if (error != ESP_OK) {
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "RF board: G48=HIGH (power enabled), G47=HIGH (RX / TX off)");
    return ESP_OK;
}
#endif

typedef enum {
    FRONTEND_CLOCKS_OFF,
    FRONTEND_CLOCKS_RX,
    FRONTEND_CLOCKS_TX_READY,
} frontend_clock_state_t;

static esp_err_t set_frontend_clock_state(si5351_t *clock,
                                          frontend_clock_state_t state,
                                          bool verify)
{
    uint8_t enabled_outputs = 0U;
    const char *description = "OFF";

    switch (state) {
    case FRONTEND_CLOCKS_OFF:
        break;
    case FRONTEND_CLOCKS_RX:
        enabled_outputs = SI5351_OUTPUT_CLK1;
        description = "RX (CLK1 QSD on)";
        break;
    case FRONTEND_CLOCKS_TX_READY:
        // Mock-only register-mask exercise, not the real-board TX sequence.
        // The real board uses G47 for RX/TX and G48 for main RF power.
        enabled_outputs = SI5351_OUTPUT_CLK0 | SI5351_OUTPUT_CLK1;
        description = "TX-ready (CLK0 PA + CLK1 QSD on)";
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t error = si5351_set_enabled_outputs(clock,
                                                        enabled_outputs,
                                                        verify);
    if (error == ESP_OK) {
        ESP_LOGI(TAG, "front-end clock state: %s", description);
    }
    return error;
}

static void stop_on_failure(si5351_t *clock, const char *stage, esp_err_t error)
{
    ESP_LOGE(TAG, "SELF-TEST FAILED at %s: %s", stage,
             esp_err_to_name(error));
    if (clock != NULL && clock->i2c_device != NULL) {
        (void)set_frontend_clock_state(clock, FRONTEND_CLOCKS_OFF, false);
    }
#if CONFIG_DXFT8_REAL_RF_BOARD
    (void)gpio_set_level(RF_RXSW_GPIO, 1);
    (void)gpio_set_level(RF_POWER_GPIO, 0);
    ESP_LOGE(TAG, "RF power requested OFF (G48=LOW); RX selected (G47=HIGH)");
#endif
    ESP_LOGE(TAG, "outputs requested OFF; reset the board to retry");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#if CONFIG_DXFT8_RX_LOOPBACK_BENCH
static esp_err_t run_rx_loopback(si5351_t *clock, pcm1808_i2s_t *adc)
{
    // Deliberately RX-selected even while CLK0 runs: G47 LOW would disconnect
    // the receiver. Safe only with the PA BS170s physically absent.
    if (adc->sample_rate_hz != 48000U ||
        CONFIG_DXFT8_I2S_DIAGNOSTIC_STARTUP_MS != 1000) {
        return ESP_ERR_INVALID_ARG;
    }
    static const struct {
        const char *label;
        uint32_t source_hz;
        bool source_on;
    } steps[] = {
        {"OFF_BEFORE", TEST_RF_HZ + 1000U, false},
        {"ON_1KHZ", TEST_RF_HZ + 1000U, true},
        {"OFF_AFTER", TEST_RF_HZ + 1000U, false},
        {"ON_2KHZ", TEST_RF_HZ + 2000U, true},
        {"HOLD_1KHZ", TEST_RF_HZ + 1000U, true},
    };
    esp_err_t error = gpio_set_level(RF_RXSW_GPIO, 1);
    if (error != ESP_OK) {
        return error;
    }
    ESP_LOGW(TAG, "RX LOOPBACK BENCH: BS170s MUST be absent; no antenna; "
                  "G48=HIGH, G47=HIGH; one-second drain before each measurement");
    uint32_t configured_source_hz = 0U;
    for (size_t step = 0; step < sizeof(steps) / sizeof(steps[0]); ++step) {
        if (steps[step].source_hz != configured_source_hz) {
            error = si5351_configure_rx_loopback(clock, TEST_RF_HZ,
                                                 steps[step].source_hz, true);
            if (error != ESP_OK) {
                return error;
            }
            configured_source_hz = steps[step].source_hz;
        }
        const uint8_t outputs = SI5351_OUTPUT_CLK1 |
            (steps[step].source_on ? SI5351_OUTPUT_CLK0 : 0U);
        error = si5351_set_enabled_outputs(clock, outputs, true);
        if (error != ESP_OK) {
            return error;
        }
        ESP_LOGI(TAG, "LOOPBACK STATE %s: CLK0=%" PRIu32 " Hz %s; "
                     "CLK1=28296000 Hz; RX=7074000 Hz; G47=HIGH",
                 steps[step].label, steps[step].source_hz,
                 steps[step].source_on ? "ON" : "OFF");
        error = rf_loopback_measure(adc, steps[step].label, 1000U);
        if (error != ESP_OK) {
            return error;
        }
    }
    ESP_LOGW(TAG, "RX LOOPBACK HOLD: CLK0=7075000 Hz; CLK1=28296000 Hz; "
                  "expected beat=1000 Hz; G48=HIGH, G47=HIGH; "
                  "I2S remains active; BS170s MUST remain absent");
    return ESP_OK;
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "TAB5 DXFT8 Si5351 host self-test");
    ESP_LOGI(TAG,
             "I2C host: SDA=GPIO%d, SCL=GPIO%d, address=0x%02X",
             CONFIG_DXFT8_TAB5_I2C_SDA_GPIO,
             CONFIG_DXFT8_TAB5_I2C_SCL_GPIO,
             CONFIG_DXFT8_SI5351_ADDRESS);
    ESP_LOGI(TAG, "reference=%d Hz, %s",
             CONFIG_DXFT8_SI5351_REFERENCE_HZ,
             TAB5_SI5351_REFERENCE_DESCRIPTION);

#if CONFIG_DXFT8_REAL_RF_BOARD
    ESP_LOGW(TAG, "REAL RF BOARD BENCH MODE: no LPF/TX interlock implemented");
    const esp_err_t power_error = enable_rf_board_power();
    if (power_error != ESP_OK) {
        stop_on_failure(NULL, "RF power/RX GPIO initialization", power_error);
    }
#endif
#if CONFIG_DXFT8_SCOPE_CLK0_CARRIER
    ESP_LOGW(TAG, "SCOPE MODE: BS170s must remain absent; CLK0 will stay on after checks");
#endif

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = (gpio_num_t)CONFIG_DXFT8_TAB5_I2C_SDA_GPIO,
        .scl_io_num = (gpio_num_t)CONFIG_DXFT8_TAB5_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = TAB5_I2C_INTERNAL_PULLUPS,
        },
    };

    i2c_master_bus_handle_t bus = NULL;
    esp_err_t error = i2c_new_master_bus(&bus_config, &bus);
    if (error != ESP_OK) {
        stop_on_failure(NULL, "I2C bus initialization", error);
    }

    si5351_t clock = {0};
    error = si5351_init(&clock, bus, CONFIG_DXFT8_SI5351_ADDRESS,
                        CONFIG_DXFT8_SI5351_REFERENCE_HZ,
                        TAB5_SI5351_REFERENCE_MODE,
                        CONFIG_DXFT8_TAB5_I2C_FREQUENCY_HZ);
    if (error != ESP_OK) {
        stop_on_failure(NULL, "Si5351 handle initialization", error);
    }

    // The mock and real daughter board can become ready after the Tab5.
    // Retry an absent/busy device for a bounded startup interval.
    const TickType_t probe_started = xTaskGetTickCount();
    do {
        error = si5351_probe(&clock, 100);
        if (error != ESP_ERR_NOT_FOUND && error != ESP_ERR_TIMEOUT) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    } while ((xTaskGetTickCount() - probe_started) <
             pdMS_TO_TICKS(DEVICE_READY_TIMEOUT_MS));
    if (error != ESP_OK) {
#if CONFIG_DXFT8_REAL_RF_BOARD
        // Address-only probes help distinguish an absent RF device from a
        // completely unavailable shared internal bus. Do not touch registers
        // of the Tab5's other peripherals.
        ESP_LOGW(TAG, "Si5351 absent; scanning internal I2C while RF power is enabled");
        for (uint8_t address = 0x08U; address < 0x78U; ++address) {
            if (i2c_master_probe(bus, address, 20) == ESP_OK) {
                ESP_LOGI(TAG, "I2C diagnostic ACK: 0x%02X", address);
            }
        }
        ESP_LOGW(TAG, "Check RF-board supply (battery+ lead / DC input), "
                      "3.3 V rail, header seating and SDA/SCL; G48 is only an enable");
#endif
        stop_on_failure(&clock, "address probe", error);
    }
    ESP_LOGI(TAG, "probe PASS: found Si5351 at 0x%02X",
             CONFIG_DXFT8_SI5351_ADDRESS);

    uint8_t device_status = 0;
    uint8_t interrupt_status = 0;
    error = si5351_read_register(&clock, 0U, &device_status);
    if (error != ESP_OK) {
        stop_on_failure(&clock, "device-status read", error);
    }
    error = si5351_read_register(&clock, 1U, &interrupt_status);
    if (error != ESP_OK) {
        stop_on_failure(&clock, "interrupt-status read", error);
    }
    ESP_LOGI(TAG, "status PASS: device=0x%02X, interrupt=0x%02X",
             device_status, interrupt_status);

    error = si5351_configure_40m_clock_plan(&clock, TEST_RF_HZ, true);
    if (error != ESP_OK) {
        stop_on_failure(&clock, "7.074 MHz DXFT8 clock plan", error);
    }
    ESP_LOGI(TAG,
             "7.074 MHz plan PASS: RX CLK1=28.296 MHz enabled; "
             "TX CLK0=7.074 MHz configured but disabled");

#if CONFIG_DXFT8_RUN_TX_PATH_SELF_TEST
    ESP_LOGW(TAG,
             "TX CLOCK SELF-TEST ENABLED: use only with the mock or an unpowered PA");
    vTaskDelay(pdMS_TO_TICKS(50));
    error = set_frontend_clock_state(&clock, FRONTEND_CLOCKS_TX_READY, true);
    if (error != ESP_OK) {
        stop_on_failure(&clock, "RX-to-TX clock-state switch", error);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    error = set_frontend_clock_state(&clock, FRONTEND_CLOCKS_RX, true);
    if (error != ESP_OK) {
        stop_on_failure(&clock, "TX-to-RX clock-state switch", error);
    }
    ESP_LOGI(TAG, "RX/TX-ready/RX clock-state switching PASS");
#else
    ESP_LOGI(TAG, "TX clock self-test disabled; RX clock selected for validation");
#endif

    ESP_LOGI(TAG, "SI5351 SELF-TEST PASS");

#if CONFIG_DXFT8_I2S_CONTINUOUS_DIAGNOSTIC
#if CONFIG_DXFT8_I2S_DIAGNOSTIC_RX_CLOCK
    error = set_frontend_clock_state(&clock, FRONTEND_CLOCKS_RX, true);
    if (error != ESP_OK) {
        stop_on_failure(&clock, "retain RX clock for ADC comparison", error);
    }
    ESP_LOGW(TAG, "CONTINUOUS I2S BENCH MODE: CLK1/QSD=28296000 Hz; CLK0 OFF; "
                  "G48=HIGH, G47=HIGH; BS170s must remain absent");
#else
    error = set_frontend_clock_state(&clock, FRONTEND_CLOCKS_OFF, true);
    if (error != ESP_OK) {
        stop_on_failure(&clock, "disable RF clocks for ADC diagnostic", error);
    }
    ESP_LOGW(TAG, "CONTINUOUS I2S BENCH MODE: Si5351 outputs OFF; "
                  "G48=HIGH, G47=HIGH; BS170s must remain absent");
#endif
#endif

#if CONFIG_DXFT8_RUN_I2S_SELF_TEST
    ESP_LOGI(TAG, "starting PCM1808-compatible I2S validation");
    const pcm1808_i2s_config_t adc_config = {
        .port = I2S_NUM_1,
        .mclk_gpio = (gpio_num_t)CONFIG_DXFT8_TAB5_I2S_MCLK_GPIO,
        .bclk_gpio = (gpio_num_t)CONFIG_DXFT8_TAB5_I2S_BCLK_GPIO,
        .lrck_gpio = (gpio_num_t)CONFIG_DXFT8_TAB5_I2S_LRCK_GPIO,
        .din_gpio = (gpio_num_t)CONFIG_DXFT8_TAB5_I2S_DIN_GPIO,
        .sample_rate_hz = CONFIG_DXFT8_TAB5_I2S_SAMPLE_RATE_HZ,
    };
    pcm1808_i2s_t adc = {0};
    error = pcm1808_i2s_init(&adc, &adc_config);
    if (error != ESP_OK) {
        stop_on_failure(&clock, "I2S master initialization", error);
    }
    error = pcm1808_i2s_enable(&adc);
    if (error != ESP_OK) {
        const esp_err_t cleanup_error = pcm1808_i2s_deinit(&adc);
        if (cleanup_error != ESP_OK) {
            ESP_LOGE(TAG, "I2S cleanup also failed: %s",
                     esp_err_to_name(cleanup_error));
        }
        stop_on_failure(&clock, "I2S clock/RX start", error);
    }

#if CONFIG_DXFT8_I2S_CONTINUOUS_DIAGNOSTIC
#if CONFIG_DXFT8_RX_LOOPBACK_BENCH
    error = run_rx_loopback(&clock, &adc);
    if (error != ESP_OK) {
        // Turn the source off before I2S teardown; stop_on_failure also requests
        // all clocks off and RF power off if the first cleanup write fails.
        (void)si5351_set_enabled_outputs(&clock, 0U, false);
        (void)pcm1808_i2s_deinit(&adc);
        stop_on_failure(&clock, "RX leakage loopback", error);
    }
#endif
    ESP_LOGI(TAG, "I2S DIAGNOSTIC ACTIVE: clocks remain on for probing; "
                  "flat data is reported, not treated as an ADC pass");
    error = pcm1808_i2s_run_diagnostics(&adc,
                                      CONFIG_DXFT8_I2S_DIAGNOSTIC_STARTUP_MS);
    // The continuous diagnostic returns only on a transport/API failure.
    // Keep ordinary fail-closed handling for that failure, not sample quality.
    const esp_err_t cleanup_error = pcm1808_i2s_deinit(&adc);
    if (cleanup_error != ESP_OK) {
        ESP_LOGE(TAG, "I2S cleanup also failed: %s",
                 esp_err_to_name(cleanup_error));
    }
    stop_on_failure(&clock, "continuous I2S transport",
                    error == ESP_OK ? ESP_ERR_INVALID_STATE : error);
#elif CONFIG_DXFT8_I2S_MOCK_PATTERN_TEST
    pcm1808_mock_test_result_t mock_result = {0};
    error = pcm1808_i2s_validate_mock(&adc,
                                      CONFIG_DXFT8_I2S_TEST_FRAMES,
                                      &mock_result);
    if (error != ESP_OK) {
        const esp_err_t cleanup_error = pcm1808_i2s_deinit(&adc);
        if (cleanup_error != ESP_OK) {
            ESP_LOGE(TAG, "I2S cleanup also failed: %s",
                     esp_err_to_name(cleanup_error));
        }
        stop_on_failure(&clock, "I2S mock-pattern validation", error);
    }
    ESP_LOGI(TAG,
             "I2S SELF-TEST PASS: %u frames checked after %u startup frames",
             (unsigned)mock_result.frames_checked,
             (unsigned)mock_result.startup_frames_discarded);
#else
    pcm1808_capture_stats_t capture_stats = {0};
    error = pcm1808_i2s_capture_stats(&adc,
                                      CONFIG_DXFT8_I2S_TEST_FRAMES,
                                      &capture_stats);
    if (error != ESP_OK) {
        const esp_err_t cleanup_error = pcm1808_i2s_deinit(&adc);
        if (cleanup_error != ESP_OK) {
            ESP_LOGE(TAG, "I2S cleanup also failed: %s",
                     esp_err_to_name(cleanup_error));
        }
        stop_on_failure(&clock, "PCM1808 audio capture", error);
    }
    ESP_LOGI(TAG, "I2S CAPTURE SANITY PASS: %u PCM1808 frames received",
             (unsigned)capture_stats.frames_captured);
#endif

    error = pcm1808_i2s_deinit(&adc);
    if (error != ESP_OK) {
        stop_on_failure(&clock, "I2S shutdown", error);
    }
#else
    ESP_LOGW(TAG, "I2S self-test disabled; only the Si5351 test was run");
#endif

#if CONFIG_DXFT8_SCOPE_CLK0_CARRIER
    error = si5351_configure_clk0_carrier(&clock, SCOPE_CLK0_HZ, true);
    if (error != ESP_OK) {
        stop_on_failure(&clock, "14.075 MHz CLK0 scope carrier", error);
    }
    ESP_LOGI(TAG, "ALL ENABLED SELF-TESTS PASS");
    ESP_LOGI(TAG,
             "CLK0 SCOPE READY: nominal %" PRIu32 " Hz; CLK0 only; "
             "G48=HIGH, G47=HIGH; I2S clocks stopped", SCOPE_CLK0_HZ);
    ESP_LOGW(TAG, "Scope CLK0/TP1 to board GND with a high-impedance probe; "
                  "this carrier restarts on every boot");
#else
    ESP_LOGI(TAG, "ALL ENABLED SELF-TESTS PASS; final RF clock state remains RX");
#endif
}
#endif // !CONFIG_DXFT8_PA_TEST
