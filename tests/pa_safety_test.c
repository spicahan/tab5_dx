// SPDX-License-Identifier: MIT
// Run from the repository root, placing the executable in a temporary path:
// cc -std=c11 -Wall -Wextra -Werror -Itab5/main/include \
//    tests/pa_safety_test.c tab5/main/pa_policy.c -o /tmp/pa_safety_test

#include "pa_safety.h"
#include "pa_policy.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

static void check_truth_table(void)
{
    // Index bits: pending, fault, powered, known-off (most to least significant).
    static const bool expected[16] = {
        false, false, false, false, false, false, false, false,
        true,  true,  false, true,  false, false, false, false,
    };
    for (unsigned bits = 0U; bits < 16U; ++bits) {
        const bool pending = (bits & 8U) != 0U;
        const bool fault = (bits & 4U) != 0U;
        const bool powered = (bits & 2U) != 0U;
        const bool known_off = (bits & 1U) != 0U;
        assert(pa_shutdown_may_clear(pending, fault, powered, known_off) ==
               expected[bits]);
    }
}

static void simulate_cleanup(bool *persistent_pending, bool fault_latched,
                             bool power_started, bool clk0_off_known)
{
    if (pa_shutdown_may_clear(*persistent_pending, fault_latched,
                              power_started, clk0_off_known)) {
        *persistent_pending = false;
    }
}

static void check_inherited_fault_off_reset(void)
{
    // This models persisted state, not the NVS driver's flash implementation.
    bool persistent_pending = true;
    for (unsigned reboot = 0U; reboot < 3U; ++reboot) {
        pa_policy_t policy;
        pa_policy_init(&policy);
        pa_policy_set_fault(&policy, persistent_pending);
        assert(pa_policy_submit(&policy, "scan", 1U).rejection == PA_REJECT_FAULT);
        assert(pa_policy_submit(&policy, "off", 2U).action == PA_ACTION_OFF);

        // The current boot has not asserted G48; that cannot clear old evidence.
        simulate_cleanup(&persistent_pending, policy.fault, false, false);
        assert(persistent_pending);
        assert(policy.fault);
        assert(!pa_policy_is_armed(&policy, 2U));

        // Even a later known-off assertion must not silently acknowledge a
        // previously latched fault. Operator-confirmed recovery remains separate.
        simulate_cleanup(&persistent_pending, policy.fault, false, true);
        assert(persistent_pending);
    }
}

static void check_keyed_failure_and_recovery_policy(void)
{
    bool persistent_pending = true; // Committed before any powered operation.
    simulate_cleanup(&persistent_pending, false, true, false);
    assert(persistent_pending); // Powered, unverified shutdown stays pending.

    pa_policy_t policy;
    pa_policy_init(&policy);
    pa_policy_set_fault(&policy, persistent_pending);
    assert(pa_policy_submit(&policy, "pa 10000", 1U).rejection == PA_REJECT_FAULT);
    const pa_decision_t ack = pa_policy_submit(&policy, "clearfault powercycled", 2U);
    assert(ack.action == PA_ACTION_CLEARFAULT);
    assert(policy.fault && policy.inhibited);
    assert(persistent_pending); // Parsing acknowledgement never writes storage.

    // Model successful explicit recovery after the operator power-cycle: this
    // is intentionally not ordinary cleanup, and it must not arm or transmit.
    persistent_pending = false;
    pa_policy_set_fault(&policy, persistent_pending);
    assert(!pa_policy_is_armed(&policy, 3U));
    assert(pa_policy_submit(&policy, "scan", 3U).action == PA_ACTION_SCAN);
    assert(pa_policy_set_lpf(&policy, true, 4U, 4U));
    assert(pa_policy_is_armed(&policy, 4U));
    assert(!policy.burst_in_progress); // Qualification alone never starts TX.
    assert(pa_policy_submit(&policy, "pa 100", 5U).action == PA_ACTION_PA);

    persistent_pending = true;
    simulate_cleanup(&persistent_pending, false, true, true);
    assert(!persistent_pending); // Verified normal shutdown may clear pending.
    pa_policy_complete(&policy, 105U);
    assert(!pa_policy_is_armed(&policy, 105U)); // Cooldown survives cleanup.
}

static void check_other_cleanup_cases(void)
{
    bool persistent_pending = true;
    simulate_cleanup(&persistent_pending, false, false, false);
    assert(!persistent_pending); // Current clean job failed before power-on.

    persistent_pending = true;
    simulate_cleanup(&persistent_pending, true, true, true);
    assert(persistent_pending); // Storage/latched faults remain conservative.

    persistent_pending = false;
    simulate_cleanup(&persistent_pending, false, true, false);
    assert(!persistent_pending); // Cleanup never invents or writes a marker.
}

int main(void)
{
    check_truth_table();
    check_inherited_fault_off_reset();
    check_keyed_failure_and_recovery_policy();
    check_other_cleanup_cases();
    puts("PA safety tests passed: 16 cleanup cases, inherited-fault OFF/reset, "
         "unverified shutdown, explicit recovery policy, normal shutdown and cooldown.");
    return 0;
}
