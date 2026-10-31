/*
 * Trusted app API table. File/display/input/audio bodies are Slice B seams.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stddef.h>
#include <stdint.h>
#ifdef ZZAPP_HOST_TEST
#include <stdlib.h>
#endif
#include "zzapp.h"
#include "zzapp_runtime.h"

#define WEAK __attribute__((weak))

WEAK void zzapp_services_poll(void) {}
WEAK int zzapp_services_take_start(uint32_t *argc) { (void)argc; return 0; }
WEAK uint8_t *zzapp_services_window(void) { return NULL; }
WEAK void zzapp_services_start_entered(int status) { (void)status; }
WEAK void zzapp_services_on_reset(void) {}
WEAK void zzapp_svc_log(const char *msg, uint32_t len) { (void)msg; (void)len; }
WEAK int zzapp_svc_should_stop(void) { return 0; }
WEAK int zzapp_svc_file_open(const char *p, uint32_t f) { (void)p; (void)f; return ZZ_ENOSYS; }
WEAK int zzapp_svc_file_close(int fd) { (void)fd; return ZZ_ENOSYS; }
WEAK int zzapp_svc_file_read(int fd, void *p, uint32_t n, uint32_t *o) { (void)fd; (void)p; (void)n; if (o) *o = 0; return ZZ_ENOSYS; }
WEAK int zzapp_svc_file_write(int fd, const void *p, uint32_t n, uint32_t *o) { (void)fd; (void)p; (void)n; if (o) *o = 0; return ZZ_ENOSYS; }
WEAK int zzapp_svc_file_seek(int fd, int64_t o, int w, int64_t *out) { (void)fd; (void)o; (void)w; if (out) *out = 0; return ZZ_ENOSYS; }
WEAK int zzapp_svc_file_stat(const char *p, zzapp_stat *o) { (void)p; (void)o; return ZZ_ENOSYS; }
WEAK int zzapp_svc_file_list_open(const char *p) { (void)p; return ZZ_ENOSYS; }
WEAK int zzapp_svc_file_list_next(int l, zzapp_dirent *o) { (void)l; (void)o; return ZZ_ENOSYS; }
WEAK int zzapp_svc_file_list_close(int l) { (void)l; return ZZ_ENOSYS; }
WEAK int zzapp_svc_display_open(uint32_t w,uint32_t h,uint32_t f,uint32_t fl,zzapp_surface *o) { (void)w;(void)h;(void)f;(void)fl;(void)o; return ZZ_ENOSYS; }
WEAK int zzapp_svc_display_flip(uint32_t id) { (void)id; return ZZ_ENOSYS; }
WEAK int zzapp_svc_display_vsync(void) { return ZZ_ENOSYS; }
WEAK int zzapp_svc_display_close(uint32_t id) { (void)id; return ZZ_ENOSYS; }
WEAK int zzapp_svc_input_poll(zzapp_event *o,uint32_t n,uint32_t *out) { (void)o;(void)n; if(out)*out=0; return ZZ_ENOSYS; }
WEAK int zzapp_svc_audio_open(uint32_t r,uint32_t c,zzapp_audio *o) { (void)r;(void)c;(void)o; return ZZ_ENOSYS; }
WEAK int zzapp_svc_audio_close(void) { return ZZ_ENOSYS; }
WEAK int zzapp_gl_bind(uint32_t id, const zzapp_gl **out) { (void)id; if(out)*out=NULL; return ZZ_ENOSYS; }
WEAK void zzapp_gl_swap(void) {}
WEAK void zzapp_gl_unbind(void) {}
WEAK void zzapp_gl_reclaim(void) {}

static void api_exit(int code)
{
    extern void zzapp_host_exit(int);
    zzapp_host_exit(code);
}
static void api_yield(void) { zzapp_api_enter(); zzapp_host_yield(); zzapp_api_leave(); }
static int api_stop(void) { return zzapp_svc_should_stop(); }
static uint64_t api_time(void) { return zzapp_host_time_us(); }
static void api_sleep(uint32_t us)
{
    uint64_t end = zzapp_host_time_us() + us;
    do { api_yield();
#ifdef ZZAPP_HOST_TEST
        break;
#endif
    } while (!api_stop() && zzapp_host_time_us() < end);
}
/* Firmware allocation is bounded by the app heap. The app's _sbrk uses this
 * same entry, avoiding a second bump pointer in the arena. */
static void *api_alloc(uint32_t bytes, uint32_t align, uint32_t flags)
{
    extern void *zzapp_host_alloc(uint32_t,uint32_t,uint32_t);
    return zzapp_host_alloc(bytes, align, flags);
}
static void api_free(void *p) { extern void zzapp_host_free(void *); zzapp_host_free(p); }
static void api_heap(void **b, uint32_t *n) { extern void zzapp_host_heap(void **,uint32_t *); zzapp_host_heap(b,n); }
#define WRAP0(name, call) static int name(void) { zzapp_api_enter(); int r=(call); zzapp_api_leave(); return r; }
#define WRAP1(name, type, call) static int name(type a) { zzapp_api_enter(); int r=(call); zzapp_api_leave(); return r; }
WRAP1(api_file_close,int,zzapp_svc_file_close(a))
WRAP1(api_list_open,const char *,zzapp_svc_file_list_open(a))
WRAP1(api_list_close,int,zzapp_svc_file_list_close(a))
WRAP1(api_display_flip,uint32_t,zzapp_svc_display_flip(a))
WRAP0(api_vsync,zzapp_svc_display_vsync())
WRAP1(api_display_close,uint32_t,zzapp_svc_display_close(a))
WRAP0(api_audio_close,zzapp_svc_audio_close())
static int api_file_open(const char *p,uint32_t f){zzapp_api_enter();int r=zzapp_svc_file_open(p,f);zzapp_api_leave();return r;}
static int api_file_read(int f,void*p,uint32_t n,uint32_t*o){zzapp_api_enter();int r=zzapp_svc_file_read(f,p,n,o);zzapp_api_leave();return r;}
static int api_file_write(int f,const void*p,uint32_t n,uint32_t*o){zzapp_api_enter();int r=zzapp_svc_file_write(f,p,n,o);zzapp_api_leave();return r;}
static int api_file_seek(int f,int64_t o,int w,int64_t*out){zzapp_api_enter();int r=zzapp_svc_file_seek(f,o,w,out);zzapp_api_leave();return r;}
static int api_file_stat(const char*p,zzapp_stat*out){zzapp_api_enter();int r=zzapp_svc_file_stat(p,out);zzapp_api_leave();return r;}
static int api_list_next(int l,zzapp_dirent*out){zzapp_api_enter();int r=zzapp_svc_file_list_next(l,out);zzapp_api_leave();return r;}
static int api_display_open(uint32_t w,uint32_t h,uint32_t f,uint32_t fl,zzapp_surface*out){zzapp_api_enter();int r=zzapp_svc_display_open(w,h,f,fl,out);zzapp_api_leave();return r;}
static int api_input(zzapp_event*out,uint32_t n,uint32_t*got){zzapp_api_enter();int r=zzapp_svc_input_poll(out,n,got);zzapp_api_leave();return r;}
static int api_audio_open(uint32_t r,uint32_t c,zzapp_audio*out){zzapp_api_enter();int x=zzapp_svc_audio_open(r,c,out);zzapp_api_leave();return x;}
static const zzapp_gl *api_gl_bind(uint32_t id){const zzapp_gl*out=NULL;zzapp_api_enter();if(zzapp_gl_bind(id,&out)!=ZZ_OK)out=NULL;zzapp_api_leave();return out;}
static void api_gl_swap(void){zzapp_api_enter();zzapp_gl_swap();zzapp_api_leave();}
static void api_gl_unbind(void){zzapp_api_enter();zzapp_gl_unbind();zzapp_api_leave();}

static const zzapp_api g_api = {
    .size=sizeof(g_api), .version=ZZAPP_API_VERSION,
    .exit=api_exit,.yield=api_yield,.should_stop=api_stop,.time_us=api_time,.sleep_us=api_sleep,
    .alloc=api_alloc,.free=api_free,.heap_range=api_heap,.log=zzapp_svc_log,
    .file_open=api_file_open,.file_close=api_file_close,.file_read=api_file_read,.file_write=api_file_write,.file_seek=api_file_seek,.file_stat=api_file_stat,.file_list_open=api_list_open,.file_list_next=api_list_next,.file_list_close=api_list_close,
    .display_open=api_display_open,.display_flip=api_display_flip,.display_vsync=api_vsync,.display_close=api_display_close,.input_poll=api_input,.audio_open=api_audio_open,.audio_close=api_audio_close,
    .gl_bind=api_gl_bind,.gl_swap=api_gl_swap,.gl_unbind=api_gl_unbind
};
const zzapp_api *zzapp_api_table(void) { return &g_api; }
