/*
 * Sliding input window for the WebM media-session backend.
 *
 * Client writes append at the tail; the demux reads through a webm_io
 * bound to the window using absolute stream offsets. Compaction drops the
 * bytes before the read cursor, so the window holds only what the demux
 * may still need. mark/rewind save and restore the read cursor around one
 * demux call: when it runs out of input (WEBM_ERR_NEED) the caller rewinds
 * and retries after the next write. Free of codec libraries so host tests
 * can drive it directly.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef WEBM_WINDOW_H
#define WEBM_WINDOW_H

#include <stdint.h>

#include "webm_parse.h"

struct webm_window {
	uint8_t *data;
	uint32_t cap;
	uint32_t start;  /* stream offset of data[0] */
	uint32_t filled; /* valid bytes from data[0] */
	uint32_t cursor; /* stream offset of the next read */
	int eof;
	uint32_t mark; /* cursor saved by webm_window_mark */
};

void webm_window_init(struct webm_window *w, uint8_t *data, uint32_t cap);
/* Points io at the window: read/seek/tell, no size (the stream length is
 * unknown until EOF). */
void webm_window_bind(struct webm_window *w, struct webm_io *io);
/* Bytes an append can take without compacting. */
uint32_t webm_window_space(const struct webm_window *w);
/* 1 when all n bytes were appended, 0 (nothing appended) when they do not
 * fit. Never compacts: before the stream header parses, the demux restarts
 * from the first byte and needs all of it. */
int webm_window_append(struct webm_window *w, const uint8_t *src, uint32_t n);
void webm_window_compact(struct webm_window *w);
void webm_window_mark(struct webm_window *w);
void webm_window_rewind(struct webm_window *w);

#endif
