/*
 * Trusted ARM app runtime, internal firmware interface.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Slice A owns this header. Slice B includes zzapp_proto.h and calls the
 * loader/host symbols below. Slice B defines the zzapp_services_* and
 * zzapp_svc_* symbols; the weak copies here let this slice link alone.
 */
#ifndef ZZAPP_RUNTIME_H
#define ZZAPP_RUNTIME_H

#include <stdint.h>

#include "zzapp_as.h"
#include "zzapp_kernel.h"
#include "zzapp_mem.h"
#include "zzapp_proto.h"
#define ZZAPP_TICK_US          5000u
/* Core 1 no longer answers at all (no app progress and no timer tick). */
#define ZZAPP_WATCHDOG_US      2000000u
#define ZZAPP_STOP_GRACE_US    500000u
#define ZZAPP_LEASE_US         2000000u
#define ZZAPP_GL_CALL_LOG_US   50000u
/*
 * Automatic stops of an app that is still taking timer ticks.  Ctrl-C and
 * the close gadget stop such an app at any time, so these only catch apps
 * that are left alone: they must clear legitimate long work (GLQuake's
 * level load computes for seconds between API calls; the first GL call
 * loads MINIGL.ZZM from SD).  2 s for both stopped ARM GLQuake while it
 * started its first frame.
 */
#define ZZAPP_APP_STALL_US     30000000u
#define ZZAPP_API_STUCK_US     10000000u
#define ZZAPP_FILE_TIMEOUT_US  30000000u

#define ZZAPP_ARENA_BYTES      (96u << 20)
#define ZZAPP_STACK_DEFAULT    (1u << 20)
#define ZZAPP_STACK_MIN        (64u << 10)
#define ZZAPP_FILE_MAX         ZZAPP_IMAGE_MAX_BYTES
#define ZZAPP_CANARY_BYTES     64u
#define ZZAPP_IMAGE_SLACK      (64u << 10)
#define ZZAPP_LOG_RING_PER_SEC 32u
#define ZZAPP_LOG_UART_PER_SEC 8u
#define ZZAPP_ENTRY_FPSCR      0u

typedef struct zzapp_proc {
    zzapp_mem_proc_t memory;
    zzapp_as_t as;
    uint32_t owner;
    uint32_t generation;
    uint32_t image_entry;
    uint32_t image_end;
    uint32_t image_frames[ZZAPP_IMAGE_MAX_BYTES / ZZAPP_MEM_FRAME_BYTES];
    uint32_t image_frame_count;
    uint32_t heap_base;
    uint32_t heap_break;
    uint32_t heap_limit;
    uint32_t heap_frames[ZZAPP_MEM_MAX_FRAMES];
    uint32_t heap_frame_count;
    uint32_t stack_base;
    uint32_t stack_top;
    uint32_t stack_frames[1];
    uint32_t stack_frame_count;
    uint32_t stack_bytes;
    uint32_t note_minor;
    uint32_t host_visible_used;
    uint32_t display_lease_base;
    uint32_t display_lease_bytes;
    uint32_t display_lease_generation;
    uint32_t audio_direct;
    uint32_t image_sessions;
    uint32_t audio_sessions;
    void *launcher_window;
    void *coherent;
    uint64_t watchdog_stamp;
    uint64_t lease_stamp;
    uint64_t stop_stamp;
} zzapp_proc_t;


/* Just below the image-session table in the coherent task-queue region.
 * Core 0 owns this block. It is not core-1 BSS: a cold restart does not
 * zero BSS, and a stale running flag would skip reclaim. */
#define ZZAPP_COHERENT_OFFSET  0x0003F000u

struct zzapp_surface;
struct zzapp_stat;
struct zzapp_dirent;
struct zzapp_event;
struct zzapp_audio;
struct zzapp_gl;

/* ---- B calls these. Strong in this slice. ---- */
int zzapp_loader_begin(void);
int zzapp_loader_chunk(uint32_t file_off, const void *src, uint32_t len);
int zzapp_loader_end(void);
int zzapp_loader_abort(void);
int zzapp_loader_alloc_back(uint32_t bytes, uint32_t *addr_out);
void zzapp_host_yield(void);
void zzapp_host_yield_wait(void);
void zzapp_host_sleep(uint32_t us);
uint64_t zzapp_host_time_us(void);
void zzapp_loader_release(void);
uint32_t zzapp_loader_state(void);
void zzapp_loader_set_state(uint32_t state);
uint32_t zzapp_loader_reset_requested(void);

/* ---- firmware hooks ---- */
void zzapp_boot_init(void);
void zzapp_worker_hook(void);
void zzapp_core0_poll(void);
void zzapp_on_core1_restart(void);
void zzapp_on_amiga_reset(void);
void zzapp_private_timer_init_core1(void);
/* Fire the running app's service tick now (doorbell SGI on core 1). */
void zzapp_private_timer_kick(void);
void scheduler_core1_service_once(void);
/* One bounded service step from an app's tick; clears the doorbell when
 * the queue drains. */
void scheduler_core1_service_from_app(void);

int zzapp_fault_is_app(uint32_t pc, uint32_t abort_lr);
void zzapp_take_app_fault(uint32_t fault, uint32_t pc, uint32_t sp,
                          uint32_t lr, uint32_t far, uint32_t fsr,
                          uint32_t cpsr);

void zzapp_api_enter(void);
void zzapp_api_leave(void);
void zzapp_api_touch(void);
void zzapp_host_commit_begin(void);
void zzapp_host_commit_end(void);
void zzapp_note_progress(void);
uint32_t zzapp_current_generation(void);
uint32_t zzapp_runtime_state(void);
uint32_t zzapp_runtime_caps(void);
void zzapp_status_observed(void);
int zzapp_flip_ack_is_current(uint32_t gen);

/* 1: core2 must fall through to record_fault (cold restart).
 * 0: app PC, already force-stopped, do not restart. */
int zzapp_fault_wants_restart(uint32_t pc, uint32_t abort_lr);

const struct zzapp_api *zzapp_api_table(void);

zzapp_proc_t *zzapp_proc_slot(uint32_t slot);
int zzapp_proc_heap_grow(uint32_t bytes, void **old_break);
int zzapp_proc_heap_shrink(uint32_t bytes, void **new_break);
int zzapp_proc_heap_info(uint32_t *committed, uint32_t *limit,
                         uint32_t *pool_free);
void zzapp_loader_memory_init(void);
int zzapp_loader_heap_grow(uint32_t bytes);
int zzapp_loader_heap_shrink(uint32_t bytes);
zzapp_kernel_t *zzapp_runtime_kernel(void);
uint32_t zzapp_loader_pool_free(void);

/* ---- B defines these. Weak no-ops here. ---- */
void zzapp_services_poll(void);
int zzapp_services_take_start(uint32_t *argc);
uint8_t *zzapp_services_window(void);
uint32_t zzapp_services_current_slot(void);
void zzapp_services_select_slot(uint32_t slot);
uint32_t zzapp_services_state(void);
uint32_t zzapp_services_memory_limit(void);
int zzapp_services_wait_registered(uint32_t slot);
void zzapp_services_start_entered(int status);
void zzapp_services_on_reset(void);
void zzapp_services_on_app_end(void);
void zzapp_services_cancel_start(int status);
void zzapp_services_on_launch(void);
void zzapp_services_set_terminal(uint32_t reason);
void zzapp_services_expire_claims(uint64_t now_us);

void zzapp_svc_log(const char *msg, uint32_t len);
int zzapp_svc_should_stop(void);
int zzapp_svc_file_open(const char *path, uint32_t flags);
int zzapp_svc_file_close(int fd);
int zzapp_svc_file_read(int fd, void *buf, uint32_t len, uint32_t *out_n);
int zzapp_svc_file_write(int fd, const void *buf, uint32_t len, uint32_t *out_n);
int zzapp_svc_file_seek(int fd, int64_t off, int whence, int64_t *out_pos);
int zzapp_svc_file_stat(const char *path, struct zzapp_stat *out);
int zzapp_svc_file_list_open(const char *path);
int zzapp_svc_file_list_next(int list, struct zzapp_dirent *out);
int zzapp_svc_file_list_close(int list);
int zzapp_svc_display_open(uint32_t w, uint32_t h, uint32_t format,
                           uint32_t flags, struct zzapp_surface *out);
int zzapp_svc_display_flip(uint32_t surface_id);
int zzapp_svc_display_flip_async(uint32_t surface_id);
int zzapp_display_flip_idle(void);
int zzapp_svc_display_refresh(struct zzapp_surface *out);
int zzapp_svc_display_set_palette(uint32_t surface_id, uint32_t first,
                                  uint32_t count, const uint32_t *rgb);
uint32_t zzapp_svc_display_mailbox_replaced(void);
int zzapp_svc_display_vsync(void);
int zzapp_svc_input_poll(struct zzapp_event *out, uint32_t max, uint32_t *out_n);
int zzapp_svc_audio_open(uint32_t rate, uint32_t channels, struct zzapp_audio *out);
int zzapp_svc_audio_close(void);

/* ---- MiniGL module binding. ---- */
int zzapp_minigl_bind(uint32_t surface_id, const struct zzapp_gl **out);
int zzapp_minigl_bind_flags(uint32_t surface_id, uint32_t flags,
                            const struct zzapp_gl **out);
void zzapp_minigl_swap(void);
void zzapp_minigl_unbind(void);
void zzapp_minigl_reclaim(uint32_t generation);
/* Core 0, while core 1 is halted: drops the binding without module code. */
void zzapp_minigl_forget(void);

#ifdef ZZAPP_HOST_TEST
void zzapp_test_set_xtime(uint64_t ticks);
void zzapp_test_set_entry(void (*fn)(const void *api, int argc, char **argv));
void zzapp_host_tick_at(uint64_t now);
void zzapp_test_mark_running(void);
uint32_t zzapp_test_service_calls(void);
uint32_t zzapp_test_in_api(void);
uint32_t zzapp_test_arena_card(void);
uint8_t *zzapp_test_arena_mem(void);
int zzapp_test_cold_restarts(void);
uint32_t zzapp_test_exit_code(void);
void zzapp_test_set_wait_registered(uint32_t slot, int registered);
#endif

#endif /* ZZAPP_RUNTIME_H */
