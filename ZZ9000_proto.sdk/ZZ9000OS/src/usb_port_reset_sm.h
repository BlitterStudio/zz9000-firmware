/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Non-blocking timing state machine for an EHCI root-port reset.
 */
#ifndef USB_PORT_RESET_SM_H
#define USB_PORT_RESET_SM_H

#include <stdint.h>

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

struct zzusb_root_reset {
    enum zzusb_root_reset_state state;
    uint64_t deadline_us;
    uint32_t observed_portsc;
};

static inline void zzusb_root_reset_init(struct zzusb_root_reset *reset)
{
    reset->state = ZZUSB_ROOT_RESET_IDLE;
    reset->deadline_us = 0;
    reset->observed_portsc = 0;
}

static inline int zzusb_root_reset_active(const struct zzusb_root_reset *reset)
{
    return reset->state != ZZUSB_ROOT_RESET_IDLE;
}

static inline enum zzusb_root_reset_action
zzusb_root_reset_start(struct zzusb_root_reset *reset, uint64_t now_us,
                       uint32_t portsc)
{
    reset->observed_portsc = portsc;
    if (portsc & (1U << 8)) {
        reset->state = ZZUSB_ROOT_RESET_WAIT_STALE_CLEAR;
        reset->deadline_us = now_us + 4000U;
        return ZZUSB_ROOT_RESET_DROP_STALE;
    }
    reset->state = ZZUSB_ROOT_RESET_HOLD;
    reset->deadline_us = now_us + 50000U;
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
            reset->state = ZZUSB_ROOT_RESET_HOLD;
            reset->deadline_us = now_us + 50000U;
            return ZZUSB_ROOT_RESET_ASSERT;
        }
        if (now_us >= reset->deadline_us) {
            reset->state = ZZUSB_ROOT_RESET_IDLE;
            reset->deadline_us = 0;
            return ZZUSB_ROOT_RESET_TIMEOUT;
        }
        break;
    case ZZUSB_ROOT_RESET_HOLD:
        if (now_us >= reset->deadline_us) {
            reset->state = ZZUSB_ROOT_RESET_WAIT_CLEAR;
            reset->deadline_us = now_us + 4000U;
            return ZZUSB_ROOT_RESET_DEASSERT;
        }
        break;
    case ZZUSB_ROOT_RESET_WAIT_CLEAR:
        if (!(portsc & (1U << 8))) {
            reset->state = ZZUSB_ROOT_RESET_RECOVERY;
            reset->deadline_us = now_us + 10000U;
            return ZZUSB_ROOT_RESET_PORT_READY;
        }
        if (now_us >= reset->deadline_us) {
            reset->state = ZZUSB_ROOT_RESET_IDLE;
            reset->deadline_us = 0;
            return ZZUSB_ROOT_RESET_TIMEOUT;
        }
        break;
    case ZZUSB_ROOT_RESET_RECOVERY:
        if (now_us >= reset->deadline_us) {
            reset->state = ZZUSB_ROOT_RESET_IDLE;
            reset->deadline_us = 0;
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
