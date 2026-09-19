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
    assert(result.action == action && result.rejection == rejection && result.duration_ms == duration);
}

static void qualify(pa_policy_t *state, uint64_t now)
{
    assert(pa_policy_set_lpf(state, true, now, now));
}

static void scan(pa_policy_t *state, uint64_t now)
{
    expect(state, "scan", now, PA_ACTION_SCAN, PA_REJECT_NONE, 0U);
    assert(!state->inhibited && !pa_policy_is_armed(state, now));
}

static void fresh(pa_policy_t *state, uint64_t now)
{
    pa_policy_init(state);
    qualify(state, now);
    assert(pa_policy_is_armed(state, now) && !state->burst_in_progress);
}

int main(void)
{
    pa_policy_t state;
    pa_policy_init(&state);
    assert(PA_POLICY_RX_HZ == 7074000U && PA_POLICY_SOURCE_HZ == 7075000U);
    assert(!state.inhibited && !state.fault && !state.burst_in_progress);
    assert(!pa_policy_is_armed(&state, 0U) && pa_policy_cooldown_remaining(&state, 0U) == 0U);
    expect(&state, "pa", 0U, PA_ACTION_REJECTED, PA_REJECT_LPF, 0U);
    qualify(&state, 1U);
    assert(!pa_policy_is_armed(&state, 1U)); // Rejection inhibits subsequent good samples.
    scan(&state, 2U);
    qualify(&state, 3U);
    expect(&state, " \t\r\nhelp\r\n", 4U, PA_ACTION_HELP, PA_REJECT_NONE, 0U);
    expect(&state, "status", 4U, PA_ACTION_STATUS, PA_REJECT_NONE, 0U);
    assert(pa_policy_is_armed(&state, 202U));
    assert(!pa_policy_is_armed(&state, 203U)); // Exact 200 ms freshness boundary.
    expect(&state, "leak", 203U, PA_ACTION_REJECTED, PA_REJECT_LPF, 0U);

    fresh(&state, 100U);
    assert(pa_policy_set_lpf(&state, false, 101U, 101U)); // Wrong/missing/ambiguous/error.
    assert(!pa_policy_is_armed(&state, 101U) && !state.inhibited);
    qualify(&state, 102U);
    assert(pa_policy_is_armed(&state, 102U) && !state.burst_in_progress);
    assert(pa_policy_set_lpf(&state, true, 102U, 302U)); // Old but correctly ordered snapshot.
    assert(!pa_policy_is_armed(&state, 302U));
    qualify(&state, 303U);
    expect(&state, "off", 304U, PA_ACTION_OFF, PA_REJECT_NONE, 0U);
    for (uint64_t now = 305U; now < 1000U; ++now) {
        qualify(&state, now);
        assert(!pa_policy_is_armed(&state, now));
    }
    scan(&state, 1000U);
    qualify(&state, 1001U);
    assert(pa_policy_is_armed(&state, 1001U));
    pa_policy_inhibit(&state);
    qualify(&state, 1002U);
    assert(!pa_policy_is_armed(&state, 1002U));
    expect(&state, "arm 40m dummyload", 1003U, PA_ACTION_SCAN, PA_REJECT_NONE, 0U);
    assert(!pa_policy_is_armed(&state, 1003U)); // Legacy text is no LPF bypass.
    assert(pa_policy_set_lpf(&state, false, 1004U, 1004U));
    expect(&state, "pa 10000", 1004U, PA_ACTION_REJECTED, PA_REJECT_LPF, 0U);

    fresh(&state, 2000U);
    pa_policy_set_fault(&state, true);
    qualify(&state, 2001U);
    assert(state.fault && state.inhibited && !pa_policy_is_armed(&state, 2001U));
    expect(&state, "scan", 2002U, PA_ACTION_REJECTED, PA_REJECT_FAULT, 0U);
    expect(&state, "arm 40m dummyload", 2003U, PA_ACTION_REJECTED, PA_REJECT_FAULT, 0U);
    expect(&state, "pa", 2004U, PA_ACTION_REJECTED, PA_REJECT_FAULT, 0U);
    expect(&state, "clearfault powercycled", 2005U, PA_ACTION_CLEARFAULT, PA_REJECT_NONE, 0U);
    assert(state.fault && state.inhibited); // No implicit persistent or policy fault clear.
    pa_policy_set_fault(&state, false);
    qualify(&state, 2006U);
    assert(!pa_policy_is_armed(&state, 2006U));
    scan(&state, 2007U);
    qualify(&state, 2008U);
    assert(pa_policy_is_armed(&state, 2008U));

    const char *valid[] = {"pa", "pa 100", "pa 250", "pa 10000", "leak", "leak 100", "leak 250"};
    const uint32_t durations[] = {100U, 100U, 250U, 10000U, 100U, 100U, 250U};
    for (size_t i = 0U; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        fresh(&state, 0U);
        expect(&state, valid[i], 1U, i < 4U ? PA_ACTION_PA : PA_ACTION_LEAK, PA_REJECT_NONE, durations[i]);
        assert(state.burst_in_progress && !pa_policy_is_armed(&state, 1U));
        qualify(&state, 2U);
        assert(!pa_policy_is_armed(&state, 2U));
        const uint64_t completed = durations[i] + 2U;
        pa_policy_complete(&state, completed);
        assert(!state.burst_in_progress && !state.inhibited);
        const uint32_t cooldown = durations[i] == 10000U ? 10000U : 5000U;
        assert(pa_policy_cooldown_remaining(&state, completed) == cooldown);
        pa_policy_complete(&state, completed + 1U);
        assert(state.completed_at_ms == completed); // Repeated idle cleanup preserves cooldown.
        qualify(&state, completed + cooldown - 1U);
        assert(!pa_policy_is_armed(&state, completed + cooldown - 1U));
        assert(pa_policy_cooldown_remaining(&state, completed + cooldown - 1U) == 1U);
        assert(pa_policy_is_armed(&state, completed + cooldown) && !state.burst_in_progress);
    }

    fresh(&state, 0U);
    expect(&state, " \tpa 10000\r\n", 1U, PA_ACTION_PA, PA_REJECT_NONE, 10000U);
    expect(&state, "pa", 2U, PA_ACTION_REJECTED, PA_REJECT_BUSY, 0U);
    expect(&state, "scan", 3U, PA_ACTION_REJECTED, PA_REJECT_BUSY, 0U);
    expect(&state, "clearfault powercycled", 4U, PA_ACTION_REJECTED, PA_REJECT_BUSY, 0U);
    expect(&state, "off", 5U, PA_ACTION_OFF, PA_REJECT_NONE, 0U);
    assert(state.burst_in_progress && state.inhibited);
    pa_policy_complete(&state, 6U);
    assert(pa_policy_cooldown_remaining(&state, 6U) == 10000U);
    expect(&state, "clearfault powercycled", 7U, PA_ACTION_CLEARFAULT, PA_REJECT_NONE, 0U);
    assert(state.inhibited && state.completed_at_ms == 6U);
    scan(&state, 8U); // SCAN during cooldown cannot shorten it.
    qualify(&state, 9U);
    assert(!pa_policy_is_armed(&state, 9U));
    expect(&state, "pa", 10U, PA_ACTION_REJECTED, PA_REJECT_COOLDOWN, 0U);
    qualify(&state, 10006U);
    assert(!pa_policy_is_armed(&state, 10006U));
    scan(&state, 10007U);
    qualify(&state, 10008U);
    assert(pa_policy_is_armed(&state, 10008U));

    const char *invalid[] = {
        "", " \r\n", "arm", "arm 20m dummyload", "arm 40m", "arm 40m antenna",
        "arm 40m dummyload junk", "arm  40m dummyload", "pa 0", "pa -1", "pa +100",
        "pa 99", "pa 101", "pa 251", "pa 1000", "pa 10001", "pa 4294967296",
        "pa 18446744073709551616", "pa 0x64", "pa 0100", "pa 100.0", "pa 100junk",
        "pa 100 250", "pa\npa", "pa\t100", "leak 0", "leak 1000", "leak 10000",
        "PA", "OFF", "status extra", "help;pa", "off\npa", "clearfault",
        "clearfault rebooted", "clearfault powercycled extra", "clearfault  powercycled",
        "clearfault POWERCYCLED", "scan extra", "SCAN",
    };
    for (size_t i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        fresh(&state, 0U);
        expect(&state, invalid[i], 1U, PA_ACTION_REJECTED, PA_REJECT_INVALID, 0U);
        qualify(&state, 2U);
        assert(state.inhibited && !pa_policy_is_armed(&state, 2U) && !state.burst_in_progress);
    }
    char overlong[PA_POLICY_MAX_LINE + 2U];
    memset(overlong, ' ', sizeof(overlong) - 1U);
    overlong[sizeof(overlong) - 1U] = '\0';
    fresh(&state, 0U);
    expect(&state, overlong, 1U, PA_ACTION_REJECTED, PA_REJECT_INVALID, 0U);

    fresh(&state, 100U);
    assert(!pa_policy_set_lpf(&state, true, 99U, 101U)); // Reordered sample.
    assert(state.inhibited && !pa_policy_is_armed(&state, 101U));
    scan(&state, 102U);
    assert(!pa_policy_set_lpf(&state, true, 104U, 103U)); // Future sample.
    scan(&state, 104U);
    assert(!pa_policy_set_lpf(&state, true, 103U, 103U)); // Backwards current time.
    expect(&state, "pa", 103U, PA_ACTION_REJECTED, PA_REJECT_CLOCK, 0U);
    expect(&state, "off", 1U, PA_ACTION_OFF, PA_REJECT_NONE, 0U);
    assert(state.last_now_ms == 104U);
    scan(&state, 105U);
    qualify(&state, 106U);
    expect(&state, "pa", 107U, PA_ACTION_PA, PA_REJECT_NONE, 100U);
    pa_policy_complete(&state, 1U);
    assert(state.completed_at_ms == 107U && state.inhibited);
    assert(pa_policy_cooldown_remaining(&state, 1U) == 5000U);
    fresh(&state, UINT64_MAX - 100U);
    expect(&state, "pa 10000", UINT64_MAX, PA_ACTION_PA, PA_REJECT_NONE, 10000U);
    pa_policy_complete(&state, UINT64_MAX);
    expect(&state, "scan", 0U, PA_ACTION_REJECTED, PA_REJECT_CLOCK, 0U);
    assert(pa_policy_cooldown_remaining(&state, UINT64_MAX) == 10000U);

    fresh(&state, 0U);
    expect(&state, NULL, 1U, PA_ACTION_REJECTED, PA_REJECT_INVALID, 0U);
    expect(NULL, "pa", 0U, PA_ACTION_REJECTED, PA_REJECT_INVALID, 0U);
    pa_policy_init(NULL);
    pa_policy_inhibit(NULL);
    pa_policy_set_fault(NULL, true);
    pa_policy_complete(NULL, 0U);
    assert(!pa_policy_set_lpf(NULL, true, 0U, 0U));
    assert(!pa_policy_is_armed(NULL, 0U) && pa_policy_cooldown_remaining(NULL, 0U) == 0U);
    assert(strcmp(pa_policy_rejection_name(PA_REJECT_LPF), "40m LPF not qualified or stale") == 0);
    puts("PA policy tests passed: fresh-LPF auto-arm without auto-TX, sticky inhibit, fault gating, scan/legacy alias, bounded durations, cooldown, timestamp/invalid input handling.");
    return 0;
}
