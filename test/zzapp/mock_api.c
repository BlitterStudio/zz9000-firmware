/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mock_api.h"
#include "mock_window.h"

#include <stdlib.h>
#include <string.h>

static uint8_t *window_ptr;
static uint64_t now_us;
static uint8_t heap[1024 * 1024];
static uint8_t *display_buffer;
static uint32_t next_surface;

static void mock_yield(void) { now_us += 5000u; }
static int mock_should_stop(void) { return zzapp_life_get(window_ptr, ZZAPP_LIFE_STOP) != 0; }
static uint64_t mock_time_us(void) { return now_us; }
static void mock_sleep_us(uint32_t us) { now_us += us; }
static void *mock_alloc(uint32_t bytes, uint32_t align, uint32_t flags) { (void)align; (void)flags; return bytes <= sizeof heap ? heap : 0; }
static void mock_free(void *p) { (void)p; }
static void mock_heap_range(void **base, uint32_t *bytes) { if (base) *base = heap; if (bytes) *bytes = sizeof heap; }
static void mock_log(const char *msg, uint32_t len) { if (msg) zzapp_log_write(window_ptr, msg, len); }
static int mock_input(zzapp_event *out, uint32_t max, uint32_t *out_n) { uint32_t n = 0; while (n < max && zzapp_input_pop(window_ptr, out + n)) n++; if (out_n) *out_n = n; return ZZ_OK; }
static int mock_display_open(uint32_t w, uint32_t h, uint32_t fmt, uint32_t flags, zzapp_surface *out)
{
    uint32_t pitch = w * (fmt == ZZ_FMT_BGRA8888 ? 4u : 2u);
    free(display_buffer); display_buffer = calloc(h, pitch);
    if (!display_buffer) return ZZ_ENOMEM;
    memset(out, 0, sizeof *out); out->id = ++next_surface; out->width = w; out->height = h; out->pitch = pitch; out->format = fmt; out->front = display_buffer; out->back = display_buffer;
    zzapp_life_set(window_ptr, ZZAPP_LIFE_WIDTH, w); zzapp_life_set(window_ptr, ZZAPP_LIFE_HEIGHT, h); zzapp_life_set(window_ptr, ZZAPP_LIFE_PITCH, pitch); zzapp_life_set(window_ptr, ZZAPP_LIFE_FORMAT, fmt); zzapp_life_set(window_ptr, ZZAPP_LIFE_FLAGS, (flags & ZZ_DISPLAY_VSYNC) ? (1u << 9) : 0);
    return ZZ_OK;
}
static int mock_display_flip(uint32_t id) { uint32_t gen; (void)id; gen = zzapp_life_get(window_ptr, ZZAPP_LIFE_FLIP_GEN) + 1u; zzapp_life_set(window_ptr, ZZAPP_LIFE_FLIP_GEN, gen); zzapp_life_set(window_ptr, ZZAPP_LIFE_FLIP_ACK, gen); return ZZ_OK; }
static int mock_display_vsync(void) { return ZZ_OK; }
static int mock_display_close(uint32_t id) { (void)id; return ZZ_OK; }

void zzapp_mock_api_init(zzapp_api *api, uint8_t *window)
{
    memset(api, 0, sizeof *api); window_ptr = window; now_us = 0; next_surface = 0;
    api->size = sizeof *api; api->version = ZZAPP_API_VERSION;
    api->yield = mock_yield; api->should_stop = mock_should_stop; api->time_us = mock_time_us; api->sleep_us = mock_sleep_us;
    api->alloc = mock_alloc; api->free = mock_free; api->heap_range = mock_heap_range; api->log = mock_log;
    api->display_open = mock_display_open; api->display_flip = mock_display_flip; api->display_vsync = mock_display_vsync; api->display_close = mock_display_close; api->input_poll = mock_input;
}
void zzapp_mock_time_set(uint64_t usec) { now_us = usec; }
void zzapp_mock_time_advance(uint64_t usec) { now_us += usec; }
