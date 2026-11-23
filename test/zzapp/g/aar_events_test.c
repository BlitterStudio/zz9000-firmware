/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * aar_event_poll (spec 8.3): the lifecycle events, stop and low memory, are
 * always delivered; the client's key, mouse, joystick and break events only
 * with AAR_GROUP_INPUT. A stop request is delivered once: the firmware's own
 * AAR_EV_STOP and the client's own are the same delivery, in either order.
 *
 * The client is mock_client: it posts events into the completion queue of the
 * claim's pair, and the runtime reads them through the pump. The completion
 * queue indices show what was consumed: a poll without AAR_GROUP_INPUT must
 * not move the head.
 */
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "aar/app.h"
#include "mock_client.h"
#include "zzapp_proto.h"
#include "zzapp_runtime.h"
#include "zzapp_services.h"

#define MAX_EVENTS 16u

#define NO_INPUT_GROUPS (AAR_GROUP_DISPLAY | AAR_GROUP_AUDIO | \
                         AAR_GROUP_GL | AAR_GROUP_SERVICES)
#define ALL_GROUPS (NO_INPUT_GROUPS | AAR_GROUP_INPUT)

static mock_client client;
/* The record the claim runs on. */
static uint32_t g_pid;

static void setup(uint32_t groups)
{
    zzapp_xport_env_t env;
    zzapp_xport_buf_t pair;

    zzapp_boot_init();
    zzapp_services_test_reset();
    mock_heap_reset();
    env = mock_heap_env();
    zzapp_xport_init(&env);
    zzapp_test_set_groups(groups, 1);
    g_pid = zzapp_test_admit();
    assert(g_pid != ZZAPP_PID_NONE);
    assert(zzapp_xport_block_get(zzapp_rec_by_pid(g_pid), ZZAPP_XPORT_BLOCK_PAIR,
                                 &pair) == 1);
    mock_client_init(&client);
    assert(mock_client_attach(&client, pair.token) == AAR_OK);
    zzapp_test_mark_running();
    zzapp_host_clear_stop_requested(g_pid);
    zzapp_host_reset_low_memory(g_pid);
}

/* The client posts one event into the completion queue. */
static void ring_push(uint16_t type, uint16_t code, int16_t dx, int16_t dy,
                      uint16_t buttons, uint32_t time_ms)
{
    aar_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.dx = dx;
    ev.dy = dy;
    ev.buttons = buttons;
    ev.time_ms = time_ms;
    assert(mock_client_post_event(&client, &ev) == AAR_OK);
}

/* Events the client posted, and the ones the runtime has taken from the
 * queue. */
static uint32_t ring_head(void)
{
    return mock_client_cb_get(&client, AAR_CB_CQ_TAIL);
}

static uint32_t ring_tail(void)
{
    return mock_client_cb_get(&client, AAR_CB_CQ_HEAD);
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

/* Stop and low memory arrive whatever the groups, and the completion queue is
 * not read without AAR_GROUP_INPUT. */
static void test_lifecycle_without_input_group(uint32_t groups)
{
    aar_event ev[MAX_EVENTS];

    setup(groups);
    ring_push(AAR_EV_KEY_DOWN, 0x45u, 0, 0, 0u, 10u);
    ring_push(AAR_EV_STOP, 0u, 0, 0, 0u, 11u);
    ring_push(AAR_EV_MOUSE, 0u, 3, -2, 1u, 12u);
    expect_empty();
    assert(ring_tail() == 0u && ring_head() == 3u);

    zzapp_host_latch_stop(g_pid);
    low_memory_crossing();
    assert(poll_n(ev, MAX_EVENTS) == 2u);
    expect_lifecycle(&ev[0], AAR_EV_STOP);
    expect_lifecycle(&ev[1], AAR_EV_LOW_MEMORY);
    assert(ring_tail() == 0u && ring_head() == 3u);
    expect_empty();
    assert(ring_tail() == 0u && ring_head() == 3u);
}

/* With AAR_GROUP_INPUT the client's events follow the lifecycle events, in
 * order. */
static void test_input_with_input_group(void)
{
    aar_event ev[MAX_EVENTS];

    setup(ALL_GROUPS);
    ring_push(AAR_EV_KEY_DOWN, 0x45u, 0, 0, 0x0008u, 100u);
    ring_push(AAR_EV_MOUSE, 0u, 3, -2, 5u, 101u);
    ring_push(AAR_EV_JOY, 0u, -1, 1, 1u, 102u);
    ring_push(AAR_EV_BREAK, AAR_BREAK_CTRL_D, 0, 0, 0u, 103u);
    zzapp_host_latch_stop(g_pid);
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

/* A stop is one delivery: the firmware's and the client's events for the same
 * request are the same event, whichever comes first. */
static void test_duplicate_stop_dropped(void)
{
    aar_event ev[MAX_EVENTS];

    /* The firmware's event first, the client's already queued. */
    setup(ALL_GROUPS);
    ring_push(AAR_EV_KEY_DOWN, 1u, 0, 0, 0u, 1u);
    ring_push(AAR_EV_STOP, 0u, 0, 0, 0u, 2u);
    ring_push(AAR_EV_KEY_UP, 1u, 0, 0, 0u, 3u);
    zzapp_host_latch_stop(g_pid);
    assert(poll_n(ev, MAX_EVENTS) == 3u);
    assert(count_type(ev, 3u, AAR_EV_STOP) == 1u);
    assert(ev[0].type == AAR_EV_STOP);
    assert(ev[1].type == AAR_EV_KEY_DOWN && ev[2].type == AAR_EV_KEY_UP);
    assert(ring_tail() == ring_head());
    expect_empty();

    /* The client's stop word and its own event: the poll latches the word,
     * the firmware's event is the delivery and the client's is dropped. */
    setup(ALL_GROUPS);
    mock_client_request_stop(&client);
    ring_push(AAR_EV_KEY_DOWN, 1u, 0, 0, 0u, 1u);
    ring_push(AAR_EV_STOP, 0u, 0, 0, 0u, 2u);
    ring_push(AAR_EV_KEY_UP, 1u, 0, 0, 0u, 3u);
    assert(poll_n(ev, MAX_EVENTS) == 3u);
    assert(count_type(ev, 3u, AAR_EV_STOP) == 1u);
    assert(ev[0].type == AAR_EV_STOP && ev[1].type == AAR_EV_KEY_DOWN &&
           ev[2].type == AAR_EV_KEY_UP);
    zzapp_host_latch_stop(g_pid);
    expect_empty();
    zzapp_host_latch_stop(g_pid);
    expect_empty();

    /* The client's event arrives after the firmware's was delivered. */
    setup(ALL_GROUPS);
    zzapp_host_latch_stop(g_pid);
    assert(poll_n(ev, MAX_EVENTS) == 1u);
    expect_lifecycle(&ev[0], AAR_EV_STOP);
    ring_push(AAR_EV_STOP, 0u, 0, 0, 0u, 9u);
    ring_push(AAR_EV_KEY_DOWN, 2u, 0, 0, 0u, 10u);
    assert(poll_n(ev, MAX_EVENTS) == 1u);
    assert(ev[0].type == AAR_EV_KEY_DOWN && ev[0].code == 2u);
    expect_empty();

    /* The client posts two stops with its stop word set: one is
     * delivered. */
    setup(ALL_GROUPS);
    mock_client_request_stop(&client);
    ring_push(AAR_EV_STOP, 0u, 0, 0, 0u, 1u);
    ring_push(AAR_EV_STOP, 0u, 0, 0, 0u, 2u);
    assert(poll_n(ev, MAX_EVENTS) == 1u);
    assert(ev[0].type == AAR_EV_STOP);
    zzapp_host_latch_stop(g_pid);
    expect_empty();

    /* Without the input group the client's event is never read, so the
     * firmware's is the delivery. */
    setup(NO_INPUT_GROUPS);
    ring_push(AAR_EV_STOP, 0u, 0, 0, 0u, 1u);
    expect_empty();
    zzapp_host_latch_stop(g_pid);
    assert(poll_n(ev, MAX_EVENTS) == 1u);
    expect_lifecycle(&ev[0], AAR_EV_STOP);
    expect_empty();
}

/* A STOP event with no stop requested is the client's event and nothing
 * more: it is delivered, but it is not the delivery of a request that comes
 * later, which still gets its own event. */
static void test_bare_stop_event_is_not_the_delivery(void)
{
    aar_event ev[MAX_EVENTS];

    setup(ALL_GROUPS);
    ring_push(AAR_EV_STOP, 0u, 0, 0, 0u, 1u);
    assert(poll_n(ev, MAX_EVENTS) == 1u);
    assert(ev[0].type == AAR_EV_STOP);
    expect_empty();
    zzapp_host_latch_stop(g_pid);
    assert(poll_n(ev, MAX_EVENTS) == 1u);
    expect_lifecycle(&ev[0], AAR_EV_STOP);
    expect_empty();

    /* The same with the request made by the client's stop word. */
    setup(ALL_GROUPS);
    ring_push(AAR_EV_STOP, 0u, 0, 0, 0u, 1u);
    assert(poll_n(ev, MAX_EVENTS) == 1u);
    mock_client_request_stop(&client);
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
    zzapp_host_latch_stop(g_pid);
    low_memory_crossing();
    ring_push(AAR_EV_KEY_DOWN, 7u, 0, 0, 0u, 1u);

    /* max 0 consumes nothing, and NULL is fine for it. */
    assert(zzapp_svc_event_poll(NULL, 0u, &n) == AAR_OK && n == 0u);
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
    zzapp_host_latch_stop(g_pid);
    assert(zzapp_svc_event_poll(&ev, 1u, NULL) == AAR_E_INVAL);
    assert(zzapp_svc_event_poll(NULL, 1u, &n) == AAR_E_INVAL && n == 0u);
    /* The refused calls consumed nothing. */
    assert(poll_n(&ev, 1u) == 1u);
    expect_lifecycle(&ev, AAR_EV_STOP);
}

/* The completion queue is 64 deep and the client drops what does not fit,
 * counting it (the contract of the event ring): a flood of events behind a
 * poll that reads nothing loses the newest, keeps the oldest in order, and
 * the control block says how many were dropped. */
static void test_event_flood_is_lossy_and_counted(void)
{
    aar_event ev[MAX_EVENTS];
    uint32_t posted = 0u;
    uint32_t dropped_by_client;
    uint32_t seen = 0u;
    uint32_t n;

    setup(ALL_GROUPS);
    for (posted = 0u; posted < 100u; posted++) {
        aar_event e;

        memset(&e, 0, sizeof(e));
        e.type = AAR_EV_MOUSE;
        e.dx = (int16_t)posted;
        e.time_ms = posted;
        if (mock_client_post_event(&client, &e) != AAR_OK)
            break;
    }
    assert(posted > 0u && posted < 100u);
    dropped_by_client = mock_client_cb_get(&client, AAR_CB_EVENTS_DROPPED);
    assert(dropped_by_client >= 1u);
    /* Everything that fit is read, oldest first, nothing twice. */
    for (;;) {
        uint32_t i;

        n = poll_n(ev, MAX_EVENTS);
        if (n == 0u)
            break;
        for (i = 0u; i < n; i++) {
            assert(ev[i].type == AAR_EV_MOUSE);
            assert(ev[i].time_ms == seen);
            seen++;
        }
    }
    assert(seen == posted);
}

int main(void)
{
    test_lifecycle_without_input_group(NO_INPUT_GROUPS);
    test_lifecycle_without_input_group(0u);
    test_input_with_input_group();
    test_duplicate_stop_dropped();
    test_bare_stop_event_is_not_the_delivery();
    test_low_memory_per_crossing();
    test_short_buffer();
    test_bad_arguments();
    test_event_flood_is_lossy_and_counted();
    zzapp_test_set_groups(0u, 0);
    return 0;
}
