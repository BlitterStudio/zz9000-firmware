/*
 * Mock AmigaOS devices/timer.h for the guest-client host harness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DEVICES_TIMER_H
#define DEVICES_TIMER_H

#include <exec/types.h>
#include <exec/io.h>

#define UNIT_MICROHZ  0
#define UNIT_VBLANK   1

#define TR_GETSYSTIME 13
/* Distinct from the host <sys/time.h> timeval: the mock's Amiga flavour
 * carries microsecond resolution fields and must not collide with libc. */
struct mgl_mock_timeval {
    ULONG tv_secs;
    ULONG tv_micro;
};

struct EClockVal {
    ULONG ev_hi;
    ULONG ev_lo;
};

struct timerequest {
    struct IORequest tr_node;
    struct mgl_mock_timeval tr_time;
};

#endif /* DEVICES_TIMER_H */
