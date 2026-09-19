// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PA_POLICY_RX_HZ UINT32_C(7074000)
#define PA_POLICY_SOURCE_HZ UINT32_C(7075000)
#define PA_POLICY_ARM_MS UINT32_C(30000)
#define PA_POLICY_COOLDOWN_MS UINT32_C(5000)
#define PA_POLICY_MAX_LINE 63U

typedef enum {
    PA_ACTION_REJECTED = 0,
    PA_ACTION_HELP,
    PA_ACTION_STATUS,
    PA_ACTION_OFF,
    PA_ACTION_CLEARFAULT,
    PA_ACTION_ARM,
    PA_ACTION_PA,
    PA_ACTION_LEAK,
} pa_action_t;

typedef enum {
    PA_REJECT_NONE = 0,
    PA_REJECT_INVALID,
    PA_REJECT_NOT_ARMED,
    PA_REJECT_ARM_EXPIRED,
    PA_REJECT_COOLDOWN,
    PA_REJECT_BUSY,
    PA_REJECT_CLOCK,
} pa_rejection_t;

typedef struct {
    bool armed;
    bool burst_in_progress;
    bool cooldown_valid;
    bool time_seen;
    uint64_t armed_at_ms;
    uint64_t completed_at_ms;
    uint64_t last_now_ms;
} pa_policy_t;

typedef struct {
    pa_action_t action;
    pa_rejection_t rejection;
    uint32_t duration_ms;
} pa_decision_t;

/* Pure command policy: these functions never touch power, GPIOs, or clocks.
 * init boots disarmed. Commands are case-sensitive, trim outer ASCII
 * whitespace, and otherwise require exact spellings. Maximum line length is
 * PA_POLICY_MAX_LINE characters, excluding NUL. The caller must provide a
 * NUL-terminated buffer and reject embedded NULs/overlong input at ingress.
 *
 * `arm 40m dummyload` grants one 30-second token; arming itself must not power
 * the RF board. `pa`, `pa 100`, `pa 250`, `leak`, `leak 100`, `leak 250` consume
 * that token BEFORE returning an action and mark a burst in progress. The
 * caller must execute at most that one burst, clean up all RF hardware, then
 * call complete (also on preparation/measurement errors). No new arm or burst
 * is accepted until >=5 seconds after completion. Invalid/rejected commands
 * disarm; help/status preserve a still-valid token without extending expiry.
 * `clearfault powercycled` returns CLEARFAULT only while no burst is active;
 * it disarms and preserves cooldown. The caller owns hardware power-cycle
 * confirmation semantics and the persistent shutdown-fault latch.
 *
 * OFF always disarms, including if time moved backwards, but does not erase
 * burst/cooldown bookkeeping: the caller must stop hardware then complete.
 * Timer values are monotonic milliseconds. A backwards timer fails closed.
 * Console ingress must also flush/reject queued commands: this policy alone
 * cannot distinguish a fresh human command from a buffered command stream.
 */
void pa_policy_init(pa_policy_t *policy);
pa_decision_t pa_policy_submit(pa_policy_t *policy, const char *line, uint64_t now_ms);
void pa_policy_complete(pa_policy_t *policy, uint64_t now_ms);
bool pa_policy_is_armed(const pa_policy_t *policy, uint64_t now_ms);
uint32_t pa_policy_cooldown_remaining(const pa_policy_t *policy, uint64_t now_ms);
const char *pa_policy_rejection_name(pa_rejection_t rejection);

#ifdef __cplusplus
}
#endif
