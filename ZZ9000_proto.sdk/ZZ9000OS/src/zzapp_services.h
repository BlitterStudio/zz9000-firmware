/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Trusted-app services: rings, files, input, log, display bind, audio.
 * Apps do not include this header. zzapp_proto.h is the wire layout.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZAPP_SERVICES_H
#define ZZAPP_SERVICES_H

#include <stdint.h>

struct zzapp_event;
struct zzapp_stat;
struct zzapp_dirent;
struct zzapp_surface;
struct zzapp_audio;

/* SDK status numbers. Match sdk_mailbox.h; guarded so either include order works. */
#ifndef SDK_STATUS_OK

#define SDK_STATUS_OK              0u
#define SDK_STATUS_QUEUED          1u
#define SDK_STATUS_BUSY            2u
#define SDK_STATUS_UNSUPPORTED     3u
#define SDK_STATUS_BAD_REQUEST     4u
#define SDK_STATUS_NO_MEMORY       6u
#define SDK_STATUS_IO_ERROR        9u
#define SDK_STATUS_NOT_FOUND       10u
#endif

/* Amiga reset: forget the window (the host heap is already reset) and detach audio. */
void zzapp_services_on_reset(void);

void zzapp_services_poll(void);
void zzapp_services_core0_poll(void);
void zzapp_services_set_window_mapping_available(int available);

uint8_t *zzapp_services_window(void);
uint32_t zzapp_services_generation(void);
uint32_t zzapp_services_state(void);
int zzapp_services_stop_requested(void);
void zzapp_services_set_state(uint32_t state);
void zzapp_services_set_generation(uint32_t generation);
void zzapp_services_set_fault(uint32_t code, uint32_t pc, uint32_t sp,
			      uint32_t lr, uint32_t far, uint32_t fsr,
			      uint32_t cpsr);
void zzapp_services_set_exit(int32_t code);
void zzapp_services_note_progress(uint32_t lo, uint32_t hi);
void zzapp_services_or_flags(uint32_t flags);
void zzapp_services_and_flags(uint32_t mask);
uint32_t zzapp_services_flags(void);

/* After poll: nonzero means APP_START is waiting for the worker to enter.
 * Copy argv from the window, then start_entered before the branch. */
int zzapp_services_take_start(uint32_t *argc);
void zzapp_services_start_entered(int status);

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

/* Slice C. card_* are ARM physical. flip publishes flip_gen and does not wait. */
int zzapp_display_bound(uint32_t *w, uint32_t *h, uint32_t *pitch,
			uint32_t *format, uint32_t *card_front,
			uint32_t *card_back);
int zzapp_display_flip_bound(void);

/* Mailbox. QUEUED means core 1 will finish it; harvest from core 0. */
uint16_t zzapp_mailbox_dispatch(uint16_t opcode, const uint8_t *req,
				uint16_t req_len, uint8_t *reply,
				uint16_t *reply_len, int z3,
				uint32_t request_id, uint32_t user_cookie);
int zzapp_mailbox_harvest(uint32_t *request_id, uint32_t *user_cookie,
			  uint16_t *opcode, uint16_t *status,
			  uint8_t *payload, uint16_t *payload_len);
void zzapp_mailbox_harvest_ack(void);

/* Slice A provides these strong. Weak stubs here return ZZ_ENOSYS / no-op. */
int zzapp_loader_begin(void);
int zzapp_loader_chunk(uint32_t file_off, const void *src, uint32_t len);
int zzapp_loader_end(void);
int zzapp_loader_abort(void);
int zzapp_loader_alloc_back(uint32_t bytes, uint32_t *addr_out);
void zzapp_loader_free_back(uint32_t addr);
void zzapp_host_yield(void);
uint64_t zzapp_host_time_us(void);

#ifdef ZZAPP_HOST_TEST
void zzapp_services_test_reset(void);
void zzapp_services_test_set_window(void *window, uint32_t bytes);
void zzapp_services_test_set_yield(void (*fn)(void));
void zzapp_services_test_set_time(uint64_t (*fn)(void));
void zzapp_services_test_set_slot_busy(uint32_t slot, int busy);
#endif

#endif /* ZZAPP_SERVICES_H */
