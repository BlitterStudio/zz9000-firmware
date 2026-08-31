/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "usb_proxy_diag.h"
#include "usb_proxy.h"

#include <string.h>

struct zzusb_diag_state {
    volatile uint32_t next_sequence;
    volatile uint32_t lost_events;
    volatile uint32_t counters[ZZUSB_DIAG_COUNTER_COUNT];
    struct zzusb_diag_event events[ZZUSB_DIAG_EVENT_COUNT];
    uint32_t generation;
};

static struct zzusb_diag_state diag;

_Static_assert(sizeof(struct zzusb_diag_event) == ZZUSB_DIAG_EVENT_SIZE,
               "diagnostic event wire size changed");
_Static_assert(ZZUSB_DIAG_OFF_EVENTS +
               ZZUSB_DIAG_EVENT_COUNT * ZZUSB_DIAG_EVENT_SIZE <=
               ZZUSB_DIAG_PAGE_SIZE,
               "diagnostic page overflow");

void zzusb_diag_reset(void)
{
    memset(&diag, 0, sizeof(diag));
}

void zzusb_diag_count(enum zzusb_diag_counter counter)
{
    if ((unsigned)counter < ZZUSB_DIAG_COUNTER_COUNT)
        __atomic_add_fetch(&diag.counters[counter], 1U, __ATOMIC_RELAXED);
}

void zzusb_diag_high_water(uint32_t depth)
{
    uint32_t observed = __atomic_load_n(
        &diag.counters[ZZUSB_DIAG_COUNT_QUEUE_HIGH_WATER], __ATOMIC_RELAXED);

    while (depth > observed &&
           !__atomic_compare_exchange_n(
               &diag.counters[ZZUSB_DIAG_COUNT_QUEUE_HIGH_WATER],
               &observed, depth, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

void zzusb_diag_record(uint16_t type, uint16_t status,
                       uint32_t request_id, uint32_t epoch,
                       uint16_t address, uint8_t endpoint,
                       uint8_t direction, uint16_t topology,
                       uint16_t schedule, uint32_t detail,
                       uint32_t timestamp)
{
    uint32_t sequence = __atomic_add_fetch(
        &diag.next_sequence, 1U, __ATOMIC_RELAXED);
    struct zzusb_diag_event *event =
        &diag.events[(sequence - 1U) % ZZUSB_DIAG_EVENT_COUNT];

    if (sequence > ZZUSB_DIAG_EVENT_COUNT)
        __atomic_add_fetch(&diag.lost_events, 1U, __ATOMIC_RELAXED);

    __atomic_store_n(&event->sequence, 0U, __ATOMIC_RELAXED);
    event->request_id = request_id;
    event->epoch = epoch;
    event->detail = detail;
    event->timestamp = timestamp;
    event->type = type;
    event->status = status;
    event->address = address;
    event->topology = topology;
    event->endpoint = endpoint;
    event->direction = direction;
    event->schedule = schedule;
    __atomic_store_n(&event->sequence, sequence, __ATOMIC_RELEASE);
}

static void encode_event(volatile uint8_t *dst,
                         const struct zzusb_diag_event *event,
                         uint32_t sequence)
{
    put_be32(dst + ZZUSB_DIAG_EVT_OFF_SEQUENCE, sequence);
    put_be32(dst + ZZUSB_DIAG_EVT_OFF_REQUEST, event->request_id);
    put_be32(dst + ZZUSB_DIAG_EVT_OFF_EPOCH, event->epoch);
    put_be32(dst + ZZUSB_DIAG_EVT_OFF_DETAIL, event->detail);
    put_be32(dst + ZZUSB_DIAG_EVT_OFF_TIMESTAMP, event->timestamp);
    put_be16(dst + ZZUSB_DIAG_EVT_OFF_TYPE, event->type);
    put_be16(dst + ZZUSB_DIAG_EVT_OFF_STATUS, event->status);
    put_be16(dst + ZZUSB_DIAG_EVT_OFF_ADDRESS, event->address);
    put_be16(dst + ZZUSB_DIAG_EVT_OFF_TOPOLOGY, event->topology);
    dst[ZZUSB_DIAG_EVT_OFF_ENDPOINT] = event->endpoint;
    dst[ZZUSB_DIAG_EVT_OFF_DIRECTION] = event->direction;
    put_be16(dst + ZZUSB_DIAG_EVT_OFF_SCHEDULE, event->schedule);
}

void zzusb_diag_publish(volatile void *page, size_t page_size,
                        uint32_t capabilities, uint32_t epoch,
                        uint32_t last_request_id, uint32_t queue_state,
                        uint32_t schedule_bits, zzusb_diag_flush_fn flush)
{
    volatile uint8_t *wire = (volatile uint8_t *)page;
    uint32_t next;
    uint32_t count;
    uint32_t first;
    uint32_t generation;
    uint32_t encoded = 0;

    if (!page || page_size < ZZUSB_DIAG_PAGE_SIZE)
        return;

    generation = (diag.generation + 2U) & ~1U;
    if (generation == 0U)
        generation = 2U;
    diag.generation = generation;

    put_be32(wire + ZZUSB_DIAG_OFF_GENERATION, generation - 1U);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    if (flush)
        flush(wire, 64U);

    next = __atomic_load_n(&diag.next_sequence, __ATOMIC_ACQUIRE);
    count = next < ZZUSB_DIAG_EVENT_COUNT ? next : ZZUSB_DIAG_EVENT_COUNT;
    first = count ? next - count + 1U : 0U;

    put_be32(wire + ZZUSB_DIAG_OFF_MAGIC, ZZUSB_DIAG_MAGIC);
    put_be16(wire + ZZUSB_DIAG_OFF_VERSION, ZZUSB_DIAG_VERSION);
    put_be16(wire + ZZUSB_DIAG_OFF_HEADER_SIZE, ZZUSB_DIAG_OFF_EVENTS);
    put_be32(wire + ZZUSB_DIAG_OFF_TOTAL_SIZE, ZZUSB_DIAG_PAGE_SIZE);
    put_be32(wire + ZZUSB_DIAG_OFF_CAPABILITIES, capabilities);
    put_be32(wire + ZZUSB_DIAG_OFF_EPOCH, epoch);
    put_be32(wire + ZZUSB_DIAG_OFF_LAST_ID, last_request_id);
    put_be32(wire + ZZUSB_DIAG_OFF_EVENT_NEXT, next);
    put_be32(wire + ZZUSB_DIAG_OFF_LOST_EVENTS,
             __atomic_load_n(&diag.lost_events, __ATOMIC_RELAXED));
    put_be32(wire + ZZUSB_DIAG_OFF_QUEUE_STATE, queue_state);
    put_be32(wire + ZZUSB_DIAG_OFF_SCHEDULE_BITS, schedule_bits);

    for (uint32_t i = 0; i < ZZUSB_DIAG_COUNTER_COUNT; i++) {
        put_be32(wire + ZZUSB_DIAG_OFF_COUNTERS + i * 4U,
                 __atomic_load_n(&diag.counters[i], __ATOMIC_RELAXED));
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t sequence = first + i;
        const struct zzusb_diag_event *event =
            &diag.events[(sequence - 1U) % ZZUSB_DIAG_EVENT_COUNT];
        uint32_t committed = __atomic_load_n(&event->sequence, __ATOMIC_ACQUIRE);

        if (committed != sequence)
            continue;
        encode_event(wire + ZZUSB_DIAG_OFF_EVENTS +
                     encoded * ZZUSB_DIAG_EVENT_SIZE, event, sequence);
        encoded++;
    }
    put_be32(wire + ZZUSB_DIAG_OFF_EVENT_COUNT, encoded);
    for (uint32_t i = encoded; i < ZZUSB_DIAG_EVENT_COUNT; i++)
        put_be32(wire + ZZUSB_DIAG_OFF_EVENTS +
                   i * ZZUSB_DIAG_EVENT_SIZE, 0U);

    __atomic_thread_fence(__ATOMIC_RELEASE);
    if (flush)
        flush(wire, ZZUSB_DIAG_PAGE_SIZE);
    put_be32(wire + ZZUSB_DIAG_OFF_GENERATION, generation);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    if (flush)
        flush(wire, 8U);
}

int zzusb_diag_read_coherent(volatile const void *page, void *snapshot,
                             size_t snapshot_size, unsigned retries)
{
    volatile const uint8_t *wire = (volatile const uint8_t *)page;
    uint8_t *copy = (uint8_t *)snapshot;

    if (!page || !snapshot || snapshot_size < ZZUSB_DIAG_PAGE_SIZE)
        return 0;

    while (retries--) {
        uint32_t before = be32(wire + ZZUSB_DIAG_OFF_GENERATION);
        if (before & 1U)
            continue;
        for (size_t i = 0; i < ZZUSB_DIAG_PAGE_SIZE; i++)
            copy[i] = wire[i];
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (before == be32(wire + ZZUSB_DIAG_OFF_GENERATION) &&
            before == be32(copy + ZZUSB_DIAG_OFF_GENERATION) &&
            be32(copy + ZZUSB_DIAG_OFF_MAGIC) == ZZUSB_DIAG_MAGIC)
            return 1;
    }
    return 0;
}
