// SPDX-License-Identifier: MIT
// Tests the production PA supervisor directly, with simulated peripherals/time.
// cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
//   -Itests/pa_precheck_stubs -Itests/lpf_sense_stubs -Itab5/main/include \
//   tests/pa_precheck_test.c tab5/main/pa_policy.c tab5/main/lpf_classify.c \
//   -o /tmp/pa_precheck_test && /tmp/pa_precheck_test
// The cooperative task/timer scheduler checks deterministic interleavings, not
// real FreeRTOS scheduling, electrical behavior, interrupt latency or RF safety.

#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include "../tab5/main/pa_test.c"

struct pa_precheck_timer { bool active; int64_t deadline; void (*callback)(void *); void *arg; };
static struct pa_precheck_timer test_prep, test_burst;
static int64_t test_time, next_sample, key_time, prep_done_time, baseline_done_time, marker_done_time;
static int64_t batch_delay, cancel_delay, stale_log_delay;
static unsigned critical_depth, adc_batches, key_writes, adc_at_key, plan_calls, baseline_calls;
static unsigned marker_writes, captures, prechecks;
static uint8_t test_mask, nvs_value;
static int levels[64];
static bool in_sampler, skip_sampler, force_burst_cutoff, cancelled;
static lpf_classification_t test_band;
static esp_err_t adc_error;
static jmp_buf sampler_return;

static void advance_us(int64_t delta);

void pa_precheck_enter(portMUX_TYPE *mux) { (void)mux; ++critical_depth; }
void pa_precheck_exit(portMUX_TYPE *mux) { (void)mux; assert(critical_depth > 0U); --critical_depth; }
int64_t esp_timer_get_time(void) { return test_time; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "simulated error"; }

static void sampler_once(void)
{
    assert(!in_sampler && critical_depth == 0U);
    in_sampler = true;
    if (setjmp(sampler_return) == 0) { lpf_sampler(NULL); }
    in_sampler = false;
    assert(critical_depth == 0U);
}

static void dispatch_cutoffs(void)
{
    struct pa_precheck_timer *timers[] = {&test_prep, &test_burst};
    for (unsigned i = 0U; i < 2U; ++i) {
        if (timers[i]->active && test_time >= timers[i]->deadline) {
            timers[i]->active = false;
            timers[i]->callback(timers[i]->arg);
        }
    }
    if (!cancelled && key_writes != 0U && cancel_delay > 0 && test_time >= key_time + cancel_delay) {
        cancelled = true;
        cancel_power();
    }
}

static void advance_us(int64_t delta)
{
    assert(delta >= 0 && critical_depth == 0U);
    const int64_t until = test_time + delta;
    while (test_time < until) {
        test_time += until - test_time < 1000 ? until - test_time : 1000;
        dispatch_cutoffs();
        if (!in_sampler && !skip_sampler && test_time >= next_sample) {
            next_sample = test_time + 30000;
            sampler_once();
        }
    }
}

void vTaskDelay(TickType_t ticks)
{
    assert(critical_depth == 0U);
    if (in_sampler) { longjmp(sampler_return, 1); }
    advance_us((int64_t)ticks * 10000);
}

void pa_precheck_log(const char *format, ...)
{
    if (strncmp(format, "LPF %s:", 7U) == 0) {
        va_list arguments;
        va_start(arguments, format);
        const char *label = va_arg(arguments, const char *);
        va_end(arguments);
        if (strcmp(label, "PREKEY") == 0) {
            ++prechecks;
            if (stale_log_delay > 0) { advance_us(stale_log_delay); }
        }
    }
}

esp_err_t gpio_set_level(gpio_num_t pin, uint32_t value)
{ assert(pin >= 0 && pin < 64); levels[pin] = (int)value; return ESP_OK; }
int gpio_get_level(gpio_num_t pin) { assert(pin >= 0 && pin < 64); return levels[pin]; }
esp_err_t gpio_reset_pin(gpio_num_t pin) { (void)pin; return ESP_OK; }
esp_err_t gpio_set_pull_mode(gpio_num_t pin, int mode) { (void)pin; (void)mode; return ESP_OK; }
esp_err_t gpio_config(const gpio_config_t *config) { (void)config; return ESP_OK; }

esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us)
{
    assert(timer != NULL && !timer->active);
    timer->active = true;
    timer->deadline = test_time + (int64_t)timeout_us;
    if (timer == burst_guard && force_burst_cutoff) { timer->deadline = test_time + 10000; }
    return ESP_OK;
}
esp_err_t esp_timer_stop(esp_timer_handle_t timer) { timer->active = false; return ESP_OK; }
esp_err_t esp_timer_create(const esp_timer_create_args_t *config, esp_timer_handle_t *timer)
{ (void)config; (void)timer; assert(!"boot entry is not used by this harness"); return ESP_FAIL; }

esp_err_t lpf_sense_init(lpf_sense_t *device) { (void)device; return ESP_OK; }
esp_err_t lpf_sense_read_batch(lpf_sense_t *device, lpf_sense_result_t *result)
{
    assert(device == &sense && in_sampler && sampling && sense_busy);
    assert((test_mask & SI5351_OUTPUT_CLK0) != 0U); // Every acquisition has CLK0 OFF.
    assert(key_writes == 0U); // No ADC sampling during or after keyed RF in each job.
    if (plan_calls != 0U) {
        assert(marker_writes != 0U && test_time >= marker_done_time);
        assert(test_time >= prep_done_time && test_time >= baseline_done_time);
    }
    ++adc_batches;
    advance_us(batch_delay);
    *result = (lpf_sense_result_t){.classification = test_band, .sample_count = 8U,
                                 .raw_mean = 1100, .mv_mean = 1055, .mv_min = 1055, .mv_max = 1055};
    return adc_error;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{ (void)handle; (void)key; nvs_value = value; if (value != 0U) { ++marker_writes; } return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t handle)
{ (void)handle; advance_us(8000); if (nvs_value != 0U) { marker_done_time = test_time; } return ESP_OK; }
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{ (void)name; (void)mode; *handle = 1U; return ESP_OK; }
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{ (void)handle; (void)key; *value = nvs_value; return ESP_OK; }

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *config, i2c_master_bus_handle_t *handle)
{ (void)config; *handle = (void *)1; return ESP_OK; }
esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t handle) { (void)handle; return ESP_OK; }
esp_err_t si5351_init(si5351_t *device, i2c_master_bus_handle_t handle, uint8_t address,
                      uint32_t reference, si5351_reference_mode_t mode, uint32_t frequency)
{ (void)handle; (void)address; (void)reference; (void)mode; (void)frequency; device->i2c_device = (void *)2; return ESP_OK; }
esp_err_t si5351_deinit(si5351_t *device) { device->i2c_device = NULL; return ESP_OK; }
esp_err_t si5351_probe(const si5351_t *device, int timeout) { (void)device; (void)timeout; return ESP_OK; }
esp_err_t si5351_configure_rx_loopback(si5351_t *device, uint32_t rx, uint32_t source, bool verify)
{
    (void)device;
    assert(rx == PA_POLICY_RX_HZ && source == PA_POLICY_SOURCE_HZ && verify);
    assert(!sampling && !sense_busy && (test_mask & SI5351_OUTPUT_CLK0) != 0U);
    ++plan_calls;
    advance_us(300000);
    test_mask = (uint8_t)~SI5351_OUTPUT_CLK1;
    prep_done_time = test_time;
    return ESP_OK;
}
esp_err_t si5351_write_register(si5351_t *device, uint8_t reg, uint8_t value)
{
    (void)device;
    if (reg == 3U) {
        if ((value & SI5351_OUTPUT_CLK0) == 0U) {
            assert(!sampling && !sense_busy && lpf_prekey_is_fresh());
            assert(test_burst.active && !test_prep.active && levels[PA_POWER_GPIO] == 1);
            assert(adc_batches >= 5U && prechecks == 1U && marker_pending);
            ++key_writes;
            adc_at_key = adc_batches;
            key_time = test_time;
            test_band = LPF_CLASS_INVALID; // RF disturbance must not be sampled.
        }
        test_mask = value;
    }
    return ESP_OK;
}
esp_err_t si5351_read_register(si5351_t *device, uint8_t reg, uint8_t *value)
{ (void)device; assert(reg == 3U); *value = test_mask; return ESP_OK; }

esp_err_t pcm1808_i2s_init(pcm1808_i2s_t *device, const pcm1808_i2s_config_t *config)
{ assert(config->sample_rate_hz == 48000U); device->rx_channel = (void *)3; return ESP_OK; }
esp_err_t pcm1808_i2s_enable(pcm1808_i2s_t *device) { device->enabled = true; return ESP_OK; }
esp_err_t pcm1808_i2s_deinit(pcm1808_i2s_t *device) { device->rx_channel = NULL; return ESP_OK; }
esp_err_t rf_loopback_capture(pcm1808_i2s_t *device, uint32_t discard, uint32_t duration,
                              rf_loopback_result_t *result)
{
    assert(device->enabled && !sampling && !sense_busy);
    ++captures;
    memset(result, 0, sizeof(*result));
    advance_us(((int64_t)discard + duration) * 1000);
    if (key_writes == 0U) { ++baseline_calls; baseline_done_time = test_time; }
    return ESP_OK;
}
esp_err_t rf_loopback_report(const char *label, const rf_loopback_result_t *result)
{ (void)label; (void)result; return ESP_OK; }

int xTaskCreatePinnedToCore(void (*entry)(void *), const char *name, unsigned stack,
                          void *argument, unsigned priority, void *handle, int core)
{ (void)entry; (void)name; (void)stack; (void)argument; (void)priority; (void)handle; (void)core; return pdPASS; }
QueueHandle_t xQueueCreate(unsigned length, unsigned item_size) { (void)length; (void)item_size; return (void *)4; }
int xQueueReceive(QueueHandle_t queue, void *item, TickType_t timeout)
{ (void)queue; (void)item; (void)timeout; return 0; }
int xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout)
{ (void)queue; (void)item; (void)timeout; return pdTRUE; }
esp_err_t usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t *config)
{ (void)config; return ESP_OK; }
int usb_serial_jtag_read_bytes(void *buffer, size_t length, TickType_t timeout)
{ (void)buffer; (void)length; (void)timeout; return 0; }
void usb_serial_jtag_vfs_use_driver(void) {}

static void reset_session(void)
{
    test_time = 1000000;
    next_sample = test_time + 1000;
    key_time = prep_done_time = baseline_done_time = marker_done_time = 0;
    batch_delay = cancel_delay = stale_log_delay = 0;
    critical_depth = adc_batches = key_writes = adc_at_key = plan_calls = baseline_calls = 0U;
    marker_writes = captures = prechecks = 0U;
    in_sampler = skip_sampler = force_burst_cutoff = cancelled = false;
    adc_error = ESP_OK;
    test_band = LPF_CLASS_40M;
    test_mask = 0xFFU;
    nvs_value = 0U;
    memset(levels, 0, sizeof(levels));
    levels[PA_POWER_GPIO] = 1;
    test_prep = (struct pa_precheck_timer){.callback = cutoff_isr};
    test_burst = (struct pa_precheck_timer){.callback = cutoff_isr};
    prep_guard = &test_prep;
    burst_guard = &test_burst;
    band = (band_snapshot_t){.sample_us = 1000, .qualified = true};
    band.reading.classification = LPF_CLASS_40M;
    sense_epoch = 0U;
    sampling = sense_busy = false;
    rf_ready = true;
    stopped = STOP_NONE;
    shutdown_fault = marker_pending = false;
    worker_done = worker_success = worker_was_burst = idle_stopped = false;
    bus = (void *)1;
    clock_device = (si5351_t){.i2c_device = (void *)2};
    clocks_off_known = all_clocks_off_verified = power_started = true;
}

static void expect_no_key(pa_action_t action)
{
    const pa_decision_t job = {.action = action, .duration_ms = 100U};
    assert(perform_burst(&job) != ESP_OK);
    assert(key_writes == 0U && !sampling && !rf_ready && levels[PA_POWER_GPIO] == 0);
}

int main(void)
{
    reset_session();
    assert(start_session() == ESP_OK);
    assert(session_permits_operation() && !sampling && !sense_busy);
    assert(levels[PA_POWER_GPIO] == 1 && key_writes == 0U && test_mask == 0xFFU);
    assert(adc_batches >= 5U && !marker_pending && !test_prep.active);
    const unsigned scan_batches = adc_batches;
    advance_us(20000000);
    assert(session_permits_operation() && adc_batches == scan_batches && key_writes == 0U);

    reset_session();
    test_band = LPF_CLASS_20M;
    assert(start_session() != ESP_OK);
    close_session(); // The worker owns fail-closed cleanup of an unsuccessful scan.
    assert(levels[PA_POWER_GPIO] == 0 && !sampling && !rf_ready && key_writes == 0U);
    assert(!test_prep.active && !test_burst.active);

    reset_session();
    advance_us(20000000);
    assert(session_permits_operation() && !lpf_prekey_is_fresh());
    assert(adc_batches == 0U && stopped == STOP_NONE && levels[PA_POWER_GPIO] == 1);

    const pa_decision_t jobs_to_test[] = {
        {.action = PA_ACTION_PA, .duration_ms = 100U},
        {.action = PA_ACTION_PA, .duration_ms = 250U},
        {.action = PA_ACTION_PA, .duration_ms = 10000U},
        {.action = PA_ACTION_LEAK, .duration_ms = 100U},
        {.action = PA_ACTION_LEAK, .duration_ms = 250U},
    };
    for (unsigned i = 0U; i < sizeof(jobs_to_test) / sizeof(jobs_to_test[0]); ++i) {
        reset_session();
        assert(perform_burst(&jobs_to_test[i]) == ESP_OK);
        assert(key_writes == 1U && adc_batches == adc_at_key && prechecks == 1U);
        assert(!sampling && !sense_busy && rf_ready && levels[PA_POWER_GPIO] == 1);
        assert(stopped == STOP_NONE && test_mask == 0xFFU && !marker_pending);
        assert(baseline_calls == (jobs_to_test[i].action == PA_ACTION_LEAK ? 1U : 0U));
        const unsigned final_adc_batches = adc_batches;
        advance_us(20000000);
        assert(session_permits_operation() && adc_batches == final_adc_batches && key_writes == 1U);
    }

    for (unsigned leakage = 0U; leakage < 2U; ++leakage) {
        const pa_action_t action = leakage != 0U ? PA_ACTION_LEAK : PA_ACTION_PA;
        reset_session(); test_band = LPF_CLASS_20M; expect_no_key(action);
        reset_session(); test_band = LPF_CLASS_INVALID; expect_no_key(action);
        reset_session(); adc_error = ESP_ERR_TIMEOUT; expect_no_key(action);
        reset_session(); skip_sampler = true; expect_no_key(action);
        reset_session(); batch_delay = 60000; expect_no_key(action); // Each batch exceeds 50 ms.
        reset_session(); skip_sampler = true; sense_busy = true; expect_no_key(action);
        reset_session(); stale_log_delay = 200000; expect_no_key(action); // PREKEY logging cannot expire the check silently.
    }

    reset_session();
    force_burst_cutoff = true;
    assert(perform_burst(&jobs_to_test[2]) != ESP_OK);
    assert(key_writes == 1U && stopped == STOP_TIMER && levels[PA_POWER_GPIO] == 0);
    assert(adc_batches == adc_at_key && !sampling && !rf_ready);

    reset_session();
    cancel_delay = 10000;
    assert(perform_burst(&jobs_to_test[2]) != ESP_OK);
    assert(key_writes == 1U && stopped == STOP_OPERATOR && levels[PA_POWER_GPIO] == 0);
    assert(adc_batches == adc_at_key && !sampling && !rf_ready);

    reset_session();
    batch_delay = 3100000; // CPU0 preparation timer still cuts power during ADC work.
    expect_no_key(PA_ACTION_PA);
    assert(stopped == STOP_TIMER && !band.qualified);

    reset_session();
    sampling = true;
    assert(!session_permits_operation() && !lpf_prekey_is_fresh());
    sampling = false; sense_busy = true;
    assert(!session_permits_operation() && !lpf_prekey_is_fresh());
    sense_busy = false; band.sample_us = test_time;
    assert(lpf_prekey_is_fresh());
    test_time += LPF_FRESH_US;
    assert(!lpf_prekey_is_fresh() && session_permits_operation());

    puts("PA production-supervisor tests passed: latched idle, per-command fresh prechecks after preparation, sampler stopped before keying, no runtime ADC, invalid/slow/missing ADC fail closed, timer/operator cutoff.");
    return 0;
}
