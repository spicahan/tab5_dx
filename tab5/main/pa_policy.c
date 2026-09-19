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
        policy->armed = false;
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
    return policy != NULL && policy->armed && !policy->burst_in_progress &&
           now_ms >= policy->armed_at_ms &&
           (!policy->time_seen || now_ms >= policy->last_now_ms) &&
           now_ms - policy->armed_at_ms < PA_POLICY_ARM_MS;
}

uint32_t pa_policy_cooldown_remaining(const pa_policy_t *policy, uint64_t now_ms)
{
    if (policy == NULL || !policy->cooldown_valid) {
        return 0U;
    }
    if (now_ms < policy->completed_at_ms) {
        return PA_POLICY_COOLDOWN_MS;
    }
    const uint64_t elapsed = now_ms - policy->completed_at_ms;
    return elapsed >= PA_POLICY_COOLDOWN_MS ? 0U :
           PA_POLICY_COOLDOWN_MS - (uint32_t)elapsed;
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
        policy->armed = false;
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
    const bool expired = policy->armed && !pa_policy_is_armed(policy, now_ms);
    if (expired) {
        policy->armed = false;
    }

    if (command_is(line, length, "help")) {
        return (pa_decision_t){.action = PA_ACTION_HELP};
    }
    if (command_is(line, length, "status")) {
        return (pa_decision_t){.action = PA_ACTION_STATUS};
    }

    pa_action_t action = PA_ACTION_REJECTED;
    uint32_t duration = 0U;
    if (command_is(line, length, "arm 40m dummyload")) {
        action = PA_ACTION_ARM;
    } else if (command_is(line, length, "clearfault powercycled")) {
        action = PA_ACTION_CLEARFAULT;
    } else if (command_is(line, length, "pa") || command_is(line, length, "pa 100")) {
        action = PA_ACTION_PA;
        duration = 100U;
    } else if (command_is(line, length, "pa 250")) {
        action = PA_ACTION_PA;
        duration = 250U;
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
        policy->armed = false;
        return (pa_decision_t){.action = PA_ACTION_CLEARFAULT};
    }
    if (pa_policy_cooldown_remaining(policy, now_ms) != 0U) {
        return reject(policy, PA_REJECT_COOLDOWN);
    }
    if (action == PA_ACTION_ARM) {
        policy->armed = true;
        policy->armed_at_ms = now_ms;
        return (pa_decision_t){.action = PA_ACTION_ARM};
    }
    if (!policy->armed) {
        return reject(policy, expired ? PA_REJECT_ARM_EXPIRED : PA_REJECT_NOT_ARMED);
    }

    policy->armed = false;
    policy->burst_in_progress = true;
    return (pa_decision_t){.action = action, .duration_ms = duration};
}

void pa_policy_complete(pa_policy_t *policy, uint64_t now_ms)
{
    if (policy == NULL) {
        return;
    }
    policy->armed = false;
    if (policy->time_seen && now_ms < policy->last_now_ms) {
        // Do not shorten a cooldown if the caller supplies a stale timestamp.
        now_ms = policy->last_now_ms;
    }
    policy->last_now_ms = now_ms;
    policy->time_seen = true;
    if (policy->burst_in_progress) {
        policy->completed_at_ms = now_ms;
        policy->cooldown_valid = true;
        policy->burst_in_progress = false;
    }
}

const char *pa_policy_rejection_name(pa_rejection_t rejection)
{
    switch (rejection) {
    case PA_REJECT_NONE: return "none";
    case PA_REJECT_INVALID: return "invalid command (disarmed)";
    case PA_REJECT_NOT_ARMED: return "not armed";
    case PA_REJECT_ARM_EXPIRED: return "arm expired";
    case PA_REJECT_COOLDOWN: return "cooldown active";
    case PA_REJECT_BUSY: return "burst already in progress";
    case PA_REJECT_CLOCK: return "monotonic clock moved backwards (disarmed)";
    default: return "unknown rejection";
    }
}
