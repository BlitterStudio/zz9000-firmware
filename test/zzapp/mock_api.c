/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mock_api.h"

#include <stdlib.h>
#include <string.h>

static zzapp_mock_app *cur;
static uint64_t now_us;
static uint8_t heap[1024 * 1024];
static uint8_t *display_buffer;
static uint32_t next_surface;

static void mock_yield(void) { now_us += 5000u; }
static int mock_should_stop(void) { return cur->stop != 0; }
static uint64_t mock_time_us(void) { return now_us; }
static void mock_sleep_us(uint32_t us) { now_us += us; }
static void mock_heap_range(void **base, uint64_t *bytes) { if (base) *base = heap; if (bytes) *bytes = sizeof heap; }
static void mock_log(const char *msg, uint32_t len)
{
    char *line;

    if (!msg)
        return;
    if (cur->log_head - cur->log_tail >= ZZAPP_MOCK_LOG_LINES) {
        cur->log_dropped++;
        return;
    }
    line = cur->log[cur->log_head % ZZAPP_MOCK_LOG_LINES];
    if (len >= ZZAPP_MOCK_LOG_BYTES)
        len = ZZAPP_MOCK_LOG_BYTES - 1u;
    memcpy(line, msg, len);
    line[len] = 0;
    cur->log_head++;
}
static int mock_event_poll(aar_event *out, uint32_t max, uint32_t *out_n)
{
    uint32_t n = 0;

    while (n < max && cur->ev_tail != cur->ev_head)
        out[n++] = cur->events[cur->ev_tail++ % ZZAPP_MOCK_EVENTS];
    if (out_n)
        *out_n = n;
    return AAR_OK;
}
static int mock_display_open(uint32_t w, uint32_t h, uint32_t fmt, uint32_t flags, aar_surface *out)
{
    uint32_t pitch = w * (fmt == AAR_FMT_BGRA8888 ? 4u : 2u);
    free(display_buffer); display_buffer = calloc(h, pitch);
    if (!display_buffer) return AAR_E_NOMEM;
    memset(out, 0, sizeof *out); out->id = ++next_surface; out->width = w; out->height = h; out->pitch = pitch; out->format = fmt; out->front = display_buffer; out->back = display_buffer; out->layout = fmt == AAR_FMT_BGRA8888 ? AAR_LAYOUT_BGRA8888 : AAR_LAYOUT_RGB565_BE;
    cur->width = w; cur->height = h; cur->pitch = pitch; cur->format = fmt; cur->display_flags = flags;
    return AAR_OK;
}
static int mock_display_flip(uint32_t id) { (void)id; cur->flips++; return AAR_OK; }
static int mock_display_vsync(void) { return AAR_OK; }
static int mock_display_close(uint32_t id) { (void)id; return AAR_OK; }

void zzapp_mock_api_init(aar_api *api, zzapp_mock_app *app)
{
    memset(app, 0, sizeof *app);
    memset(api, 0, sizeof *api); cur = app; now_us = 0; next_surface = 0;
    api->size = sizeof *api; api->version = AAR_API_VERSION; api->platform_id = AAR_PLATFORM_ZZ9000;
    api->yield = mock_yield; api->should_stop = mock_should_stop; api->time_us = mock_time_us; api->sleep_us = mock_sleep_us;
    api->heap_range = mock_heap_range; api->log = mock_log; api->event_poll = mock_event_poll;
    api->display_open = mock_display_open; api->display_flip = mock_display_flip; api->display_vsync = mock_display_vsync; api->display_close = mock_display_close;
}
void zzapp_mock_time_set(uint64_t usec) { now_us = usec; }
void zzapp_mock_time_advance(uint64_t usec) { now_us += usec; }

int zzapp_mock_app_log_read(zzapp_mock_app *app, char *buf, uint32_t cap)
{
    const char *line;
    uint32_t len;

    if (app->log_tail == app->log_head)
        return 0;
    line = app->log[app->log_tail % ZZAPP_MOCK_LOG_LINES];
    len = (uint32_t)strlen(line);
    if (cap <= len)
        return -1;
    memcpy(buf, line, len + 1u);
    app->log_tail++;
    return (int)len;
}

int zzapp_mock_app_event_push(zzapp_mock_app *app, const aar_event *ev)
{
    if (app->ev_head - app->ev_tail >= ZZAPP_MOCK_EVENTS) {
        app->events_dropped++;
        return -1;
    }
    app->events[app->ev_head++ % ZZAPP_MOCK_EVENTS] = *ev;
    return 0;
}
