/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "host_loader.h"
#include "mock_api.h"
#include "mock_window.h"

#include <stdio.h>
#include <string.h>

static int calls;
static void mock_begin(uint32_t mode) { calls += (int)mode; }
static const zzapp_gl *bind_gl(uint32_t id) { static zzapp_gl gl; (void)id; memset(&gl, 0, sizeof gl); gl.size = sizeof gl; gl.Begin = mock_begin; return &gl; }
static int app_main(int argc, char **argv) { static const char msg[] = "hello"; (void)argc; (void)argv; zz_log(msg, 5); glBegin(9); if (!zz_gl_bind(1)) return 1; glBegin(9); zz_gl_unbind(); return 7; }

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

int main(void)
{
    uint8_t *w = zzapp_mock_window_new(); zzapp_api api; zzapp_event ev; struct zzapp_file_wire f; char line[16]; int code; unsigned i;
    CHECK(w); zzapp_mock_api_init(&api, w); api.gl_bind = bind_gl;
    CHECK(zzapp_host_enter(&api, app_main, 0, 0, &code) == ZZ_OK); CHECK(code == 7); CHECK(calls == 9); CHECK(zzapp_log_read(w, line, sizeof line) == 5); CHECK(strcmp(line, "hello") == 0);
    CHECK(zz_file_open("x", ZZ_FILE_READ) == ZZ_ENOSYS);
    memset(&ev, 0, sizeof ev); ev.type = ZZ_EV_KEY_DOWN; for (i = 0; i < ZZAPP_INPUT_CAP; i++) CHECK(zzapp_input_push(w, &ev) == 0); CHECK(zzapp_input_push(w, &ev) == -1); CHECK(zzapp_life_get(w, ZZAPP_LIFE_IN_DROP) == 1);
    memset(&f, 0, sizeof f); f.op = 3; f.fd = 3; f.length = 12; zzapp_file_store(w, &f); CHECK(zzapp_file_load(w, &f) == 0); zzapp_file_complete(w, ZZ_OK, 12, 99, 1, 2); CHECK(zzapp_file_load(w, &f) == 0); CHECK(f.err == ZZ_OK && f.out_n == 12 && f.out_pos == 99);
    zzapp_mock_window_free(w); return 0;
}
