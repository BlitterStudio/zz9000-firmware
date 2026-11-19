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
#include "zzapp_state.h"
#define ZZAPP_TICK_US          5000u
/* Core 1 no longer answers at all (no app progress and no timer tick). */
#define ZZAPP_WATCHDOG_US      2000000u
#define ZZAPP_STOP_GRACE_US    500000u
/* Legacy launchers refresh the app heartbeat from their work loop. */
#define ZZAPP_LEASE_US         2000000u
/* Advertised launchers tolerate a short Amiga scheduling blackout. */
#define ZZAPP_LAUNCHER_LEASE_US 10000000u
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

#ifdef ZZAPP_HOST_TEST
/* Host harness only: the size of the malloc'd window that stands in for an
 * app's private memory. The card has no fixed-size region. A scale build
 * with many live apps overrides it to keep the virtual footprint bounded. */
#ifndef ZZAPP_HOST_WINDOW_BYTES
#define ZZAPP_HOST_WINDOW_BYTES (96u << 20)
#endif
#endif

/* zzapp_loader_end() keeps the deferred LOAD_END request running while it
 * zeroes a private frame batch; this never crosses the mailbox ABI. */
#define ZZAPP_LOADER_IN_PROGRESS 1
#define ZZAPP_STACK_DEFAULT    (1u << 20)
/* Largest stack a note may ask for on this platform (spec 2.3). */
#define ZZAPP_STACK_MAX        (1u << 20)
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
    uint32_t image_frames[ZZAPP_MEM_MAX_FRAMES];
    uint32_t image_frame_count;
    uint32_t heap_base;
    uint32_t heap_start;
    uint32_t heap_break;
    uint32_t heap_limit;
    uint32_t heap_frames[ZZAPP_MEM_MAX_FRAMES];
    uint32_t heap_frame_count;
    uint32_t stack_base;
    uint32_t stack_top;
    uint32_t stack_frames[1];
    uint32_t stack_frame_count;
    uint32_t stack_bytes;
    uint32_t host_visible_used;
    uint32_t display_lease_base;
    uint32_t display_lease_bytes;
    uint32_t display_lease_generation;
    uint32_t audio_direct;
    uint32_t image_sessions;
    uint32_t audio_sessions;
    void *coherent;
    uint64_t watchdog_stamp;
    uint64_t lease_stamp;
    uint64_t stop_stamp;
} zzapp_proc_t;


/* The core heartbeat sits at the head of the coherent task-queue region.
 * Core 0 owns this word. It is not core-1 BSS: a cold restart does not zero
 * BSS. Per-app coherent state lives in the process record (zzapp_rec.h). */
#define ZZAPP_COHERENT_OFFSET  0x0003F000u

struct aar_surface;
struct aar_stat;
struct aar_dirent;
struct aar_event;
struct aar_audio;
struct aar_gl;

/* ---- B calls these. Strong in this slice. ---- */
int zzapp_loader_begin(void);
int zzapp_loader_chunk(uint32_t file_off, const void *src, uint32_t len);
int zzapp_loader_end(void);
int zzapp_loader_abort(void);
int zzapp_loader_set_generation(uint32_t generation);
int zzapp_loader_alloc_back(uint32_t bytes, uint32_t *addr_out);
void zzapp_host_yield(void);
void zzapp_host_yield_wait(void);
void zzapp_host_sleep(uint32_t us);
uint64_t zzapp_host_time_us(void);
uint64_t zzapp_ticks_to_us(uint64_t ticks);
/* Release every resource the loader holds for the selected record (staging,
 * frames, address space, commit) without ending the claim: the record, its
 * L2 reserve and its handle stay. */
void zzapp_loader_release(void);
/* Retire stages the loader owns, called in this order by zzapp_rec_retire()
 * (and together by zzapp_loader_release()) for the selected record:
 * stage 3 returns host-visible blocks, API resources, staging and the back
 * buffer; stage 5 returns frames and clears the process and loader state.
 * Between them the caller destroys the address space (stage 4). */
void zzapp_loader_retire_resources(void);
void zzapp_loader_retire_commit(void);
/*
 * The single teardown path of a process record, idempotent and staged
 * (zzapp_retire_stage_t marks the last completed stage). Core 1, or core 0
 * with core 1 halted, or a CLAIMED record that never loaded. Stops the
 * thread, returns every resource of the record, then lets services detach
 * the wire (zzapp_services_retire_wire), which holds the shell. The
 * callers that end an app (exit, fault, reset) run set_terminal, on_app_end
 * and the GL reclaim first, as before.
 *   1   the record is FREE (do not touch it afterwards)
 *   0   the shell is held (reclaimed), services frees it later
 *  -1   no such record
 */
int zzapp_rec_retire(uint32_t pid);
uint32_t zzapp_loader_state(void);
void zzapp_loader_set_state(uint32_t state);
void zzapp_loader_work_begin_pass(void);
uint32_t zzapp_loader_work_last_bytes(void);
uint32_t zzapp_loader_work_last_kind(void);
/* Union of the PF_X/PF_W/PF_R flags ({vaddr, memsz, flags} per PT_LOAD)
 * of every segment that touches the image page starting at `page`; the page
 * holding `image_end` is also writable because the heap follows it. */
uint32_t zzapp_loader_page_flags(const uint32_t (*segs)[3], uint32_t nseg,
                                 uint32_t image_end, uint32_t page);
uint32_t zzapp_loader_reset_requested(void);

/* ---- firmware hooks ---- */
void zzapp_boot_init(void);
void zzapp_worker_hook(void);
void zzapp_core0_poll(void);
void zzapp_recover_halted_core1(void);
/* Every cross-core lock core 1 could hold. Before context retirement. */
void zzapp_recover_external_locks(void);
void zzapp_on_core1_restart(void);
void zzapp_on_amiga_reset(void);
/* How long core 0 waits for every live record to acknowledge a reset: one
 * shared deadline, 1 s plus 20 ms per live record, at most 4 s. */
uint32_t zzapp_reset_deadline_us(uint32_t live_records);
void zzapp_private_timer_init_core1(void);
void scheduler_core1_service_once(void);
/* One bounded service step from an app's tick; clears the doorbell when
 * the queue drains. */
void scheduler_core1_service_from_app(void);

/* spsr is the interrupted CPSR. Only user mode is an app fault. */
int zzapp_fault_is_app(uint32_t pc, uint32_t abort_lr, uint32_t spsr);
void zzapp_take_app_fault(uint32_t fault, uint32_t pc, uint32_t sp,
                          uint32_t lr, uint32_t far, uint32_t fsr,
                          uint32_t cpsr);

void zzapp_api_enter(void);
void zzapp_api_leave(void);
void zzapp_api_touch(void);
int zzapp_host_stop_requested(void);
void zzapp_host_latch_stop(uint32_t pid);
void zzapp_host_clear_stop_requested(uint32_t pid);
void zzapp_host_revoke_visible(void);
void zzapp_host_commit_begin(void);
void zzapp_host_commit_end(void);
void zzapp_note_progress(void);
uint32_t zzapp_current_generation(void);
uint32_t zzapp_runtime_state(void);
uint32_t zzapp_runtime_caps(void);
/* AAR_GROUP_* bits this runtime implements: what the table's `groups`
 * word and aar_info.groups report, and what a note's groups_required is
 * checked against. */
uint32_t zzapp_runtime_groups(void);
void zzapp_host_low_memory(void *context);
int zzapp_host_take_low_memory_event(uint32_t pid);
/* 1 once per stop request, when the app has not yet been told through
 * event_poll; 0 otherwise. */
int zzapp_host_take_stop_event(uint32_t pid);
/* The launcher's own AAR_EV_STOP counts as the delivery of that request.
 * Returns nonzero when the request had already been delivered. */
int zzapp_host_note_stop_event_delivered(uint32_t pid);
void zzapp_host_register_low_memory_callback(zzapp_mem_t *memory);
void zzapp_host_reset_low_memory(uint32_t pid);
/* Services admitted and published this record. */
void zzapp_host_claimed(uint32_t pid);

/* 1: core2 must fall through to record_fault (cold restart).
 * 0: user-mode app fault, already force-stopped, do not restart.
 * A fault in SYS or SVC, including inside an API body, restarts. */
int zzapp_fault_wants_restart(uint32_t pc, uint32_t abort_lr, uint32_t spsr);

const struct aar_api *zzapp_api_table(void);

/* The process of record `pid`; NULL for a pid outside the arena. */
zzapp_proc_t *zzapp_proc_slot(uint32_t pid);
int zzapp_proc_heap_grow(uint32_t bytes, void **old_break);
int zzapp_proc_heap_shrink(uint32_t bytes, void **new_break);
int zzapp_proc_heap_info(uint32_t *committed, uint32_t *limit,
                         uint32_t *pool_free);
void zzapp_loader_memory_init(void);
int zzapp_loader_heap_grow(uint32_t bytes);
int zzapp_loader_heap_shrink(uint32_t bytes);
zzapp_kernel_t *zzapp_runtime_kernel(void);
uint32_t zzapp_loader_pool_free(void);

/* ---- Host-visible (shared) blocks, info and heap range of the current app. */
struct aar_shared;
struct aar_info;
void *zzapp_host_alloc(uint32_t bytes, uint32_t align);
int zzapp_host_free(void *p);
int zzapp_host_shared_alloc(uint64_t bytes, uint32_t align, uint32_t flags,
                            struct aar_shared *out);
int zzapp_host_shared_range(const void *p, uint64_t bytes);
int zzapp_host_info(struct aar_info *out);
void zzapp_host_heap(void **base, uint32_t *bytes);
int zzapp_host_faulted(void);

/* ---- B defines these. Weak no-ops here. All of them name a record by pid
 * (the process-record index). ---- */
struct zzapp_rec;
void zzapp_services_poll(void);
int zzapp_services_take_start(uint32_t *argc);
/* The record the calling context works on: the running app's own thread, or
 * the record the system thread selected. ZZAPP_PID_NONE when there is none. */
uint32_t zzapp_services_current_pid(void);
void zzapp_services_select_pid(uint32_t pid);
uint32_t zzapp_services_state(void);
uint32_t zzapp_services_memory_limit(void);
/* Per-app host-visible limit in bytes the launcher requested at LOAD_BEGIN;
 * zero selects ZZAPP_HOST_VISIBLE_DEFAULT_BYTES. */
uint32_t zzapp_services_host_visible_limit(uint32_t pid);
/* Largest host-visible limit the card can honour: the surface heap, capped
 * by one app's alias window. */
uint32_t zzapp_host_visible_capacity(void);
int zzapp_services_wait_registered(uint32_t pid);
int zzapp_services_start_entered(uint32_t generation);
/* Once, after the host retired every record. */
void zzapp_services_on_reset(void);
/* Stage 3 resource hook of an ending app (grants, audio, fds, waits). */
void zzapp_services_on_app_end(void);
void zzapp_services_cancel_start(int status);
void zzapp_services_on_launch(void);
void zzapp_services_set_terminal(uint32_t reason);
void zzapp_services_expire_claims(uint64_t now_us);
/* Stages 6 and 7 of a retire: the control block reads IDLE with the end
 * reason and the record becomes RECLAIMED, holding its queues and buffers for
 * the client. 1 freed (its release was requested: do not touch the record
 * again), 0 shell held until RELEASE, a stale request of a quarantined claim,
 * or reset. Idempotent. */
int zzapp_services_retire_wire(struct zzapp_rec *rec);
/* A resource failure of the selected record (AAR_REASON_*), noted before
 * LOAD_END or START returns so the lifecycle word carries the reason. */
void zzapp_services_note_refusal(uint32_t reason);

void zzapp_svc_log(const char *msg, uint32_t len);
int zzapp_svc_should_stop(void);
int zzapp_svc_file_open(const char *path, uint32_t flags);
int zzapp_svc_file_close(int fd);
int zzapp_svc_file_read(int fd, void *buf, uint32_t len, uint32_t *out_n);
int zzapp_svc_file_write(int fd, const void *buf, uint32_t len, uint32_t *out_n);
int zzapp_svc_file_seek(int fd, int64_t off, int whence, int64_t *out_pos);
int zzapp_svc_file_stat(const char *path, struct aar_stat *out);
int zzapp_svc_file_list_open(const char *path);
int zzapp_svc_file_list_next(int list, struct aar_dirent *out);
int zzapp_svc_file_list_close(int list);
int zzapp_svc_display_open(uint32_t w, uint32_t h, uint32_t format,
                           uint32_t flags, struct aar_surface *out);
int zzapp_svc_display_flip(uint32_t surface_id);
int zzapp_svc_display_flip_async(uint32_t surface_id);
int zzapp_display_flip_idle(void);
int zzapp_svc_display_refresh(struct aar_surface *out);
int zzapp_svc_display_set_palette(uint32_t surface_id, uint32_t first,
                                  uint32_t count, const uint32_t *rgb);
uint32_t zzapp_svc_display_latest_dropped(void);
int zzapp_svc_display_vsync(void);
int zzapp_svc_event_poll(struct aar_event *out, uint32_t max, uint32_t *out_n);
int zzapp_svc_audio_open(uint32_t rate, uint32_t channels, struct aar_audio *out);
int zzapp_svc_audio_close(void);

/* ---- MiniGL module binding. ---- */
int zzapp_minigl_bind(uint32_t surface_id, const struct aar_gl **out);
int zzapp_minigl_bind_flags(uint32_t surface_id, uint32_t flags,
                            const struct aar_gl **out);
int zzapp_minigl_swap(void);
void zzapp_minigl_unbind(void);
void zzapp_minigl_reclaim(uint32_t generation);
/* Core 0, while core 1 is halted: drops the binding without module code. */
void zzapp_minigl_forget(void);

#ifdef ZZAPP_HOST_TEST
void zzapp_test_set_xtime(uint64_t ticks);
void zzapp_test_set_entry(void (*fn)(const void *api, int argc, char **argv));
void zzapp_host_tick_at(uint64_t now);
void zzapp_test_system_tick_at(uint64_t now);
void zzapp_test_mark_running(void);
uint32_t zzapp_test_service_calls(void);
uint32_t zzapp_test_in_api(void);
uint32_t zzapp_test_arena_card(void);
uint8_t *zzapp_test_arena_mem(void);
void zzapp_test_set_groups(uint32_t groups, int override);
uint32_t zzapp_test_loader_zero_last_bytes(void);
uint32_t zzapp_test_loader_work_last_bytes(void);
uint32_t zzapp_test_loader_mapping_calls(void);
int zzapp_test_cold_restarts(void);
uint32_t zzapp_test_exit_code(void);
uint32_t zzapp_test_exit_state(uint32_t pid);
void zzapp_test_set_wait_registered(uint32_t pid, int registered);
/* Admit the next record (its index is its pid) and select it. The services
 * layer does this at claim; harness tests without services call it. Returns
 * the pid, or ZZAPP_PID_NONE when the arena is full. */
uint32_t zzapp_test_admit(void);
/* Capacity of the next zzapp_boot_init() arena, MIN_APPS..ZZAPP_HOST_ARENA_APPS;
 * 0 boots without an arena, as a card whose boot carve found no memory. */
void zzapp_test_set_arena_apps(uint32_t apps);
/* Stop zzapp_rec_retire() after the stage with this number
 * (zzapp_retire_stage_t: 1 DETACHED .. 4 COMMIT), as a core-1 halt would; 0
 * runs it to the end. */
extern unsigned zzapp_test_retire_halt_stage;
#endif

#endif /* ZZAPP_RUNTIME_H */
