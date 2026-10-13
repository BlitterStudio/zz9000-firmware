/*
 * Sliding input window for the WebM backend. See webm_window.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "webm_window.h"

#include <string.h>

static int win_read(void *ctx, void *dst, uint32_t n)
{
	struct webm_window *w = (struct webm_window *)ctx;
	uint32_t avail;

	if (w->cursor < w->start)
		return -1;
	avail = w->start + w->filled - w->cursor;
	if (avail == 0U)
		return w->eof ? 0 : -2;
	if (n > avail)
		n = avail;
	memcpy(dst, w->data + (w->cursor - w->start), n);
	w->cursor += n;
	return (int)n;
}

static int win_seek(void *ctx, uint32_t pos)
{
	struct webm_window *w = (struct webm_window *)ctx;

	if (pos < w->start)
		return -1;
	if (pos - w->start <= w->filled) {
		w->cursor = pos;
		return 0;
	}
	return w->eof ? -1 : -2;
}

/* Past the held bytes, nothing before pos is read again: empty the window
 * at pos and drop the rest of the skipped bytes as they are appended. */
static int win_skip(void *ctx, uint32_t pos)
{
	struct webm_window *w = (struct webm_window *)ctx;
	uint32_t end = w->start + w->filled;
	int rc = win_seek(ctx, pos);

	/* Still dropping: the bytes before pos have not arrived. */
	if (rc != -2 || w->drop_left != 0U)
		return rc;
	w->drop_left = pos - end;
	w->start = pos;
	w->filled = 0U;
	w->cursor = pos;
	w->mark = pos;
	return 1;
}

static uint32_t win_tell(void *ctx)
{
	return ((struct webm_window *)ctx)->cursor;
}

void webm_window_init(struct webm_window *w, uint8_t *data, uint32_t cap)
{
	memset(w, 0, sizeof(*w));
	w->data = data;
	w->cap = cap;
}

void webm_window_bind(struct webm_window *w, struct webm_io *io)
{
	memset(io, 0, sizeof(*io));
	io->ctx = w;
	io->read = win_read;
	io->seek = win_seek;
	io->tell = win_tell;
	io->skip = win_skip;
}

uint32_t webm_window_space(const struct webm_window *w)
{
	uint32_t space = w->cap - w->filled;

	/* Skipped bytes still to arrive are dropped, not stored. */
	return w->drop_left > UINT32_MAX - space ? UINT32_MAX
	                                         : space + w->drop_left;
}

int webm_window_append(struct webm_window *w, const uint8_t *src, uint32_t n)
{
	uint32_t drop = n < w->drop_left ? n : w->drop_left;

	if (n - drop > w->cap - w->filled)
		return 0;
	w->drop_left -= drop;
	src += drop;
	n -= drop;
	if (n != 0U)
		memcpy(w->data + w->filled, src, n);
	w->filled += n;
	return 1;
}

void webm_window_compact(struct webm_window *w)
{
	uint32_t drop;

	if (w->cursor < w->start || w->cursor - w->start > w->filled)
		return;
	drop = w->cursor - w->start;
	if (drop == 0U)
		return;
	memmove(w->data, w->data + drop, w->filled - drop);
	w->filled -= drop;
	w->start += drop;
}

void webm_window_mark(struct webm_window *w)
{
	w->mark = w->cursor;
}

/* Only the cursor moves during a demux call; appends and compaction
 * happen between calls, never between mark and rewind. */
void webm_window_rewind(struct webm_window *w)
{
	w->cursor = w->mark;
}

int webm_window_exhausted(const struct webm_window *w, uint32_t origin)
{
	uint32_t end = w->start + w->filled;

	/* Compaction can drop everything before origin, never past it, so
	 * end - origin is the most a retry from origin could ever see. */
	return !w->eof && origin >= w->start && origin <= end &&
	       end - origin >= w->cap;
}
