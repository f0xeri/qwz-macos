/* Native scan freshness is established by RX, never by reading the cache.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef QWZMAC_SCAN_OBSERVATION_H
#define QWZMAC_SCAN_OBSERVATION_H
#include <stdint.h>
struct qwzmac_scan_observation { uint32_t ticket; };
/* Returns 1 for the first real observation in this native scan, allowing
 * RSSI to start afresh rather than preserve a strongest-ever cached value.
 * Ordinary RX outside a native scan does not impersonate a native result. */
static inline int
qwzmac_scan_observe(struct qwzmac_scan_observation *seen, uint32_t active)
{
    int first;
    if (active == 0) return 0;
    first = seen->ticket != active;
    seen->ticket = active;
    return first;
}
static inline int
qwzmac_scan_observed(const struct qwzmac_scan_observation *seen, uint32_t ticket)
{
    return ticket != 0 && seen->ticket == ticket;
}
#endif
