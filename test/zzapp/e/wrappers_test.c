/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "host_loader.h"
#include "mock_api.h"

#include <stdio.h>
#include <string.h>

static int calls;
static void mock_begin(uint32_t mode) { calls += (int)mode; }
static const aar_gl *bind_gl(uint32_t id) { static aar_gl gl; (void)id; memset(&gl, 0, sizeof gl); gl.size = sizeof gl; gl.Begin = mock_begin; return &gl; }
static int flip_async(uint32_t id) { return (int)id + 10; }
static int flip_idle(void) { return 1; }
static int app_main(int argc, char **argv) { static const char msg[] = "hello"; (void)argc; (void)argv; aar_log(msg, 5); glBegin(9); if (!aar_gl_bind(1)) return 1; glBegin(9); aar_gl_unbind(); if (aar_display_flip_async(3) != 13 || aar_display_flip_idle() != 1) return 2; return 7; }

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

int main(void)
{
    static zzapp_mock_app app;
    aar_api api;
    aar_event ev;
    char line[16];
    int code;
    unsigned i;

    zzapp_mock_api_init(&api, &app);
    api.gl_bind = bind_gl; api.display_flip_async = flip_async; api.display_flip_idle = flip_idle;
    CHECK(zzapp_host_enter(&api, app_main, 0, 0, &code) == AAR_OK); CHECK(code == 7); CHECK(calls == 9);
    CHECK(zzapp_mock_app_log_read(&app, line, sizeof line) == 5); CHECK(strcmp(line, "hello") == 0);
    CHECK(aar_file_open("x", AAR_FILE_READ) == AAR_E_NOSYS);
    /* Events are lossy: the newest is dropped and counted when the queue is full. */
    memset(&ev, 0, sizeof ev); ev.type = AAR_EV_KEY_DOWN;
    for (i = 0; i < ZZAPP_MOCK_EVENTS; i++) CHECK(zzapp_mock_app_event_push(&app, &ev) == 0);
    CHECK(zzapp_mock_app_event_push(&app, &ev) == -1); CHECK(app.events_dropped == 1);
    return 0;
}
