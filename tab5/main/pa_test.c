// SPDX-License-Identifier: MIT
#include "sdkconfig.h"
#if CONFIG_DXFT8_PA_TEST

#include "pa_test.h"
#include "pa_policy.h"
#include "pa_safety.h"
#include "lpf_sense.h"
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
#error "PA bench requires ISR timers and IRAM GPIO control"
#endif
#if !CONFIG_ESP_TIMER_ISR_AFFINITY_CPU0 || CONFIG_FREERTOS_UNICORE
#error "PA ADC monitor requires CPU1, with cutoff timer ISR on CPU0"
#endif

#define PA_POWER_GPIO GPIO_NUM_48
#define PA_RXSW_GPIO GPIO_NUM_47
#define PA_PREP_LIMIT_US 3000000ULL
#define PA_SWITCH_BLANK_MS 35U
#define LPF_STABLE_US 100000LL
#define LPF_FRESH_US ((int64_t)PA_POLICY_LPF_FRESH_MS * 1000LL)
#define LPF_WATCH_PERIOD_US 20000ULL

static const char *TAG = "pa_test";
static portMUX_TYPE guard_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t prep_guard, burst_guard, lpf_guard;
static QueueHandle_t jobs;
static nvs_handle_t safety_store;
static lpf_sense_t sense;

typedef enum { STOP_NONE, STOP_TIMER, STOP_LPF, STOP_STALE, STOP_OPERATOR } stop_t;
typedef struct {
    lpf_sense_result_t reading;
    int64_t sample_us;
    int64_t stable_since_us;
    unsigned consecutive;
    bool qualified;
} band_snapshot_t;

static band_snapshot_t band;
static uint32_t sense_epoch;
static bool sampling;
static bool band_watch;
static bool rf_ready;
static stop_t stopped;
static bool shutdown_fault;
static bool worker_done, worker_success, worker_was_burst, idle_stopped;

// Only the RF worker owns these handles / clock-state assertions.
static i2c_master_bus_handle_t bus;
static si5351_t clock_device;
static bool clocks_off_known;
static bool all_clocks_off_verified;
static bool marker_pending;
static bool power_started;

static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000U; }

// Caller holds guard_mux; no flash, I2C or logging is allowed here.
// GPIO LOW is a power-off request, not proof that RF/rail energy is already zero.
static void IRAM_ATTR trip_locked(stop_t reason)
{
    gpio_set_level(PA_POWER_GPIO, 0);
    gpio_set_level(PA_RXSW_GPIO, 0);
    if (stopped == STOP_NONE) { stopped = reason; }
    band_watch = false;
    sampling = false;
    rf_ready = false;
    band.qualified = false;
    ++sense_epoch; // Discard a conversion that was in flight before shutdown.
}

static void IRAM_ATTR cutoff_isr(void *argument)
{
    (void)argument;
    portENTER_CRITICAL_ISR(&guard_mux);
    trip_locked(STOP_TIMER);
    portEXIT_CRITICAL_ISR(&guard_mux);
}

static void IRAM_ATTR lpf_watch_isr(void *argument)
{
    (void)argument;
    portENTER_CRITICAL_ISR(&guard_mux);
    const int64_t current = esp_timer_get_time();
    if (band_watch) {
        if (!band.qualified) { trip_locked(STOP_LPF); }
        else if (current < band.sample_us || current - band.sample_us > LPF_FRESH_US) {
            trip_locked(STOP_STALE);
        }
    }
    portEXIT_CRITICAL_ISR(&guard_mux);
}

static void cancel_power(void)
{
    portENTER_CRITICAL(&guard_mux);
    trip_locked(STOP_OPERATOR);
    portEXIT_CRITICAL(&guard_mux);
}

static bool interrupted(void)
{
    portENTER_CRITICAL(&guard_mux);
    const bool result = stopped != STOP_NONE;
    portEXIT_CRITICAL(&guard_mux);
    return result;
}

static void power_off(void)
{
    portENTER_CRITICAL(&guard_mux);
    gpio_set_level(PA_POWER_GPIO, 0);
    gpio_set_level(PA_RXSW_GPIO, 0);
    sampling = false;
    band_watch = false;
    rf_ready = false;
    band.qualified = false;
    ++sense_epoch;
    portEXIT_CRITICAL(&guard_mux);
}

static bool has_fault(void)
{
    portENTER_CRITICAL(&guard_mux);
    const bool fault = shutdown_fault;
    portEXIT_CRITICAL(&guard_mux);
    return fault;
}

static void latch_fault(void)
{
    portENTER_CRITICAL(&guard_mux);
    shutdown_fault = true;
    portEXIT_CRITICAL(&guard_mux);
}

static esp_err_t store_marker(bool pending)
{
    esp_err_t error = nvs_set_u8(safety_store, "unverified", pending ? 1U : 0U);
    if (error == ESP_OK) { error = nvs_commit(safety_store); }
    if (error != ESP_OK) { latch_fault(); }
    else { marker_pending = pending; }
    return error;
}

static bool lpf_permits_key(void)
{
    portENTER_CRITICAL(&guard_mux);
    const int64_t current = esp_timer_get_time();
    const bool result = rf_ready && sampling && band_watch &&
        stopped == STOP_NONE && !shutdown_fault && band.qualified &&
        current >= band.sample_us && current - band.sample_us < LPF_FRESH_US;
    portEXIT_CRITICAL(&guard_mux);
    return result;
}

static void log_band(const char *label)
{
    portENTER_CRITICAL(&guard_mux);
    const band_snapshot_t copy = band;
    portEXIT_CRITICAL(&guard_mux);
    ESP_LOGI(TAG, "LPF %s: band=%s GPIO51 raw=%d mV=%d range=%d..%d spread=%d qualified=%s",
             label, lpf_classification_name(copy.reading.classification),
             copy.reading.raw_mean, copy.reading.mv_mean,
             copy.reading.mv_min, copy.reading.mv_max, copy.reading.mv_spread,
             copy.qualified ? "yes" : "no");
}

// IDF oneshot conversion waits inside a critical section. Keep that on CPU1,
// away from the CPU0 timer ISR; the ISR independently detects stale samples.
static void lpf_monitor(void *argument)
{
    (void)argument;
    for (;;) {
        portENTER_CRITICAL(&guard_mux);
        const bool enabled = sampling;
        const uint32_t epoch = sense_epoch;
        portEXIT_CRITICAL(&guard_mux);
        if (enabled) {
            lpf_sense_result_t reading = {0};
            const int64_t started_us = esp_timer_get_time();
            const esp_err_t error = lpf_sense_read_batch(&sense, &reading);
            const int64_t finished_us = esp_timer_get_time();
            // A slow acquisition must not make an old sample appear fresh.
            const bool good = error == ESP_OK && reading.classification == LPF_CLASS_40M &&
                finished_us >= started_us && finished_us - started_us <= 50000LL;
            portENTER_CRITICAL(&guard_mux);
            if (sampling && epoch == sense_epoch) {
                const bool adjacent = band.sample_us > 0 && started_us > band.sample_us &&
                    started_us - band.sample_us <= 100000LL;
                band.reading = reading;
                band.sample_us = started_us;
                if (good) {
                    if (band.consecutive == 0U || !adjacent) {
                        band.consecutive = 1U;
                        band.stable_since_us = started_us;
                    } else if (band.consecutive < 100U) { ++band.consecutive; }
                    band.qualified = band.consecutive >= 5U &&
                        started_us - band.stable_since_us >= LPF_STABLE_US;
                } else {
                    band.consecutive = 0U;
                    band.qualified = false;
                    if (error != ESP_OK) { band.reading.classification = LPF_CLASS_ADC_ERROR; }
                }
                if (band_watch && !band.qualified) { trip_locked(STOP_LPF); }
            }
            portEXIT_CRITICAL(&guard_mux);
        }
        vTaskDelay(pdMS_TO_TICKS(25) + 1);
    }
}

static esp_err_t disable_clocks_checked(void)
{
    esp_err_t error = si5351_write_register(&clock_device, 3U, 0xFFU);
    uint8_t mask = 0U;
    if (error == ESP_OK) {
        error = si5351_read_register(&clock_device, 3U, &mask);
        if (error == ESP_OK && mask != 0xFFU) { error = ESP_ERR_INVALID_RESPONSE; }
    }
    if (error == ESP_OK) { clocks_off_known = true; all_clocks_off_verified = true; }
    return error;
}

// Immediate physical-off request precedes every potentially blocking cleanup.
// A failed keyed shutdown is not excused by a LOW G48 readback.
static void close_session(void)
{
    power_off();
    (void)esp_timer_stop(prep_guard);
    (void)esp_timer_stop(burst_guard);
    if (!all_clocks_off_verified && clock_device.i2c_device != NULL) {
        (void)disable_clocks_checked(); // Bounded best effort, after power cutoff.
    }
    // Never let OFF/cleanup clear an inherited or already-latched fault.
    // Only explicit CLEARFAULT may acknowledge that physical power-cycle.
    if (pa_shutdown_may_clear(marker_pending, has_fault(), power_started, clocks_off_known)) {
        (void)store_marker(false);
    }
    if (marker_pending) { latch_fault(); }
    if (clock_device.i2c_device != NULL) { (void)si5351_deinit(&clock_device); }
    if (bus != NULL) { (void)i2c_del_master_bus(bus); bus = NULL; }
    memset(&clock_device, 0, sizeof(clock_device));
    power_started = false;
    portENTER_CRITICAL(&guard_mux);
    const stop_t reason = stopped;
    portEXIT_CRITICAL(&guard_mux);
    ESP_LOGI(TAG, "PA POWER OFF: G48=%d G47=%d; clk0_off_known=%s; stop_reason=%d",
             gpio_get_level(PA_POWER_GPIO), gpio_get_level(PA_RXSW_GPIO),
             clocks_off_known ? "yes" : "no", (int)reason);
    if (has_fault()) {
        ESP_LOGE(TAG, "PA SHUTDOWN FAULT LATCHED: full RF-board power-cycle, "
                     "then clearfault powercycled; USB reset alone is insufficient");
    }
}

static esp_err_t start_session(void)
{
    if (has_fault() || interrupted()) { return ESP_ERR_INVALID_STATE; }
    close_session();
    if (has_fault() || interrupted()) { return ESP_ERR_INVALID_STATE; }
    clocks_off_known = false;
    all_clocks_off_verified = false;
    power_started = false;
    // Stored before *any* RF power; reset during an unverified scan stays latched.
    esp_err_t error = store_marker(true);
    if (error != ESP_OK || interrupted()) { return error == ESP_OK ? ESP_ERR_TIMEOUT : error; }
    error = esp_timer_start_once(prep_guard, PA_PREP_LIMIT_US);
    if (error != ESP_OK) { return error; }
    portENTER_CRITICAL(&guard_mux);
    if (stopped == STOP_NONE) {
        error = gpio_set_level(PA_POWER_GPIO, 1);
        power_started = error == ESP_OK;
        // Keep timestamps monotonic across scan epochs. Qualification resets,
        // but publishing a synthetic timestamp zero would inhibit the policy.
        const int64_t previous_sample_us = band.sample_us;
        memset(&band, 0, sizeof(band));
        band.sample_us = previous_sample_us;
        ++sense_epoch;
        sampling = power_started;
    } else { error = ESP_ERR_TIMEOUT; }
    portEXIT_CRITICAL(&guard_mux);
    if (error != ESP_OK) { return error; }
    vTaskDelay(pdMS_TO_TICKS(200));
    if (interrupted()) { return ESP_ERR_TIMEOUT; }

    const i2c_master_bus_config_t config = {
        .i2c_port = I2C_NUM_0, .sda_io_num = GPIO_NUM_31, .scl_io_num = GPIO_NUM_32,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,
    };
    error = i2c_new_master_bus(&config, &bus);
    if (error != ESP_OK || interrupted()) { return error == ESP_OK ? ESP_ERR_TIMEOUT : error; }
    error = si5351_init(&clock_device, bus, SI5351_DEFAULT_I2C_ADDRESS, 26000000U,
                       SI5351_REFERENCE_EXTERNAL_CLOCK, 100000U);
    if (error != ESP_OK || interrupted()) { return error == ESP_OK ? ESP_ERR_TIMEOUT : error; }
    error = si5351_probe(&clock_device, 100);
    if (error != ESP_OK || interrupted()) { return error == ESP_OK ? ESP_ERR_TIMEOUT : error; }
    error = disable_clocks_checked();
    if (error != ESP_OK || interrupted()) { return error == ESP_OK ? ESP_ERR_TIMEOUT : error; }
    // No output-enable writes occur in scanning; disabled output levels are LOW.
    error = si5351_write_register(&clock_device, 24U, 0U);
    if (error == ESP_OK) { error = si5351_write_register(&clock_device, 25U, 0U); }
    if (error != ESP_OK || interrupted()) { return error == ESP_OK ? ESP_ERR_TIMEOUT : error; }
    const int64_t until = esp_timer_get_time() + 600000LL;
    bool accepted = false;
    while (!interrupted() && esp_timer_get_time() < until) {
        portENTER_CRITICAL(&guard_mux);
        const int64_t current = esp_timer_get_time();
        accepted = band.qualified && current >= band.sample_us &&
            current - band.sample_us < LPF_FRESH_US && stopped == STOP_NONE;
        if (accepted) { band_watch = true; rf_ready = true; }
        portEXIT_CRITICAL(&guard_mux);
        if (accepted) { break; }
        vTaskDelay(1);
    }
    log_band("SCAN");
    if (!accepted) { return ESP_ERR_INVALID_RESPONSE; }
    // Freshness ISR now overlaps the preparation guard. Never disable freshness
    // for flash operations; a slow NVS operation is allowed to fail closed.
    error = store_marker(false);
    (void)esp_timer_stop(prep_guard);
    if (error != ESP_OK || !lpf_permits_key()) { return error == ESP_OK ? ESP_ERR_TIMEOUT : error; }
    ESP_LOGI(TAG, "LPF SCAN COMPLETE: 40m qualified; G48=HIGH for sensing; "
                 "CLK0=OFF; auto-arm eligible, never auto-TX");
    return ESP_OK;
}

static void float_i2s_pins(void)
{
    const gpio_num_t pins[] = {GPIO_NUM_16, GPIO_NUM_45, GPIO_NUM_3, GPIO_NUM_4};
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        gpio_reset_pin(pins[i]);
        gpio_set_pull_mode(pins[i], GPIO_FLOATING);
    }
}

static esp_err_t perform_burst(const pa_decision_t *job)
{
    esp_err_t error = ESP_OK;
    const char *stage = "fresh LPF precheck";
    pcm1808_i2s_t adc = {0};
    rf_loopback_result_t baseline = {0}, keyed = {0};
    bool baseline_valid = false, keyed_valid = false, key_attempted = false;
    int64_t key_started_us = 0, key_finished_us = 0;
    const bool leakage = job->action == PA_ACTION_LEAK;

    ESP_LOGW(TAG, "PA PREP: 40m CLK0=7075000 Hz; G47=LOW; mode=%s; limit=%" PRIu32
                  " ms; LPF sensed, dummy load/current/temperature NOT sensed",
             leakage ? "leak" : "pa", job->duration_ms);
    if (!lpf_permits_key()) { error = ESP_ERR_INVALID_STATE; goto cleanup; }
    log_band("PREKEY");
    stage = "clock plan";
    // The helper leaves CLK0 OFF throughout and enables CLK1 only at the end.
    all_clocks_off_verified = false;
    error = si5351_configure_rx_loopback(&clock_device, PA_POLICY_RX_HZ,
                                        PA_POLICY_SOURCE_HZ, true);
    if (error != ESP_OK || !lpf_permits_key()) { goto cleanup; }
    if (!leakage) {
        error = disable_clocks_checked();
        if (error != ESP_OK || !lpf_permits_key()) { goto cleanup; }
    } else {
        stage = "I2S startup/baseline";
        const pcm1808_i2s_config_t config = {
            .port = I2S_NUM_1, .mclk_gpio = GPIO_NUM_16, .bclk_gpio = GPIO_NUM_45,
            .lrck_gpio = GPIO_NUM_3, .din_gpio = GPIO_NUM_4, .sample_rate_hz = 48000U,
        };
        error = pcm1808_i2s_init(&adc, &config);
        if (error != ESP_OK || !lpf_permits_key()) { goto cleanup; }
        error = pcm1808_i2s_enable(&adc);
        if (error != ESP_OK || !lpf_permits_key()) { goto cleanup; }
        error = rf_loopback_capture(&adc, 1000U, 100U, &baseline);
        if (error != ESP_OK || !lpf_permits_key()) { goto cleanup; }
        baseline_valid = true;
        if (baseline.nonzero_padding_words != 0U ||
            baseline.channels[0].rail_hits != 0U || baseline.channels[1].rail_hits != 0U) {
            error = ESP_ERR_INVALID_RESPONSE; goto cleanup;
        }
    }
    stage = "persistent key marker";
    error = store_marker(true); // Flash writes finish before CLK0 is enabled.
    if (error != ESP_OK || !lpf_permits_key()) { goto cleanup; }
    stage = "burst guard";
    error = esp_timer_start_once(burst_guard, (uint64_t)job->duration_ms * 1000ULL);
    if (error != ESP_OK || !lpf_permits_key()) { goto cleanup; }
    stage = "CLK0 enable";
    key_started_us = esp_timer_get_time();
    // Last permission check is under the cutoff lock, with the guard armed.
    // I2C itself cannot be in that lock. A concurrent trip cuts G48 and cannot
    // be undone by any later write in this function (it never raises G48).
    if (!lpf_permits_key()) { error = ESP_ERR_INVALID_STATE; goto cleanup; }
    key_attempted = true;
    clocks_off_known = false;
    all_clocks_off_verified = false;
    const uint8_t enabled = SI5351_OUTPUT_CLK0 | (leakage ? SI5351_OUTPUT_CLK1 : 0U);
    error = si5351_write_register(&clock_device, 3U, (uint8_t)~enabled);
    if (error != ESP_OK || !lpf_permits_key()) { goto cleanup; }
    uint8_t mask = 0U;
    error = si5351_read_register(&clock_device, 3U, &mask);
    if (error != ESP_OK || mask != (uint8_t)~enabled || !lpf_permits_key()) {
        if (error == ESP_OK) { error = ESP_ERR_INVALID_RESPONSE; }
        goto cleanup;
    }
    stage = "bounded burst";
    if (leakage) {
        const uint32_t capture_ms = job->duration_ms == 100U ? 50U : 200U;
        error = rf_loopback_capture(&adc, PA_SWITCH_BLANK_MS, capture_ms, &keyed);
        if (error != ESP_OK || !lpf_permits_key()) { goto cleanup; }
        keyed_valid = true;
    } else {
        const int64_t margin_ms = job->duration_ms == 10000U ? 50LL : 20LL;
        const int64_t finish = key_started_us + ((int64_t)job->duration_ms - margin_ms) * 1000LL;
        while (lpf_permits_key() && esp_timer_get_time() < finish) { vTaskDelay(1); }
        if (!lpf_permits_key()) { error = ESP_ERR_TIMEOUT; goto cleanup; }
    }
    stage = "CLK0 disable";
    error = disable_clocks_checked();
    key_finished_us = esp_timer_get_time();

cleanup:
    if (error == ESP_OK && (!lpf_permits_key() || !clocks_off_known)) {
        error = ESP_ERR_TIMEOUT;
    }
    if (error != ESP_OK) {
        // Cut power before attempting any I2C/ADC cleanup.
        close_session();
    } else {
        (void)esp_timer_stop(burst_guard);
        error = store_marker(false); // Outputs already confirmed OFF.
        if (error != ESP_OK || !lpf_permits_key()) {
            if (error == ESP_OK) { error = ESP_ERR_TIMEOUT; }
            close_session();
        }
    }
    if (adc.rx_channel != NULL) {
        const esp_err_t adc_error = pcm1808_i2s_deinit(&adc);
        float_i2s_pins();
        if (error == ESP_OK && adc_error != ESP_OK) {
            error = adc_error;
            close_session();
        }
    }
    ESP_LOGI(TAG, "PA RF OFF: G48=%d G47=%d; key_attempted=%s; "
                  "clock_mask_off_verified=%s; sensing=%s",
             gpio_get_level(PA_POWER_GPIO), gpio_get_level(PA_RXSW_GPIO),
             key_attempted ? "yes" : "no", all_clocks_off_verified ? "yes" : "no",
             lpf_permits_key() ? "active" : "off");
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "PA JOB FAILED at %s: %s; power off; use scan after diagnosis",
                 stage, esp_err_to_name(error));
        log_band("STOPPED");
    } else {
        ESP_LOGI(TAG, "PA JOB COMPLETE: mode=%s limit_ms=%" PRIu32
                     " command_interval_us=%" PRId64 "; RF envelope not measured",
                 leakage ? "leak" : "pa", job->duration_ms, key_finished_us - key_started_us);
    }
    if (baseline_valid) { (void)rf_loopback_report("PA_OFF_BASELINE", &baseline); }
    if (keyed_valid && error == ESP_OK) { (void)rf_loopback_report("PA_KEYED_LEAK", &keyed); }
    return error;
}

static void pa_worker(void *argument)
{
    (void)argument;
    pa_decision_t job;
    for (;;) {
        if (xQueueReceive(jobs, &job, pdMS_TO_TICKS(10)) == pdTRUE) {
            esp_err_t error = ESP_OK;
            if (job.action == PA_ACTION_SCAN) {
                ESP_LOGI(TAG, "LPF SCAN: power-on inspection only; no TX");
                error = start_session();
                if (error != ESP_OK) {
                    close_session();
                    ESP_LOGE(TAG, "LPF SCAN FAILED: %s; no automatic retry; inspect LPF/power, then scan",
                             esp_err_to_name(error));
                }
            } else if (job.action == PA_ACTION_OFF) {
                close_session();
            } else if (job.action == PA_ACTION_CLEARFAULT) {
                close_session();
                error = store_marker(false);
                if (error == ESP_OK) {
                    portENTER_CRITICAL(&guard_mux);
                    shutdown_fault = false;
                    portEXIT_CRITICAL(&guard_mux);
                    ESP_LOGW(TAG, "PA FAULT CLEARED: operator confirmed full RF-board power-cycle; "
                                  "DISARMED; send scan to resume; this is not sensed");
                } else { ESP_LOGE(TAG, "PA FAULT REMAINS LATCHED: NVS update failed"); }
            } else if (job.action == PA_ACTION_PA || job.action == PA_ACTION_LEAK) {
                error = perform_burst(&job);
            } else {
                cancel_power();
                close_session();
                error = ESP_ERR_INVALID_ARG;
            }
            portENTER_CRITICAL(&guard_mux);
            worker_success = error == ESP_OK;
            worker_was_burst = job.action == PA_ACTION_PA || job.action == PA_ACTION_LEAK;
            worker_done = true;
            portEXIT_CRITICAL(&guard_mux);
        } else if (bus != NULL && interrupted()) {
            log_band("TRIP");
            close_session();
            portENTER_CRITICAL(&guard_mux);
            idle_stopped = true;
            portEXIT_CRITICAL(&guard_mux);
        }
    }
}

static bool discard_usb(void)
{
    uint8_t bytes[128];
    bool discarded = false;
    for (unsigned pass = 0; pass < 8U; ++pass) {
        if (usb_serial_jtag_read_bytes(bytes, sizeof(bytes), 0) <= 0) { break; }
        discarded = true;
    }
    return discarded;
}

static void help(void)
{
    ESP_LOGI(TAG, "PA COMMANDS: status | help | off | scan | pa [100|250|10000] | "
                 "leak [100|250] | clearfault powercycled");
    ESP_LOGW(TAG, "FIXED 40m 7075000 Hz. Auto-arm requires calibrated stable 40m LPF ID. "
                 "Fit rated 50-ohm dummy load; no antenna. Load/current/temperature NOT sensed.");
    ESP_LOGI(TAG, "Powered sensing keeps G48 HIGH with CLK0 OFF. off inhibits until scan/reboot. "
                 "5s cooldown, 10s after pa 10000. No automatic TX/repeat; no 10s leak mode.");
    ESP_LOGW(TAG, "LPF resistor ID is not RF filter/load verification; do not hot-swap LPFs.");
}

static void status(const pa_policy_t *policy)
{
    portENTER_CRITICAL(&guard_mux);
    const band_snapshot_t copy = band;
    const bool active = sampling;
    const bool fault = shutdown_fault;
    portEXIT_CRITICAL(&guard_mux);
    ESP_LOGI(TAG, "PA STATUS: %s; G48=%d G47=%d; band=40m source=7075000 Hz; "
                 "cooldown_ms=%" PRIu32 "; shutdown_fault=%s; sensing=%s; LPF=%s mV=%d",
             pa_policy_is_armed(policy, now_ms()) ? "ARMED_AUTO" : "DISARMED",
             gpio_get_level(PA_POWER_GPIO), gpio_get_level(PA_RXSW_GPIO),
             pa_policy_cooldown_remaining(policy, now_ms()), fault ? "LATCHED" : "none",
             active ? "on" : "off", lpf_classification_name(copy.reading.classification),
             copy.reading.mv_mean);
}

static void fatal_idle(const char *stage, esp_err_t error)
{
    power_off();
    ESP_LOGE(TAG, "PA INIT FAILED %s: %s; RF power held off", stage, esp_err_to_name(error));
    for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}

void pa_test_run(void)
{
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
    error = nvs_flash_init();
    if (error == ESP_OK) { error = nvs_open("pa_bench", NVS_READWRITE, &safety_store); }
    if (error != ESP_OK) { fatal_idle("persistent safety state", error); }
    uint8_t pending = 0U;
    error = nvs_get_u8(safety_store, "unverified", &pending);
    if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) {
        fatal_idle("persistent safety state", error);
    }
    shutdown_fault = pending != 0U;
    marker_pending = shutdown_fault;
    const esp_timer_create_args_t timer = {
        .callback = cutoff_isr, .dispatch_method = ESP_TIMER_ISR, .name = "pa_cutoff",
    };
    error = esp_timer_create(&timer, &prep_guard);
    if (error == ESP_OK) { error = esp_timer_create(&timer, &burst_guard); }
    const esp_timer_create_args_t watcher = {
        .callback = lpf_watch_isr, .dispatch_method = ESP_TIMER_ISR, .name = "lpf_watch",
    };
    if (error == ESP_OK) { error = esp_timer_create(&watcher, &lpf_guard); }
    if (error != ESP_OK) { fatal_idle("cutoff timers", error); }
    error = esp_timer_start_once(burst_guard, 10000U);
    if (error != ESP_OK) { fatal_idle("cutoff self-check", error); }
    vTaskDelay(pdMS_TO_TICKS(30));
    if (stopped != STOP_TIMER || gpio_get_level(PA_POWER_GPIO) != 0) {
        fatal_idle("cutoff self-check", ESP_FAIL);
    }
    (void)esp_timer_stop(burst_guard);
    stopped = STOP_NONE;
    ESP_LOGI(TAG, "PA GUARD SELF-CHECK PASS: ISR fired with RF power held off");
    error = lpf_sense_init(&sense);
    if (error != ESP_OK) { fatal_idle("GPIO51 calibrated LPF ADC", error); }
    error = esp_timer_start_periodic(lpf_guard, LPF_WATCH_PERIOD_US);
    if (error != ESP_OK) { fatal_idle("LPF freshness timer", error); }
    usb_serial_jtag_driver_config_t usb = {.rx_buffer_size = 256U, .tx_buffer_size = 1024U};
    error = usb_serial_jtag_driver_install(&usb);
    if (error != ESP_OK) { fatal_idle("USB console", error); }
    usb_serial_jtag_vfs_use_driver();
    jobs = xQueueCreate(1, sizeof(pa_decision_t));
    if (jobs == NULL ||
        xTaskCreatePinnedToCore(lpf_monitor, "lpf_adc", 4096, NULL, 5, NULL, 1) != pdPASS ||
        xTaskCreatePinnedToCore(pa_worker, "pa_worker", 8192, NULL, 5, NULL, 0) != pdPASS) {
        fatal_idle("workers", ESP_ERR_NO_MEM);
    }
    pa_policy_t policy;
    pa_policy_init(&policy);
    pa_policy_set_fault(&policy, shutdown_fault);
    bool busy = false, bad_line = false, cancelling = false, was_armed = false;
    char line[PA_POLICY_MAX_LINE + 1U];
    size_t used = 0;
    (void)discard_usb();
    ESP_LOGI(TAG, "PA TEST READY: LPF AUTO mode; boot starts CLK0 OFF, G47 LOW; no boot TX");
    help();
    status(&policy);
    // A missing/failed scan powers off and does NOT retry automatically.
    if (!has_fault()) {
        const pa_decision_t initial = {.action = PA_ACTION_SCAN};
        busy = xQueueSend(jobs, &initial, 0) == pdTRUE;
        if (!busy) { fatal_idle("initial scan queue", ESP_FAIL); }
    }
    for (;;) {
        portENTER_CRITICAL(&guard_mux);
        const bool done = worker_done, succeeded = worker_success;
        const bool was_burst = worker_was_burst, idle_trip = idle_stopped;
        const bool fault = shutdown_fault;
        const bool ready = rf_ready && band.qualified && sampling && stopped == STOP_NONE;
        const int64_t sample_us = band.sample_us;
        if (done) { worker_done = false; }
        if (idle_trip) { idle_stopped = false; }
        portEXIT_CRITICAL(&guard_mux);
        // Obtain time AFTER the snapshot: CPU1 may have just published a sample.
        // Completion is observed here, conservatively after actual RF shutdown.
        const uint64_t current_ms = now_ms();
        pa_policy_set_fault(&policy, fault);
        if (done) {
            if (was_burst) { pa_policy_complete(&policy, current_ms); }
            if (!succeeded) { pa_policy_inhibit(&policy); }
            busy = false;
            used = 0; bad_line = false;
            (void)discard_usb();
            ESP_LOGI(TAG, "PA IDLE: cooldown_ms=%" PRIu32 "; TX requires a new command",
                     pa_policy_cooldown_remaining(&policy, current_ms));
        }
        if (idle_trip) {
            // A scan may already be queued while the previous idle trip is
            // finishing cleanup. Do not let that old notification inhibit the
            // new scan; its own result/cancellation controls new permission.
            if (!busy && !done) { pa_policy_inhibit(&policy); }
            ESP_LOGW(TAG, "LPF monitoring stopped RF power; inspect readings and use scan; no auto-retry");
        }
        (void)pa_policy_set_lpf(&policy, ready, sample_us > 0 ? (uint64_t)sample_us / 1000U : 0U,
                               current_ms);
        const bool armed = !busy && pa_policy_is_armed(&policy, current_ms);
        if (armed && !was_armed) {
            ESP_LOGI(TAG, "PA AUTO ARMED: stable 40m LPF; CLK0 OFF; awaiting pa/leak command; "
                         "dummy load is NOT sensed");
            log_band("READY");
        }
        was_armed = armed;
        uint8_t input[128];
        const int count = usb_serial_jtag_read_bytes(input, sizeof(input), pdMS_TO_TICKS(10));
        if (count <= 0) { continue; }
        if (busy) {
            bool substantive = false;
            for (int i = 0; i < count; ++i) { substantive |= input[i] != '\r' && input[i] != '\n'; }
            if (!substantive) { continue; }
            cancel_power();
            pa_policy_inhibit(&policy);
            if (!cancelling) { ESP_LOGW(TAG, "PA CANCEL: USB input received during job"); }
            cancelling = true;
            continue;
        }
        for (int index = 0; index < count; ++index) {
            const uint8_t byte = input[index];
            if (byte == 3U || byte == 27U) {
                cancel_power(); pa_policy_inhibit(&policy); used = 0; bad_line = false;
                ESP_LOGI(TAG, "PA OFF: keyboard cancellation; sensing inhibited until scan/reboot");
                break;
            }
            if (byte != '\r' && byte != '\n') {
                if (byte < 32U || byte > 126U || used >= PA_POLICY_MAX_LINE) {
                    bad_line = true;
                    pa_policy_inhibit(&policy);
                    cancel_power();
                } else if (!bad_line) { line[used++] = (char)byte; }
                continue;
            }
            if (used == 0U && !bad_line) { continue; }
            line[used] = '\0';
            bool extra = false;
            for (int tail = index + 1; tail < count; ++tail) {
                extra |= input[tail] != '\r' && input[tail] != '\n';
            }
            extra |= discard_usb();
            const pa_decision_t decision = pa_policy_submit(&policy,
                bad_line || extra ? "invalid" : line, now_ms());
            used = 0; bad_line = false;
            switch (decision.action) {
            case PA_ACTION_HELP: help(); break;
            case PA_ACTION_STATUS: status(&policy); break;
            case PA_ACTION_CLEARFAULT:
            case PA_ACTION_SCAN:
            case PA_ACTION_PA:
            case PA_ACTION_LEAK:
            case PA_ACTION_OFF:
                if (decision.action == PA_ACTION_OFF || decision.action == PA_ACTION_CLEARFAULT) {
                    cancel_power();
                }
                portENTER_CRITICAL(&guard_mux);
                if (decision.action == PA_ACTION_SCAN) { stopped = STOP_NONE; }
                worker_done = false;
                portEXIT_CRITICAL(&guard_mux);
                busy = true; cancelling = false;
                if (xQueueSend(jobs, &decision, 0) != pdTRUE) {
                    cancel_power(); pa_policy_inhibit(&policy); busy = false;
                    pa_policy_complete(&policy, now_ms());
                    ESP_LOGE(TAG, "PA queue failed; disarmed");
                }
                break;
            default:
                cancel_power();
                ESP_LOGW(TAG, "PA REJECTED: %s; sensing inhibited; use scan to resume",
                         pa_policy_rejection_name(decision.rejection));
                break;
            }
            break;
        }
    }
}
#endif
