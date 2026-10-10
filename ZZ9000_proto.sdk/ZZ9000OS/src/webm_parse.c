/*
 * Bounded WebM/Matroska demux. See webm_parse.h for the subset.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "webm_parse.h"

#include <string.h>

enum {
	ID_EBML = 0x1A45DFA3,
	ID_DOCTYPE = 0x4282,
	ID_SEGMENT = 0x18538067,
	ID_INFO = 0x1549A966,
	ID_SCALE = 0x2AD7B1,
	ID_TRACKS = 0x1654AE6B,
	ID_TRACK = 0xAE,
	ID_TRKNUM = 0xD7,
	ID_TRKTYPE = 0x83,
	ID_CODECID = 0x86,
	ID_CODECPRIV = 0x63A2,
	ID_VIDEO = 0xE0,
	ID_AUDIO = 0xE1,
	ID_WIDTH = 0xB0,
	ID_HEIGHT = 0xBA,
	ID_CHANNELS = 0x9F,
	ID_SAMPLERATE = 0xB5,
	ID_DURATION = 0x23E383,
	ID_ENCODINGS = 0x6D80,
	ID_ENCRYPTION = 0x5035,
	ID_CLUSTER = 0x1F43B675,
	ID_CLUSTERTC = 0xE7,
	ID_SIMPLE = 0xA3,
	ID_GROUP = 0xA0,
	ID_BLOCK = 0xA1,
	ID_REFBLOCK = 0xFB,
	ID_CUES = 0x1C53BB6B,
	ID_SEEKHEAD = 0x114D9B74,
	ID_CHAPTERS = 0x1043A770,
	ID_TAGS = 0x1254C367,
	ID_ATTACH = 0x1941A469,
	ID_VOID = 0xEC,
	ID_CRC = 0xBF
};

static int encrypted_id(uint32_t id)
{
	return id == ID_ENCODINGS || id == ID_ENCRYPTION;
}
static int fail(struct webm_demux *d, int err)
{
	d->error = err;
	return -1;
}

static uint32_t tell(struct webm_demux *d)
{
	return d->io.tell(d->io.ctx);
}

/* Element sizes are hostile input: compare them with the bytes left in the
 * parent instead of adding them to the cursor, so a size near 2^32 cannot
 * wrap an end offset backwards and re-parse (or loop over) earlier bytes.
 * room(): bytes from the cursor to a bounded end. room_in(): the same with
 * end 0 meaning an unbounded parent, limited by the 32-bit offset space. */
static uint32_t room(struct webm_demux *d, uint32_t end)
{
	uint32_t pos = tell(d);

	return pos < end ? end - pos : 0U;
}

static uint32_t room_in(struct webm_demux *d, uint32_t end)
{
	return end ? room(d, end) : 0xFFFFFFFFU - tell(d);
}

static int read_full(struct webm_demux *d, void *dst, uint32_t n)
{
	uint8_t *p = dst;

	while (n) {
		int got = d->io.read(d->io.ctx, p, n);

		if (got == -2)
			return fail(d, WEBM_ERR_NEED);
		if (got < 0)
			return fail(d, WEBM_ERR_IO);
		if (got == 0)
			return fail(d, WEBM_ERR_TRUNC);
		p += got;
		n -= (uint32_t)got;
	}
	return 0;
}

static int skip_n(struct webm_demux *d, uint64_t n)
{
	int rc;

	if (n == 0)
		return 0;
	if (n > room_in(d, 0U))
		return fail(d, WEBM_ERR_LAYOUT);
	rc = d->io.seek(d->io.ctx, tell(d) + (uint32_t)n);
	if (rc == -2)
		return fail(d, WEBM_ERR_NEED);
	if (rc != 0)
		return fail(d, WEBM_ERR_IO);
	return 0;
}

static int read_vint(struct webm_demux *d, uint64_t *val, uint32_t *len,
		     int mask_len, int *unknown)
{
	uint8_t b[8];
	uint32_t n = 1, i;
	uint8_t mark = 0x80;
	uint64_t v;

	if (read_full(d, b, 1) != 0)
		return -1;
	while (n < 8 && (b[0] & mark) == 0) {
		mark >>= 1;
		n++;
	}
	if ((b[0] & mark) == 0)
		return fail(d, WEBM_ERR_LAYOUT);
	if (n > 1 && read_full(d, b + 1, n - 1) != 0)
		return -1;
	v = mask_len ? (uint64_t)(b[0] & (mark - 1U)) : b[0];
	for (i = 1; i < n; i++)
		v = (v << 8) | b[i];
	if (unknown)
		*unknown = mask_len && v == ((1ULL << (7 * n)) - 1ULL);
	*val = v;
	*len = n;
	return 0;
}

static int read_id_size(struct webm_demux *d, uint32_t *id, uint64_t *size,
			int *unknown)
{
	uint64_t raw;
	uint32_t len;

	if (read_vint(d, &raw, &len, 0, NULL) != 0)
		return -1;
	if (len > 4 || raw > 0xFFFFFFFFULL)
		return fail(d, WEBM_ERR_LAYOUT);
	*id = (uint32_t)raw;
	return read_vint(d, size, &len, 1, unknown);
}

static uint64_t be_uint(const uint8_t *p, uint32_t n)
{
	uint64_t v = 0;
	uint32_t i;

	for (i = 0; i < n; i++)
		v = (v << 8) | p[i];
	return v;
}

static int parse_track(struct webm_demux *d, uint32_t end)
{
	struct webm_track *t;
	uint8_t scratch[16];

	if (d->ntracks >= WEBM_MAX_TRACKS)
		return fail(d, WEBM_ERR_LIMIT);
	t = &d->tracks[d->ntracks];
	memset(t, 0, sizeof(*t));
	while (room(d, end) > 1U) {
		uint32_t id, n, child_end;
		uint64_t size;
		int unknown = 0;

		if (read_id_size(d, &id, &size, &unknown) != 0)
			return -1;
		if (unknown || size > room(d, end))
			return fail(d, WEBM_ERR_LAYOUT);
		n = (uint32_t)size;
		child_end = tell(d) + n;
		if (id == ID_VIDEO || id == ID_AUDIO) {
			while (room(d, child_end) > 1U) {
				uint32_t cid, cn;
				uint64_t csz;
				int cu = 0;

				if (read_id_size(d, &cid, &csz, &cu) != 0)
					return -1;
				if (cu || csz > room(d, child_end))
					return fail(d, WEBM_ERR_LAYOUT);
				cn = (uint32_t)csz;
				/* Colour, Projection and other metadata the
				 * demux does not read can be masters or longer
				 * than any number it does read: skip them. */
				if (cn > 8U) {
					if (cid == ID_WIDTH || cid == ID_HEIGHT ||
					    cid == ID_CHANNELS ||
					    cid == ID_SAMPLERATE)
						return fail(d, WEBM_ERR_LAYOUT);
					if (skip_n(d, cn) != 0)
						return -1;
					continue;
				}
				if (read_full(d, scratch, cn) != 0)
					return -1;
				if (cid == ID_WIDTH)
					t->width = (uint32_t)be_uint(scratch, cn);
				else if (cid == ID_HEIGHT)
					t->height = (uint32_t)be_uint(scratch, cn);
				else if (cid == ID_CHANNELS)
					t->channels = (uint16_t)be_uint(scratch, cn);
				else if (cid == ID_SAMPLERATE && (cn == 4 || cn == 8)) {
					uint8_t le[8];
					uint32_t i;
					double rate;

					for (i = 0; i < cn; i++)
						le[i] = scratch[cn - 1U - i];
					if (cn == 4) {
						float f;
						memcpy(&f, le, 4);
						rate = (double)f;
					} else {
						memcpy(&rate, le, 8);
					}
					if (rate > 0.0 && rate < 1000000.0)
						t->rate = (uint32_t)(rate + 0.5);
				}
			}
			if (skip_n(d, room(d, child_end)) != 0)
				return -1;
			continue;
		}
		if (encrypted_id(id))
			return fail(d, WEBM_ERR_CODEC);
		if (id == ID_CODECPRIV) {
			if (n > WEBM_MAX_PRIV || d->priv_used > WEBM_MAX_PRIV - n)
				return fail(d, WEBM_ERR_LIMIT);
			t->priv_off = d->priv_used;
			if (n && read_full(d, d->priv_store + d->priv_used, n) != 0)
				return -1;
			d->priv_used += n;
			t->priv_len = n;
			continue;
		}
		if (id == ID_DURATION) {
			uint8_t b[8];

			if (n == 0 || n > 8)
				return fail(d, WEBM_ERR_LAYOUT);
			if (read_full(d, b, n) != 0)
				return -1;
			t->default_duration_ns = be_uint(b, n);
			continue;
		}
		if (id == ID_TRKNUM || id == ID_TRKTYPE || id == ID_CODECID) {
			uint8_t tmp[64];

			if (n >= sizeof(tmp))
				return fail(d, WEBM_ERR_LIMIT);
			if (read_full(d, tmp, n) != 0)
				return -1;
			if (id == ID_TRKNUM)
				t->number = (uint32_t)be_uint(tmp, n);
			else if (id == ID_TRKTYPE)
				t->type = (uint8_t)be_uint(tmp, n);
			else if (n >= 5 && memcmp(tmp, "V_VP8", 5) == 0)
				t->codec = WEBM_CODEC_VP8;
			else if (n >= 5 && memcmp(tmp, "V_VP9", 5) == 0)
				t->codec = WEBM_CODEC_VP9;
			else if (n >= 6 && memcmp(tmp, "A_OPUS", 6) == 0)
				t->codec = WEBM_CODEC_OPUS;
			else if (n >= 8 && memcmp(tmp, "A_VORBIS", 8) == 0)
				t->codec = WEBM_CODEC_VORBIS;
			continue;
		}
		if (skip_n(d, n) != 0)
			return -1;
	}
	if (skip_n(d, room(d, end)) != 0)
		return -1;
	if (t->type == 1) {
		if (d->have_video ||
		    (t->codec != WEBM_CODEC_VP8 && t->codec != WEBM_CODEC_VP9))
			return fail(d, WEBM_ERR_CODEC);
		d->have_video = 1;
	} else if (t->type == 2) {
		if (d->have_audio ||
		    (t->codec != WEBM_CODEC_OPUS && t->codec != WEBM_CODEC_VORBIS))
			return fail(d, WEBM_ERR_CODEC);
		d->have_audio = 1;
	}
	d->ntracks++;
	return 0;
}

static int skip_container(struct webm_demux *d, uint32_t end, int kind)
{
	while (room(d, end) > 2U) {
		uint32_t id;
		uint64_t size;
		int unknown = 0;

		if (read_id_size(d, &id, &size, &unknown) != 0)
			return -1;
		if (unknown || size > room(d, end))
			return fail(d, WEBM_ERR_LAYOUT);
		if (kind == 1 && id == ID_SCALE) {
			uint8_t b[8];
			uint32_t n = (uint32_t)size;

			if (n == 0 || n > 8)
				return fail(d, WEBM_ERR_LAYOUT);
			/* Keep NEED: a header split across writes retries. */
			if (read_full(d, b, n) != 0)
				return -1;
			d->scale_ns = (uint32_t)be_uint(b, n);
			if (d->scale_ns == 0)
				d->scale_ns = 1000000U;
			continue;
		}
		if (kind == 2 && id == ID_TRACK) {
			if (parse_track(d, tell(d) + (uint32_t)size) != 0)
				return -1;
			continue;
		}
		if (encrypted_id(id))
			return fail(d, WEBM_ERR_CODEC);
		if (skip_n(d, (uint32_t)size) != 0)
			return -1;
	}
	return skip_n(d, room(d, end));
}

/* In-place. payload_off is the first byte after the block header. */
static int split_lace(struct webm_demux *d, uint32_t payload_off,
		      uint32_t payload_len, unsigned lacing)
{
	const uint8_t *data = d->buf + payload_off;
	uint32_t sizes[WEBM_MAX_LACE];
	uint32_t i, nframes, left, hdr;
	const uint8_t *s;

	if (lacing == 0) {
		if (payload_len == 0 || payload_len > WEBM_MAX_FRAME)
			return fail(d, WEBM_ERR_LIMIT);
		d->lace_off[0] = payload_off;
		d->lace_len[0] = payload_len;
		d->lace_n = 1;
		d->lace_i = 0;
		return 0;
	}
	if (payload_len < 2)
		return fail(d, WEBM_ERR_LAYOUT);
	nframes = (uint32_t)data[0] + 1U;
	if (nframes > WEBM_MAX_LACE)
		return fail(d, WEBM_ERR_LIMIT);
	s = data + 1;
	left = payload_len - 1U;
	if (lacing == 1) {
		for (i = 0; i + 1U < nframes; i++) {
			uint32_t sz = 0;

			for (;;) {
				if (left == 0)
					return fail(d, WEBM_ERR_LAYOUT);
				sz += *s;
				left--;
				if (*s++ != 255)
					break;
			}
			sizes[i] = sz;
		}
	} else if (lacing == 2) {
		uint32_t each = left / nframes;

		if (left % nframes)
			return fail(d, WEBM_ERR_LAYOUT);
		for (i = 0; i + 1U < nframes; i++)
			sizes[i] = each;
		s = data + 1;
	} else {
		uint32_t len, k;
		uint8_t mark;
		uint64_t v;
		int64_t prev;

		len = 1;
		mark = 0x80;
		if (left == 0)
			return fail(d, WEBM_ERR_LAYOUT);
		while (len < 8 && (s[0] & mark) == 0) {
			mark >>= 1;
			len++;
		}
		if (len > left)
			return fail(d, WEBM_ERR_LAYOUT);
		v = (uint64_t)(s[0] & (mark - 1U));
		for (k = 1; k < len; k++)
			v = (v << 8) | s[k];
		sizes[0] = (uint32_t)v;
		prev = (int64_t)v;
		s += len;
		left -= len;
		for (i = 1; i + 1U < nframes; i++) {
			int64_t delta, bias;

			len = 1;
			mark = 0x80;
			if (left == 0)
				return fail(d, WEBM_ERR_LAYOUT);
			while (len < 8 && (s[0] & mark) == 0) {
				mark >>= 1;
				len++;
			}
			if (len > left)
				return fail(d, WEBM_ERR_LAYOUT);
			v = (uint64_t)(s[0] & (mark - 1U));
			for (k = 1; k < len; k++)
				v = (v << 8) | s[k];
			bias = (int64_t)((1ULL << (7U * len - 1U)) - 1ULL);
			delta = (int64_t)v - bias;
			prev += delta;
			if (prev < 0 || prev > 0xFFFFFFFFLL)
				return fail(d, WEBM_ERR_LAYOUT);
			sizes[i] = (uint32_t)prev;
			s += len;
			left -= len;
		}
	}
	{
		uint32_t used = 0;

		for (i = 0; i + 1U < nframes; i++)
			used += sizes[i];
		if (used > left)
			return fail(d, WEBM_ERR_LAYOUT);
		sizes[nframes - 1U] = left - used;
	}
	hdr = (uint32_t)(s - data);
	for (i = 0; i < nframes; i++) {
		d->lace_off[i] = payload_off + hdr;
		d->lace_len[i] = sizes[i];
		hdr += sizes[i];
		if (sizes[i] > WEBM_MAX_FRAME)
			return fail(d, WEBM_ERR_LIMIT);
	}
	d->lace_n = (uint8_t)nframes;
	d->lace_i = 0;
	return 0;
}

/* A Block's key flag lives in its BlockGroup; webm_next sets it after the
 * group. A SimpleBlock carries its own. */
static int take_block(struct webm_demux *d, uint32_t size, int simple)
{
	uint32_t tlen = 1, i, track;
	uint8_t mark = 0x80;
	int16_t rel;
	unsigned lacing;

	if (size < 4 || size > d->buf_cap || size > WEBM_MAX_FRAME)
		return fail(d, WEBM_ERR_LIMIT);
	if (read_full(d, d->buf, size) != 0)
		return -1;
	while (tlen < 8 && (d->buf[0] & mark) == 0) {
		mark >>= 1;
		tlen++;
	}
	if (tlen + 3 > size)
		return fail(d, WEBM_ERR_LAYOUT);
	track = (uint32_t)(d->buf[0] & (mark - 1U));
	for (i = 1; i < tlen; i++)
		track = (track << 8) | d->buf[i];
	rel = (int16_t)((d->buf[tlen] << 8) | d->buf[tlen + 1]);
	lacing = (unsigned)((d->buf[tlen + 2] >> 1) & 3);
	d->lace_key = simple ? (d->buf[tlen + 2] & 0x80) != 0 : 1;
	if (split_lace(d, tlen + 3, size - tlen - 3, lacing) != 0)
		return -1;
	d->lace_track = track;
	d->lace_tc = d->cluster_tc + rel;
	return 0;
}

static int emit(struct webm_demux *d, struct webm_block *blk)
{
	const struct webm_track *t = webm_track(d, d->lace_track);

	if (!t)
		return fail(d, WEBM_ERR_LAYOUT);
	blk->track = d->lace_track;
	blk->codec = t->codec;
	blk->keyframe = d->lace_key;
	blk->lace_index = d->lace_i;
	blk->lace_count = d->lace_n;
	blk->timecode = d->lace_tc;
	blk->timecode_ms = d->scale_ns == 1000000U
		? d->lace_tc
		: (d->lace_tc * (int64_t)d->scale_ns) / 1000000;
	blk->data = d->buf + d->lace_off[d->lace_i];
	blk->size = d->lace_len[d->lace_i];
	d->lace_i++;
	return 1;
}

static int enter_cluster(struct webm_demux *d, uint64_t size, int unknown)
{
	d->in_cluster = 1;
	d->cluster_tc = 0;
	d->cluster_end = 0U;
	if (unknown)
		return 0;
	if (size > room_in(d, 0U)) {
		/* Ends past the 32-bit offsets: it cannot fit a bounded
		 * Segment or input, and an unbounded one runs it to the next
		 * Cluster or EOF like an unknown size. */
		return d->segment_end ? fail(d, WEBM_ERR_LAYOUT) : 0;
	}
	d->cluster_end = tell(d) + (uint32_t)size;
	return 0;
}

int webm_open(struct webm_demux *d, const struct webm_io *io, uint8_t *buf,
	      uint32_t cap)
{
	uint32_t id;
	uint64_t size;
	int unknown = 0;
	uint8_t doctype[16];
	struct webm_io saved;

	if (!d || !io || !buf || cap < 4096 || !io->read || !io->seek || !io->tell)
		return -1;
	saved = *io;
	memset(d, 0, sizeof(*d));
	d->io = saved;
	d->buf = buf;
	d->buf_cap = cap;
	d->scale_ns = 1000000U;
	if (read_id_size(d, &id, &size, &unknown) != 0)
		return -1;
	if (id != ID_EBML || unknown || size > 256)
		return fail(d, WEBM_ERR_LAYOUT);
	{
		uint32_t end = tell(d) + (uint32_t)size;

		while (room(d, end) > 2U) {
			uint32_t cid;
			uint64_t csz;
			int cu = 0;

			if (read_id_size(d, &cid, &csz, &cu) != 0)
				return -1;
			if (cu || csz > 64)
				return fail(d, WEBM_ERR_LAYOUT);
			if (cid == ID_DOCTYPE) {
				if (csz >= sizeof(doctype))
					return fail(d, WEBM_ERR_LAYOUT);
				if (read_full(d, doctype, (uint32_t)csz) != 0)
					return -1;
				doctype[csz] = 0;
				if (strcmp((char *)doctype, "webm") != 0 &&
				    strcmp((char *)doctype, "matroska") != 0)
					return fail(d, WEBM_ERR_CODEC);
			} else if (skip_n(d, (uint32_t)csz) != 0) {
				return -1;
			}
		}
	}
	if (read_id_size(d, &id, &size, &unknown) != 0)
		return -1;
	if (id != ID_SEGMENT)
		return fail(d, WEBM_ERR_LAYOUT);
	if (unknown || size > room_in(d, 0U)) {
		/* Unknown, or a Segment ending past the 32-bit offsets this
		 * demux tracks (>= 4 GB): bounded by the input instead. */
		d->segment_end = io->size ? io->size(io->ctx) : 0U;
	} else {
		d->segment_end = tell(d) + (uint32_t)size;
	}
	for (;;) {
		if (d->segment_end && room(d, d->segment_end) <= 2U)
			return fail(d, WEBM_ERR_LAYOUT);
		if (read_id_size(d, &id, &size, &unknown) != 0)
			return -1;
		if (id == ID_CLUSTER) {
			if (!d->have_video)
				return fail(d, WEBM_ERR_CODEC);
			return enter_cluster(d, size, unknown);
		}
		if (unknown || size > room_in(d, d->segment_end))
			return fail(d, WEBM_ERR_LAYOUT);
		if (id == ID_INFO) {
			if (skip_container(d, tell(d) + (uint32_t)size, 1) != 0)
				return -1;
		} else if (id == ID_TRACKS) {
			if (skip_container(d, tell(d) + (uint32_t)size, 2) != 0)
				return -1;
		} else if (encrypted_id(id)) {
			return fail(d, WEBM_ERR_CODEC);
		} else if (skip_n(d, size) != 0) {
			return -1;
		}
	}
}

int webm_next(struct webm_demux *d, struct webm_block *blk)
{
	if (d->lace_i < d->lace_n)
		return emit(d, blk);
	for (;;) {
		uint32_t id, pos;
		uint64_t size;
		int unknown = 0;

		if (d->in_cluster && d->cluster_end &&
		    room(d, d->cluster_end) == 0U)
			d->in_cluster = 0;
		pos = tell(d);
		if (d->segment_end && room(d, d->segment_end) <= 2U)
			return 0;
		if (d->io.size && pos >= d->io.size(d->io.ctx))
			return 0;
		if (read_id_size(d, &id, &size, &unknown) != 0)
			return d->error == WEBM_ERR_TRUNC ? 0 : -1;
		if (!d->in_cluster) {
			if (id == ID_CLUSTER) {
				if (enter_cluster(d, size, unknown) != 0)
					return -1;
				continue;
			}
			if (unknown || size > room_in(d, d->segment_end))
				return fail(d, WEBM_ERR_LAYOUT);
			if (encrypted_id(id))
				return fail(d, WEBM_ERR_CODEC);
			if (skip_n(d, size) != 0)
				return -1;
			continue;
		}
		if (id == ID_CLUSTERTC) {
			uint8_t b[8];

			if (unknown || size == 0 || size > 8)
				return fail(d, WEBM_ERR_LAYOUT);
			if (read_full(d, b, (uint32_t)size) != 0)
				return -1;
			d->cluster_tc = (int64_t)be_uint(b, (uint32_t)size);
			continue;
		}
		if (id == ID_SIMPLE || id == ID_BLOCK) {
			if (unknown || size > room_in(d, d->cluster_end))
				return fail(d, WEBM_ERR_LAYOUT);
			if (take_block(d, (uint32_t)size, id == ID_SIMPLE) != 0)
				return -1;
			return emit(d, blk);
		}
		if (id == ID_GROUP) {
			uint32_t end;
			int saw = 0, key = 1;

			if (unknown || size > room_in(d, d->cluster_end))
				return fail(d, WEBM_ERR_LAYOUT);
			end = tell(d) + (uint32_t)size;
			while (room(d, end) > 1U) {
				uint32_t cid;
				uint64_t csz;
				int cu = 0;

				if (read_id_size(d, &cid, &csz, &cu) != 0)
					return -1;
				if (cu || csz > room(d, end))
					return fail(d, WEBM_ERR_LAYOUT);
				if (cid == ID_REFBLOCK)
					key = 0;
				if (cid == ID_BLOCK) {
					if (take_block(d, (uint32_t)csz, 0) != 0)
						return -1;
					saw = 1;
				} else if (skip_n(d, csz) != 0) {
					return -1;
				}
			}
			if (skip_n(d, room(d, end)) != 0)
				return -1;
			if (saw) {
				/* A ReferenceBlock may follow the Block, so the
				 * key flag is known only once the group is read. */
				d->lace_key = (uint8_t)key;
				return emit(d, blk);
			}
			continue;
		}
		if (id == ID_CLUSTER) {
			if (enter_cluster(d, size, unknown) != 0)
				return -1;
			continue;
		}
		if (unknown || size > room_in(d, d->cluster_end))
			return fail(d, WEBM_ERR_LAYOUT);
		if (encrypted_id(id))
			return fail(d, WEBM_ERR_CODEC);
		if (skip_n(d, size) != 0)
			return -1;
	}
}

void webm_close(struct webm_demux *d)
{
	if (!d)
		return;
	d->ntracks = 0;
	d->priv_used = 0;
}

const struct webm_track *webm_track(const struct webm_demux *d, uint32_t number)
{
	unsigned i;

	for (i = 0; i < d->ntracks; i++)
		if (d->tracks[i].number == number)
			return &d->tracks[i];
	return NULL;
}

const uint8_t *webm_track_priv(const struct webm_demux *d,
			       const struct webm_track *t)
{
	if (!d || !t || t->priv_len == 0 ||
	    t->priv_off > WEBM_MAX_PRIV - t->priv_len)
		return NULL;
	return d->priv_store + t->priv_off;
}

int webm_size_allowed(uint32_t width, uint32_t height)
{
	uint32_t side;

	if (width == 0U || height == 0U)
		return 0;
	if (width > 0xFFFFFFFFU / height)
		return 0;
	side = width > height ? width : height;
	return side <= WEBM_SAFETY_MAX_SIDE &&
	       width * height <= WEBM_SAFETY_MAX_PIXELS;
}

static int bit_get(const uint8_t *p, uint32_t nbits, uint32_t *bit, uint32_t *out)
{
	uint32_t i, v = 0;

	if (*bit + nbits > 256U)
		return 0;
	for (i = 0; i < nbits; i++) {
		uint32_t at = *bit + i;
		uint32_t byte = at >> 3;

		if (byte >= 16U)
			return 0;
		v = (v << 1) | ((p[byte] >> (7U - (at & 7U))) & 1U);
	}
	*bit += nbits;
	*out = v;
	return 1;
}

int webm_payload_is_keyframe(uint8_t codec, const uint8_t *data, uint32_t size)
{
	uint32_t bit = 0, v = 0;

	if (!data || size < 1U)
		return 0;
	if (codec == WEBM_CODEC_VP8)
		return size >= 3U && (data[0] & 1U) == 0U;
	if (codec != WEBM_CODEC_VP9)
		return 0;
	/* Uncompressed header: marker, profile, optional reserved,
	 * show_existing_frame, then frame_type. A show-existing frame
	 * is not a reference reset. */
	if (!bit_get(data, 2, &bit, &v) || v != 2U)
		return 0;
	if (!bit_get(data, 1, &bit, &v))
		return 0;
	{
		uint32_t profile = v;

		if (!bit_get(data, 1, &bit, &v))
			return 0;
		profile |= v << 1;
		if (profile == 3U && !bit_get(data, 1, &bit, &v))
			return 0;
	}
	if (!bit_get(data, 1, &bit, &v))
		return 0;
	if (v != 0U)
		return 0;
	if (!bit_get(data, 1, &bit, &v))
		return 0;
	return v == 0U;
}

void webm_skip_start(struct webm_skip *s, int active, uint32_t budget)
{
	s->active = active != 0;
	s->left = budget;
}

int webm_skip_block(struct webm_skip *s, const struct webm_block *blk)
{
	if (!s->active ||
	    webm_payload_is_keyframe(blk->codec, blk->data, blk->size))
		return WEBM_SKIP_DECODE;
	if (s->left <= 1U) {
		s->left = 0U;
		return WEBM_SKIP_YIELD;
	}
	s->left--;
	return WEBM_SKIP_DROP;
}

int webm_pts90(int64_t ticks, uint32_t scale_ns, uint64_t *out)
{
	uint64_t product;

	if (!out)
		return 0;
	if (ticks < 0)
		ticks = 0;
	if (scale_ns == 0U)
		scale_ns = 1000000U;
	if ((uint64_t)ticks > UINT64_MAX / scale_ns)
		return 0;
	product = (uint64_t)ticks * scale_ns;
	if (product > UINT64_MAX / 9U)
		return 0;
	/* 90 kHz = ticks * scale_ns * 90000 / 1e9 = ticks * scale_ns * 9 / 1e5. */
	*out = product * 9U / 100000U;
	return 1;
}

uint32_t webm_rate_milli(uint64_t duration_ns)
{
	uint64_t milli;

	if (duration_ns == 0U || duration_ns > 1000000000ULL)
		return 30000U;
	milli = 1000000000000ULL / duration_ns;
	if (milli == 0U || milli > 240000U)
		return 30000U;
	return (uint32_t)milli;
}

int webm_xiph_split(const uint8_t *p, uint32_t n, const uint8_t *pkt[3],
		    uint32_t len[3])
{
	uint32_t off = 1U;
	uint32_t i;

	if (!p || n < 3U || p[0] != 2U)
		return 0;
	for (i = 0U; i < 2U; i++) {
		uint32_t acc = 0U;

		for (;;) {
			if (off >= n)
				return 0;
			acc += p[off];
			if (p[off++] != 255U)
				break;
		}
		len[i] = acc;
	}
	if (off + len[0] + len[1] > n)
		return 0;
	len[2] = n - off - len[0] - len[1];
	pkt[0] = p + off;
	pkt[1] = pkt[0] + len[0];
	pkt[2] = pkt[1] + len[1];
	return 1;
}
