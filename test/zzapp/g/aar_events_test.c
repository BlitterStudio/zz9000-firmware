/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * aar_event_poll (spec 8.3): the lifecycle events, stop and low memory, are
 * always delivered; the launcher's key, mouse, joystick and break events only
 * with AAR_GROUP_INPUT. A stop request is delivered once: the firmware's own
 * AAR_EV_STOP and the launcher's own are the same delivery, in either order.
 */
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "aar/app.h"
#include "zzapp_proto.h"
#include "zzapp_runtime.h"
#include "zzapp_services.h"

#define SLOT 0u
#define MAX_EVENTS 16u

#define NO_INPUT_GROUPS (AAR_GROUP_DISPLAY | AAR_GROUP_AUDIO | \
                         AAR_GROUP_GL | AAR_GROUP_SERVICES)
#define ALL_GROUPS (NO_INPUT_GROUPS | AAR_GROUP_INPUT)

static uint8_t windows[ZZAPP_SLOT_COUNT][ZZAPP_WINDOW_BYTES];

static void setup(uint32_t groups)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_len = 0u;
    uint32_t i;

    memset(windows, 0, sizeof(windows));
    zzapp_boot_init();
    zzapp_services_test_reset();
    for (i = 0u; i < ZZAPP_SLOT_COUNT; i++)
        zzapp_services_test_set_window_slot(i, windows[i], sizeof(windows[i]));
    zzapp_test_set_groups(groups, 1);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request,
                                  sizeof(request), reply, &reply_len, 1, 1U,
                                  0U) == SDK_STATUS_OK);
    assert(zzapp_get_be32(reply + ZZAPP_ALLOC_SLOT) == SLOT);
    zzapp_services_select_slot(SLOT);
    zzapp_test_mark_running();
    zzapp_host_clear_stop_requested(SLOT);
    zzapp_host_reset_low_memory(SLOT);
}

/* The launcher posts one event into the slot's input ring. */
static void ring_push(uint16_t type, uint16_t code, int16_t dx, int16_t dy,
                      uint16_t buttons, uint32_t time_ms)
{
    uint8_t *window = windows[SLOT];
    uint32_t head = zzapp_get_be32(window + ZZAPP_INPUT_OFF);
    uint32_t off = ZZAPP_INPUT_OFF + ZZAPP_INPUT_HDR +
                   (head & (ZZAPP_INPUT_CAP - 1u)) * ZZAPP_EVENT_BYTES;

    zzapp_put_be16(window + off + 0u, type);
    zzapp_put_be16(window + off + 2u, code);
    zzapp_put_be16(window + off + 4u, (uint16_t)dx);
    zzapp_put_be16(window + off + 6u, (uint16_t)dy);
    zzapp_put_be16(window + off + 8u, buttons);
    zzapp_put_be16(window + off + 10u, 0u);
    zzapp_put_be32(window + off + 12u, time_ms);
    zzapp_put_be32(window + ZZAPP_INPUT_OFF, head + 1u);
}

static uint32_t ring_head(void)
{
    return zzapp_get_be32(windows[SLOT] + ZZAPP_INPUT_OFF);
}

static uint32_t ring_tail(void)
{
    return zzapp_get_be32(windows[SLOT] + ZZAPP_INPUT_OFF + 4u);
}

static void low_memory_crossing(void)
{
    zzapp_host_low_memory(NULL);
}

static uint32_t poll_n(aar_event *ev, uint32_t max)
{
    uint32_t n = 0xdeadbeefu;

    memset(ev, 0xa5, max * sizeof(*ev));
    assert(zzapp_svc_event_poll(ev, max, &n) == AAR_OK);
    assert(n <= max);
    return n;
}

static void expect_lifecycle(const aar_event *ev, uint16_t type)
{
    aar_event zero;

    memset(&zero, 0, sizeof(zero));
    zero.type = type;
    assert(memcmp(ev, &zero, sizeof(zero)) == 0);
}

static uint32_t count_type(const aar_event *ev, uint32_t n, uint16_t type)
{
    uint32_t count = 0u;
    uint32_t i;

    for (i = 0u; i < n; i++)
        if (ev[i].type == type)
            count++;
    return count;
}

static void expect_empty(void)
{
    aar_event ev[MAX_EVENTS];

    assert(poll_n(ev, MAX_EVENTS) == 0u);
}

/* Stop and low memory arrive whatever the groups, and the input ring is not
 * touched without AAR_GROUP_INPUT. */
static void test_lifecycle_without_input_group(uint32_t groups)
{
    aar_event ev[MAX_EVENTS];

    setup(groups);
    ring_push(AAR_EV_KEY_DOWN, 0x45u, 0, 0, 0u, 10u);
    ring_push(AAR_EV_STOP, 0u, 0, 0, 0u, 11u);
    ring_push(AAR_EV_MOUSE, 0u, 3, -2, 1u, 12u);
    expect_empty();
    assert(ring_tail() == 0u && ring_head() == 3u);

    zzapp_host_latch_stop(SLOT);
    low_memory_crossing();
    assert(poll_n(ev, MAX_EVENTS) == 2u);
    expect_lifecycle(&ev[0], AAR_EV_STOP);
    expect_lifecycle(&ev[1], AAR_EV_LOW_MEMORY);
    assert(ring_tail() == 0u && ring_head() == 3u);
    expect_empty();
    assert(ring_tail() == 0u && ring_head() == 3u);
}

/* With AAR_GROUP_INPUT the ring follows the lifecycle events, in order. */
static void test_input_with_input_group(void)
{
    aar_event ev[MAX_EVENTS];

    setup(ALL_GROUPS);
    ring_push(AAR_EV_KEY_DOWN, 0x45u, 0, 0, 0x0008u, 100u);
    ring_push(AAR_EV_MOUSE, 0u, 3, -2, 5u, 101u);
    ring_push(AAR_EV_JOY, 0u, -1, 1, 1u, 102u);
    ring_push(AAR_EV_BREAK, AAR_BREAK_CTRL_D, 0, 0, 0u, 103u);
    zzapp_host_latch_stop(SLOT);
    low_memory_crossing();

    assert(poll_n(ev, MAX_EVENTS) == 6u);
    expect_lifecycle(&ev[0], AAR_EV_STOP);
    expect_lifecycle(&ev[1], AAR_EV_LOW_MEMORY);
    assert(ev[2].type == AAR_EV_KEY_DOWN && ev[2].code == 0x45u &&
           ev[2].buttons == 0x0008u && ev[2].time_ms == 100u);
    assert(ev[3].type == AAR_EV_MOUSE && ev[3].dx == 3 && ev[3].dy == -2 &&
           ev[3].buttons == 5u && ev[3].time_ms == 101u);
    assert(ev[4].type == AAR_EV_JOY && ev[4].dx == -1 && ev[4].dy == 1 &&
           ev[4].buttons == 1u && ev[4].time_ms == 102u);
    assert(ev[5].type == AAR_EV_BREAK && ev[5].time_ms == 103u);
    assert(ring_tail() == ring_head());
    expect_empty();
}

/* A stop is one delivery: the firmware's and the launcher's events for the
 * same request are the same event, whichever comes first. */
static void test_duplicate_stop_dropped(void)
{
    aar_event ev[MAX_EVENTS];

    /* The firmware's event first, the launcher's already in the ring. */
    setup(ALL_GROUPS);
    ring_push(AAR_EV_KEY_DOWN, 1u, 0, 0, 0u, 1u);
    ring_push(AAR_EV_STOP, 0u, 0, 0, 0u, 2u);
    ring_push(AAR_EV_KEY_UP, 1u, 0, 0, 0u, 3u);
    zzapp_host_latch_stop(SLOT);
    assert(poll_n(ev, MAX_EVENTS) == 3u);
    assert(count_type(ev, 3u, AAR_EV_STOP) == 1u);
    assert(ev[0].type == AAR_EV_STOP);
    assert(ev[1].type == AAR_EV_KEY_DOWN && ev[2].type == AAR_EV_KEY_UP);
    assert(ring_tail() == ring_head());
    expect_empty();

    /* The launcher's own event first: it counts as the delivery, and the
     * latch that follows queues nothing more. */
    setup(ALL_GROUPS);
    ring_push(AAR_EV_KEY_DOWN, 1u, 0, 0, 0u, 1u);
    ring_push(AAR_EV_STOP, 0u, 0, 0, 0u, 2u);
    ring_push(AAR_EV_KEY_UP, 1u, 0, 0, 0u, 3u);
    assert(poll_n(ev, MAX_EVENTS) == 3u);
    assert(ev[0].type == AAR_EV_KEY_DOWN && ev[1].type == AAR_EV_STOP &&
           ev[2].type == AAR_EV_KEY_UP);
    zzapp_host_latch_stop(SLOT);
    expect_empty();
    zzapp_host_latch_stop(SLOT);
    expect_empty();

    /* The launcher's event arrives after the firmware's was delivered. */
    setup(ALL_GROUPS);
    zzapp_host_latch_stop(SLOT);
    assert(poll_n(ev, MAX_EVENTS) == 1u);
    expect_lifecycle(&ev[0], AAR_EV_STOP);
    ring_push(AAR_EV_STOP, 0u, 0, 0, 0u, 9u);
    ring_push(AAR_EV_KEY_DOWN, 2u, 0, 0, 0u, 10u);
    assert(poll_n(ev, MAX_EVENTS) == 1u);
    assert(ev[0].type == AAR_EV_KEY_DOWN && ev[0].code == 2u);
    expect_empty();

    /* The launcher posts two stops: one is delivered. */
    setup(ALL_GROUPS);
    ring_push(AAR_EV_STOP, 0u, 0, 0, 0u, 1u);
    ring_push(AAR_EV_STOP, 0u, 0, 0, 0u, 2u);
    assert(poll_n(ev, MAX_EVENTS) == 1u);
    assert(ev[0].type == AAR_EV_STOP);
    zzapp_host_latch_stop(SLOT);
    expect_empty();

    /* Without the input group the launcher's event is never read, so the
     * firmware's is the delivery. */
    setup(NO_INPUT_GROUPS);
    ring_push(AAR_EV_STOP, 0u, 0, 0, 0u, 1u);
    expect_empty();
    zzapp_host_latch_stop(SLOT);
    assert(poll_n(ev, MAX_EVENTS) == 1u);
    expect_lifecycle(&ev[0], AAR_EV_STOP);
    expect_empty();
}

/* No stop request, no stop event; one low-memory event per crossing. */
static void test_low_memory_per_crossing(void)
{
    aar_event ev[MAX_EVENTS];
    uint32_t n;
    uint32_t total = 0u;
    uint32_t calls;

    setup(NO_INPUT_GROUPS);
    expect_empty();
    low_memory_crossing();
    low_memory_crossing();
    for (calls = 0u; calls < 8u; calls++) {
        n = poll_n(ev, MAX_EVENTS);
        assert(count_type(ev, n, AAR_EV_STOP) == 0u);
        assert(count_type(ev, n, AAR_EV_LOW_MEMORY) == n);
        total += n;
        if (n == 0u)
            break;
    }
    assert(total == 2u);
    expect_empty();
}

/* A short buffer loses nothing: what does not fit stays queued. */
static void test_short_buffer(void)
{
    aar_event ev[MAX_EVENTS];
    uint32_t n = 0xdeadbeefu;

    setup(ALL_GROUPS);
    zzapp_host_latch_stop(SLOT);
    low_memory_crossing();
    ring_push(AAR_EV_KEY_DOWN, 7u, 0, 0, 0u, 1u);

    /* max 0 consumes nothing, and NULL is fine for it. */
    assert(zzapp_svc_event_poll(NULL, 0u, &n) == AAR_OK && n == 0u);
    assert(ring_tail() == 0u);
    assert(poll_n(ev, 1u) == 1u);
    expect_lifecycle(&ev[0], AAR_EV_STOP);
    assert(poll_n(ev, 1u) == 1u);
    expect_lifecycle(&ev[0], AAR_EV_LOW_MEMORY);
    assert(poll_n(ev, 1u) == 1u);
    assert(ev[0].type == AAR_EV_KEY_DOWN && ev[0].code == 7u);
    expect_empty();
}

static void test_bad_arguments(void)
{
    aar_event ev;
    uint32_t n = 7u;

    setup(ALL_GROUPS);
    zzapp_host_latch_stop(SLOT);
    assert(zzapp_svc_event_poll(&ev, 1u, NULL) == AAR_E_INVAL);
    assert(zzapp_svc_event_poll(NULL, 1u, &n) == AAR_E_INVAL && n == 0u);
    /* The refused calls consumed nothing. */
    assert(poll_n(&ev, 1u) == 1u);
    expect_lifecycle(&ev, AAR_EV_STOP);
}

int main(void)
{
    test_lifecycle_without_input_group(NO_INPUT_GROUPS);
    test_lifecycle_without_input_group(0u);
    test_input_with_input_group();
    test_duplicate_stop_dropped();
    test_low_memory_per_crossing();
    test_short_buffer();
    test_bad_arguments();
    zzapp_test_set_groups(0u, 0);
    return 0;
}
