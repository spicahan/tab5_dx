// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Whether ordinary RF-session cleanup may clear the persistent pending marker.
 *
 * A pending marker may be cleared only when no fault has been latched and this
 * session either never asserted RF power or verified CLK0 disabled. In
 * particular, an inherited boot fault must not be cleared merely because this
 * boot has not yet powered the board. OFF followed by reset is not a physical
 * power-cycle acknowledgement. Explicit operator-confirmed recovery is a
 * separate operation and must not use this predicate to bypass the fault.
 *
 * This pure helper neither writes storage nor controls hardware. The caller
 * owns synchronization, the provenance of these assertions, and fail-closed
 * handling when persistence fails.
 */
static inline bool pa_shutdown_may_clear(bool marker_pending,
                                         bool fault_latched,
                                         bool power_started,
                                         bool clk0_off_known)
{
    return marker_pending && !fault_latched &&
           (!power_started || clk0_off_known);
}

#ifdef __cplusplus
}
#endif
