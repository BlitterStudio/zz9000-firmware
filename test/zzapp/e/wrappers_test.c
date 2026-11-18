/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "host_loader.h"
#include "mock_api.h"
#include "mock_window.h"

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
    uint8_t *w = zzapp_mock_window_new(); aar_api api; aar_event ev; struct zzapp_file_wire f; char line[16]; int code; unsigned i;
    CHECK(w); zzapp_mock_api_init(&api, w); api.gl_bind = bind_gl; api.display_flip_async = flip_async; api.display_flip_idle = flip_idle;
    CHECK(zzapp_host_enter(&api, app_main, 0, 0, &code) == AAR_OK); CHECK(code == 7); CHECK(calls == 9); CHECK(zzapp_log_read(w, line, sizeof line) == 5); CHECK(strcmp(line, "hello") == 0);
    CHECK(aar_file_open("x", AAR_FILE_READ) == AAR_E_NOSYS);
    memset(&ev, 0, sizeof ev); ev.type = AAR_EV_KEY_DOWN; for (i = 0; i < ZZAPP_INPUT_CAP; i++) CHECK(zzapp_input_push(w, &ev) == 0); CHECK(zzapp_input_push(w, &ev) == -1); CHECK(zzapp_life_get(w, ZZAPP_LIFE_IN_DROP) == 1);
    memset(&f, 0, sizeof f); f.op = 3; f.fd = 3; f.length = 12; zzapp_file_store(w, &f); CHECK(zzapp_file_load(w, &f) == 0); zzapp_file_complete(w, AAR_OK, 12, 99, 1, 2); CHECK(zzapp_file_load(w, &f) == 0); CHECK(f.err == AAR_OK && f.out_n == 12 && f.out_pos == 99);
    zzapp_mock_window_free(w); return 0;
}
