// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PA_POLICY_RX_HZ UINT32_C(7074000)
#define PA_POLICY_SOURCE_HZ UINT32_C(7075000)
#define PA_POLICY_LPF_FRESH_MS UINT32_C(200)
#define PA_POLICY_COOLDOWN_MS UINT32_C(5000)
#define PA_POLICY_LONG_COOLDOWN_MS UINT32_C(10000)
#define PA_POLICY_MAX_LINE 63U

typedef enum {
    PA_ACTION_REJECTED = 0,
    PA_ACTION_HELP,
    PA_ACTION_STATUS,
    PA_ACTION_OFF,
    PA_ACTION_CLEARFAULT,
    PA_ACTION_SCAN,
    PA_ACTION_PA,
    PA_ACTION_LEAK,
} pa_action_t;

typedef enum {
    PA_REJECT_NONE = 0,
    PA_REJECT_INVALID,
    PA_REJECT_NOT_ARMED,
    PA_REJECT_COOLDOWN,
    PA_REJECT_BUSY,
    PA_REJECT_CLOCK,
    PA_REJECT_FAULT,
    PA_REJECT_LPF,
} pa_rejection_t;

typedef struct {
    bool inhibited;
    bool fault;
    bool qualified_40m;
    bool lpf_sample_seen;
    bool burst_in_progress;
    bool cooldown_valid;
    bool time_seen;
    uint32_t accepted_duration_ms;
    uint32_t cooldown_ms;
    uint64_t lpf_sample_ms;
    uint64_t completed_at_ms;
    uint64_t last_now_ms;
} pa_policy_t;

typedef struct {
    pa_action_t action;
    pa_rejection_t rejection;
    uint32_t duration_ms;
} pa_decision_t;

/* Pure command policy: these functions never touch power, GPIOs, or clocks.
 * init enables automatic qualification but boots disarmed (no LPF evidence).
 * Commands are case-sensitive, trim outer ASCII
 * whitespace, and otherwise require exact spellings. Maximum line length is
 * PA_POLICY_MAX_LINE characters, excluding NUL. The caller must provide a
 * NUL-terminated buffer and reject embedded NULs/overlong input at ingress.
 *
 * Automatic arming means fresh (<200 ms), externally qualified 40m LPF evidence,
 * no shutdown fault, no inhibit, no active burst and no cooldown. It never
 * returns a TX action. The hardware layer owns stable ADC classification,
 * continuous monitoring, immediate pre-key verification and physical abort.
 * `scan` clears inhibit and old LPF evidence, requesting fresh qualification;
 * the deprecated exact `arm 40m dummyload` text is a SCAN alias, never bypass.
 * SCAN is allowed during cooldown but cannot arm until cooldown has finished.
 * `pa [100|250|10000]` or `leak [100|250]` must still be explicitly requested;
 * omitted duration means 100 ms. Acceptance marks a burst in progress BEFORE
 * returning. The caller executes at most one burst, safely ends RF, then calls
 * complete (including on preparation errors). Cooldown is 5 s for short jobs,
 * 10 s for a 10000 ms job, measured from completion. No thermal claim is made.
 * Invalid/rejected commands, OFF and cancel inhibit automatic rearming until
 * another accepted SCAN. Help/status leave qualification and inhibit unchanged.
 * `clearfault powercycled` returns CLEARFAULT only while no burst is active;
 * it inhibits, clears qualification, and preserves cooldown. It does NOT clear
 * the policy fault flag. The caller owns hardware power-cycle
 * confirmation semantics and the persistent shutdown-fault latch.
 *
 * OFF always disarms, including if time moved backwards, but does not erase
 * burst/cooldown bookkeeping: the caller must stop hardware then complete.
 * Timer values are monotonic milliseconds. A backwards timer fails closed.
 * Console ingress must also flush/reject queued commands: this policy alone
 * cannot distinguish a fresh human command from a buffered command stream.
 */
void pa_policy_init(pa_policy_t *policy);
/* qualified_40m must already include the hardware classifier's stable-window
 * checks; false represents wrong/missing/ambiguous/error LPF. Returns false for
 * NULL, future/out-of-order timestamps or backwards now (and inhibits). Valid
 * bad/stale samples disqualify without removing or adding a manual inhibit.
 * These APIs are not thread-safe: serialize updates with submit/complete.
 */
bool pa_policy_set_lpf(pa_policy_t *policy, bool qualified_40m,
                       uint64_t sample_ms, uint64_t now_ms);
/* A true fault inhibits and clears qualification. Clearing the flag never
 * clears an existing inhibit; a fresh SCAN is still required after recovery.
 * Neither operation writes or clears the caller's persistent fault storage.
 */
void pa_policy_set_fault(pa_policy_t *policy, bool fault);
void pa_policy_inhibit(pa_policy_t *policy);
pa_decision_t pa_policy_submit(pa_policy_t *policy, const char *line, uint64_t now_ms);
void pa_policy_complete(pa_policy_t *policy, uint64_t now_ms);
bool pa_policy_is_armed(const pa_policy_t *policy, uint64_t now_ms);
uint32_t pa_policy_cooldown_remaining(const pa_policy_t *policy, uint64_t now_ms);
const char *pa_policy_rejection_name(pa_rejection_t rejection);

#ifdef __cplusplus
}
#endif
