/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Rings, file seqlock, input, log, display bind, and the app audio producer.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zzapp_services.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "zzapp_proto.h"
#ifdef ZZAPP_HOST_TEST
#include <stdlib.h>
#include "zzapp.h"
#else
#undef SDK_STATUS_OK
#undef SDK_STATUS_QUEUED
#undef SDK_STATUS_BUSY
#undef SDK_STATUS_UNSUPPORTED
#undef SDK_STATUS_BAD_REQUEST
#undef SDK_STATUS_NO_MEMORY
#undef SDK_STATUS_IO_ERROR
#undef SDK_STATUS_NOT_FOUND
#include <stdio.h>
#include "audio_fabric.h"
#include "audio_fabric_internal.h"
#include "memorymap.h"
#include "sdk_aperture_layout.h"
#include "surface_allocator.h"
#include "xil_cache.h"
#ifndef ZZAPP_HOST_TEST
typedef char zzapp_window_size_matches_memorymap[
	(ZZAPP_WINDOW_BYTES == ZZAPP_WINDOW_Z3_SIZE) ? 1 : -1];
#endif

#define ZZ_OK          0
#define ZZ_EIO         (-1)
#define ZZ_EINVAL      (-2)
#define ZZ_ENOMEM      (-3)
#define ZZ_EBUSY       (-4)
#define ZZ_ENOENT      (-5)
#define ZZ_EACCES      (-6)
#define ZZ_ENOSYS      (-7)
#define ZZ_ETIMEDOUT   (-8)
#define ZZ_EFAULT      (-9)
#define ZZ_EBADF       (-12)

#define ZZ_FMT_RGB565   1u
#define ZZ_FMT_BGRA8888 2u
#define ZZ_DISPLAY_VSYNC 1u

typedef struct zzapp_surface {
	uint32_t id;
	uint32_t width, height, pitch, format;
	void *front;
	void *back;
	uint32_t card_front;
	uint32_t card_back;
} zzapp_surface;

typedef struct zzapp_event {
	uint16_t type, code;
	int16_t dx, dy;
	uint16_t buttons, reserved;
	uint32_t time_ms;
} zzapp_event;

typedef struct zzapp_stat {
	uint64_t size;
	uint32_t mtime;
	uint32_t mode;
} zzapp_stat;

typedef struct zzapp_dirent {
	uint32_t mode;
	char name[256];
} zzapp_dirent;

typedef struct zzapp_audio {
	int16_t *samples;
	uint32_t frames;
	volatile uint32_t *read_frame;
	volatile uint32_t *write_frame;
	uint32_t rate;
	uint32_t channels;
} zzapp_audio;
#endif

#define ZZAPP_FILE_TIMEOUT_US 30000000ull
#define ZZAPP_LOG_RING_HZ     32u
#define ZZAPP_LOG_UART_HZ     8u
#define ZZAPP_AUDIO_FRAMES    8192u
#define ZZAPP_AUDIO_RATE      22050u
#define ZZAPP_P96_LO          0x00200000u
#define ZZAPP_P96_HI          0x033F0000u
#define ZZAPP_HEAP_LO         0x033F0000u
#define ZZAPP_HEAP_HI         0x08200000u
#define ZZAPP_UNCACHED_LO     0x08100000u
#define ZZAPP_UNCACHED_HI     0x08200000u

#ifndef AUDIO_FABRIC_SLOT_MAILBOX
#define AUDIO_FABRIC_SLOT_MAILBOX  1u
#define AUDIO_FABRIC_SLOT_RESERVED 2u
#endif

enum {
	DEFER_IDLE = 0,
	DEFER_POSTED = 1,
	DEFER_DONE = 2,
	AUDIO_REQUEST_NONE = 0,
	AUDIO_REQUEST_OPEN = 1,
	AUDIO_REQUEST_CLOSE = 2
};

static uint8_t *window;
static uint32_t window_bytes;
static int window_uncached;
static int window_mapping_available;
static uint32_t file_seq;
static int file_open_slot[ZZAPP_FD_COUNT];
static int list_open_slot[ZZAPP_FD_COUNT];
static volatile int display_bound;
static int display_back_owned;
static uint32_t display_id;
static volatile uint32_t defer_state;
static uint16_t defer_opcode;
static uint16_t defer_status;
static uint16_t defer_req_len;
static uint16_t defer_reply_len;
static uint32_t defer_request_id;
static uint32_t defer_cookie;
static uint8_t defer_req[ZZAPP_MAILBOX_PAYLOAD_BYTES];
static uint8_t defer_reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
static volatile uint32_t start_pending;
static uint32_t start_argc;
static uint32_t log_ring_count;
static uint64_t log_ring_window;
#ifndef ZZAPP_HOST_TEST
static uint32_t log_uart_count;
static uint64_t log_uart_window;
#endif
static volatile int audio_open;
static uint32_t audio_slot;
static volatile uint32_t *audio_read_frame;
static volatile uint32_t *audio_write_frame;
static int16_t *audio_samples;
static uint64_t audio_staged;
static void *audio_block;
static volatile uint32_t audio_request;
#ifndef ZZAPP_HOST_TEST
static int audio_result;
#endif
static int slot_busy_test[4];

static void (*yield_hook)(void);
static uint64_t (*time_hook)(void);

static void barrier(void)
{
#if defined(__arm__) && !defined(ZZAPP_HOST_TEST)
	__asm__ __volatile__("dsb sy" ::: "memory");
#else
	__asm__ __volatile__("" ::: "memory");
#endif
}

static int have_window(void)
{
	return window != NULL && window_bytes >= ZZAPP_WINDOW_BYTES;
}

static void win_flush(uint32_t off, uint32_t len)
{
#ifndef ZZAPP_HOST_TEST
	if (!window_uncached && have_window() && len != 0U)
		Xil_DCacheFlushRange((INTPTR)(window + off), len);
#else
	(void)off;
	(void)len;
#endif
}

static void win_inval(uint32_t off, uint32_t len)
{
#ifndef ZZAPP_HOST_TEST
	if (!window_uncached && have_window() && len != 0U)
		Xil_DCacheInvalidateRange((INTPTR)(window + off), len);
#else
	(void)off;
	(void)len;
#endif
}

static void win_put32(uint32_t off, uint32_t value)
{
	zzapp_put_be32(window + off, value);
	win_flush(off, 4U);
}

static uint32_t win_get32(uint32_t off)
{
	win_inval(off, 4U);
	return zzapp_get_be32(window + off);
}

static void life_put(uint32_t off, uint32_t value)
{
	if (!have_window())
		return;
	win_put32(ZZAPP_LIFE_OFF + off, value);
}

static uint32_t life_get(uint32_t off)
{
	if (!have_window())
		return 0U;
	return win_get32(ZZAPP_LIFE_OFF + off);
}

static void yield_once(void)
{
	if (yield_hook)
		yield_hook();
	else
		zzapp_host_yield();
}

static uint64_t now_us(void)
{
	if (time_hook)
		return time_hook();
	return zzapp_host_time_us();
}

__attribute__((weak)) int zzapp_loader_begin(void)
{
	return ZZ_ENOSYS;
}

__attribute__((weak)) int zzapp_loader_chunk(uint32_t file_off, const void *src,
					     uint32_t len)
{
	(void)file_off;
	(void)src;
	(void)len;
	return ZZ_ENOSYS;
}

__attribute__((weak)) int zzapp_loader_end(void)
{
	return ZZ_ENOSYS;
}

__attribute__((weak)) int zzapp_loader_abort(void)
{
	return ZZ_ENOSYS;
}

__attribute__((weak)) int zzapp_loader_alloc_back(uint32_t bytes, uint32_t *addr_out)
{
	(void)bytes;
	(void)addr_out;
	return ZZ_ENOSYS;
}

__attribute__((weak)) void zzapp_loader_free_back(uint32_t addr)
{
	(void)addr;
}

__attribute__((weak)) void zzapp_host_yield(void)
{
}

__attribute__((weak)) uint64_t zzapp_host_time_us(void)
{
	return 0U;
}

uint8_t *zzapp_services_window(void)
{
	return have_window() ? window : NULL;
}

void zzapp_services_set_window_mapping_available(int available)
{
	window_mapping_available = available ? 1 : 0;
}

uint32_t zzapp_services_generation(void)
{
	return life_get(ZZAPP_LIFE_GENERATION_OFF);
}

uint32_t zzapp_services_state(void)
{
	return have_window() ? life_get(ZZAPP_LIFE_STATE_OFF) : ZZAPP_ST_IDLE;
}

int zzapp_services_stop_requested(void)
{
	return life_get(ZZAPP_LIFE_STOP_OFF) != 0U;
}

void zzapp_services_set_state(uint32_t state)
{
	life_put(ZZAPP_LIFE_STATE_OFF, state);
}

void zzapp_services_set_generation(uint32_t generation)
{
	life_put(ZZAPP_LIFE_GENERATION_OFF, generation);
}

void zzapp_services_set_fault(uint32_t code, uint32_t pc, uint32_t sp,
			      uint32_t lr, uint32_t far, uint32_t fsr,
			      uint32_t cpsr)
{
	life_put(ZZAPP_LIFE_FAULT_OFF, code);
	life_put(ZZAPP_LIFE_PC_OFF, pc);
	life_put(ZZAPP_LIFE_SP_OFF, sp);
	life_put(ZZAPP_LIFE_LR_OFF, lr);
	life_put(ZZAPP_LIFE_FAR_OFF, far);
	life_put(ZZAPP_LIFE_FSR_OFF, fsr);
	life_put(ZZAPP_LIFE_CPSR_OFF, cpsr);
}

void zzapp_services_set_exit(int32_t code)
{
	life_put(ZZAPP_LIFE_EXIT_OFF, (uint32_t)code);
}

void zzapp_services_note_progress(uint32_t lo, uint32_t hi)
{
	life_put(ZZAPP_LIFE_PROGRESS_LO_OFF, lo);
	life_put(ZZAPP_LIFE_PROGRESS_HI_OFF, hi);
}

void zzapp_services_or_flags(uint32_t flags)
{
	life_put(ZZAPP_LIFE_FLAGS_OFF, life_get(ZZAPP_LIFE_FLAGS_OFF) | flags);
}

void zzapp_services_and_flags(uint32_t mask)
{
	life_put(ZZAPP_LIFE_FLAGS_OFF, life_get(ZZAPP_LIFE_FLAGS_OFF) & mask);
}

uint32_t zzapp_services_flags(void)
{
	return life_get(ZZAPP_LIFE_FLAGS_OFF);
}

static void forget_window(void)
{
	window = NULL;
	window_bytes = 0U;
	window_uncached = 0;
	file_seq = 0U;
	display_bound = 0;
	display_back_owned = 0;
	display_id = 0U;
	memset(file_open_slot, 0, sizeof(file_open_slot));
	memset(list_open_slot, 0, sizeof(list_open_slot));
}

static void audio_detach(void);

void zzapp_services_on_reset(void)
{
	audio_detach();
	audio_request = AUDIO_REQUEST_NONE;
	if (display_back_owned)
		zzapp_loader_free_back(life_get(ZZAPP_LIFE_CARD_BACK_OFF));
	forget_window();
	defer_state = DEFER_IDLE;
	start_pending = 0U;
}

static int fd_index(int fd)
{
	if (fd < ZZAPP_FD_BASE || fd >= ZZAPP_FD_BASE + ZZAPP_FD_COUNT)
		return -1;
	return fd - ZZAPP_FD_BASE;
}

static int alloc_fd(int *table)
{
	int i;

	for (i = 0; i < ZZAPP_FD_COUNT; i++) {
		if (!table[i]) {
			table[i] = 1;
			return ZZAPP_FD_BASE + i;
		}
	}
	return -1;
}

static uint16_t zz_to_sdk(int zz)
{
	switch (zz) {
	case ZZ_OK:
		return SDK_STATUS_OK;
	case ZZ_EINVAL:
	case ZZ_EACCES:
	case ZZ_EBADF:
		return SDK_STATUS_BAD_REQUEST;
	case ZZ_ENOMEM:
		return SDK_STATUS_NO_MEMORY;
	case ZZ_EBUSY:
		return SDK_STATUS_BUSY;
	case ZZ_ENOENT:
		return SDK_STATUS_NOT_FOUND;
	case ZZ_ENOSYS:
		return SDK_STATUS_UNSUPPORTED;
	default:
		return SDK_STATUS_IO_ERROR;
	}
}

static int publish_file(uint32_t op, int fd, int32_t flags, int32_t whence,
			uint32_t path_off, uint32_t length, uint64_t offset,
			uint32_t mode)
{
	uint32_t odd;
	uint32_t even;

	if (!have_window())
		return ZZ_EIO;
	odd = file_seq + 1U;
	even = file_seq + 2U;
	win_put32(ZZAPP_FILE_OFF + 0U, odd);
	barrier();
	win_put32(ZZAPP_FILE_OFF + 4U, op);
	win_put32(ZZAPP_FILE_OFF + 8U, (uint32_t)fd);
	win_put32(ZZAPP_FILE_OFF + 12U, (uint32_t)flags);
	win_put32(ZZAPP_FILE_OFF + 16U, (uint32_t)whence);
	win_put32(ZZAPP_FILE_OFF + 20U, 0U);
	win_put32(ZZAPP_FILE_OFF + 24U, path_off);
	win_put32(ZZAPP_FILE_OFF + 28U, length);
	win_put32(ZZAPP_FILE_OFF + 32U, 0U);
	win_put32(ZZAPP_FILE_OFF + 36U, mode);
	zzapp_put_be64(window + ZZAPP_FILE_OFF + 40U, offset);
	win_flush(ZZAPP_FILE_OFF + 40U, 8U);
	zzapp_put_be64(window + ZZAPP_FILE_OFF + 48U, 0U);
	win_flush(ZZAPP_FILE_OFF + 48U, 8U);
	win_put32(ZZAPP_FILE_OFF + 56U, 0U);
	win_put32(ZZAPP_FILE_OFF + 60U, 0U);
	barrier();
	win_put32(ZZAPP_FILE_OFF + 0U, even);
	file_seq = even;
	return ZZ_OK;
}

static int wait_file(uint32_t *out_n, uint64_t *out_pos, uint32_t *mode,
		     uint32_t *mtime, int32_t *err)
{
	uint32_t generation = zzapp_services_generation();
	uint64_t start = now_us();

	for (;;) {
		uint32_t s1;
		uint32_t s2;
		uint32_t n;
		uint64_t pos;
		uint32_t reply_mode;
		uint32_t reply_mtime;
		int32_t reply_err;

		if (zzapp_services_generation() != generation ||
		    zzapp_services_stop_requested())
			return ZZ_EIO;
		win_inval(ZZAPP_FILE_OFF, ZZAPP_FILE_BYTES);
		s1 = zzapp_get_be32(window + ZZAPP_FILE_OFF);
		if ((s1 & 1U) == 0U && s1 != file_seq) {
			n = zzapp_get_be32(window + ZZAPP_FILE_OFF + 32U);
			pos = zzapp_get_be64(window + ZZAPP_FILE_OFF + 48U);
			reply_mode = zzapp_get_be32(window + ZZAPP_FILE_OFF + 36U);
			reply_mtime = zzapp_get_be32(window + ZZAPP_FILE_OFF + 56U);
			reply_err = zzapp_get_be32s(window + ZZAPP_FILE_OFF + 20U);
			s2 = win_get32(ZZAPP_FILE_OFF + 0U);
			if (s1 == s2 && zzapp_services_generation() == generation) {
				if (out_n)
					*out_n = n;
				if (out_pos)
					*out_pos = pos;
				if (mode)
					*mode = reply_mode;
				if (mtime)
					*mtime = reply_mtime;
				if (err)
					*err = reply_err;
				file_seq = s1;
				return ZZ_OK;
			}
		}
		if (now_us() - start >= ZZAPP_FILE_TIMEOUT_US)
			return ZZ_ETIMEDOUT;
		yield_once();
	}
}

static int put_path(const char *path, uint32_t *path_off)
{
	size_t n;

	if (!path)
		return ZZ_EINVAL;
	n = strlen(path);
	if (n == 0U || n > ZZAPP_PATH_MAX)
		return ZZ_EINVAL;
	memcpy(window + ZZAPP_PATH_OFF, path, n + 1U);
	win_flush(ZZAPP_PATH_OFF, (uint32_t)(n + 1U));
	*path_off = ZZAPP_PATH_OFF;
	return ZZ_OK;
}

int zzapp_svc_file_open(const char *path, uint32_t flags)
{
	uint32_t path_off = 0U;
	uint32_t mode = 0U;
	int32_t err = 0;
	int fd;
	int rc;

	if (!have_window())
		return ZZ_EIO;
	if ((flags & ~(ZZAPP_FILE_READ | ZZAPP_FILE_WRITE | ZZAPP_FILE_CREATE)) != 0U)
		return ZZ_EINVAL;
	rc = put_path(path, &path_off);
	if (rc != ZZ_OK)
		return rc;
	fd = alloc_fd(file_open_slot);
	if (fd < 0)
		return ZZ_ENOMEM;
	rc = publish_file(ZZAPP_FILE_OP_OPEN, fd, (int32_t)flags, 0, path_off,
			  0U, 0U, 0U);
	if (rc != ZZ_OK) {
		file_open_slot[fd_index(fd)] = 0;
		return rc;
	}
	rc = wait_file(NULL, NULL, &mode, NULL, &err);
	if (rc != ZZ_OK || err != 0) {
		file_open_slot[fd_index(fd)] = 0;
		return rc != ZZ_OK ? rc : err;
	}
	(void)mode;
	return fd;
}

int zzapp_svc_file_close(int fd)
{
	int ix = fd_index(fd);
	int32_t err = 0;
	int rc;

	if (ix < 0 || !file_open_slot[ix])
		return ZZ_EBADF;
	rc = publish_file(ZZAPP_FILE_OP_CLOSE, fd, 0, 0, 0U, 0U, 0U, 0U);
	if (rc != ZZ_OK)
		return rc;
	rc = wait_file(NULL, NULL, NULL, NULL, &err);
	file_open_slot[ix] = 0;
	if (rc != ZZ_OK)
		return rc;
	return err;
}

int zzapp_svc_file_read(int fd, void *buf, uint32_t len, uint32_t *out_n)
{
	uint8_t *dst = (uint8_t *)buf;
	uint32_t got = 0U;
	int ix = fd_index(fd);

	if (!out_n)
		return ZZ_EINVAL;
	*out_n = 0U;
	if (ix < 0 || !file_open_slot[ix] || !dst)
		return ZZ_EBADF;
	while (got < len) {
		uint32_t chunk = len - got;
		uint32_t n = 0U;
		int32_t err = 0;
		int rc;

		if (chunk > ZZAPP_DATA_BYTES)
			chunk = ZZAPP_DATA_BYTES;
		rc = publish_file(ZZAPP_FILE_OP_READ, fd, 0, 0, 0U, chunk, 0U, 0U);
		if (rc != ZZ_OK)
			return rc;
		rc = wait_file(&n, NULL, NULL, NULL, &err);
		if (rc != ZZ_OK)
			return rc;
		if (err != 0)
			return err;
		if (n > chunk)
			n = chunk;
		if (n != 0U) {
			win_inval(ZZAPP_DATA_OFF, n);
			memcpy(dst + got, window + ZZAPP_DATA_OFF, n);
		}
		got += n;
		*out_n = got;
		if (n < chunk)
			break;
	}
	return ZZ_OK;
}

int zzapp_svc_file_write(int fd, const void *buf, uint32_t len, uint32_t *out_n)
{
	const uint8_t *src = (const uint8_t *)buf;
	uint32_t got = 0U;
	int ix = fd_index(fd);

	if (!out_n)
		return ZZ_EINVAL;
	*out_n = 0U;
	if (ix < 0 || !file_open_slot[ix] || (!src && len != 0U))
		return ZZ_EBADF;
	while (got < len) {
		uint32_t chunk = len - got;
		uint32_t n = 0U;
		int32_t err = 0;
		int rc;

		if (chunk > ZZAPP_DATA_BYTES)
			chunk = ZZAPP_DATA_BYTES;
		memcpy(window + ZZAPP_DATA_OFF, src + got, chunk);
		win_flush(ZZAPP_DATA_OFF, chunk);
		rc = publish_file(ZZAPP_FILE_OP_WRITE, fd, 0, 0, 0U, chunk, 0U, 0U);
		if (rc != ZZ_OK)
			return rc;
		rc = wait_file(&n, NULL, NULL, NULL, &err);
		if (rc != ZZ_OK)
			return rc;
		if (err != 0)
			return err;
		if (n > chunk)
			n = chunk;
		got += n;
		*out_n = got;
		if (n < chunk)
			break;
	}
	return ZZ_OK;
}

int zzapp_svc_file_seek(int fd, int64_t off, int whence, int64_t *out_pos)
{
	uint64_t pos = 0U;
	int32_t err = 0;
	int ix = fd_index(fd);
	int rc;

	if (ix < 0 || !file_open_slot[ix])
		return ZZ_EBADF;
	if (whence != ZZAPP_SEEK_SET && whence != ZZAPP_SEEK_CUR &&
	    whence != ZZAPP_SEEK_END)
		return ZZ_EINVAL;
	rc = publish_file(ZZAPP_FILE_OP_SEEK, fd, 0, whence, 0U, 0U,
			  (uint64_t)off, 0U);
	if (rc != ZZ_OK)
		return rc;
	rc = wait_file(NULL, &pos, NULL, NULL, &err);
	if (rc != ZZ_OK)
		return rc;
	if (err != 0)
		return err;
	if (out_pos)
		*out_pos = (int64_t)pos;
	return ZZ_OK;
}

int zzapp_svc_file_stat(const char *path, zzapp_stat *out)
{
	uint32_t path_off = 0U;
	uint64_t size = 0U;
	uint32_t mode = 0U;
	uint32_t mtime = 0U;
	int32_t err = 0;
	int rc;

	if (!out)
		return ZZ_EINVAL;
	rc = put_path(path, &path_off);
	if (rc != ZZ_OK)
		return rc;
	rc = publish_file(ZZAPP_FILE_OP_STAT, 0, 0, 0, path_off, 0U, 0U, 0U);
	if (rc != ZZ_OK)
		return rc;
	rc = wait_file(NULL, &size, &mode, &mtime, &err);
	if (rc != ZZ_OK)
		return rc;
	if (err != 0)
		return err;
	out->size = size;
	out->mtime = mtime;
	out->mode = mode;
	return ZZ_OK;
}

int zzapp_svc_file_list_open(const char *path)
{
	uint32_t path_off = 0U;
	int32_t err = 0;
	int list;
	int rc;

	rc = put_path(path, &path_off);
	if (rc != ZZ_OK)
		return rc;
	list = alloc_fd(list_open_slot);
	if (list < 0)
		return ZZ_ENOMEM;
	rc = publish_file(ZZAPP_FILE_OP_LIST_OPEN, list, 0, 0, path_off, 0U, 0U, 0U);
	if (rc != ZZ_OK) {
		list_open_slot[fd_index(list)] = 0;
		return rc;
	}
	rc = wait_file(NULL, NULL, NULL, NULL, &err);
	if (rc != ZZ_OK || err != 0) {
		list_open_slot[fd_index(list)] = 0;
		return rc != ZZ_OK ? rc : err;
	}
	return list;
}

int zzapp_svc_file_list_next(int list, zzapp_dirent *out)
{
	uint32_t n = 0U;
	uint32_t mode = 0U;
	int32_t err = 0;
	int ix = fd_index(list);
	int rc;

	if (ix < 0 || !list_open_slot[ix] || !out)
		return ZZ_EBADF;
	rc = publish_file(ZZAPP_FILE_OP_LIST_NEXT, list, 0, 0, 0U, 0U, 0U, 0U);
	if (rc != ZZ_OK)
		return rc;
	rc = wait_file(&n, NULL, &mode, NULL, &err);
	if (rc != ZZ_OK)
		return rc;
	if (err != 0)
		return err;
	if (n == 0U)
		return ZZ_ENOENT;
	if (n > 255U)
		n = 255U;
	win_inval(ZZAPP_DATA_OFF, n + 1U);
	memcpy(out->name, window + ZZAPP_DATA_OFF, n);
	out->name[n] = '\0';
	out->mode = mode;
	return ZZ_OK;
}

int zzapp_svc_file_list_close(int list)
{
	int ix = fd_index(list);
	int32_t err = 0;
	int rc;

	if (ix < 0 || !list_open_slot[ix])
		return ZZ_EBADF;
	rc = publish_file(ZZAPP_FILE_OP_LIST_CLOSE, list, 0, 0, 0U, 0U, 0U, 0U);
	if (rc != ZZ_OK)
		return rc;
	rc = wait_file(NULL, NULL, NULL, NULL, &err);
	list_open_slot[ix] = 0;
	return rc != ZZ_OK ? rc : err;
}

static uint32_t log_usable(void)
{
	return ZZAPP_LOG_BYTES - ZZAPP_LOG_HDR;
}

static void log_drop(void)
{
	life_put(ZZAPP_LIFE_LOG_DROPS_OFF, life_get(ZZAPP_LIFE_LOG_DROPS_OFF) + 1U);
}

void zzapp_svc_log(const char *msg, uint32_t len)
{
	uint32_t usable;
	uint32_t rec;
	uint32_t head;
	uint32_t tail;
	uint32_t pos;
	uint32_t i;
	uint64_t t;
	uint8_t hdr[2];

	if (!have_window() || !msg)
		return;
	if (len > 1024U)
		len = 1024U;
	t = now_us();
	if (t - log_ring_window >= 1000000ull) {
		log_ring_window = t;
		log_ring_count = 0U;
	}
	if (log_ring_count >= ZZAPP_LOG_RING_HZ) {
		log_drop();
		return;
	}
	usable = log_usable();
	rec = (2U + len + 3U) & ~3U;
	head = win_get32(ZZAPP_LOG_OFF + 0U);
	tail = win_get32(ZZAPP_LOG_OFF + 4U);
	if (head - tail + rec > usable) {
		log_drop();
		return;
	}
	zzapp_put_be16(hdr, (uint16_t)len);
	pos = head % usable;
	for (i = 0; i < rec; i++) {
		uint8_t b = 0U;
		uint32_t at = (pos + i) % usable;

		if (i < 2U)
			b = hdr[i];
		else if (i < 2U + len)
			b = (uint8_t)msg[i - 2U];
		window[ZZAPP_LOG_OFF + ZZAPP_LOG_HDR + at] = b;
	}
	win_flush(ZZAPP_LOG_OFF + ZZAPP_LOG_HDR, usable);
	barrier();
	win_put32(ZZAPP_LOG_OFF + 0U, head + rec);
	log_ring_count++;
#ifndef ZZAPP_HOST_TEST
	if (t - log_uart_window >= 1000000ull) {
		log_uart_window = t;
		log_uart_count = 0U;
	}
	if (log_uart_count < ZZAPP_LOG_UART_HZ) {
		printf("[app] %.*s\r\n", (int)len, msg);
		log_uart_count++;
	}
#endif
}

int zzapp_svc_should_stop(void)
{
	return zzapp_services_stop_requested();
}

int zzapp_svc_input_poll(zzapp_event *out, uint32_t max, uint32_t *out_n)
{
	uint32_t head;
	uint32_t tail;
	uint32_t n = 0U;

	if (!out_n)
		return ZZ_EINVAL;
	*out_n = 0U;
	if (!have_window())
		return ZZ_OK;
	if (max != 0U && !out)
		return ZZ_EINVAL;
	head = win_get32(ZZAPP_INPUT_OFF + 0U);
	tail = win_get32(ZZAPP_INPUT_OFF + 4U);
	if (head - tail > ZZAPP_INPUT_CAP)
		tail = head - ZZAPP_INPUT_CAP;
	while (n < max && tail != head) {
		uint32_t slot = tail & (ZZAPP_INPUT_CAP - 1U);
		uint32_t off = ZZAPP_INPUT_OFF + ZZAPP_INPUT_HDR +
			       slot * ZZAPP_EVENT_BYTES;
		zzapp_event *ev = &out[n];

		win_inval(off, ZZAPP_EVENT_BYTES);
		ev->type = zzapp_get_be16(window + off + 0U);
		ev->code = zzapp_get_be16(window + off + 2U);
		ev->dx = zzapp_get_be16s(window + off + 4U);
		ev->dy = zzapp_get_be16s(window + off + 6U);
		ev->buttons = zzapp_get_be16(window + off + 8U);
		ev->reserved = zzapp_get_be16(window + off + 10U);
		ev->time_ms = zzapp_get_be32(window + off + 12U);
		tail++;
		n++;
	}
	if (n != 0U) {
		barrier();
		win_put32(ZZAPP_INPUT_OFF + 4U, tail);
	}
	*out_n = n;
	return ZZ_OK;
}

static int span_ok(uint32_t addr, uint32_t bytes)
{
	uint64_t end;

	if (addr == 0U || bytes == 0U)
		return 0;
	end = (uint64_t)addr + bytes;
	if (addr >= ZZAPP_P96_LO && end <= ZZAPP_P96_HI)
		return 1;
	if (addr >= ZZAPP_HEAP_LO && end <= ZZAPP_HEAP_HI)
		return 1;
	return 0;
}

static uint32_t format_bpp(uint32_t format)
{
	if (format == ZZ_FMT_RGB565)
		return 2U;
	if (format == ZZ_FMT_BGRA8888)
		return 4U;
	return 0U;
}

static void drop_owned_back(void)
{
	uint32_t back;

	if (!display_back_owned)
		return;
	back = life_get(ZZAPP_LIFE_CARD_BACK_OFF);
	display_back_owned = 0;
	if (back != 0U)
		zzapp_loader_free_back(back);
}

int zzapp_display_bound(uint32_t *w, uint32_t *h, uint32_t *pitch,
			uint32_t *format, uint32_t *card_front,
			uint32_t *card_back)
{
	uint32_t front;
	uint32_t back;

	if (!display_bound)
		return ZZ_EINVAL;
	front = life_get(ZZAPP_LIFE_CARD_FRONT_OFF);
	back = life_get(ZZAPP_LIFE_CARD_BACK_OFF);
	if (!display_back_owned && back != 0U &&
	    life_get(ZZAPP_LIFE_FLIP_SHOWING_OFF) != 0U) {
		uint32_t tmp = front;

		front = back;
		back = tmp;
	}
	if (w)
		*w = life_get(ZZAPP_LIFE_WIDTH_OFF);
	if (h)
		*h = life_get(ZZAPP_LIFE_HEIGHT_OFF);
	if (pitch)
		*pitch = life_get(ZZAPP_LIFE_PITCH_OFF);
	if (format)
		*format = life_get(ZZAPP_LIFE_FORMAT_OFF);
	if (card_front)
		*card_front = front;
	if (card_back)
		*card_back = back;
	return ZZ_OK;
}

int zzapp_display_flip_bound(void)
{
	uint32_t gen;
	uint32_t ack;

	if (!display_bound)
		return ZZ_EINVAL;
	gen = life_get(ZZAPP_LIFE_FLIP_GEN_OFF);
	ack = life_get(ZZAPP_LIFE_FLIP_ACK_OFF);
	if (gen != ack)
		return ZZ_EBUSY;
	life_put(ZZAPP_LIFE_FLIP_GEN_OFF, gen + 1U);
	return ZZ_OK;
}

static int wait_flip(uint32_t gen)
{
	uint32_t generation = zzapp_services_generation();

	for (;;) {
		uint32_t ack;

		if (zzapp_services_stop_requested() ||
		    zzapp_services_generation() != generation)
			return ZZ_EIO;
		ack = life_get(ZZAPP_LIFE_FLIP_ACK_OFF);
		if (ack == gen) {
			life_put(ZZAPP_LIFE_FLIP_SHOWING_OFF,
				 life_get(ZZAPP_LIFE_CARD_BACK_OFF) != 0U &&
				 !display_back_owned
					 ? (life_get(ZZAPP_LIFE_FLIP_SHOWING_OFF) ^ 1U)
					 : 0U);
			return ZZ_OK;
		}
		yield_once();
	}
}

int zzapp_svc_display_open(uint32_t w, uint32_t h, uint32_t format,
			   uint32_t flags, zzapp_surface *out)
{
	uint32_t bpp;
	uint32_t bits;

	if (!out || !have_window())
		return ZZ_EINVAL;
	bpp = format_bpp(format);
	if (w == 0U || h == 0U || w > 4096U || h > 4096U || bpp == 0U)
		return ZZ_EINVAL;
	if (display_bound)
		return ZZ_EBUSY;
	bits = life_get(ZZAPP_LIFE_FLAGS_OFF);
	bits |= ZZAPP_F_DISPLAY_UNBOUND;
	if ((flags & ZZ_DISPLAY_VSYNC) != 0U)
		bits |= ZZAPP_F_VSYNC;
	life_put(ZZAPP_LIFE_WIDTH_OFF, w);
	life_put(ZZAPP_LIFE_HEIGHT_OFF, h);
	life_put(ZZAPP_LIFE_FORMAT_OFF, format);
	life_put(ZZAPP_LIFE_FLAGS_OFF, bits);
	for (;;) {
		if (zzapp_services_stop_requested())
			return ZZ_EIO;
		if ((life_get(ZZAPP_LIFE_FLAGS_OFF) & ZZAPP_F_DISPLAY_UNBOUND) == 0U &&
		    display_bound)
			break;
		yield_once();
	}
	out->id = display_id;
	out->width = life_get(ZZAPP_LIFE_WIDTH_OFF);
	out->height = life_get(ZZAPP_LIFE_HEIGHT_OFF);
	out->pitch = life_get(ZZAPP_LIFE_PITCH_OFF);
	out->format = life_get(ZZAPP_LIFE_FORMAT_OFF);
	out->card_front = life_get(ZZAPP_LIFE_CARD_FRONT_OFF);
	out->card_back = life_get(ZZAPP_LIFE_CARD_BACK_OFF);
	out->front = (void *)(uintptr_t)out->card_front;
	out->back = (void *)(uintptr_t)out->card_back;
	return ZZ_OK;
}

static void flush_presented(uint32_t addr, uint32_t bytes)
{
#ifndef ZZAPP_HOST_TEST
	if (addr != 0U && bytes != 0U)
		Xil_DCacheFlushRange((INTPTR)(uintptr_t)addr, bytes);
#else
	(void)addr;
	(void)bytes;
#endif
	barrier();
}

int zzapp_svc_display_flip(uint32_t surface_id)
{
	uint32_t gen;
	uint32_t pitch;
	uint32_t height;
	uint32_t bytes;
	uint32_t front;
	uint32_t back;
	int rc;

	if (!display_bound || surface_id != display_id)
		return ZZ_EINVAL;
	gen = life_get(ZZAPP_LIFE_FLIP_GEN_OFF);
	if (gen == life_get(ZZAPP_LIFE_FLIP_ACK_OFF)) {
		pitch = life_get(ZZAPP_LIFE_PITCH_OFF);
		height = life_get(ZZAPP_LIFE_HEIGHT_OFF);
		if (pitch == 0U || height > (0xffffffffU / pitch))
			return ZZ_EINVAL;
		bytes = pitch * height;
		front = life_get(ZZAPP_LIFE_CARD_FRONT_OFF);
		back = life_get(ZZAPP_LIFE_CARD_BACK_OFF);
		if (display_back_owned) {
			if (front == 0U || back == 0U)
				return ZZ_EINVAL;
			memcpy((void *)(uintptr_t)front, (const void *)(uintptr_t)back,
			       bytes);
			flush_presented(front, bytes);
		} else {
			if (back != 0U && life_get(ZZAPP_LIFE_FLIP_SHOWING_OFF) != 0U) {
				uint32_t tmp = front;

				front = back;
				back = tmp;
			}
			flush_presented(back != 0U ? back : front, bytes);
		}
		rc = zzapp_display_flip_bound();
		if (rc != ZZ_OK)
			return rc;
		gen = life_get(ZZAPP_LIFE_FLIP_GEN_OFF);
	}
	return wait_flip(gen);
}

int zzapp_svc_display_vsync(void)
{
	if (!display_bound)
		return ZZ_EINVAL;
	zzapp_services_or_flags(ZZAPP_F_VSYNC);
	yield_once();
	return ZZ_OK;
}

int zzapp_svc_display_close(uint32_t surface_id)
{
	if (!display_bound || surface_id != display_id)
		return ZZ_EINVAL;
	drop_owned_back();
	display_bound = 0;
	display_id = 0U;
	zzapp_services_and_flags(~(ZZAPP_F_DISPLAY_UNBOUND | ZZAPP_F_VSYNC));
	life_put(ZZAPP_LIFE_CARD_FRONT_OFF, 0U);
	life_put(ZZAPP_LIFE_CARD_BACK_OFF, 0U);
	return ZZ_OK;
}

static int slot_is_busy(uint32_t slot)
{
	if (slot < 4U && slot_busy_test[slot])
		return 1;
#ifndef ZZAPP_HOST_TEST
	{
		struct audio_fabric_slot *s = fabric_slot(slot);

		if (s != NULL && s->attached)
			return 1;
	}
#endif
	return 0;
}

#ifndef ZZAPP_HOST_TEST
static int app_audio_snapshot(struct audio_fabric_source *source)
{
	uint32_t wr;

	if (!audio_open || !audio_samples || !audio_write_frame)
		return 0;
	wr = *audio_write_frame;
	memset(source, 0, sizeof(*source));
	source->ring = (uint8_t *)audio_samples;
	source->capacity = ZZAPP_AUDIO_FRAMES * 4U;
	source->produced_bytes = (uint64_t)wr * 4U;
	source->staged_bytes = audio_staged;
	source->sample_rate = ZZAPP_AUDIO_RATE;
	source->channels = 2U;
	source->sample_format = 1U; /* SDK_AUDIO_SAMPLE_FORMAT_S16LE */
	return 1;
}

static int app_audio_stage(uint32_t bytes)
{
	uint64_t produced;

	if (!audio_open || !audio_write_frame)
		return 0;
	produced = (uint64_t)(*audio_write_frame) * 4U;
	if (produced < audio_staged || produced - audio_staged < bytes)
		return 0;
	audio_staged += bytes;
	return 1;
}

static void app_audio_retire(uint32_t bytes)
{
	if (!audio_open || !audio_read_frame || (bytes & 3U) != 0U)
		return;
	*audio_read_frame += bytes / 4U;
}

static void app_audio_nop(void)
{
}

static const struct audio_fabric_producer_ops app_audio_ops = {
	.snapshot = app_audio_snapshot,
	.stage = app_audio_stage,
	.retire = app_audio_retire,
	.underrun = app_audio_nop,
	.gone = app_audio_nop,
	.tail_real = app_audio_nop,
	.tail_drained = app_audio_nop
};
#endif

static void audio_detach(void)
{
	if (!audio_open)
		return;
#ifndef ZZAPP_HOST_TEST
	audio_fabric_producer_detach(audio_slot);
	if (audio_block)
		surface_allocator_free((uint32_t)(uintptr_t)audio_block);
#endif
#ifdef ZZAPP_HOST_TEST
	free(audio_block);
#endif
	audio_open = 0;
	audio_block = NULL;
	audio_samples = NULL;
	audio_read_frame = NULL;
	audio_write_frame = NULL;
	audio_staged = 0U;
}

static int audio_attach(void)
{
	uint32_t slot;
	uint32_t bytes = 8U + ZZAPP_AUDIO_FRAMES * 4U;

	if (audio_open)
		return ZZ_EBUSY;
	slot = AUDIO_FABRIC_SLOT_RESERVED;
	if (slot_is_busy(slot))
		slot = AUDIO_FABRIC_SLOT_MAILBOX;
	if (slot_is_busy(slot))
		return ZZ_EBUSY;
#ifdef ZZAPP_HOST_TEST
	audio_block = calloc(1, bytes);
	if (!audio_block)
		return ZZ_ENOMEM;
#else
	if (!audio_fabric_conversion_admissible(ZZAPP_AUDIO_RATE))
		return ZZ_EBUSY;
	audio_block = (void *)(uintptr_t)surface_allocator_alloc_range(
		bytes, 64U, SDK_MAILBOX_Z3_RESERVE_END,
		SDK_AUDIO_DIRECT_RING_Z3_RESERVE_ADDRESS);
	if (!audio_block)
		return ZZ_ENOMEM;
#endif
	audio_read_frame = (volatile uint32_t *)audio_block;
	audio_write_frame = audio_read_frame + 1;
	audio_samples = (int16_t *)(audio_read_frame + 2);
	*audio_read_frame = 0U;
	*audio_write_frame = 0U;
	audio_staged = 0U;
	audio_slot = slot;
	/* The ISR may snapshot as soon as go_live returns. */
	audio_open = 1;
	barrier();
#ifndef ZZAPP_HOST_TEST
	if (!audio_fabric_producer_attach(slot, &app_audio_ops)) {
		audio_open = 0;
		surface_allocator_free((uint32_t)(uintptr_t)audio_block);
		audio_block = NULL;
		audio_samples = NULL;
		audio_read_frame = NULL;
		audio_write_frame = NULL;
		return ZZ_EBUSY;
	}
	audio_fabric_producer_rate_set(slot, ZZAPP_AUDIO_RATE);
	if (!audio_fabric_others_live(slot))
		audio_fabric_producer_restart(slot);
	audio_fabric_producer_go_live(slot);
#endif
	return ZZ_OK;
}

static void audio_fill(zzapp_audio *out)
{
	out->samples = audio_samples;
	out->frames = ZZAPP_AUDIO_FRAMES;
	out->read_frame = audio_read_frame;
	out->write_frame = audio_write_frame;
	out->rate = ZZAPP_AUDIO_RATE;
	out->channels = 2U;
}

int zzapp_svc_audio_open(uint32_t rate, uint32_t channels, zzapp_audio *out)
{
	int rc;

	if (!out)
		return ZZ_EINVAL;
	if (channels != 2U || rate != ZZAPP_AUDIO_RATE)
		return ZZ_EINVAL;
	if (audio_open || audio_request != AUDIO_REQUEST_NONE)
		return ZZ_EBUSY;
#ifdef ZZAPP_HOST_TEST
	rc = audio_attach();
#else
	audio_result = ZZ_EBUSY;
	barrier();
	audio_request = AUDIO_REQUEST_OPEN;
	while (audio_request != AUDIO_REQUEST_NONE)
		yield_once();
	rc = audio_result;
#endif
	if (rc != ZZ_OK)
		return rc;
	audio_fill(out);
	return ZZ_OK;
}

int zzapp_svc_audio_close(void)
{
	if (!audio_open || audio_request != AUDIO_REQUEST_NONE)
		return ZZ_EINVAL;
#ifdef ZZAPP_HOST_TEST
	audio_detach();
#else
	audio_result = ZZ_EIO;
	barrier();
	audio_request = AUDIO_REQUEST_CLOSE;
	while (audio_request != AUDIO_REQUEST_NONE)
		yield_once();
	if (audio_result != ZZ_OK)
		return audio_result;
#endif
	return ZZ_OK;
}

void zzapp_services_core0_poll(void)
{
#ifndef ZZAPP_HOST_TEST
	uint32_t request = audio_request;

	if (request == AUDIO_REQUEST_OPEN)
		audio_result = audio_attach();
	else if (request == AUDIO_REQUEST_CLOSE) {
		audio_detach();
		audio_result = ZZ_OK;
	} else {
		return;
	}
	barrier();
	audio_request = AUDIO_REQUEST_NONE;
#endif
}

static void init_window(uint8_t *base, uint32_t bytes, int uncached)
{
	window = base;
	window_bytes = bytes;
	window_uncached = uncached;
	file_seq = 0U;
	memset(window, 0, ZZAPP_WINDOW_BYTES);
	win_flush(0U, ZZAPP_WINDOW_BYTES);
	life_put(ZZAPP_LIFE_MAGIC_OFF, ZZAPP_LIFE_MAGIC);
	life_put(ZZAPP_LIFE_VERSION_OFF, ZZAPP_LIFE_VERSION);
	life_put(ZZAPP_LIFE_STATE_OFF, ZZAPP_ST_IDLE);
}

static int alloc_window(uint32_t *arm_addr, uint32_t *host_off)
{
#ifdef ZZAPP_HOST_TEST
	(void)arm_addr;
	(void)host_off;
	return ZZ_ENOMEM;
#else
	uint32_t addr;

	if (have_window()) {
		*arm_addr = (uint32_t)(uintptr_t)window;
		*host_off = *arm_addr - ADDR_ADJ;
		return ZZ_OK;
	}
	if (!window_mapping_available)
		return ZZ_ENOMEM;
	addr = surface_allocator_alloc_range(ZZAPP_WINDOW_BYTES, 4096U,
					    ZZAPP_WINDOW_Z3_ADDRESS,
					    ZZAPP_WINDOW_Z3_END);
	if (addr == 0U)
		return ZZ_ENOMEM;
	init_window((uint8_t *)(uintptr_t)addr, ZZAPP_WINDOW_BYTES, 1);
	*arm_addr = addr;
	*host_off = addr - ADDR_ADJ;
	return ZZ_OK;
#endif
}

static int bind_surface(const uint8_t *req, uint16_t req_len)
{
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t format;
	uint32_t front;
	uint32_t back;
	uint32_t bpp;
	uint32_t bytes;
	int rc;

	if (req_len < ZZAPP_BIND_REQ_BYTES)
		return ZZ_EINVAL;
	width = zzapp_get_be32(req + ZZAPP_BIND_WIDTH);
	height = zzapp_get_be32(req + ZZAPP_BIND_HEIGHT);
	pitch = zzapp_get_be32(req + ZZAPP_BIND_PITCH);
	format = zzapp_get_be32(req + ZZAPP_BIND_FORMAT);
	front = zzapp_get_be32(req + ZZAPP_BIND_CARD_FRONT);
	back = zzapp_get_be32(req + ZZAPP_BIND_CARD_BACK);
	bpp = format_bpp(format);
	if (width == 0U || height == 0U || width > 4096U || height > 4096U ||
	    bpp == 0U || pitch < width * bpp || height > (0xffffffffU / pitch))
		return ZZ_EINVAL;
	bytes = pitch * height;
	if (!span_ok(front, bytes))
		return ZZ_EINVAL;
	drop_owned_back();
	if (back == 0U) {
		rc = zzapp_loader_alloc_back(bytes, &back);
		if (rc != ZZ_OK)
			return rc == ZZ_ENOSYS ? ZZ_ENOMEM : rc;
		display_back_owned = 1;
	} else if (!span_ok(back, bytes)) {
		return ZZ_EINVAL;
	} else {
		display_back_owned = 0;
	}
	life_put(ZZAPP_LIFE_WIDTH_OFF, width);
	life_put(ZZAPP_LIFE_HEIGHT_OFF, height);
	life_put(ZZAPP_LIFE_PITCH_OFF, pitch);
	life_put(ZZAPP_LIFE_FORMAT_OFF, format);
	life_put(ZZAPP_LIFE_CARD_FRONT_OFF, front);
	life_put(ZZAPP_LIFE_CARD_BACK_OFF, back);
	life_put(ZZAPP_LIFE_FLIP_SHOWING_OFF, 0U);
	display_bound = 1;
	display_id = 1U;
	barrier();
	zzapp_services_and_flags(~ZZAPP_F_DISPLAY_UNBOUND);
	return ZZ_OK;
}

static void run_deferred(void)
{
	int rc = ZZ_EINVAL;
	const uint8_t *req = defer_req;
	uint16_t len = defer_req_len;

	defer_reply_len = 0U;
	switch (defer_opcode) {
	case ZZAPP_OP_LOAD_BEGIN:
		if (zzapp_services_state() != ZZAPP_ST_IDLE) {
			rc = ZZ_EBUSY;
			break;
		}
		rc = zzapp_loader_begin();
		if (rc == ZZ_OK && zzapp_services_state() == ZZAPP_ST_IDLE)
			zzapp_services_set_state(ZZAPP_ST_LOADING);
		break;
	case ZZAPP_OP_LOAD_CHUNK: {
		uint32_t off;
		uint32_t n;
		uint32_t staging;

		if (len < ZZAPP_CHUNK_REQ_BYTES) {
			rc = ZZ_EINVAL;
			break;
		}
		off = zzapp_get_be32(req + ZZAPP_CHUNK_FILE_OFF);
		n = zzapp_get_be32(req + ZZAPP_CHUNK_LENGTH);
		staging = zzapp_get_be32(req + ZZAPP_CHUNK_STAGING);
		if (n < ZZAPP_CHUNK_MIN || n > ZZAPP_CHUNK_MAX ||
		    staging != ZZAPP_STAGING_OFF ||
		    zzapp_services_state() != ZZAPP_ST_LOADING) {
			rc = ZZ_EINVAL;
			break;
		}
		win_inval(ZZAPP_STAGING_OFF, n);
		rc = zzapp_loader_chunk(off, window + ZZAPP_STAGING_OFF, n);
		break;
	}
	case ZZAPP_OP_LOAD_END:
		if (zzapp_services_state() != ZZAPP_ST_LOADING) {
			rc = ZZ_EINVAL;
			break;
		}
		rc = zzapp_loader_end();
		if (rc != ZZ_OK)
			zzapp_services_set_state(ZZAPP_ST_IDLE);
		break;
	case ZZAPP_OP_START:
		if (zzapp_services_state() != ZZAPP_ST_LOADING &&
		    zzapp_services_state() != ZZAPP_ST_STARTING) {
			rc = ZZ_EINVAL;
			break;
		}
		start_argc = len >= ZZAPP_START_REQ_BYTES
			? zzapp_get_be32(req + ZZAPP_START_ARGC) : 0U;
		if (start_argc > ZZAPP_ARGV_MAX) {
			rc = ZZ_EINVAL;
			break;
		}
		zzapp_services_set_state(ZZAPP_ST_STARTING);
		start_pending = 1U;
		return;
	default:
		rc = ZZ_ENOSYS;
		break;
	}
	defer_status = zz_to_sdk(rc);
	barrier();
	defer_state = DEFER_DONE;
}

void zzapp_services_poll(void)
{
	if (defer_state == DEFER_POSTED)
		run_deferred();
}

int zzapp_services_take_start(uint32_t *argc)
{
	if (!start_pending)
		return 0;
	start_pending = 0U;
	if (argc)
		*argc = start_argc;
	return 1;
}

void zzapp_services_start_entered(int status)
{
	if (status == ZZ_OK)
		zzapp_services_set_state(ZZAPP_ST_RUNNING);
	else {
		zzapp_services_set_state(ZZAPP_ST_FAULTED);
		zzapp_services_set_exit(status);
	}
	defer_status = zz_to_sdk(status);
	defer_reply_len = 0U;
	barrier();
	defer_state = DEFER_DONE;
}

static int queue_deferred(uint16_t opcode, const uint8_t *req, uint16_t req_len,
			  uint32_t request_id, uint32_t user_cookie)
{
	if (defer_state != DEFER_IDLE)
		return 0;
	if (req_len > sizeof(defer_req))
		req_len = sizeof(defer_req);
	memset(defer_req, 0, sizeof(defer_req));
	if (req_len != 0U && req)
		memcpy(defer_req, req, req_len);
	defer_opcode = opcode;
	defer_req_len = req_len;
	defer_request_id = request_id;
	defer_cookie = user_cookie;
	defer_reply_len = 0U;
	barrier();
	defer_state = DEFER_POSTED;
	return 1;
}

static void put_query(uint8_t *reply, uint16_t *reply_len, uint32_t caps)
{
	memset(reply, 0, ZZAPP_QUERY_REPLY_BYTES);
	zzapp_put_be32(reply + ZZAPP_QUERY_ABI_MAJOR, ZZAPP_ABI_MAJOR);
	zzapp_put_be32(reply + ZZAPP_QUERY_ABI_MINOR, ZZAPP_ABI_MINOR);
	zzapp_put_be32(reply + ZZAPP_QUERY_STATE, zzapp_services_state());
	zzapp_put_be32(reply + ZZAPP_QUERY_CAPS, caps);
	*reply_len = ZZAPP_QUERY_REPLY_BYTES;
}

uint16_t zzapp_mailbox_dispatch(uint16_t opcode, const uint8_t *req,
				uint16_t req_len, uint8_t *reply,
				uint16_t *reply_len, int z3,
				uint32_t request_id, uint32_t user_cookie)
{
	int rc;

	if (!reply || !reply_len)
		return SDK_STATUS_BAD_REQUEST;
	*reply_len = 0U;
	if (opcode < ZZAPP_OP_FIRST || opcode > ZZAPP_OP_RANGE_HI)
		return SDK_STATUS_UNSUPPORTED;
	if (!z3) {
		if (opcode == ZZAPP_OP_QUERY)
			put_query(reply, reply_len, 0U);
		return SDK_STATUS_UNSUPPORTED;
	}
	switch (opcode) {
	case ZZAPP_OP_QUERY:
		put_query(reply, reply_len,
			  ZZAPP_CAP_Z3 | ZZAPP_CAP_DISPLAY | ZZAPP_CAP_AUDIO |
			  ZZAPP_CAP_GL);
		return SDK_STATUS_OK;
	case ZZAPP_OP_ALLOC_WINDOW: {
		uint32_t arm = 0U;
		uint32_t host = 0U;

		rc = alloc_window(&arm, &host);
		if (rc != ZZ_OK)
			return zz_to_sdk(rc);
		zzapp_put_be32(reply + ZZAPP_ALLOC_ARM_ADDRESS, arm);
		zzapp_put_be32(reply + ZZAPP_ALLOC_SIZE, ZZAPP_WINDOW_BYTES);
		zzapp_put_be32(reply + ZZAPP_ALLOC_HOST_OFFSET, host);
		*reply_len = ZZAPP_ALLOC_REPLY_BYTES;
		return SDK_STATUS_OK;
	}
	case ZZAPP_OP_STOP:
		life_put(ZZAPP_LIFE_STOP_OFF, 1U);
		return SDK_STATUS_OK;
	case ZZAPP_OP_STATUS:
		zzapp_put_be32(reply + ZZAPP_STATUS_STATE, zzapp_services_state());
		zzapp_put_be32(reply + ZZAPP_STATUS_GENERATION,
			       zzapp_services_generation());
		zzapp_put_be32(reply + ZZAPP_STATUS_FAULT, life_get(ZZAPP_LIFE_FAULT_OFF));
		zzapp_put_be32(reply + ZZAPP_STATUS_EXIT, life_get(ZZAPP_LIFE_EXIT_OFF));
		zzapp_put_be32(reply + ZZAPP_STATUS_FLAGS, zzapp_services_flags());
		zzapp_put_be32(reply + ZZAPP_STATUS_PC, life_get(ZZAPP_LIFE_PC_OFF));
		zzapp_put_be32(reply + ZZAPP_STATUS_SP, life_get(ZZAPP_LIFE_SP_OFF));
		zzapp_put_be32(reply + ZZAPP_STATUS_LR, life_get(ZZAPP_LIFE_LR_OFF));
		zzapp_put_be32(reply + ZZAPP_STATUS_FAR, life_get(ZZAPP_LIFE_FAR_OFF));
		zzapp_put_be32(reply + ZZAPP_STATUS_FSR, life_get(ZZAPP_LIFE_FSR_OFF));
		zzapp_put_be32(reply + ZZAPP_STATUS_CPSR, life_get(ZZAPP_LIFE_CPSR_OFF));
		zzapp_put_be32(reply + ZZAPP_STATUS_FLIP_GEN,
			       life_get(ZZAPP_LIFE_FLIP_GEN_OFF));
		*reply_len = ZZAPP_STATUS_REPLY_BYTES;
		return SDK_STATUS_OK;
	case ZZAPP_OP_DISPLAY_BIND:
		rc = bind_surface(req, req_len);
		return zz_to_sdk(rc);
	case ZZAPP_OP_LOAD_BEGIN:
	case ZZAPP_OP_LOAD_CHUNK:
	case ZZAPP_OP_LOAD_END:
	case ZZAPP_OP_START:
		if (request_id == 0U)
			return SDK_STATUS_BAD_REQUEST;
		if (!queue_deferred(opcode, req, req_len, request_id, user_cookie))
			return SDK_STATUS_BUSY;
#ifdef ZZAPP_HOST_TEST
		run_deferred();
		if (opcode != ZZAPP_OP_START && defer_state == DEFER_DONE) {
			defer_state = DEFER_IDLE;
			return defer_status;
		}
#endif
		return SDK_STATUS_QUEUED;
	default:
		return SDK_STATUS_UNSUPPORTED;
	}
}

int zzapp_mailbox_harvest(uint32_t *request_id, uint32_t *user_cookie,
			  uint16_t *opcode, uint16_t *status,
			  uint8_t *payload, uint16_t *payload_len)
{
	if (defer_state != DEFER_DONE)
		return 0;
	if (request_id)
		*request_id = defer_request_id;
	if (user_cookie)
		*user_cookie = defer_cookie;
	if (opcode)
		*opcode = defer_opcode;
	if (status)
		*status = defer_status;
	if (payload_len)
		*payload_len = defer_reply_len;
	if (payload && defer_reply_len != 0U)
		memcpy(payload, defer_reply, defer_reply_len);
	return 1;
}

void zzapp_mailbox_harvest_ack(void)
{
	if (defer_state == DEFER_DONE) {
		barrier();
		defer_state = DEFER_IDLE;
	}
}

#ifdef ZZAPP_HOST_TEST
void zzapp_services_test_reset(void)
{
	audio_detach();
	forget_window();
	defer_state = DEFER_IDLE;
	start_pending = 0U;
	yield_hook = NULL;
	time_hook = NULL;
	memset(slot_busy_test, 0, sizeof(slot_busy_test));
	log_ring_count = 0U;
}

void zzapp_services_test_set_window(void *base, uint32_t bytes)
{
	forget_window();
	if (base && bytes >= ZZAPP_WINDOW_BYTES)
		init_window((uint8_t *)base, bytes, 1);
}

void zzapp_services_test_set_yield(void (*fn)(void))
{
	yield_hook = fn;
}

void zzapp_services_test_set_time(uint64_t (*fn)(void))
{
	time_hook = fn;
}

void zzapp_services_test_set_slot_busy(uint32_t slot, int busy)
{
	if (slot < 4U)
		slot_busy_test[slot] = busy ? 1 : 0;
}
#endif
