/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * The decision behind MAILBOX_POLL_DEADLINE_US (main.c): an idle main-loop
 * pass that is not the periodic (every 256th) one scans the SDK request
 * ring when `deadline_ticks` have passed since the last scan, whatever
 * triggered that scan. Kept apart so a host test can drive it.
 *
 * `now` and `last_scan` are top-of-pass reads of the 64-bit global timer.
 * The difference is taken in unsigned arithmetic and read as signed, so it
 * is exact across any wrap and a `now` that is older than `last_scan` (a
 * stamp from before the last scan) is never due instead of reading as a
 * huge interval.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZ9K_MAILBOX_POLL_DEADLINE_H
#define ZZ9K_MAILBOX_POLL_DEADLINE_H

#include <stdint.h>

/* `divider` is the idle-pass counter after this pass's increment. The
 * periodic scan (every 256th) is not a deadline scan. */
static inline int mailbox_poll_deadline_due(uint32_t divider, uint64_t now,
                                            uint64_t last_scan,
                                            uint64_t deadline_ticks)
{
	return (divider & 0xffU) != 0U &&
	       (int64_t)(now - last_scan) >= (int64_t)deadline_ticks;
}

#endif /* ZZ9K_MAILBOX_POLL_DEADLINE_H */
