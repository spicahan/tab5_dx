// SPDX-License-Identifier: MIT

#include "si5351.h"

#include <inttypes.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SI5351_XFER_TIMEOUT_MS       250
#define SI5351_MAX_BURST_BYTES       16U
#define SI5351_FRACTION_MAX          1048575U
#define SI5351_PLL_MIN_HZ            600000000ULL
#define SI5351_PLL_MAX_HZ            900000000ULL
#define SI5351_STATUS_POLL_MS         10U
#define SI5351_PLL_RESET_TIMEOUT_MS   50U

#define SI5351_REG_DEVICE_STATUS     0U
#define SI5351_REG_OUTPUT_ENABLE     3U
#define SI5351_REG_PLL_INPUT_SOURCE  15U
#define SI5351_REG_CLK0_CONTROL      16U
#define SI5351_REG_CLK1_CONTROL      17U
#define SI5351_REG_CLK3_0_DISABLE    24U
#define SI5351_REG_PLLA_PARAMETERS   26U
#define SI5351_REG_PLLB_PARAMETERS   34U
#define SI5351_REG_MS0_PARAMETERS    42U
#define SI5351_REG_MS1_PARAMETERS    50U
#define SI5351_REG_SPREAD_SPECTRUM   149U
#define SI5351_REG_CLK0_PHASE        165U
#define SI5351_REG_PLL_RESET         177U
#define SI5351_REG_XTAL_LOAD         183U

#define SI5351_STATUS_SYS_INIT       (1U << 7)
#define SI5351_STATUS_LOL_B          (1U << 6)
#define SI5351_STATUS_LOL_A          (1U << 5)
#define SI5351_STATUS_LOS_XTAL       (1U << 3)

#define SI5351_ALL_OUTPUTS_DISABLED  0xFFU
#define SI5351_CLOCK_POWER_DOWN      0x80U
#define SI5351_CLOCK_INTEGER_PLLA    0x4FU
#define SI5351_CLOCK_INTEGER_PLLB    0x6FU
#define SI5351_RESET_BOTH_PLLS       0xACU
#define SI5351_RESET_STROBE_MASK     ((1U << 7) | (1U << 5))
#define SI5351_SPREAD_SPECTRUM_ENABLE (1U << 7)
#define SI5351_CARRIER_MS_DIVIDER    56U

typedef struct {
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t p1;
    uint32_t p2;
    uint32_t p3;
} si5351_synth_parameters_t;

static const char *TAG = "si5351";

static uint64_t gcd_u64(uint64_t lhs, uint64_t rhs)
{
    while (rhs != 0U) {
        const uint64_t remainder = lhs % rhs;
        lhs = rhs;
        rhs = remainder;
    }
    return lhs;
}

static esp_err_t make_fraction(uint64_t numerator, uint64_t denominator,
                               uint32_t *b, uint32_t *c)
{
    ESP_RETURN_ON_FALSE(denominator != 0U && b != NULL && c != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid fraction");

    if (numerator == 0U) {
        *b = 0U;
        *c = 1U;
        return ESP_OK;
    }

    const uint64_t divisor = gcd_u64(numerator, denominator);
    numerator /= divisor;
    denominator /= divisor;

    if (denominator > SI5351_FRACTION_MAX) {
        // A bounded, rounded approximation.  The direct reduced fraction is
        // used whenever it fits (including all acceptance-test frequencies).
        const uint64_t original_denominator = denominator;
        denominator = SI5351_FRACTION_MAX;
        numerator = (numerator * denominator +
                     (original_denominator / 2U)) /
                    original_denominator;
        if (numerator >= denominator) {
            numerator = denominator - 1U;
        }
        const uint64_t reduced = gcd_u64(numerator, denominator);
        numerator /= reduced;
        denominator /= reduced;
    }

    ESP_RETURN_ON_FALSE(numerator < denominator, ESP_ERR_INVALID_ARG, TAG,
                        "fraction must be less than one");
    *b = (uint32_t)numerator;
    *c = (uint32_t)denominator;
    return ESP_OK;
}

static esp_err_t calculate_parameters(uint64_t numerator_hz,
                                      uint64_t denominator_hz,
                                      uint32_t minimum_a,
                                      uint32_t maximum_a,
                                      si5351_synth_parameters_t *parameters)
{
    ESP_RETURN_ON_FALSE(denominator_hz != 0U && parameters != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid synthesizer ratio");

    const uint64_t integer_part = numerator_hz / denominator_hz;
    const uint64_t remainder = numerator_hz % denominator_hz;
    ESP_RETURN_ON_FALSE(integer_part >= minimum_a && integer_part <= maximum_a,
                        ESP_ERR_INVALID_ARG, TAG,
                        "synthesizer divider out of range: %" PRIu64,
                        integer_part);

    uint32_t b = 0;
    uint32_t c = 1;
    ESP_RETURN_ON_ERROR(make_fraction(remainder, denominator_hz, &b, &c),
                        TAG, "cannot represent synthesizer fraction");

    const uint64_t fractional_term = (128ULL * b) / c;
    parameters->a = (uint32_t)integer_part;
    parameters->b = b;
    parameters->c = c;
    parameters->p1 = (uint32_t)(128ULL * integer_part + fractional_term - 512ULL);
    parameters->p2 = (uint32_t)(128ULL * b - (uint64_t)c * fractional_term);
    parameters->p3 = c;
    return ESP_OK;
}

static void encode_parameters(const si5351_synth_parameters_t *parameters,
                              uint8_t encoded[8])
{
    encoded[0] = (uint8_t)((parameters->p3 >> 8) & 0xFFU);
    encoded[1] = (uint8_t)(parameters->p3 & 0xFFU);
    encoded[2] = (uint8_t)((parameters->p1 >> 16) & 0x03U);
    encoded[3] = (uint8_t)((parameters->p1 >> 8) & 0xFFU);
    encoded[4] = (uint8_t)(parameters->p1 & 0xFFU);
    encoded[5] = (uint8_t)(((parameters->p3 >> 12) & 0xF0U) |
                           ((parameters->p2 >> 16) & 0x0FU));
    encoded[6] = (uint8_t)((parameters->p2 >> 8) & 0xFFU);
    encoded[7] = (uint8_t)(parameters->p2 & 0xFFU);
}

esp_err_t si5351_init(si5351_t *device, i2c_master_bus_handle_t bus,
                      uint8_t address, uint32_t reference_hz,
                      si5351_reference_mode_t reference_mode,
                      uint32_t i2c_frequency_hz)
{
    ESP_RETURN_ON_FALSE(device != NULL && bus != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid device or bus");
    ESP_RETURN_ON_FALSE(address >= 0x08U && address <= 0x77U,
                        ESP_ERR_INVALID_ARG, TAG, "invalid 7-bit address");
    ESP_RETURN_ON_FALSE(reference_hz >= 10000000U &&
                            reference_hz <= 40000000U,
                        ESP_ERR_INVALID_ARG, TAG,
                        "invalid reference frequency");
    ESP_RETURN_ON_FALSE(reference_mode == SI5351_REFERENCE_EXTERNAL_CLOCK ||
                            reference_mode == SI5351_REFERENCE_CRYSTAL_6PF ||
                            reference_mode == SI5351_REFERENCE_CRYSTAL_8PF ||
                            reference_mode == SI5351_REFERENCE_CRYSTAL_10PF,
                        ESP_ERR_INVALID_ARG, TAG, "invalid reference mode");
    ESP_RETURN_ON_FALSE(i2c_frequency_hz > 0U && i2c_frequency_hz <= 400000U,
                        ESP_ERR_INVALID_ARG, TAG, "invalid I2C frequency");

    memset(device, 0, sizeof(*device));
    device->bus = bus;
    device->address = address;
    device->reference_hz = reference_hz;
    device->reference_load_register = (uint8_t)reference_mode;

    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = i2c_frequency_hz,
    };
    return i2c_master_bus_add_device(bus, &config, &device->i2c_device);
}

esp_err_t si5351_deinit(si5351_t *device)
{
    ESP_RETURN_ON_FALSE(device != NULL && device->i2c_device != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "device is not initialized");
    const esp_err_t error = i2c_master_bus_rm_device(device->i2c_device);
    if (error == ESP_OK) {
        device->i2c_device = NULL;
    }
    return error;
}

esp_err_t si5351_probe(const si5351_t *device, int timeout_ms)
{
    ESP_RETURN_ON_FALSE(device != NULL && device->bus != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "device is not initialized");
    return i2c_master_probe(device->bus, device->address, timeout_ms);
}

esp_err_t si5351_write_registers(si5351_t *device, uint8_t start_reg,
                                 const uint8_t *values, size_t length)
{
    ESP_RETURN_ON_FALSE(device != NULL && device->i2c_device != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "device is not initialized");
    ESP_RETURN_ON_FALSE(values != NULL && length > 0U &&
                            length <= SI5351_MAX_BURST_BYTES,
                        ESP_ERR_INVALID_ARG, TAG, "invalid register write");

    uint8_t transfer[SI5351_MAX_BURST_BYTES + 1U];
    transfer[0] = start_reg;
    memcpy(&transfer[1], values, length);
    return i2c_master_transmit(device->i2c_device, transfer, length + 1U,
                               SI5351_XFER_TIMEOUT_MS);
}

esp_err_t si5351_write_register(si5351_t *device, uint8_t reg, uint8_t value)
{
    return si5351_write_registers(device, reg, &value, 1U);
}

esp_err_t si5351_read_register(si5351_t *device, uint8_t reg, uint8_t *value)
{
    ESP_RETURN_ON_FALSE(device != NULL && device->i2c_device != NULL &&
                            value != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid register read");

    // Keep the pointer write and one-byte read as separate transactions.  This
    // is accepted by real Si5351A silicon and matches the IDF 5.5 mock's
    // deliberately one-byte-deep response path.
    ESP_RETURN_ON_ERROR(i2c_master_transmit(device->i2c_device, &reg, 1U,
                                             SI5351_XFER_TIMEOUT_MS),
                        TAG, "register-pointer write failed at 0x%02X", reg);
    return i2c_master_receive(device->i2c_device, value, 1U,
                              SI5351_XFER_TIMEOUT_MS);
}

static esp_err_t verify_registers(si5351_t *device, uint8_t start_reg,
                                  const uint8_t *expected, size_t length)
{
    for (size_t index = 0; index < length; ++index) {
        const uint8_t reg = (uint8_t)(start_reg + index);
        uint8_t actual = 0;
        ESP_RETURN_ON_ERROR(si5351_read_register(device, reg, &actual), TAG,
                            "readback failed at register 0x%02X", reg);
        ESP_RETURN_ON_FALSE(actual == expected[index], ESP_ERR_INVALID_RESPONSE,
                            TAG,
                            "readback mismatch reg 0x%02X: wrote 0x%02X, read 0x%02X",
                            reg, expected[index], actual);
    }
    ESP_LOGI(TAG, "verified registers 0x%02X..0x%02X", start_reg,
             (uint8_t)(start_reg + length - 1U));
    return ESP_OK;
}

static esp_err_t write_checked(si5351_t *device, uint8_t start_reg,
                               const uint8_t *values, size_t length,
                               bool verify)
{
    ESP_RETURN_ON_ERROR(si5351_write_registers(device, start_reg, values,
                                                length),
                        TAG, "write failed at register 0x%02X", start_reg);
    if (!verify) {
        return ESP_OK;
    }
    return verify_registers(device, start_reg, values, length);
}

static esp_err_t write_byte_checked(si5351_t *device, uint8_t reg,
                                    uint8_t value, bool verify)
{
    return write_checked(device, reg, &value, 1U, verify);
}

static esp_err_t wait_status_stably_clear(si5351_t *device, uint8_t mask,
                                          uint32_t timeout_ms,
                                          unsigned required_clear_reads)
{
    ESP_RETURN_ON_FALSE(required_clear_reads > 0U, ESP_ERR_INVALID_ARG, TAG,
                        "at least one clear status read is required");

    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    uint8_t status = 0xFFU;
    unsigned clear_reads = 0U;

    do {
        ESP_RETURN_ON_ERROR(si5351_read_register(device,
                                                  SI5351_REG_DEVICE_STATUS,
                                                  &status),
                            TAG, "status read failed");
        if ((status & mask) == 0U) {
            clear_reads++;
            if (clear_reads >= required_clear_reads) {
                return ESP_OK;
            }
        } else {
            clear_reads = 0U;
        }
        vTaskDelay(pdMS_TO_TICKS(SI5351_STATUS_POLL_MS));
    } while ((xTaskGetTickCount() - start) < timeout);

    ESP_LOGE(TAG, "status 0x%02X did not clear mask 0x%02X in %" PRIu32 " ms",
             status, mask, timeout_ms);
    return ESP_ERR_TIMEOUT;
}

esp_err_t si5351_wait_ready(si5351_t *device, uint32_t timeout_ms)
{
    return wait_status_stably_clear(device, SI5351_STATUS_SYS_INIT |
                                                SI5351_STATUS_LOS_XTAL,
                                    timeout_ms, 2U);
}

static esp_err_t reset_plls(si5351_t *device, bool verify)
{
    ESP_RETURN_ON_ERROR(si5351_write_register(device, SI5351_REG_PLL_RESET,
                                               SI5351_RESET_BOTH_PLLS),
                        TAG, "PLL reset write failed");
    if (!verify) {
        return ESP_OK;
    }

    // Register 177 is a strobe. Allow real silicon time to self-clear the two
    // reset bits, and ignore unrelated/reserved readback bits.
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(SI5351_PLL_RESET_TIMEOUT_MS);
    uint8_t value = 0xFFU;
    do {
        ESP_RETURN_ON_ERROR(si5351_read_register(device,
                                                  SI5351_REG_PLL_RESET,
                                                  &value),
                            TAG, "PLL reset-strobe readback failed");
        if ((value & SI5351_RESET_STROBE_MASK) == 0U) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(SI5351_STATUS_POLL_MS));
    } while ((xTaskGetTickCount() - start) < timeout);
    ESP_RETURN_ON_FALSE((value & SI5351_RESET_STROBE_MASK) == 0U,
                        ESP_ERR_TIMEOUT, TAG,
                        "PLL reset strobe bits did not clear: 0x%02X", value);
    ESP_LOGI(TAG, "verified PLL reset strobe self-cleared");
    return ESP_OK;
}

static esp_err_t prepare_device(si5351_t *device, bool verify)
{
    static const uint8_t powered_down[8] = {
        SI5351_CLOCK_POWER_DOWN, SI5351_CLOCK_POWER_DOWN,
        SI5351_CLOCK_POWER_DOWN, SI5351_CLOCK_POWER_DOWN,
        SI5351_CLOCK_POWER_DOWN, SI5351_CLOCK_POWER_DOWN,
        SI5351_CLOCK_POWER_DOWN, SI5351_CLOCK_POWER_DOWN,
    };
    static const uint8_t disabled_output_low[2] = {0x00U, 0x00U};

    ESP_RETURN_ON_ERROR(si5351_wait_ready(device, 1000U), TAG,
                        "Si5351 did not finish initialization");
    ESP_RETURN_ON_ERROR(write_byte_checked(device, SI5351_REG_OUTPUT_ENABLE,
                                            SI5351_ALL_OUTPUTS_DISABLED,
                                            verify),
                        TAG, "cannot disable outputs");
    ESP_RETURN_ON_ERROR(write_checked(device, SI5351_REG_CLK0_CONTROL,
                                       powered_down, sizeof(powered_down),
                                       verify),
                        TAG, "cannot power down clock drivers");
    ESP_RETURN_ON_ERROR(write_checked(device, SI5351_REG_CLK3_0_DISABLE,
                                       disabled_output_low,
                                       sizeof(disabled_output_low), verify),
                        TAG, "cannot force disabled clocks low");
    ESP_RETURN_ON_ERROR(write_byte_checked(device,
                                            SI5351_REG_PLL_INPUT_SOURCE,
                                            0x00U, verify),
                        TAG, "cannot select XA/XO reference path");
    return write_byte_checked(device, SI5351_REG_XTAL_LOAD,
                              device->reference_load_register, verify);
}

esp_err_t si5351_set_enabled_outputs(si5351_t *device,
                                     uint8_t enabled_outputs, bool verify)
{
    ESP_RETURN_ON_FALSE(device != NULL && device->i2c_device != NULL,
                        ESP_ERR_INVALID_ARG, TAG,
                        "device is not initialized");

    // Register 3 is active-low: a set bit disables the corresponding output.
    const uint8_t output_disable_register = (uint8_t)~enabled_outputs;

    ESP_RETURN_ON_ERROR(write_byte_checked(device, SI5351_REG_OUTPUT_ENABLE,
                                            output_disable_register, verify),
                        TAG, "cannot set enabled outputs");
    ESP_LOGI(TAG, "enabled output mask=0x%02X (register 3=0x%02X)",
             enabled_outputs, output_disable_register);
    return ESP_OK;
}

esp_err_t si5351_configure_40m_clock_plan(si5351_t *device, uint32_t rf_hz,
                                          bool verify)
{
    ESP_RETURN_ON_FALSE(device != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "device is null");
    const uint64_t pll_hz = (uint64_t)rf_hz * 112ULL;
    ESP_RETURN_ON_FALSE(pll_hz >= SI5351_PLL_MIN_HZ &&
                            pll_hz <= SI5351_PLL_MAX_HZ,
                        ESP_ERR_INVALID_ARG, TAG,
                        "RF frequency is outside integer 40 m clock-plan range");

    si5351_synth_parameters_t pll = {0};
    si5351_synth_parameters_t ms_rx = {0};
    si5351_synth_parameters_t ms_tx = {0};
    ESP_RETURN_ON_ERROR(calculate_parameters(pll_hz, device->reference_hz,
                                              15U, 90U, &pll),
                        TAG, "cannot calculate 40 m PLL");
    ESP_RETURN_ON_ERROR(calculate_parameters(pll_hz, (uint64_t)rf_hz * 4ULL,
                                              8U, 1800U, &ms_rx),
                        TAG, "cannot calculate RX Multisynth");
    ESP_RETURN_ON_ERROR(calculate_parameters(pll_hz, rf_hz, 8U, 1800U,
                                              &ms_tx),
                        TAG, "cannot calculate TX Multisynth");
    ESP_RETURN_ON_FALSE(ms_rx.b == 0U && ms_rx.a == 28U &&
                            ms_tx.b == 0U && ms_tx.a == 112U,
                        ESP_ERR_INVALID_STATE, TAG,
                        "40 m plan did not produce integer dividers");

    uint8_t pll_registers[8];
    uint8_t ms_rx_registers[8];
    uint8_t ms_tx_registers[8];
    encode_parameters(&pll, pll_registers);
    encode_parameters(&ms_rx, ms_rx_registers);
    encode_parameters(&ms_tx, ms_tx_registers);

    if (device->reference_hz == 26000000U && rf_hz == 7074000U) {
        static const uint8_t expected_pll[8] = {
            0x06U, 0x59U, 0x00U, 0x0DU, 0x3CU, 0x00U, 0x03U, 0x24U,
        };
        static const uint8_t expected_ms_rx[8] = {
            0x00U, 0x01U, 0x00U, 0x0CU, 0x00U, 0x00U, 0x00U, 0x00U,
        };
        static const uint8_t expected_ms_tx[8] = {
            0x00U, 0x01U, 0x00U, 0x36U, 0x00U, 0x00U, 0x00U, 0x00U,
        };
        ESP_RETURN_ON_FALSE(memcmp(pll_registers, expected_pll,
                                   sizeof(expected_pll)) == 0 &&
                                memcmp(ms_rx_registers, expected_ms_rx,
                                       sizeof(expected_ms_rx)) == 0 &&
                                memcmp(ms_tx_registers, expected_ms_tx,
                                       sizeof(expected_ms_tx)) == 0,
                            ESP_ERR_INVALID_STATE, TAG,
                            "7.074 MHz register-vector regression");
        ESP_LOGI(TAG, "7.074 MHz register vectors match the reviewed design");
    }

    ESP_LOGI(TAG,
             "40 m plan: RF=%" PRIu32 " Hz, TX CLK0=%" PRIu32
             " Hz, RX CLK1=%" PRIu32 " Hz, PLLA/B=%" PRIu64 " Hz",
             rf_hz, rf_hz, rf_hz * 4U, pll_hz);

    ESP_RETURN_ON_ERROR(prepare_device(device, verify), TAG,
                        "device preparation failed");
    ESP_RETURN_ON_ERROR(write_checked(device, SI5351_REG_PLLA_PARAMETERS,
                                       pll_registers, sizeof(pll_registers),
                                       verify),
                        TAG, "PLLA programming failed");
    ESP_RETURN_ON_ERROR(write_checked(device, SI5351_REG_PLLB_PARAMETERS,
                                       pll_registers, sizeof(pll_registers),
                                       verify),
                        TAG, "PLLB programming failed");
    ESP_RETURN_ON_ERROR(write_checked(device, SI5351_REG_MS0_PARAMETERS,
                                       ms_tx_registers,
                                       sizeof(ms_tx_registers), verify),
                        TAG, "MS0/CLK0 TX programming failed");
    ESP_RETURN_ON_ERROR(write_checked(device, SI5351_REG_MS1_PARAMETERS,
                                       ms_rx_registers,
                                       sizeof(ms_rx_registers), verify),
                        TAG, "MS1/CLK1 RX programming failed");
    ESP_RETURN_ON_ERROR(write_byte_checked(device, SI5351_REG_CLK0_CONTROL,
                                            SI5351_CLOCK_INTEGER_PLLA, verify),
                        TAG, "CLK0 control programming failed");
    ESP_RETURN_ON_ERROR(write_byte_checked(device, SI5351_REG_CLK1_CONTROL,
                                            SI5351_CLOCK_INTEGER_PLLB, verify),
                        TAG, "CLK1 control programming failed");
    ESP_RETURN_ON_ERROR(reset_plls(device, verify), TAG, "PLL reset failed");
    ESP_RETURN_ON_ERROR(wait_status_stably_clear(
                            device,
                            SI5351_STATUS_SYS_INIT |
                                SI5351_STATUS_LOS_XTAL |
                                SI5351_STATUS_LOL_A |
                                SI5351_STATUS_LOL_B,
                            1000U, 3U),
                        TAG, "PLLs did not lock");

    // Safe default for a real daughter board: receive clock on, transmitter off.
    return si5351_set_enabled_outputs(device, SI5351_OUTPUT_CLK1, verify);
}

static esp_err_t configure_rx_loopback(si5351_t *device,
                                        uint32_t rx_frequency_hz,
                                        uint32_t source_frequency_hz,
                                        bool verify)
{
    const uint64_t plla_hz = (uint64_t)source_frequency_hz * 112ULL;
    const uint64_t pllb_hz = (uint64_t)rx_frequency_hz * 112ULL;
    ESP_RETURN_ON_FALSE(plla_hz >= SI5351_PLL_MIN_HZ &&
                            plla_hz <= SI5351_PLL_MAX_HZ &&
                            pllb_hz >= SI5351_PLL_MIN_HZ &&
                            pllb_hz <= SI5351_PLL_MAX_HZ,
                        ESP_ERR_INVALID_ARG, TAG,
                        "loopback frequencies outside divide-by-112 range "
                        "(5357143..8035714 Hz)");

    si5351_synth_parameters_t plla = {0};
    si5351_synth_parameters_t pllb = {0};
    si5351_synth_parameters_t ms0 = {0};
    si5351_synth_parameters_t ms1 = {0};
    ESP_RETURN_ON_ERROR(calculate_parameters(plla_hz, device->reference_hz,
                                              15U, 90U, &plla),
                        TAG, "cannot calculate loopback source PLLA");
    ESP_RETURN_ON_ERROR(calculate_parameters(pllb_hz, device->reference_hz,
                                              15U, 90U, &pllb),
                        TAG, "cannot calculate loopback RX PLLB");
    ESP_RETURN_ON_ERROR(calculate_parameters(plla_hz, source_frequency_hz,
                                              8U, 1800U, &ms0),
                        TAG, "cannot calculate loopback source MS0");
    ESP_RETURN_ON_ERROR(calculate_parameters(pllb_hz,
                                              (uint64_t)rx_frequency_hz * 4ULL,
                                              8U, 1800U, &ms1),
                        TAG, "cannot calculate loopback RX MS1");
    ESP_RETURN_ON_FALSE(ms0.a == 112U && ms0.b == 0U &&
                            ms1.a == 28U && ms1.b == 0U,
                        ESP_ERR_INVALID_STATE, TAG,
                        "loopback plan did not produce integer dividers");

    uint8_t plla_registers[8];
    uint8_t pllb_registers[8];
    uint8_t ms0_registers[8];
    uint8_t ms1_registers[8];
    encode_parameters(&plla, plla_registers);
    encode_parameters(&pllb, pllb_registers);
    encode_parameters(&ms0, ms0_registers);
    encode_parameters(&ms1, ms1_registers);

    if (device->reference_hz == 26000000U && rx_frequency_hz == 7074000U &&
        (source_frequency_hz == 7075000U || source_frequency_hz == 7076000U)) {
        // AN619 equations at the nominal 26 MHz reference:
        // PLLB=26 MHz*(30+768/1625)=792.288 MHz; MS1=28.
        // PLLA=26 MHz*(30+31/65)=792.400 MHz for +1 kHz, or
        // PLLA=26 MHz*(30+782/1625)=792.512 MHz for +2 kHz; MS0=112.
        static const uint8_t expected_plla_1khz[8] = {
            0x00U, 0x41U, 0x00U, 0x0DU, 0x3DU, 0x00U, 0x00U, 0x03U,
        };
        static const uint8_t expected_plla_2khz[8] = {
            0x06U, 0x59U, 0x00U, 0x0DU, 0x3DU, 0x00U, 0x03U, 0xCBU,
        };
        static const uint8_t expected_pllb[8] = {
            0x06U, 0x59U, 0x00U, 0x0DU, 0x3CU, 0x00U, 0x03U, 0x24U,
        };
        static const uint8_t expected_ms0[8] = {
            0x00U, 0x01U, 0x00U, 0x36U, 0x00U, 0x00U, 0x00U, 0x00U,
        };
        static const uint8_t expected_ms1[8] = {
            0x00U, 0x01U, 0x00U, 0x0CU, 0x00U, 0x00U, 0x00U, 0x00U,
        };
        const uint8_t *expected_plla = source_frequency_hz == 7075000U
                                          ? expected_plla_1khz
                                          : expected_plla_2khz;
        ESP_RETURN_ON_FALSE(memcmp(plla_registers, expected_plla, 8U) == 0 &&
                                memcmp(pllb_registers, expected_pllb, 8U) == 0 &&
                                memcmp(ms0_registers, expected_ms0, 8U) == 0 &&
                                memcmp(ms1_registers, expected_ms1, 8U) == 0,
                            ESP_ERR_INVALID_STATE, TAG,
                            "RX loopback register-vector regression");
        ESP_LOGI(TAG, "RX loopback nominal 26 MHz register vectors match");
    }

    ESP_LOGI(TAG,
             "loopback plan: RX=%" PRIu32 " Hz, source CLK0=%" PRIu32
             " Hz, QSD CLK1=%" PRIu32 " Hz, PLLA=%" PRIu64
             " Hz, PLLB=%" PRIu64 " Hz",
             rx_frequency_hz, source_frequency_hz, rx_frequency_hz * 4U,
             plla_hz, pllb_hz);
    ESP_RETURN_ON_ERROR(prepare_device(device, verify), TAG,
                        "loopback device preparation failed");

    uint8_t spread_spectrum = 0U;
    ESP_RETURN_ON_ERROR(si5351_read_register(device,
                                              SI5351_REG_SPREAD_SPECTRUM,
                                              &spread_spectrum),
                        TAG, "cannot read loopback spread-spectrum control");
    spread_spectrum &= (uint8_t)~SI5351_SPREAD_SPECTRUM_ENABLE;
    ESP_RETURN_ON_ERROR(write_byte_checked(device,
                                            SI5351_REG_SPREAD_SPECTRUM,
                                            spread_spectrum, verify),
                        TAG, "cannot disable loopback spread spectrum");
    static const uint8_t cleared_phases[2] = {0x00U, 0x00U};
    ESP_RETURN_ON_ERROR(write_checked(device, SI5351_REG_CLK0_PHASE,
                                       cleared_phases, sizeof(cleared_phases),
                                       verify),
                        TAG, "cannot clear CLK0/CLK1 phase offsets");
    ESP_RETURN_ON_ERROR(write_checked(device, SI5351_REG_PLLA_PARAMETERS,
                                       plla_registers, sizeof(plla_registers),
                                       verify),
                        TAG, "loopback PLLA programming failed");
    ESP_RETURN_ON_ERROR(write_checked(device, SI5351_REG_PLLB_PARAMETERS,
                                       pllb_registers, sizeof(pllb_registers),
                                       verify),
                        TAG, "loopback PLLB programming failed");
    ESP_RETURN_ON_ERROR(write_checked(device, SI5351_REG_MS0_PARAMETERS,
                                       ms0_registers, sizeof(ms0_registers),
                                       verify),
                        TAG, "loopback MS0 programming failed");
    ESP_RETURN_ON_ERROR(write_checked(device, SI5351_REG_MS1_PARAMETERS,
                                       ms1_registers, sizeof(ms1_registers),
                                       verify),
                        TAG, "loopback MS1 programming failed");
    ESP_RETURN_ON_ERROR(write_byte_checked(device, SI5351_REG_CLK0_CONTROL,
                                            SI5351_CLOCK_INTEGER_PLLA, verify),
                        TAG, "loopback CLK0 control programming failed");
    ESP_RETURN_ON_ERROR(write_byte_checked(device, SI5351_REG_CLK1_CONTROL,
                                            SI5351_CLOCK_INTEGER_PLLB, verify),
                        TAG, "loopback CLK1 control programming failed");
    ESP_RETURN_ON_ERROR(reset_plls(device, verify), TAG,
                        "loopback PLL reset failed");
    ESP_RETURN_ON_ERROR(wait_status_stably_clear(
                            device,
                            SI5351_STATUS_SYS_INIT |
                                SI5351_STATUS_LOS_XTAL |
                                SI5351_STATUS_LOL_A |
                                SI5351_STATUS_LOL_B,
                            1000U, 3U),
                        TAG, "loopback PLLs did not lock");

    // The caller must explicitly opt into source-on; no PA/GPIO control here.
    return si5351_set_enabled_outputs(device, SI5351_OUTPUT_CLK1, verify);
}

esp_err_t si5351_configure_rx_loopback(si5351_t *device,
                                       uint32_t rx_frequency_hz,
                                       uint32_t source_frequency_hz,
                                       bool verify)
{
    ESP_RETURN_ON_FALSE(device != NULL && device->i2c_device != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "device is not initialized");

    // Disable any previous source before calculations or initialization waits.
    // Keep one cleanup path even if a write succeeded and only readback failed.
    esp_err_t error = si5351_set_enabled_outputs(device, 0U, verify);
    if (error == ESP_OK) {
        error = configure_rx_loopback(device, rx_frequency_hz,
                                       source_frequency_hz, verify);
    }
    if (error != ESP_OK) {
        const esp_err_t disable_error = si5351_set_enabled_outputs(device, 0U,
                                                                   false);
        if (disable_error != ESP_OK) {
            ESP_LOGE(TAG, "loopback cleanup could not disable outputs: %s",
                     esp_err_to_name(disable_error));
        }
    }
    return error;
}

esp_err_t si5351_configure_clk0_carrier(si5351_t *device,
                                        uint32_t frequency_hz, bool verify)
{
    ESP_RETURN_ON_FALSE(device != NULL && device->i2c_device != NULL,
                        ESP_ERR_INVALID_ARG, TAG,
                        "device is not initialized");

    // Keep MS0 in even-integer mode around the 20 m band. R0 is divide-by-one;
    // the supported frequency range follows directly from the PLL VCO limits.
    const uint64_t pll_hz = (uint64_t)frequency_hz * SI5351_CARRIER_MS_DIVIDER;
    ESP_RETURN_ON_FALSE(pll_hz >= SI5351_PLL_MIN_HZ &&
                            pll_hz <= SI5351_PLL_MAX_HZ,
                        ESP_ERR_INVALID_ARG, TAG,
                        "carrier outside divide-by-56 range (10714286..16071428 Hz)");

    si5351_synth_parameters_t pll = {0};
    si5351_synth_parameters_t ms0 = {0};
    ESP_RETURN_ON_ERROR(calculate_parameters(pll_hz, device->reference_hz,
                                              15U, 90U, &pll),
                        TAG, "cannot calculate carrier PLLA");
    ESP_RETURN_ON_ERROR(calculate_parameters(pll_hz, frequency_hz,
                                              8U, 1800U, &ms0),
                        TAG, "cannot calculate carrier MS0");
    ESP_RETURN_ON_FALSE(ms0.a == SI5351_CARRIER_MS_DIVIDER && ms0.b == 0U,
                        ESP_ERR_INVALID_STATE, TAG,
                        "carrier plan did not produce integer MS0");

    uint8_t pll_registers[8];
    uint8_t ms0_registers[8];
    encode_parameters(&pll, pll_registers);
    encode_parameters(&ms0, ms0_registers);

    if (device->reference_hz == 26000000U && frequency_hz == 14075000U) {
        // 26 MHz * (30 + 41/130) / 56 = 14.075 MHz exactly at nominal reference.
        static const uint8_t expected_pll[8] = {
            0x00U, 0x82U, 0x00U, 0x0DU, 0x28U, 0x00U, 0x00U, 0x30U,
        };
        static const uint8_t expected_ms0[8] = {
            0x00U, 0x01U, 0x00U, 0x1AU, 0x00U, 0x00U, 0x00U, 0x00U,
        };
        ESP_RETURN_ON_FALSE(memcmp(pll_registers, expected_pll,
                                   sizeof(expected_pll)) == 0 &&
                                memcmp(ms0_registers, expected_ms0,
                                       sizeof(expected_ms0)) == 0,
                            ESP_ERR_INVALID_STATE, TAG,
                            "14.075 MHz carrier register-vector regression");
    }

    ESP_LOGI(TAG,
             "carrier plan: CLK0=%" PRIu32 " Hz, PLLA=%" PRIu64
             " Hz, MS0=%u, reference=%" PRIu32 " Hz",
             frequency_hz, pll_hz, SI5351_CARRIER_MS_DIVIDER,
             device->reference_hz);

    ESP_RETURN_ON_ERROR(prepare_device(device, verify), TAG,
                        "carrier device preparation failed");

    // A previously programmed device may have spread spectrum enabled. Retain
    // its parameter bits but clear SSC_EN for a constant-frequency carrier.
    uint8_t spread_spectrum = 0U;
    ESP_RETURN_ON_ERROR(si5351_read_register(device,
                                              SI5351_REG_SPREAD_SPECTRUM,
                                              &spread_spectrum),
                        TAG, "cannot read spread-spectrum control");
    spread_spectrum &= (uint8_t)~SI5351_SPREAD_SPECTRUM_ENABLE;
    ESP_RETURN_ON_ERROR(write_byte_checked(device,
                                            SI5351_REG_SPREAD_SPECTRUM,
                                            spread_spectrum, verify),
                        TAG, "cannot disable spread spectrum");
    ESP_RETURN_ON_ERROR(write_byte_checked(device, SI5351_REG_CLK0_PHASE,
                                            0x00U, verify),
                        TAG, "cannot clear CLK0 phase offset");
    ESP_RETURN_ON_ERROR(write_checked(device, SI5351_REG_PLLA_PARAMETERS,
                                       pll_registers, sizeof(pll_registers),
                                       verify),
                        TAG, "carrier PLLA programming failed");
    ESP_RETURN_ON_ERROR(write_checked(device, SI5351_REG_MS0_PARAMETERS,
                                       ms0_registers, sizeof(ms0_registers),
                                       verify),
                        TAG, "carrier MS0 programming failed");
    ESP_RETURN_ON_ERROR(write_byte_checked(device, SI5351_REG_CLK0_CONTROL,
                                            SI5351_CLOCK_INTEGER_PLLA, verify),
                        TAG, "carrier CLK0 control programming failed");
    ESP_RETURN_ON_ERROR(reset_plls(device, verify), TAG,
                        "carrier PLL reset failed");

    // PLLB is unused and may report loss-of-lock after reset. Only the reference,
    // initialization and PLLA status determine whether CLK0 is ready to enable.
    ESP_RETURN_ON_ERROR(wait_status_stably_clear(
                            device,
                            SI5351_STATUS_SYS_INIT |
                                SI5351_STATUS_LOS_XTAL |
                                SI5351_STATUS_LOL_A,
                            1000U, 3U),
                        TAG, "carrier PLLA did not lock");

    const esp_err_t error = si5351_set_enabled_outputs(
        device, SI5351_OUTPUT_CLK0, verify);
    if (error != ESP_OK) {
        // The enable write can succeed even if its verification read fails.
        // Request all outputs off before propagating that failure to the caller.
        const esp_err_t disable_error = si5351_set_enabled_outputs(device, 0U,
                                                                   false);
        if (disable_error != ESP_OK) {
            ESP_LOGE(TAG, "carrier cleanup could not disable outputs: %s",
                     esp_err_to_name(disable_error));
        }
    }
    return error;
}
