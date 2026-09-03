/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ZZUSB_PROXY_PERIODIC_RING_H
#define ZZUSB_PROXY_PERIODIC_RING_H

#include <stdint.h>

#define ZZUSB_PERIODIC_RING_CAPACITY 16U

static inline int zzusb_periodic_ring_push(
    uint8_t entries[ZZUSB_PERIODIC_RING_CAPACITY],
    uint8_t head, uint8_t *count, uint8_t value)
{
    unsigned position;
    unsigned tail;

    if (!entries || !count || *count >= ZZUSB_PERIODIC_RING_CAPACITY)
        return 0;
    for (position = 0; position < *count; position++)
        if (entries[(head + position) % ZZUSB_PERIODIC_RING_CAPACITY] == value)
            return 0;
    tail = (head + *count) % ZZUSB_PERIODIC_RING_CAPACITY;
    entries[tail] = value;
    (*count)++;
    return 1;
}

static inline int zzusb_periodic_ring_remove(
    uint8_t entries[ZZUSB_PERIODIC_RING_CAPACITY],
    uint8_t *head, uint8_t *count, unsigned position)
{
    unsigned current;

    if (!entries || !head || !count || position >= *count)
        return 0;
    for (current = position; current + 1U < *count; current++)
        entries[(*head + current) % ZZUSB_PERIODIC_RING_CAPACITY] =
            entries[(*head + current + 1U) % ZZUSB_PERIODIC_RING_CAPACITY];
    (*count)--;
    if (!*count)
        *head = 0;
    return 1;
}

static inline int zzusb_periodic_rearm_ready(
    int needs_rearm, int host_requested)
{
    return needs_rearm && host_requested;
}

static inline int zzusb_periodic_completion_needs_rearm(
    int failed, int direction_in)
{
    return !failed && direction_in;
}

static inline int zzusb_periodic_defer_split_stall(
    int stalled, int split, int direction_in, int already_deferred)
{
    return stalled && split && direction_in && !already_deferred;
}

#endif
