// SPDX-License-Identifier: MIT
// cc -std=c11 -Wall -Wextra -Werror -Itab5/main/include \
//    tests/pa_policy_test.c tab5/main/pa_policy.c -o /tmp/pa_policy_test

#include "pa_policy.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void expect(pa_policy_t *state, const char *line, uint64_t now,
                   pa_action_t action, pa_rejection_t rejection, uint32_t duration)
{
    const pa_decision_t result = pa_policy_submit(state, line, now);
    assert(result.action == action);
    assert(result.rejection == rejection);
    assert(result.duration_ms == duration);
}

static void arm(pa_policy_t *state, uint64_t now)
{
    expect(state, "arm 40m dummyload", now, PA_ACTION_ARM, PA_REJECT_NONE, 0U);
    assert(pa_policy_is_armed(state, now));
    assert(!state->burst_in_progress);
}

int main(void)
{
    pa_policy_t state;
    pa_policy_init(&state);
    assert(PA_POLICY_RX_HZ == 7074000U && PA_POLICY_SOURCE_HZ == 7075000U);
    assert(!pa_policy_is_armed(&state, 0U));
    assert(!state.burst_in_progress);
    assert(pa_policy_cooldown_remaining(&state, 0U) == 0U);
    expect(&state, "pa", 0U, PA_ACTION_REJECTED, PA_REJECT_NOT_ARMED, 0U);
    expect(&state, "leak", 0U, PA_ACTION_REJECTED, PA_REJECT_NOT_ARMED, 0U);
    expect(&state, " \t\r\nhelp\r\n", 1U, PA_ACTION_HELP, PA_REJECT_NONE, 0U);
    expect(&state, "status", 1U, PA_ACTION_STATUS, PA_REJECT_NONE, 0U);

    arm(&state, 100U);
    expect(&state, "help", 101U, PA_ACTION_HELP, PA_REJECT_NONE, 0U);
    expect(&state, "status", 102U, PA_ACTION_STATUS, PA_REJECT_NONE, 0U);
    assert(state.armed_at_ms == 100U);
    assert(pa_policy_is_armed(&state, 30099U));
    assert(!pa_policy_is_armed(&state, 30100U));
    expect(&state, "pa", 30100U, PA_ACTION_REJECTED, PA_REJECT_ARM_EXPIRED, 0U);

    arm(&state, 31000U);
    expect(&state, " \tpa 250\r\n", 31001U, PA_ACTION_PA, PA_REJECT_NONE, 250U);
    assert(!state.armed && state.burst_in_progress);
    expect(&state, "pa", 31002U, PA_ACTION_REJECTED, PA_REJECT_BUSY, 0U);
    expect(&state, "arm 40m dummyload", 31003U, PA_ACTION_REJECTED, PA_REJECT_BUSY, 0U);
    expect(&state, "clearfault powercycled", 31003U, PA_ACTION_REJECTED, PA_REJECT_BUSY, 0U);
    expect(&state, "off", 31004U, PA_ACTION_OFF, PA_REJECT_NONE, 0U);
    assert(state.burst_in_progress);
    pa_policy_complete(&state, 31251U);
    assert(!state.burst_in_progress && !state.armed);
    assert(pa_policy_cooldown_remaining(&state, 31251U) == 5000U);
    expect(&state, "clearfault powercycled", 31252U, PA_ACTION_CLEARFAULT, PA_REJECT_NONE, 0U);
    assert(!state.armed && state.completed_at_ms == 31251U);
    assert(pa_policy_cooldown_remaining(&state, 31252U) == 4999U);
    expect(&state, "arm 40m dummyload", 36250U, PA_ACTION_REJECTED, PA_REJECT_COOLDOWN, 0U);
    assert(pa_policy_cooldown_remaining(&state, 36250U) == 1U);
    expect(&state, "off", 36250U, PA_ACTION_OFF, PA_REJECT_NONE, 0U);
    assert(pa_policy_cooldown_remaining(&state, 36250U) == 1U);
    arm(&state, 36251U);
    expect(&state, "leak", 36252U, PA_ACTION_LEAK, PA_REJECT_NONE, 100U);
    pa_policy_complete(&state, 36352U);
    expect(&state, "leak 100", 41352U, PA_ACTION_REJECTED, PA_REJECT_NOT_ARMED, 0U);

    const char *valid[] = {"pa", "pa 100", "pa 250", "leak", "leak 100", "leak 250"};
    for (size_t index = 0U; index < sizeof(valid) / sizeof(valid[0]); ++index) {
        pa_policy_init(&state);
        arm(&state, 0U);
        expect(&state, valid[index], 1U,
               index < 3U ? PA_ACTION_PA : PA_ACTION_LEAK,
               PA_REJECT_NONE, index % 3U == 2U ? 250U : 100U);
        pa_policy_complete(&state, 300U);
        pa_policy_complete(&state, 400U); // Repeated idle cleanup cannot clear/restart cooldown.
        assert(state.completed_at_ms == 300U);
        expect(&state, valid[index], 5300U, PA_ACTION_REJECTED, PA_REJECT_NOT_ARMED, 0U);
    }

    const char *invalid[] = {
        "", " \r\n", "arm", "arm 20m dummyload", "arm 40m", "arm 40m antenna",
        "arm 40m dummyload junk", "arm  40m dummyload", "pa 0", "pa -1", "pa +100",
        "pa 99", "pa 101", "pa 251", "pa 1000", "pa 4294967296", "pa 18446744073709551616",
        "pa 0x64", "pa 0100", "pa 100.0", "pa 100junk", "pa 100 250", "pa\npa", "pa\t100",
        "leak 0", "leak 1000", "PA", "OFF", "status extra", "help;pa", "off\npa",
        "clearfault", "clearfault rebooted", "clearfault powercycled extra",
        "clearfault  powercycled", "clearfault POWERCYCLED",
    };
    for (size_t index = 0U; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        pa_policy_init(&state);
        arm(&state, 0U);
        expect(&state, invalid[index], 1U, PA_ACTION_REJECTED, PA_REJECT_INVALID, 0U);
        assert(!state.armed && !state.burst_in_progress);
    }
    char overlong[PA_POLICY_MAX_LINE + 2U];
    memset(overlong, ' ', sizeof(overlong) - 1U);
    overlong[sizeof(overlong) - 1U] = '\0';
    pa_policy_init(&state);
    arm(&state, 0U);
    expect(&state, overlong, 1U, PA_ACTION_REJECTED, PA_REJECT_INVALID, 0U);

    pa_policy_init(&state);
    arm(&state, 100U);
    expect(&state, "pa", 99U, PA_ACTION_REJECTED, PA_REJECT_CLOCK, 0U);
    assert(!state.armed);
    expect(&state, "off", 98U, PA_ACTION_OFF, PA_REJECT_NONE, 0U);
    assert(state.last_now_ms == 100U);
    arm(&state, 101U);
    expect(&state, "pa", 102U, PA_ACTION_PA, PA_REJECT_NONE, 100U);
    pa_policy_complete(&state, 1U);
    assert(state.completed_at_ms == 102U);
    assert(pa_policy_cooldown_remaining(&state, 1U) == 5000U);

    // No timestamp addition is used: expiry/cooldown arithmetic cannot wrap.
    pa_policy_init(&state);
    arm(&state, UINT64_MAX - 100U);
    expect(&state, "pa", UINT64_MAX, PA_ACTION_PA, PA_REJECT_NONE, 100U);
    pa_policy_complete(&state, UINT64_MAX);
    expect(&state, "arm 40m dummyload", 0U, PA_ACTION_REJECTED, PA_REJECT_CLOCK, 0U);
    assert(pa_policy_cooldown_remaining(&state, UINT64_MAX) == 5000U);

    pa_policy_init(&state);
    arm(&state, 0U);
    expect(&state, "clearfault powercycled", 1U, PA_ACTION_CLEARFAULT, PA_REJECT_NONE, 0U);
    assert(!state.armed && !state.burst_in_progress && !state.cooldown_valid);
    arm(&state, 1U);
    expect(&state, "off", 1U, PA_ACTION_OFF, PA_REJECT_NONE, 0U);
    assert(!state.armed);
    expect(&state, NULL, 2U, PA_ACTION_REJECTED, PA_REJECT_INVALID, 0U);
    expect(NULL, "pa", 0U, PA_ACTION_REJECTED, PA_REJECT_INVALID, 0U);
    pa_policy_init(NULL);
    pa_policy_complete(NULL, 0U);
    assert(!pa_policy_is_armed(NULL, 0U));
    assert(pa_policy_cooldown_remaining(NULL, 0U) == 0U);
    assert(strcmp(pa_policy_rejection_name(PA_REJECT_ARM_EXPIRED), "arm expired") == 0);
    puts("PA policy tests passed: disarmed boot, exact commands, single-use arm, expiry, cooldown, invalid/overflow, monotonic clock.");
    return 0;
}
