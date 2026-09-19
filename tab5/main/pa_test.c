// SPDX-License-Identifier: MIT

#include "sdkconfig.h"
#if CONFIG_DXFT8_PA_TEST

#include "pa_test.h"
#include "pa_policy.h"
#include "pcm1808_i2s.h"
#include "rf_loopback.h"
#include "si5351.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#if !CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD || !CONFIG_GPIO_CTRL_FUNC_IN_IRAM
#error "PA bench requires ISR timer dispatch and IRAM GPIO control"
#endif

#define PA_POWER_GPIO GPIO_NUM_48
#define PA_RXSW_GPIO GPIO_NUM_47
#define PA_PREP_LIMIT_US 3000000ULL
#define PA_SWITCH_BLANK_MS 35U

static const char *TAG = "pa_test";
static portMUX_TYPE guard_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool guard_fired;
static volatile bool cancel_requested;
static volatile bool worker_done;
static esp_timer_handle_t prep_guard;
static esp_timer_handle_t burst_guard;
static QueueHandle_t jobs;
static nvs_handle_t safety_store;
static bool shutdown_fault;

// Persist before powering the board, and clear only after a known-safe job.
// A host reset must not hide a possibly still-enabled/backpowered Si5351.
static esp_err_t store_shutdown_fault(bool pending)
{
    esp_err_t error = nvs_set_u8(safety_store, "unverified", pending ? 1U : 0U);
    if (error == ESP_OK) { error = nvs_commit(safety_store); }
    if (error != ESP_OK || pending) { shutdown_fault = true; }
    else { shutdown_fault = false; }
    return error;
}

// Independent of I2C, USB logging and the acquisition task. This requests
// power cutoff, not instantaneous RF cessation: rail capacitors still discharge.
static void IRAM_ATTR cutoff_isr(void *argument)
{
    (void)argument;
    portENTER_CRITICAL_ISR(&guard_mux);
    gpio_set_level(PA_POWER_GPIO, 0);
    guard_fired = true;
    portEXIT_CRITICAL_ISR(&guard_mux);
}

static bool interrupted(void)
{
    portENTER_CRITICAL(&guard_mux);
    const bool stopped = guard_fired || cancel_requested;
    portEXIT_CRITICAL(&guard_mux);
    return stopped;
}

static void cancel_power(void)
{
    portENTER_CRITICAL(&guard_mux);
    gpio_set_level(PA_POWER_GPIO, 0);
    cancel_requested = true;
    portEXIT_CRITICAL(&guard_mux);
}

static void power_off(void)
{
    gpio_set_level(PA_POWER_GPIO, 0);
    gpio_set_level(PA_RXSW_GPIO, 0);
}

static esp_err_t start_power(bool *power_started)
{
    // Guards are stopped and RF is off before flags are reset for a new job.
    power_off();
    (void)esp_timer_stop(prep_guard);
    (void)esp_timer_stop(burst_guard);
    portENTER_CRITICAL(&guard_mux);
    guard_fired = false;
    // cancel_requested is reset by the console BEFORE publishing the job;
    // never clear a cancellation that arrived after it was accepted.
    portEXIT_CRITICAL(&guard_mux);
    esp_err_t error = esp_timer_start_once(prep_guard, PA_PREP_LIMIT_US);
    if (error != ESP_OK) {
        return error;
    }
    portENTER_CRITICAL(&guard_mux);
    if (guard_fired || cancel_requested) {
        error = ESP_ERR_INVALID_STATE;
    } else {
        error = gpio_set_level(PA_POWER_GPIO, 1);
        *power_started = error == ESP_OK;
    }
    portEXIT_CRITICAL(&guard_mux);
    return error;
}

static bool discard_usb(void)
{
    uint8_t bytes[128];
    bool discarded = false;
    // Bounded: a continuously sending host must not stall the console.
    for (unsigned pass = 0; pass < 8U; ++pass) {
        if (usb_serial_jtag_read_bytes(bytes, sizeof(bytes), 0) <= 0) {
            break;
        }
        discarded = true;
    }
    return discarded;
}

static esp_err_t disable_clocks_checked(si5351_t *clock)
{
    // Quiet while keyed. The independent burst guard stays armed during I2C.
    esp_err_t error = si5351_write_register(clock, 3U, 0xFFU);
    uint8_t mask = 0U;
    if (error == ESP_OK) {
        error = si5351_read_register(clock, 3U, &mask);
        if (error == ESP_OK && mask != 0xFFU) {
            error = ESP_ERR_INVALID_RESPONSE;
        }
    }
    return error;
}

static void perform_burst(const pa_decision_t *job)
{
    esp_err_t error = ESP_OK;
    const char *stage = "power preparation";
    i2c_master_bus_handle_t bus = NULL;
    si5351_t clock = {0};
    pcm1808_i2s_t adc = {0};
    rf_loopback_result_t baseline = {0};
    rf_loopback_result_t keyed = {0};
    bool baseline_valid = false;
    bool keyed_valid = false;
    bool key_attempted = false;
    bool power_started = false;
    bool prekey_off_verified = false;
    bool mask_off_verified = false;
    int64_t key_started_us = 0;
    int64_t key_finished_us = 0;
    const bool leakage = job->action == PA_ACTION_LEAK;

    // Log only before the bounded keyed region. No automatic repeat or retry.
    ESP_LOGW(TAG, "PA PREP: 40m CLK0=7075000 Hz; G47=LOW; mode=%s; limit=%" PRIu32
                  " ms; manual LPF/load confirmation only", leakage ? "leak" : "pa",
             job->duration_ms);
    error = store_shutdown_fault(true);
    if (error != ESP_OK) { goto cleanup; }
    error = start_power(&power_started);
    if (error != ESP_OK) { goto cleanup; }
    vTaskDelay(pdMS_TO_TICKS(200));
    if (interrupted()) { error = ESP_ERR_TIMEOUT; goto cleanup; }

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0, .sda_io_num = GPIO_NUM_31, .scl_io_num = GPIO_NUM_32,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,
    };
    stage = "I2C initialization";
    error = i2c_new_master_bus(&bus_config, &bus);
    if (error != ESP_OK || interrupted()) { goto cleanup; }
    error = si5351_init(&clock, bus, SI5351_DEFAULT_I2C_ADDRESS, 26000000U,
                        SI5351_REFERENCE_EXTERNAL_CLOCK, 100000U);
    if (error != ESP_OK || interrupted()) { goto cleanup; }
    stage = "Si5351 probe";
    error = si5351_probe(&clock, 100);
    if (error != ESP_OK || interrupted()) { goto cleanup; }
    stage = "initial clocks off";
    error = disable_clocks_checked(&clock);
    prekey_off_verified = error == ESP_OK;
    if (error != ESP_OK || interrupted()) { goto cleanup; }
    stage = "clock plan";
    // This helper never enables CLK0. G47 stays LOW even for the leakage test.
    error = si5351_configure_rx_loopback(&clock, PA_POLICY_RX_HZ,
                                        PA_POLICY_SOURCE_HZ, true);
    if (error != ESP_OK || interrupted()) { goto cleanup; }
    if (!leakage) {
        error = si5351_set_enabled_outputs(&clock, 0U, true);
        if (error != ESP_OK || interrupted()) { goto cleanup; }
    } else {
        stage = "I2S startup/baseline";
        const pcm1808_i2s_config_t config = {
            .port = I2S_NUM_1, .mclk_gpio = GPIO_NUM_16, .bclk_gpio = GPIO_NUM_45,
            .lrck_gpio = GPIO_NUM_3, .din_gpio = GPIO_NUM_4, .sample_rate_hz = 48000U,
        };
        error = pcm1808_i2s_init(&adc, &config);
        if (error != ESP_OK || interrupted()) { goto cleanup; }
        error = pcm1808_i2s_enable(&adc);
        if (error != ESP_OK || interrupted()) { goto cleanup; }
        // Startup settling happens with CLK0 OFF, not with the PA keyed.
        error = rf_loopback_capture(&adc, 1000U, 100U, &baseline);
        if (error != ESP_OK || interrupted()) { goto cleanup; }
        baseline_valid = true;
        if (baseline.nonzero_padding_words != 0U ||
            baseline.channels[0].rail_hits != 0U || baseline.channels[1].rail_hits != 0U) {
            error = ESP_ERR_INVALID_RESPONSE;
            goto cleanup;
        }
    }

    stage = "burst guard";
    error = esp_timer_start_once(burst_guard, (uint64_t)job->duration_ms * 1000ULL);
    if (error != ESP_OK || interrupted()) { goto cleanup; }
    // Overlapping guards avoid an unguarded interval while power is enabled.
    (void)esp_timer_stop(prep_guard);
    if (interrupted()) { error = ESP_ERR_TIMEOUT; goto cleanup; }
    key_started_us = esp_timer_get_time();
    stage = "CLK0 enable";
    key_attempted = true;
    const uint8_t enabled = SI5351_OUTPUT_CLK0 | (leakage ? SI5351_OUTPUT_CLK1 : 0U);
    error = si5351_write_register(&clock, 3U, (uint8_t)~enabled);
    if (error != ESP_OK || interrupted()) { goto cleanup; }
    uint8_t actual_mask = 0U;
    error = si5351_read_register(&clock, 3U, &actual_mask);
    if (error != ESP_OK || actual_mask != (uint8_t)~enabled || interrupted()) {
        if (error == ESP_OK) { error = ESP_ERR_INVALID_RESPONSE; }
        goto cleanup;
    }
    stage = "bounded burst";
    if (leakage) {
        // Drain > the 30 ms DMA queue; measure less than the RF guard interval.
        // These sample windows are not a calibrated analog settling guarantee.
        const uint32_t capture_ms = job->duration_ms == 100U ? 50U : 200U;
        error = rf_loopback_capture(&adc, PA_SWITCH_BLANK_MS, capture_ms, &keyed);
        if (error != ESP_OK || interrupted()) { goto cleanup; }
        keyed_valid = true;
    } else {
        // Leave 20 ms for normal quiet I2C shutdown before the ISR cutoff.
        const int64_t finish_at = key_started_us + ((int64_t)job->duration_ms - 20) * 1000;
        while (!interrupted() && esp_timer_get_time() < finish_at) {
            vTaskDelay(1);
        }
        if (interrupted()) { error = ESP_ERR_TIMEOUT; goto cleanup; }
    }
    stage = "CLK0 disable";
    error = disable_clocks_checked(&clock);
    mask_off_verified = error == ESP_OK;
    key_finished_us = esp_timer_get_time();

cleanup:
    // Never let I2C/USB/ADC cleanup delay the power-off request.
    power_off();
    (void)esp_timer_stop(burst_guard);
    (void)esp_timer_stop(prep_guard);
    const bool aborted = interrupted();
    if (aborted && error == ESP_OK) { error = ESP_ERR_TIMEOUT; }
    if (adc.rx_channel != NULL) {
        const esp_err_t adc_error = pcm1808_i2s_deinit(&adc);
        if (error == ESP_OK) { error = adc_error; }
        gpio_reset_pin(GPIO_NUM_16);
        gpio_reset_pin(GPIO_NUM_45);
        gpio_reset_pin(GPIO_NUM_3);
        gpio_reset_pin(GPIO_NUM_4);
        // gpio_reset_pin enables a pull-up in IDF; explicitly float the pins
        // so stopped I2S does not unnecessarily backpower the unpowered ADC.
        gpio_set_pull_mode(GPIO_NUM_16, GPIO_FLOATING);
        gpio_set_pull_mode(GPIO_NUM_45, GPIO_FLOATING);
        gpio_set_pull_mode(GPIO_NUM_3, GPIO_FLOATING);
        gpio_set_pull_mode(GPIO_NUM_4, GPIO_FLOATING);
    }
    if (clock.i2c_device != NULL) { (void)si5351_deinit(&clock); }
    if (bus != NULL) { (void)i2c_del_master_bus(bus); }
    if (!power_started || (!key_attempted && prekey_off_verified) || mask_off_verified) {
        const esp_err_t store_error = store_shutdown_fault(false);
        if (error == ESP_OK) { error = store_error; }
    }
    if (shutdown_fault) {
        ESP_LOGE(TAG, "PA SHUTDOWN FAULT LATCHED: full RF-board power-cycle required; "
                     "USB reset alone is insufficient; then clearfault powercycled");
    }
    ESP_LOGI(TAG, "PA SAFE OFF: G48=%d G47=%d; key_attempted=%s; "
                  "clock_mask_off_verified=%s; interrupted=%s",
             gpio_get_level(PA_POWER_GPIO), gpio_get_level(PA_RXSW_GPIO),
             key_attempted ? "yes" : "no", mask_off_verified ? "yes" : "no",
             aborted ? "yes" : "no");
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "PA JOB FAILED at %s: %s; no automatic retry", stage,
                 esp_err_to_name(error));
    } else {
        ESP_LOGI(TAG, "PA JOB COMPLETE: mode=%s limit_ms=%" PRIu32
                      " command_interval_us=%" PRId64 "; RF envelope not measured",
                 leakage ? "leak" : "pa", job->duration_ms,
                 key_finished_us - key_started_us);
    }
    if (baseline_valid) { (void)rf_loopback_report("PA_OFF_BASELINE", &baseline); }
    if (keyed_valid && error == ESP_OK) { (void)rf_loopback_report("PA_KEYED_LEAK", &keyed); }
    portENTER_CRITICAL(&guard_mux);
    worker_done = true;
    portEXIT_CRITICAL(&guard_mux);
}

static void pa_worker(void *argument)
{
    (void)argument;
    pa_decision_t job;
    for (;;) {
        if (xQueueReceive(jobs, &job, portMAX_DELAY) == pdTRUE) {
            perform_burst(&job);
        }
    }
}

static void help(void)
{
    ESP_LOGI(TAG, "PA COMMANDS: status | help | off | arm 40m dummyload | "
                  "pa [100|250] | leak [100|250] | clearfault powercycled");
    ESP_LOGW(TAG, "40m FIXED 7075000 Hz. Fit 40m LPF + rated 50-ohm dummy load; "
                  "no antenna. Band/load/current/temperature are NOT sensed.");
    ESP_LOGI(TAG, "Arm lasts 30s, one use; 5s cooldown; non-newline USB input during a job "
                  "requests power off. No continuous TX or automatic repeat.");
}

static void status(const pa_policy_t *policy)
{
    const uint64_t now = (uint64_t)esp_timer_get_time() / 1000U;
    ESP_LOGI(TAG, "PA STATUS: %s; G48=%d G47=%d; band=40m source=7075000 Hz; "
                  "cooldown_ms=%" PRIu32 "; shutdown_fault=%s",
             pa_policy_is_armed(policy, now) ? "ARMED" : "DISARMED",
             gpio_get_level(PA_POWER_GPIO), gpio_get_level(PA_RXSW_GPIO),
             pa_policy_cooldown_remaining(policy, now), shutdown_fault ? "LATCHED" : "none");
}

static void fatal_idle(const char *stage, esp_err_t error)
{
    power_off();
    ESP_LOGE(TAG, "PA INIT FAILED %s: %s; RF power held off", stage, esp_err_to_name(error));
    for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}

void pa_test_run(void)
{
    // Runs instead of the old automatic real-board/loopback startup, not after it.
    gpio_set_level(PA_POWER_GPIO, 0);
    gpio_set_level(PA_RXSW_GPIO, 0);
    const gpio_config_t pins = {
        .pin_bit_mask = (1ULL << PA_POWER_GPIO) | (1ULL << PA_RXSW_GPIO),
        .mode = GPIO_MODE_INPUT_OUTPUT, .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE, .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t error = gpio_config(&pins);
    if (error != ESP_OK) { fatal_idle("GPIO", error); }
    power_off();
    // Fail closed if NVS cannot be read; never erase a user's NVS on an error.
    error = nvs_flash_init();
    if (error == ESP_OK) { error = nvs_open("pa_bench", NVS_READWRITE, &safety_store); }
    if (error != ESP_OK) { fatal_idle("persistent safety state", error); }
    uint8_t pending = 0U;
    error = nvs_get_u8(safety_store, "unverified", &pending);
    if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) {
        fatal_idle("persistent safety state", error);
    }
    shutdown_fault = pending != 0U;
    const esp_timer_create_args_t timer = {
        .callback = cutoff_isr, .dispatch_method = ESP_TIMER_ISR, .name = "pa_cutoff",
    };
    error = esp_timer_create(&timer, &prep_guard);
    if (error == ESP_OK) { error = esp_timer_create(&timer, &burst_guard); }
    if (error != ESP_OK) { fatal_idle("cutoff timers", error); }
    // Exercise the cutoff ISR without ever enabling RF power.
    error = esp_timer_start_once(burst_guard, 10000U);
    if (error != ESP_OK) { fatal_idle("cutoff self-check", error); }
    vTaskDelay(pdMS_TO_TICKS(30));
    if (!guard_fired || gpio_get_level(PA_POWER_GPIO) != 0) {
        fatal_idle("cutoff self-check", ESP_FAIL);
    }
    (void)esp_timer_stop(burst_guard);
    ESP_LOGI(TAG, "PA GUARD SELF-CHECK PASS: ISR fired with RF power held off");
    usb_serial_jtag_driver_config_t usb = {.rx_buffer_size = 256U, .tx_buffer_size = 1024U};
    error = usb_serial_jtag_driver_install(&usb);
    if (error != ESP_OK) { fatal_idle("USB console", error); }
    usb_serial_jtag_vfs_use_driver();
    jobs = xQueueCreate(1, sizeof(pa_decision_t));
    if (jobs == NULL || xTaskCreate(pa_worker, "pa_worker", 8192, NULL, 5, NULL) != pdPASS) {
        fatal_idle("worker", ESP_ERR_NO_MEM);
    }
    pa_policy_t policy;
    pa_policy_init(&policy);
    bool busy = false;
    bool bad_line = false;
    bool cancelling = false;
    char line[PA_POLICY_MAX_LINE + 1U];
    size_t used = 0;
    (void)discard_usb();
    ESP_LOGI(TAG, "PA TEST READY: DISARMED; G48=LOW G47=LOW; fixed 40m; no boot TX");
    help();
    status(&policy);
    for (;;) {
        portENTER_CRITICAL(&guard_mux);
        const bool done = worker_done;
        if (done) { worker_done = false; }
        portEXIT_CRITICAL(&guard_mux);
        if (done) {
            pa_policy_complete(&policy, (uint64_t)esp_timer_get_time() / 1000U);
            busy = false;
            used = 0;
            bad_line = false;
            (void)discard_usb();
            ESP_LOGI(TAG, "PA DISARMED: job ended; 5000 ms cooldown; fresh arm required");
        }
        uint8_t input[128];
        const int count = usb_serial_jtag_read_bytes(input, sizeof(input), pdMS_TO_TICKS(10));
        if (count <= 0) { continue; }
        if (busy) {
            bool substantive = false;
            for (int index = 0; index < count; ++index) {
                substantive |= input[index] != '\r' && input[index] != '\n';
            }
            if (!substantive) { continue; } // Tolerate a split CRLF terminator.
            cancel_power();
            if (!cancelling) { ESP_LOGW(TAG, "PA CANCEL: USB input received during job"); }
            cancelling = true;
            continue;
        }
        for (int index = 0; index < count; ++index) {
            const uint8_t byte = input[index];
            if (byte == 3U || byte == 27U) {
                cancel_power();
                (void)pa_policy_submit(&policy, "off", (uint64_t)esp_timer_get_time() / 1000U);
                used = 0;
                bad_line = false;
                ESP_LOGI(TAG, "PA OFF: keyboard cancellation; disarmed");
                break;
            }
            if (byte != '\r' && byte != '\n') {
                if (byte < 32U || byte > 126U || used >= PA_POLICY_MAX_LINE) {
                    bad_line = true;
                    (void)pa_policy_submit(&policy, "invalid", (uint64_t)esp_timer_get_time() / 1000U);
                } else if (!bad_line) {
                    line[used++] = (char)byte;
                }
                continue;
            }
            if (used == 0U && !bad_line) { continue; }
            line[used] = '\0';
            // Reject already queued command batches; fragmented future USB
            // packets cannot be distinguished from separately typed commands.
            bool extra = false;
            for (int tail = index + 1; tail < count; ++tail) {
                extra |= input[tail] != '\r' && input[tail] != '\n';
            }
            extra |= discard_usb();
            const pa_decision_t decision = pa_policy_submit(&policy,
                bad_line || extra ? "invalid" : line,
                (uint64_t)esp_timer_get_time() / 1000U);
            used = 0;
            bad_line = false;
            switch (decision.action) {
            case PA_ACTION_HELP: help(); break;
            case PA_ACTION_STATUS: status(&policy); break;
            case PA_ACTION_OFF: power_off(); ESP_LOGI(TAG, "PA OFF: disarmed"); break;
            case PA_ACTION_CLEARFAULT:
                power_off();
                if (store_shutdown_fault(false) == ESP_OK) {
                    ESP_LOGW(TAG, "PA FAULT CLEARED: operator confirmed full RF-board "
                                  "power-cycle; DISARMED; this is not sensed");
                } else {
                    ESP_LOGE(TAG, "PA FAULT REMAINS LATCHED: NVS update failed");
                }
                break;
            case PA_ACTION_ARM:
                if (shutdown_fault) {
                    (void)pa_policy_submit(&policy, "off", (uint64_t)esp_timer_get_time() / 1000U);
                    ESP_LOGE(TAG, "PA REJECTED: shutdown fault latched; full RF-board "
                                 "power-cycle then clearfault powercycled required");
                    break;
                }
                ESP_LOGW(TAG, "PA ARMED: one use, expires in 30s; RF remains OFF; "
                              "send pa or leak separately");
                break;
            case PA_ACTION_PA:
            case PA_ACTION_LEAK:
                portENTER_CRITICAL(&guard_mux);
                cancel_requested = false;
                worker_done = false;
                portEXIT_CRITICAL(&guard_mux);
                busy = true;
                cancelling = false;
                if (xQueueSend(jobs, &decision, 0) != pdTRUE) {
                    power_off();
                    busy = false;
                    pa_policy_complete(&policy, (uint64_t)esp_timer_get_time() / 1000U);
                    ESP_LOGE(TAG, "PA queue failed; disarmed");
                }
                break;
            default:
                ESP_LOGW(TAG, "PA REJECTED: %s", pa_policy_rejection_name(decision.rejection));
                break;
            }
            // Discard all other bytes in this packet after a complete command.
            break;
        }
    }
}

#endif // CONFIG_DXFT8_PA_TEST
