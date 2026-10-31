/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mock_window.h"

#include <stdlib.h>
#include <string.h>

_Static_assert(ZZAPP_LIFE_OFF + ZZAPP_LIFE_BYTES == ZZAPP_INPUT_OFF, "life");
_Static_assert(ZZAPP_INPUT_OFF + ZZAPP_INPUT_BYTES == ZZAPP_LOG_OFF, "input");
_Static_assert(ZZAPP_LOG_OFF + ZZAPP_LOG_BYTES == ZZAPP_FILE_OFF, "log");
_Static_assert(ZZAPP_FILE_OFF + ZZAPP_FILE_BYTES == ZZAPP_PATH_OFF, "file");
_Static_assert(ZZAPP_PATH_OFF + ZZAPP_PATH_BYTES == ZZAPP_ARGV_OFF, "path");
_Static_assert(ZZAPP_ARGV_OFF + ZZAPP_ARGV_BYTES == ZZAPP_DATA_OFF, "argv");
_Static_assert(ZZAPP_DATA_OFF + ZZAPP_DATA_BYTES == ZZAPP_STAGING_OFF, "data");
_Static_assert(ZZAPP_STAGING_OFF + ZZAPP_STAGING_BYTES == 0x59b40u, "staging");
_Static_assert(sizeof(struct zzapp_file_wire) == ZZAPP_FILE_BYTES, "file wire");

uint16_t zzapp_be_u16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
void zzapp_be_set_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
uint32_t zzapp_be_u32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
void zzapp_be_set_u32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
uint64_t zzapp_be_u64(const uint8_t *p) { return ((uint64_t)zzapp_be_u32(p) << 32) | zzapp_be_u32(p + 4); }
void zzapp_be_set_u64(uint8_t *p, uint64_t v) { zzapp_be_set_u32(p, (uint32_t)(v >> 32)); zzapp_be_set_u32(p + 4, (uint32_t)v); }

uint8_t *zzapp_mock_window_new(void)
{
    uint8_t *w = calloc(1, ZZAPP_WINDOW_BYTES);
    if (w) { zzapp_life_set(w, ZZAPP_LIFE_MAGIC, 0x5a415050u); zzapp_life_set(w, ZZAPP_LIFE_LAYOUT, 1); }
    return w;
}
void zzapp_mock_window_free(uint8_t *window) { free(window); }
uint32_t zzapp_life_get(const uint8_t *w, uint32_t f) { return zzapp_be_u32(w + ZZAPP_LIFE_OFF + f); }
void zzapp_life_set(uint8_t *w, uint32_t f, uint32_t v) { zzapp_be_set_u32(w + ZZAPP_LIFE_OFF + f, v); }

static uint8_t *log_byte(uint8_t *w, uint32_t at) { return w + ZZAPP_LOG_OFF + 8u + (at % (ZZAPP_LOG_BYTES - 8u)); }
static void log_put(uint8_t *w, uint32_t at, uint8_t v) { *log_byte(w, at) = v; }
static uint8_t log_get(uint8_t *w, uint32_t at) { return *log_byte(w, at); }

int zzapp_log_write(uint8_t *w, const char *msg, uint32_t len)
{
    uint32_t head = zzapp_be_u32(w + ZZAPP_LOG_OFF), tail = zzapp_be_u32(w + ZZAPP_LOG_OFF + 4), i;
    uint32_t used = head - tail, rec = (2u + len + 3u) & ~3u, cap = ZZAPP_LOG_BYTES - 8u;
    if (len > 0xffffu || rec > cap - used) { zzapp_life_set(w, ZZAPP_LIFE_LOG_DROP, zzapp_life_get(w, ZZAPP_LIFE_LOG_DROP) + 1u); return -1; }
    log_put(w, head, (uint8_t)(len >> 8)); log_put(w, head + 1u, (uint8_t)len);
    for (i = 0; i < len; i++) log_put(w, head + 2u + i, (uint8_t)msg[i]);
    for (i = 2u + len; i < rec; i++) log_put(w, head + i, 0);
    zzapp_be_set_u32(w + ZZAPP_LOG_OFF, head + rec); return 0;
}
int zzapp_log_read(uint8_t *w, char *buf, uint32_t cap)
{
    uint32_t head = zzapp_be_u32(w + ZZAPP_LOG_OFF), tail = zzapp_be_u32(w + ZZAPP_LOG_OFF + 4), len, i, rec;
    if (tail == head) return 0;
    len = ((uint32_t)log_get(w, tail) << 8) | log_get(w, tail + 1u);
    if (cap <= len) return -1;
    for (i = 0; i < len; i++) buf[i] = (char)log_get(w, tail + 2u + i);
    buf[len] = 0; rec = (2u + len + 3u) & ~3u;
    zzapp_be_set_u32(w + ZZAPP_LOG_OFF + 4, tail + rec); return (int)len;
}

int zzapp_input_push(uint8_t *w, const zzapp_event *e)
{
    uint32_t head = zzapp_be_u32(w + ZZAPP_INPUT_OFF), tail = zzapp_be_u32(w + ZZAPP_INPUT_OFF + 4), at;
    uint8_t *p;
    if (head - tail >= ZZAPP_INPUT_CAP) { zzapp_life_set(w, ZZAPP_LIFE_IN_DROP, zzapp_life_get(w, ZZAPP_LIFE_IN_DROP) + 1u); return -1; }
    at = ZZAPP_INPUT_OFF + ZZAPP_INPUT_HDR + ((head & 255u) * ZZAPP_EVENT_BYTES); p = w + at;
    zzapp_be_set_u16(p, e->type); zzapp_be_set_u16(p + 2, e->code); zzapp_be_set_u16(p + 4, (uint16_t)e->dx); zzapp_be_set_u16(p + 6, (uint16_t)e->dy); zzapp_be_set_u16(p + 8, e->buttons); zzapp_be_set_u16(p + 10, e->reserved); zzapp_be_set_u32(p + 12, e->time_ms);
    zzapp_be_set_u32(w + ZZAPP_INPUT_OFF, head + 1u); return 0;
}
int zzapp_input_pop(uint8_t *w, zzapp_event *e)
{
    uint32_t head = zzapp_be_u32(w + ZZAPP_INPUT_OFF), tail = zzapp_be_u32(w + ZZAPP_INPUT_OFF + 4), at; uint8_t *p;
    if (tail == head) return 0;
    at = ZZAPP_INPUT_OFF + ZZAPP_INPUT_HDR + ((tail & 255u) * ZZAPP_EVENT_BYTES); p = w + at;
    e->type = zzapp_be_u16(p); e->code = zzapp_be_u16(p + 2); e->dx = (int16_t)zzapp_be_u16(p + 4); e->dy = (int16_t)zzapp_be_u16(p + 6); e->buttons = zzapp_be_u16(p + 8); e->reserved = zzapp_be_u16(p + 10); e->time_ms = zzapp_be_u32(p + 12);
    zzapp_be_set_u32(w + ZZAPP_INPUT_OFF + 4, tail + 1u); return 1;
}

static void file_put(uint8_t *p, const struct zzapp_file_wire *f)
{
    zzapp_be_set_u32(p, f->seq); zzapp_be_set_u32(p+4, f->op); zzapp_be_set_u32(p+8, (uint32_t)f->fd); zzapp_be_set_u32(p+12, (uint32_t)f->flags); zzapp_be_set_u32(p+16, (uint32_t)f->whence); zzapp_be_set_u32(p+20, (uint32_t)f->err); zzapp_be_set_u32(p+24, f->path_off); zzapp_be_set_u32(p+28, f->length); zzapp_be_set_u32(p+32, f->out_n); zzapp_be_set_u32(p+36, f->mode); zzapp_be_set_u64(p+40, f->offset); zzapp_be_set_u64(p+48, f->out_pos); zzapp_be_set_u32(p+56, f->mtime); zzapp_be_set_u32(p+60, f->reserved);
}
static void file_get(const uint8_t *p, struct zzapp_file_wire *f)
{
    f->seq=zzapp_be_u32(p); f->op=zzapp_be_u32(p+4); f->fd=(int32_t)zzapp_be_u32(p+8); f->flags=(int32_t)zzapp_be_u32(p+12); f->whence=(int32_t)zzapp_be_u32(p+16); f->err=(int32_t)zzapp_be_u32(p+20); f->path_off=zzapp_be_u32(p+24); f->length=zzapp_be_u32(p+28); f->out_n=zzapp_be_u32(p+32); f->mode=zzapp_be_u32(p+36); f->offset=zzapp_be_u64(p+40); f->out_pos=zzapp_be_u64(p+48); f->mtime=zzapp_be_u32(p+56); f->reserved=zzapp_be_u32(p+60);
}
void zzapp_file_store(uint8_t *w, const struct zzapp_file_wire *in)
{
    struct zzapp_file_wire f = *in; uint8_t *p = w + ZZAPP_FILE_OFF; f.seq = (f.seq | 1u); zzapp_be_set_u32(p, f.seq); file_put(p, &f); zzapp_be_set_u32(p, f.seq + 1u);
}
int zzapp_file_load(const uint8_t *w, struct zzapp_file_wire *out)
{
    const uint8_t *p = w + ZZAPP_FILE_OFF; uint32_t a, b; int n;
    for (n=0;n<8;n++) { a=zzapp_be_u32(p); if (a&1u) continue; file_get(p,out); b=zzapp_be_u32(p); if (a==b) { out->seq=b; return 0; } } return -1;
}
void zzapp_file_complete(uint8_t *w, int32_t err, uint32_t out_n, uint64_t out_pos, uint32_t mode, uint32_t mtime)
{
    struct zzapp_file_wire f; if (zzapp_file_load(w,&f)) memset(&f,0,sizeof f); f.err=err; f.out_n=out_n; f.out_pos=out_pos; f.mode=mode; f.mtime=mtime; zzapp_file_store(w,&f);
}
