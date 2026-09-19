// SPDX-License-Identifier: MIT

#include "pa_policy.h"

#include <stddef.h>
#include <string.h>

static bool outer_space(char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
           value == '\v' || value == '\f';
}

static bool command_is(const char *start, size_t length, const char *expected)
{
    return strlen(expected) == length && memcmp(start, expected, length) == 0;
}

static pa_decision_t reject(pa_policy_t *policy, pa_rejection_t reason)
{
    if (policy != NULL) {
        pa_policy_inhibit(policy);
    }
    return (pa_decision_t){.action = PA_ACTION_REJECTED, .rejection = reason};
}

void pa_policy_init(pa_policy_t *policy)
{
    if (policy != NULL) {
        memset(policy, 0, sizeof(*policy));
    }
}

bool pa_policy_is_armed(const pa_policy_t *policy, uint64_t now_ms)
{
    return policy != NULL && !policy->inhibited && !policy->fault &&
           policy->qualified_40m && policy->lpf_sample_seen &&
           !policy->burst_in_progress &&
           now_ms >= policy->lpf_sample_ms &&
           (!policy->time_seen || now_ms >= policy->last_now_ms) &&
           pa_policy_cooldown_remaining(policy, now_ms) == 0U;
}

void pa_policy_inhibit(pa_policy_t *policy)
{
    if (policy != NULL) {
        policy->inhibited = true;
        policy->qualified_40m = false;
    }
}

void pa_policy_set_fault(pa_policy_t *policy, bool fault)
{
    if (policy != NULL) {
        policy->fault = fault;
        if (fault) {
            pa_policy_inhibit(policy);
        }
    }
}

bool pa_policy_set_lpf(pa_policy_t *policy, bool qualified_40m,
                       uint64_t sample_ms, uint64_t now_ms)
{
    if (policy == NULL) {
        return false;
    }
    if ((policy->time_seen && now_ms < policy->last_now_ms) ||
        sample_ms > now_ms ||
        (policy->lpf_sample_seen && sample_ms < policy->lpf_sample_ms)) {
        pa_policy_inhibit(policy);
        return false;
    }
    policy->last_now_ms = now_ms;
    policy->time_seen = true;
    policy->lpf_sample_seen = true;
    policy->lpf_sample_ms = sample_ms;
    // Readiness is latched between explicit acquisitions. The hardware layer
    // checks sample freshness while qualifying a scan or the next pre-key check.
    policy->qualified_40m = qualified_40m;
    return true;
}

uint32_t pa_policy_cooldown_remaining(const pa_policy_t *policy, uint64_t now_ms)
{
    if (policy == NULL || !policy->cooldown_valid) {
        return 0U;
    }
    if (now_ms < policy->completed_at_ms) {
        return policy->cooldown_ms;
    }
    const uint64_t elapsed = now_ms - policy->completed_at_ms;
    return elapsed >= policy->cooldown_ms ? 0U :
           policy->cooldown_ms - (uint32_t)elapsed;
}

pa_decision_t pa_policy_submit(pa_policy_t *policy, const char *line, uint64_t now_ms)
{
    if (policy == NULL || line == NULL) {
        return reject(policy, PA_REJECT_INVALID);
    }

    size_t length = 0U;
    while (length <= PA_POLICY_MAX_LINE && line[length] != '\0') {
        ++length;
    }
    if (length > PA_POLICY_MAX_LINE) {
        return reject(policy, PA_REJECT_INVALID);
    }
    while (length != 0U && outer_space(*line)) {
        ++line;
        --length;
    }
    while (length != 0U && outer_space(line[length - 1U])) {
        --length;
    }

    // An OFF command is effective even if the caller's clock is faulty.
    if (command_is(line, length, "off")) {
        pa_policy_inhibit(policy);
        if (!policy->time_seen || now_ms >= policy->last_now_ms) {
            policy->last_now_ms = now_ms;
            policy->time_seen = true;
        }
        return (pa_decision_t){.action = PA_ACTION_OFF};
    }
    if (policy->time_seen && now_ms < policy->last_now_ms) {
        return reject(policy, PA_REJECT_CLOCK);
    }
    policy->last_now_ms = now_ms;
    policy->time_seen = true;
    if (command_is(line, length, "help")) {
        return (pa_decision_t){.action = PA_ACTION_HELP};
    }
    if (command_is(line, length, "status")) {
        return (pa_decision_t){.action = PA_ACTION_STATUS};
    }

    pa_action_t action = PA_ACTION_REJECTED;
    uint32_t duration = 0U;
    if (command_is(line, length, "scan") || command_is(line, length, "arm 40m dummyload")) {
        action = PA_ACTION_SCAN;
    } else if (command_is(line, length, "clearfault powercycled")) {
        action = PA_ACTION_CLEARFAULT;
    } else if (command_is(line, length, "pa") || command_is(line, length, "pa 100")) {
        action = PA_ACTION_PA;
        duration = 100U;
    } else if (command_is(line, length, "pa 250")) {
        action = PA_ACTION_PA;
        duration = 250U;
    } else if (command_is(line, length, "pa 10000")) {
        action = PA_ACTION_PA;
        duration = 10000U;
    } else if (command_is(line, length, "leak") || command_is(line, length, "leak 100")) {
        action = PA_ACTION_LEAK;
        duration = 100U;
    } else if (command_is(line, length, "leak 250")) {
        action = PA_ACTION_LEAK;
        duration = 250U;
    } else {
        return reject(policy, PA_REJECT_INVALID);
    }

    if (policy->burst_in_progress) {
        return reject(policy, PA_REJECT_BUSY);
    }
    if (action == PA_ACTION_CLEARFAULT) {
        pa_policy_inhibit(policy);
        return (pa_decision_t){.action = PA_ACTION_CLEARFAULT};
    }
    if (policy->fault) {
        return reject(policy, PA_REJECT_FAULT);
    }
    if (action == PA_ACTION_SCAN) {
        policy->inhibited = false;
        policy->qualified_40m = false;
        return (pa_decision_t){.action = PA_ACTION_SCAN};
    }
    if (pa_policy_cooldown_remaining(policy, now_ms) != 0U) {
        return reject(policy, PA_REJECT_COOLDOWN);
    }
    if (policy->inhibited) {
        return reject(policy, PA_REJECT_NOT_ARMED);
    }
    if (!pa_policy_is_armed(policy, now_ms)) {
        return reject(policy, PA_REJECT_LPF);
    }

    policy->burst_in_progress = true;
    policy->accepted_duration_ms = duration;
    return (pa_decision_t){.action = action, .duration_ms = duration};
}

void pa_policy_complete(pa_policy_t *policy, uint64_t now_ms)
{
    if (policy == NULL) {
        return;
    }
    if (policy->time_seen && now_ms < policy->last_now_ms) {
        // Do not shorten a cooldown if the caller supplies a stale timestamp.
        now_ms = policy->last_now_ms;
        pa_policy_inhibit(policy);
    }
    policy->last_now_ms = now_ms;
    policy->time_seen = true;
    if (policy->burst_in_progress) {
        policy->completed_at_ms = now_ms;
        policy->cooldown_ms = policy->accepted_duration_ms == 10000U ?
                              PA_POLICY_LONG_COOLDOWN_MS : PA_POLICY_COOLDOWN_MS;
        policy->cooldown_valid = true;
        policy->burst_in_progress = false;
        policy->accepted_duration_ms = 0U;
    }
}

const char *pa_policy_rejection_name(pa_rejection_t rejection)
{
    switch (rejection) {
    case PA_REJECT_NONE: return "none";
    case PA_REJECT_INVALID: return "invalid command (inhibited; scan required)";
    case PA_REJECT_NOT_ARMED: return "not armed (inhibited; scan required)";
    case PA_REJECT_COOLDOWN: return "cooldown active";
    case PA_REJECT_BUSY: return "burst already in progress";
    case PA_REJECT_CLOCK: return "monotonic clock moved backwards (inhibited)";
    case PA_REJECT_FAULT: return "shutdown fault latched";
    case PA_REJECT_LPF: return "40m LPF not qualified";
    default: return "unknown rejection";
    }
}
