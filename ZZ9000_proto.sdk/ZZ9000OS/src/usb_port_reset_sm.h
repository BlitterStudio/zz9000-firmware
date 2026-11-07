/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Non-blocking timing state machine for an EHCI root-port reset.
 *
 * Every timed phase measures from the hardware action it times: start and
 * step return the action to perform, and the caller performs it and then
 * calls zzusb_root_reset_arm() with the time after the action completed.
 * PHY setup or register writes can therefore never shorten the 50 ms reset
 * hold, the 4 ms PR-clear budget or the 10 ms recovery. Elapsed time is
 * compared as now - start, so the comparison stays correct if the clock
 * wraps.
 */
#ifndef USB_PORT_RESET_SM_H
#define USB_PORT_RESET_SM_H

#include <stdint.h>

#define ZZUSB_ROOT_RESET_HOLD_US     50000U /* USB 2.0 root-port TDRSTR */
#define ZZUSB_ROOT_RESET_CLEAR_US     4000U /* EHCI 2.3.9 PR-clear budget */
#define ZZUSB_ROOT_RESET_RECOVERY_US 10000U /* USB 2.0 TRSTRCY */

enum zzusb_root_reset_state {
    ZZUSB_ROOT_RESET_IDLE,
    ZZUSB_ROOT_RESET_WAIT_STALE_CLEAR,
    ZZUSB_ROOT_RESET_HOLD,
    ZZUSB_ROOT_RESET_WAIT_CLEAR,
    ZZUSB_ROOT_RESET_RECOVERY
};

enum zzusb_root_reset_action {
    ZZUSB_ROOT_RESET_NONE,
    ZZUSB_ROOT_RESET_DROP_STALE,
    ZZUSB_ROOT_RESET_ASSERT,
    ZZUSB_ROOT_RESET_DEASSERT,
    ZZUSB_ROOT_RESET_PORT_READY,
    ZZUSB_ROOT_RESET_COMPLETE,
    ZZUSB_ROOT_RESET_TIMEOUT
};

/* Convert timer ticks to microseconds. Quotient and remainder keep it
 * exact without overflow: ticks * 1000000 overflows uint64_t after about
 * 15 hours of uptime at the 333 MHz global timer. */
static inline uint64_t zzusb_ticks_to_us(uint64_t ticks, uint64_t hz)
{
    return (ticks / hz) * 1000000U + ((ticks % hz) * 1000000U) / hz;
}

struct zzusb_root_reset {
    enum zzusb_root_reset_state state;
    uint64_t started_us;
    uint32_t duration_us;
    uint32_t observed_portsc;
};

static inline void zzusb_root_reset_init(struct zzusb_root_reset *reset)
{
    reset->state = ZZUSB_ROOT_RESET_IDLE;
    reset->started_us = 0;
    reset->duration_us = 0;
    reset->observed_portsc = 0;
}

static inline int zzusb_root_reset_active(const struct zzusb_root_reset *reset)
{
    return reset->state != ZZUSB_ROOT_RESET_IDLE;
}

/* Enter a timed phase. The provisional start is the decision time; the
 * caller re-arms it once the action that begins the phase is done. */
static inline void zzusb_root_reset_enter(struct zzusb_root_reset *reset,
                                          enum zzusb_root_reset_state state,
                                          uint64_t now_us,
                                          uint32_t duration_us)
{
    reset->state = state;
    reset->started_us = now_us;
    reset->duration_us = duration_us;
}

/* Start timing the current phase from now_us: call it right after the
 * PORTSC/PHY action the last start or step returned has completed. */
static inline void zzusb_root_reset_arm(struct zzusb_root_reset *reset,
                                        uint64_t now_us)
{
    reset->started_us = now_us;
}

static inline int zzusb_root_reset_elapsed(const struct zzusb_root_reset *reset,
                                           uint64_t now_us)
{
    return now_us - reset->started_us >= (uint64_t)reset->duration_us;
}

static inline enum zzusb_root_reset_action
zzusb_root_reset_start(struct zzusb_root_reset *reset, uint64_t now_us,
                       uint32_t portsc)
{
    reset->observed_portsc = portsc;
    if (portsc & (1U << 8)) {
        zzusb_root_reset_enter(reset, ZZUSB_ROOT_RESET_WAIT_STALE_CLEAR,
                               now_us, ZZUSB_ROOT_RESET_CLEAR_US);
        return ZZUSB_ROOT_RESET_DROP_STALE;
    }
    zzusb_root_reset_enter(reset, ZZUSB_ROOT_RESET_HOLD, now_us,
                           ZZUSB_ROOT_RESET_HOLD_US);
    return ZZUSB_ROOT_RESET_ASSERT;
}

static inline enum zzusb_root_reset_action
zzusb_root_reset_step(struct zzusb_root_reset *reset, uint64_t now_us,
                      uint32_t portsc)
{
    reset->observed_portsc = portsc;
    switch (reset->state) {
    case ZZUSB_ROOT_RESET_WAIT_STALE_CLEAR:
        if (!(portsc & (1U << 8))) {
            zzusb_root_reset_enter(reset, ZZUSB_ROOT_RESET_HOLD, now_us,
                                   ZZUSB_ROOT_RESET_HOLD_US);
            return ZZUSB_ROOT_RESET_ASSERT;
        }
        if (zzusb_root_reset_elapsed(reset, now_us)) {
            zzusb_root_reset_init(reset);
            reset->observed_portsc = portsc;
            return ZZUSB_ROOT_RESET_TIMEOUT;
        }
        break;
    case ZZUSB_ROOT_RESET_HOLD:
        if (zzusb_root_reset_elapsed(reset, now_us)) {
            zzusb_root_reset_enter(reset, ZZUSB_ROOT_RESET_WAIT_CLEAR,
                                   now_us, ZZUSB_ROOT_RESET_CLEAR_US);
            return ZZUSB_ROOT_RESET_DEASSERT;
        }
        break;
    case ZZUSB_ROOT_RESET_WAIT_CLEAR:
        if (!(portsc & (1U << 8))) {
            zzusb_root_reset_enter(reset, ZZUSB_ROOT_RESET_RECOVERY, now_us,
                                   ZZUSB_ROOT_RESET_RECOVERY_US);
            return ZZUSB_ROOT_RESET_PORT_READY;
        }
        if (zzusb_root_reset_elapsed(reset, now_us)) {
            zzusb_root_reset_init(reset);
            reset->observed_portsc = portsc;
            return ZZUSB_ROOT_RESET_TIMEOUT;
        }
        break;
    case ZZUSB_ROOT_RESET_RECOVERY:
        if (zzusb_root_reset_elapsed(reset, now_us)) {
            uint32_t observed = reset->observed_portsc;

            zzusb_root_reset_init(reset);
            reset->observed_portsc = observed;
            return ZZUSB_ROOT_RESET_COMPLETE;
        }
        break;
    case ZZUSB_ROOT_RESET_IDLE:
    default:
        break;
    }
    return ZZUSB_ROOT_RESET_NONE;
}

static inline void zzusb_root_reset_abort(struct zzusb_root_reset *reset)
{
    zzusb_root_reset_init(reset);
}

#endif
