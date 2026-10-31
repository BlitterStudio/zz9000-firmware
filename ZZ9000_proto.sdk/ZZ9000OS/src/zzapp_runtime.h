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

#define ZZAPP_TICK_US          5000u
#define ZZAPP_WATCHDOG_US      2000000u
#define ZZAPP_STOP_GRACE_US    500000u
#define ZZAPP_LEASE_US         2000000u
#define ZZAPP_GL_CALL_LOG_US   50000u
#define ZZAPP_API_STUCK_US     2000000u
#define ZZAPP_FILE_TIMEOUT_US  30000000u

#define ZZAPP_ARENA_BYTES      (96u << 20)
#define ZZAPP_STACK_DEFAULT    (1u << 20)
#define ZZAPP_STACK_MIN        (64u << 10)
#define ZZAPP_FILE_MAX         (16u << 20)
#define ZZAPP_CANARY_BYTES     64u
#define ZZAPP_IMAGE_SLACK      (64u << 10)
#define ZZAPP_LOG_RING_PER_SEC 32u
#define ZZAPP_LOG_UART_PER_SEC 8u

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
void zzapp_loader_free_back(uint32_t addr);
void zzapp_host_yield(void);
void zzapp_host_yield_wait(void);
uint64_t zzapp_host_time_us(void);
void zzapp_loader_release(void);

/* ---- firmware hooks ---- */
void zzapp_boot_init(void);
void zzapp_worker_hook(void);
void zzapp_core0_poll(void);
void zzapp_on_core1_restart(void);
void zzapp_on_amiga_reset(void);
void zzapp_private_timer_init_core1(void);
void scheduler_core1_service_once(void);

int zzapp_fault_is_app(uint32_t pc, uint32_t abort_lr);
void zzapp_take_app_fault(uint32_t fault, uint32_t pc, uint32_t sp,
                          uint32_t lr, uint32_t far, uint32_t fsr,
                          uint32_t cpsr);

void zzapp_api_enter(void);
void zzapp_api_leave(void);
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

/* ---- B defines these. Weak no-ops here. ---- */
void zzapp_services_poll(void);
int zzapp_services_take_start(uint32_t *argc);
uint8_t *zzapp_services_window(void);
void zzapp_services_start_entered(int status);
void zzapp_services_on_reset(void);

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
int zzapp_svc_display_vsync(void);
int zzapp_svc_display_close(uint32_t surface_id);
int zzapp_svc_input_poll(struct zzapp_event *out, uint32_t max, uint32_t *out_n);
int zzapp_svc_audio_open(uint32_t rate, uint32_t channels, struct zzapp_audio *out);
int zzapp_svc_audio_close(void);

/* ---- MiniGL module binding. ---- */
int zzapp_minigl_bind(uint32_t surface_id, const struct zzapp_gl **out);
void zzapp_minigl_swap(void);
void zzapp_minigl_unbind(void);
void zzapp_minigl_reclaim(uint32_t generation);

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
#endif

#endif /* ZZAPP_RUNTIME_H */
