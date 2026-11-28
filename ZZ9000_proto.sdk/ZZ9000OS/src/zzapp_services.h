/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * The app runtime's services over the AAR transport (zzapp_services.c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZAPP_SERVICES_H
#define ZZAPP_SERVICES_H

#include <stdint.h>

struct aar_event;
struct aar_stat;
struct aar_dirent;
struct aar_surface;
struct aar_audio;
struct aar_volume_info;
struct aar_locale_info;
struct aar_display_info;
struct aar_display_mode;
struct aar_pointer;
struct aar_present_result;
struct zzapp_rec;

/* SDK status numbers. Match sdk_mailbox.h; guarded so either include order works. */
#ifndef SDK_STATUS_OK

#define SDK_STATUS_OK              0u
#define SDK_STATUS_QUEUED          1u
#define SDK_STATUS_BUSY            2u
#define SDK_STATUS_UNSUPPORTED     3u
#define SDK_STATUS_BAD_REQUEST     4u
#define SDK_STATUS_BAD_HANDLE      5u
#define SDK_STATUS_NO_MEMORY       6u
#define SDK_STATUS_IO_ERROR        9u
#define SDK_STATUS_NOT_FOUND       10u
#endif

/* Amiga reset, once, after the host retired every record and before the
 * allocator is reset: every held shell is released and gives its buffers back
 * (a shell with an audio detach still owed to core 0, or an unharvested reply,
 * goes as soon as that is done). The quarantine flag stays on a quarantined
 * shell until its release, so the release returns the app's own host-visible
 * blocks along with the transport blocks. */
void zzapp_services_on_reset(void);
/* Retire stage 3: display grants, audio post-teardown, files, waits and
 * requests in flight of the selected record. The state moves to STOPPING;
 * IDLE comes from zzapp_services_retire_wire(). */
void zzapp_services_on_app_end(void);
void zzapp_services_cancel_start(int status);
void zzapp_services_on_launch(void);

/* Core 1, the system thread: the next deferred mailbox op, the pass over the
 * records with a parked wait, a doorbell or console output, the audio close
 * handoff. */
void zzapp_services_poll(void);
/* Core 0: audio open, close and go-live of every record that has audio. */
void zzapp_services_core0_poll(void);
/* Core 0, while core 1 is halted and cannot be inside an audio handoff. */
void zzapp_services_reclaim_halted_audio(void);

/* The launch generation of the selected record: the generation of its handle. */
uint32_t zzapp_services_generation(void);
uint32_t zzapp_services_state(void);
/* The record services act for: the running app's own on core 1, else the one
 * selected by zzapp_services_select_pid(); ZZAPP_PID_NONE when there is none. */
uint32_t zzapp_services_current_pid(void);
/* Select the record as this core's context; 1 when it is selected. A pid that
 * is out of range or FREE clears the record context and returns 0. */
int zzapp_services_select_pid(uint32_t pid);
/* 1 when the selected record has a stop latched, or the client asked for one
 * in its control block. */
int zzapp_services_stop_requested(void);
int zzapp_services_wait_registered(uint32_t pid);
/* Publish the state of the selected record in the control block (IDLE of a
 * claim comes from retire_wire). */
void zzapp_services_set_state(uint32_t state);
uint32_t zzapp_services_memory_limit(void);
void zzapp_services_set_fault(uint32_t code, uint32_t pc, uint32_t sp,
			      uint32_t lr, uint32_t far, uint32_t fsr,
			      uint32_t cpsr);
void zzapp_services_set_exit(int32_t code);
void zzapp_services_note_progress(uint32_t lo, uint32_t hi);
/* The end reason: kept in the record and in AAR_CB_END_REASON. */
void zzapp_services_set_terminal(uint32_t reason);
/* Core 0: ends a claim whose lease ran out while it was CLAIMED or LOADING. */
void zzapp_services_expire_claims(uint64_t now_us);
/* After poll: nonzero means START is waiting for the worker to enter. Copy
 * argv from the load buffer, then start_entered with the captured generation
 * before publishing RUNNING or branching into the app. */
int zzapp_services_take_start(uint32_t *argc);
int zzapp_services_start_entered(uint32_t generation);
/*
 * Retire stages 6 and 7 (the host's zzapp_rec_retire calls it last): publish
 * IDLE with the end reason in the control block and hold the record, its
 * queues and its buffers (the client drains messages and reads the final
 * status after the app ended). 1: the shell was released (its release was
 * requested) and the record freed; 0: it stays until RELEASE, a stale request
 * of a quarantined claim, or reset. A record that is already free reports 1.
 */
int zzapp_services_retire_wire(struct zzapp_rec *rec);
/* A resource refusal of the selected record during load (AAR_REASON_*): the
 * refused op replies with it. */
void zzapp_services_note_refusal(uint32_t reason);

void zzapp_svc_log(const char *msg, uint32_t len);
int zzapp_svc_should_stop(void);
int zzapp_svc_file_open(const char *path, uint32_t flags);
int zzapp_svc_file_close(int fd);
int zzapp_svc_file_read(int fd, void *buf, uint32_t len, uint32_t *out_n);
int zzapp_svc_file_write(int fd, const void *buf, uint32_t len, uint32_t *out_n);
int zzapp_svc_file_read_async(int fd, void *buf, uint32_t len, uint32_t *ticket);
int zzapp_svc_file_write_async(int fd, const void *buf, uint32_t len, uint32_t *ticket);
int zzapp_svc_file_collect(uint32_t ticket, int32_t *status, uint32_t *out_n);
int zzapp_svc_file_seek(int fd, int64_t off, int whence, int64_t *out_pos);
int zzapp_svc_file_stat(const char *path, struct aar_stat *out);
int zzapp_svc_file_list_open(const char *path);
int zzapp_svc_file_list_next(int list, struct aar_dirent *out);
int zzapp_svc_file_list_close(int list);
/* API 0.6 files. Paths follow the rules of file_open; every call waits the
 * file timeout (AAR_E_TIMEDOUT) and a stop ends it with AAR_E_IO. */
int zzapp_svc_file_delete(const char *path);
int zzapp_svc_file_rename(const char *from, const char *to, uint32_t flags);
int zzapp_svc_dir_create(const char *path);
int zzapp_svc_file_set_date(const char *path, int64_t mtime);
int zzapp_svc_file_set_protect(const char *path, uint32_t protect);
int zzapp_svc_file_truncate(int fd, uint64_t size);
/* Structure results write at most min(out->size, sizeof) bytes, with the
 * size field set to that. */
int zzapp_svc_volume_info(const char *path, struct aar_volume_info *out);
/* API 0.6 time, locale, environment and clipboard: parked requests with no
 * deadline of their own. env_get and clip_read answer AAR_E_NOSPC with *out_n
 * set when the value does not fit; a NULL buffer is valid only with 0 bytes. */
int zzapp_svc_time_unix_us(int64_t *out_us);
int zzapp_svc_locale_info(struct aar_locale_info *out);
int zzapp_svc_env_get(const char *name, char *buf, uint32_t bytes,
		      uint32_t *out_n);
int zzapp_svc_clip_read(uint32_t unit, uint32_t type, void *buf,
			uint32_t bytes, uint32_t *out_n);
int zzapp_svc_clip_write(uint32_t unit, uint32_t type, const void *data,
			 uint32_t bytes);
/* API 0.6 file comments: parked requests like the above, the comment in the
 * native bytes (at most AAR_FILE_COMMENT_MAX, no NUL, no terminator written).
 * get_comment copies min(bytes, length), sets *out_n to the full length and
 * answers AAR_E_NOSPC when it was cut; a NULL buffer is valid only with 0
 * bytes. set_comment with 0 bytes clears. */
int zzapp_svc_file_get_comment(const char *path, void *buf, uint32_t bytes,
			       uint32_t *out_n);
int zzapp_svc_file_set_comment(const char *path, const void *data,
			       uint32_t bytes);
int zzapp_svc_display_open(uint32_t w, uint32_t h, uint32_t format,
			   uint32_t flags, struct aar_surface *out);
int zzapp_svc_display_flip(uint32_t surface_id);
/* Queue a completed buffer without waiting for the client's acknowledgement. */
int zzapp_svc_display_flip_async(uint32_t surface_id);
/* The GL module transfers an already reserved frame to the ordinary async
 * flip path. A failed post leaves it with the module. On the core-1 system
 * thread this never parks; frame_id 0 passes through an untracked frame. */
int zzapp_display_flip_tracked(uint32_t surface_id, uint64_t frame_id);
void zzapp_display_finish_tracked(uint64_t frame_id, uint32_t reason);
int zzapp_minigl_swap_tracked(uint32_t surface_id, uint64_t frame_id);
int zzapp_svc_display_refresh(struct aar_surface *out);
int zzapp_svc_display_set_palette(uint32_t surface_id, uint32_t first,
                                  uint32_t count, const uint32_t *rgb);
int zzapp_svc_display_close(uint32_t surface_id);
int zzapp_display_flip_idle(void);
/* Blocks the app thread until the last queued flip is acknowledged, as a
 * registered wait the core-1 service pass wakes.  AAR_OK once idle. */
int zzapp_display_wait_flip_idle(void);
uint32_t zzapp_svc_display_latest_dropped(void);
/* 1 while the selected record's display is card-owned (LATEST): flips never
 * reach the client. */
int zzapp_display_latest(void);
int zzapp_svc_display_vsync(void);
/* API 0.6 display. Resize needs AAR_DISPCAP_RESIZE in the granted caps
 * (AAR_E_NOSYS otherwise) and no flip pending (AAR_E_BUSY); the surface is
 * remapped and *out refreshed, or, when the client's reply cannot be used, the
 * display is closed. wait_vblank on a display the card scans out directly
 * waits for a blank of that screen (zzapp_services_display_vblank and
 * zzapp_services_display_mailbox_end count one when the address VDMA was
 * pointed at is the display's own), AAR_E_TIMEDOUT at the timeout if none was
 * seen; on a window or scaled display it is the client's: its AAR_OK,
 * AAR_E_TIMEDOUT or AAR_E_NOSYS pass through, unless the client named the
 * P96 screen that owns the window (AAR_DISP_REPLY_SCANOUT, SPARE of the open
 * reply, a card address inside the P96 window and not the app's own buffers):
 * then the wait is native and only a blank that scans out exactly that
 * screen counts. A name that is not usable degrades to the client's wait.
 * last_present_us is the time the last flip or GL swap was seen shown, 0
 * before the first. */
int zzapp_svc_display_info(uint32_t surface_id, struct aar_display_info *out);
int zzapp_svc_display_modes(struct aar_display_mode *out, uint32_t max,
			    uint32_t *out_n);
int zzapp_svc_display_resize(uint32_t surface_id, uint32_t w, uint32_t h,
			     struct aar_surface *out);
int zzapp_svc_display_set_title(uint32_t surface_id, const char *title);
int zzapp_svc_display_pointer(uint32_t surface_id, uint32_t mode,
			      const struct aar_pointer *image);
int zzapp_svc_display_wait_vblank(uint32_t timeout_us);
uint64_t zzapp_svc_display_last_present_us(void);
int zzapp_svc_display_present(uint32_t surface_id, uint32_t flags,
			      uint64_t *frame_id);
int zzapp_svc_display_present_poll(struct aar_present_result *out);
int zzapp_svc_event_poll(struct aar_event *out, uint32_t max, uint32_t *out_n);
int zzapp_svc_audio_open(uint32_t rate, uint32_t channels, struct aar_audio *out);
int zzapp_svc_audio_close(void);
/* Console output is appended to a firmware ring and goes out as one
 * outstanding AAR_OP_CONSOLE_WRITE at a time; input is an AAR_OP_CONSOLE_READ
 * request. */
int zzapp_svc_console_write(const void *buf, uint32_t len, uint32_t *out_n);
int zzapp_svc_console_read(void *buf, uint32_t len, uint32_t *out_n);
int zzapp_svc_msg_send(const void *msg);
int zzapp_svc_msg_poll(void *msg);
/* Parks until the inbox holds a message (AAR_OK), the timeout in microseconds
 * passes (AAR_E_TIMEDOUT), or the app is stopped (AAR_E_IO). Zero only tests
 * the inbox. The message stays queued for zzapp_svc_msg_poll. */
int zzapp_svc_msg_wait(uint32_t timeout_us);

/* The bound display, for the GL module. */
int zzapp_display_bound(uint32_t *w, uint32_t *h, uint32_t *pitch,
			uint32_t *format, uint32_t *card_front,
			uint32_t *card_back);
int zzapp_display_layout(uint32_t *layout);
/* Core 0 brackets VDMA programming at vblank. begin returns nonzero with the
 * cross-core mailbox lock held; end must be called exactly once in that case. */
int zzapp_services_display_mailbox_begin(uint32_t normal_scanout,
					 uint32_t *scanout);
void zzapp_services_display_mailbox_end(int latched);
/* Strict-presentation acknowledgement after core 0 re-points VDMA. */
void zzapp_services_display_vblank(uint32_t scanout_address);
/* Core 0 calls this only while core 1 is halted in cold reset. */
void zzapp_services_reset_core1_lock(void);


/* Mailbox. QUEUED means core 1 will finish it; harvest from core 0. Every
 * operation but QUERY, CLAIM and LIST names its claim by the handle at byte 40
 * of the payload (zzapp_proto.h). */
uint16_t zzapp_mailbox_dispatch(uint16_t opcode, const uint8_t *req,
				uint16_t req_len, uint8_t *reply,
				uint16_t *reply_len, int z3,
				uint32_t request_id, uint32_t user_cookie);
int zzapp_mailbox_harvest(uint32_t *request_id, uint32_t *user_cookie,
			  uint16_t *opcode, uint16_t *status,
			  uint8_t *payload, uint16_t *payload_len);
void zzapp_mailbox_harvest_ack(void);

/* Slice A provides these strong. Weak stubs here return AAR_E_NOSYS / no-op. */
int zzapp_loader_begin(void);
int zzapp_loader_chunk(uint32_t file_off, const void *src, uint32_t len);
int zzapp_loader_end(void);
int zzapp_loader_abort(void);
int zzapp_loader_alloc_back(uint32_t bytes, uint32_t *addr_out);
void zzapp_loader_free_back(uint32_t addr);
void zzapp_host_yield(void);
uint64_t zzapp_host_time_us(void);

#ifdef ZZAPP_HOST_TEST
/* Between sub-tests: the service statics and hooks go back to their initial
 * state. Records and their transport blocks are the test's to release. */
void zzapp_services_test_reset(void);
void zzapp_services_test_set_core(int core);
/* Called inside every wait loop of the services, where a test's client model
 * drains the SQ and posts completions. */
void zzapp_services_test_set_yield(void (*fn)(void));
void zzapp_services_test_set_time(uint64_t (*fn)(void));
void zzapp_services_test_set_slot_busy(uint32_t slot, int busy);
void zzapp_services_test_set_audio_request(uint32_t request);
uint32_t zzapp_services_test_audio_request(void);
uint32_t zzapp_services_test_audio_detach_count(void);
void zzapp_services_test_audio_underrun(void);
#endif

#endif /* ZZAPP_SERVICES_H */
