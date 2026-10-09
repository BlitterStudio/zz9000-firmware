/*
 * Ogg Vorbis backend for the codec-aware audio stream (U8). See
 * sdk_audio_vorbis.h for the page/packet contract with libogg and Tremor.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_audio_vorbis.h"

#include "sdk_mailbox.h"

#include <ogg/ogg.h>
#include "tremor/ivorbiscodec.h"
#include "tremor/codec_internal.h"

#include <string.h>

enum {
	PH_ID = 0,      /* expecting the identification header (BOS page) */
	PH_COMMENT,     /* streaming through the comment header */
	PH_SETUP,
	PH_AUDIO,
	PH_TRAILER,     /* EOS seen: only a clean end or a new BOS may follow */
	PH_IGNORE       /* non-page bytes after EOS: discarded */
};

enum {
	NP_PACKET = 0,  /* next_packet: op filled; next_page: page ready */
	NP_NEED_INPUT,
	NP_END,
	NP_SECOND_STREAM,
	NP_FAIL
};

/* Streaming comment-header parser states. */
enum {
	CM_MAGIC = 0,
	CM_VENDOR_LEN,
	CM_VENDOR,
	CM_COUNT,
	CM_LEN,
	CM_BODY,
	CM_FRAMING,
	CM_DONE
};

#define OGG_SYNC_CHUNK 4096U

struct vorbis_codec {
	ogg_sync_state oy;
	ogg_page og;                /* current page while page_active */
	vorbis_info vi;
	vorbis_comment vc;
	vorbis_dsp_state vd;
	vorbis_block vb;
	unsigned char *pkt;         /* packets spanning pages */
	uint32_t pkt_len;
	uint32_t pkt_cap;
	uint32_t phase;
	uint32_t page_active;
	uint32_t seg;               /* next lacing value of og */
	uint32_t body_off;          /* body offset of seg */
	uint32_t carry;             /* a packet continues on the next page */
	uint32_t pages;
	uint32_t serial;
	uint32_t pageno;
	uint32_t dsp_ready;
	ogg_int64_t packetno;
	/* comment parser */
	uint32_t cm_state;
	uint32_t cm_pos;            /* bytes of the current fixed field seen */
	uint32_t cm_word;           /* little-endian field accumulator */
	uint32_t cm_skip;           /* string bytes still to skip */
	uint32_t cm_count;          /* comments still to parse */
};

static const unsigned char empty_comment[16] = {
	0x03, 'v', 'o', 'r', 'b', 'i', 's',
	0, 0, 0, 0,                 /* vendor length 0 */
	0, 0, 0, 0,                 /* no user comments */
	0x01                        /* framing bit */
};

static int fail(struct sdk_vorbis_state *st, uint16_t status)
{
	st->status = status;
	return NP_FAIL;
}

static int is_vorbis_header(const unsigned char *p, uint32_t len, int type)
{
	return len >= 7U && p[0] == (unsigned char)type &&
	       memcmp(p + 1, "vorbis", 6U) == 0;
}

/* Validate the comment header as it streams past; 0 on malformed input. */
static int comment_feed(struct vorbis_codec *c, const unsigned char *p,
                        uint32_t len)
{
	static const unsigned char magic[7] = {
		0x03, 'v', 'o', 'r', 'b', 'i', 's'
	};
	uint32_t i = 0U;

	while (i < len) {
		switch (c->cm_state) {
		case CM_MAGIC:
			if (p[i++] != magic[c->cm_pos])
				return 0;
			if (++c->cm_pos == sizeof(magic)) {
				c->cm_pos = 0U;
				c->cm_state = CM_VENDOR_LEN;
			}
			break;
		case CM_VENDOR_LEN:
		case CM_COUNT:
		case CM_LEN:
			c->cm_word |= (uint32_t)p[i++] << (8U * c->cm_pos);
			if (++c->cm_pos < 4U)
				break;
			c->cm_pos = 0U;
			if (c->cm_state == CM_COUNT) {
				c->cm_count = c->cm_word;
				c->cm_state = c->cm_count ? CM_LEN : CM_FRAMING;
			} else {
				c->cm_skip = c->cm_word;
				c->cm_state = c->cm_state == CM_VENDOR_LEN ?
				              CM_VENDOR : CM_BODY;
			}
			c->cm_word = 0U;
			break;
		case CM_VENDOR:
		case CM_BODY: {
			uint32_t take = len - i;

			if (take > c->cm_skip)
				take = c->cm_skip;
			i += take;
			c->cm_skip -= take;
			if (c->cm_skip != 0U)
				break;
			if (c->cm_state == CM_VENDOR)
				c->cm_state = CM_COUNT;
			else
				c->cm_state = --c->cm_count ? CM_LEN : CM_FRAMING;
			break;
		}
		case CM_FRAMING:
			if ((p[i++] & 1U) == 0U)
				return 0;
			c->cm_state = CM_DONE;
			break;
		default:
			/* Bytes after the framing bit are ignored, as Tremor
			 * and libvorbis do. */
			return 1;
		}
	}
	return 1;
}

/* Copy the next CRC-checked page out of the sync layer, feeding it input as
 * needed, and check it against the single logical stream. */
static int next_page(struct sdk_vorbis_state *st, struct vorbis_codec *c,
                     struct sdk_audio_codec_io *io)
{
	uint32_t serial, pageno;
	int bos, cont;

	for (;;) {
		uint32_t avail, chunk;
		char *buf;
		int r = ogg_sync_pageout(&c->oy, &c->og);

		if (r > 0)
			break;
		if (r < 0) {
			/* Lost sync (garbage or a CRC mismatch). After the
			 * stream's EOS that is trailing data, ignored. */
			if (c->phase == PH_TRAILER) {
				c->phase = PH_IGNORE;
				return NP_END;
			}
			return fail(st, SDK_STATUS_IO_ERROR);
		}
		avail = io->input_length - io->consumed;
		if (avail == 0U) {
			if (!io->eof)
				return NP_NEED_INPUT;
			/* A partial page after EOS is ignored trailing data;
			 * before it the stream is truncated. */
			if (c->phase == PH_TRAILER)
				return NP_END;
			return fail(st, SDK_STATUS_IO_ERROR);
		}
		chunk = avail < OGG_SYNC_CHUNK ? avail : OGG_SYNC_CHUNK;
		buf = ogg_sync_buffer(&c->oy, (long)chunk);
		if (!buf)
			return fail(st, SDK_STATUS_NO_MEMORY);
		memcpy(buf, io->input + io->consumed, chunk);
		(void)ogg_sync_wrote(&c->oy, (long)chunk);
		io->consumed += chunk;
	}

	serial = (uint32_t)ogg_page_serialno(&c->og);
	pageno = (uint32_t)ogg_page_pageno(&c->og);
	bos = ogg_page_bos(&c->og) != 0;
	cont = ogg_page_continued(&c->og) != 0;
	if (ogg_page_version(&c->og) != 0)
		return fail(st, SDK_STATUS_IO_ERROR);
	if (c->pages == 0U) {
		if (!bos)
			return fail(st, SDK_STATUS_IO_ERROR);
		c->serial = serial;
	} else if (serial != c->serial) {
		/* Another logical stream: multiplexed or chained. */
		if (bos)
			return NP_SECOND_STREAM;
		return fail(st, SDK_STATUS_IO_ERROR);
	} else if (c->phase >= PH_TRAILER || bos || pageno != c->pageno + 1U) {
		/* Page after EOS, repeated BOS, or a lost/duplicated page. */
		return fail(st, SDK_STATUS_IO_ERROR);
	}
	if ((uint32_t)cont != c->carry)
		return fail(st, SDK_STATUS_IO_ERROR);
	c->pageno = pageno;
	c->pages++;
	c->page_active = 1U;
	c->seg = 0U;
	c->body_off = 0U;
	return NP_PACKET;
}

static int append_packet(struct sdk_vorbis_state *st, struct vorbis_codec *c,
                         const unsigned char *data, uint32_t len)
{
	if (len > SDK_VORBIS_MAX_PACKET_BYTES - c->pkt_len)
		return fail(st, SDK_STATUS_UNSUPPORTED);
	if (c->pkt_len + len > c->pkt_cap) {
		uint32_t cap = c->pkt_cap ? c->pkt_cap : 4096U;

		while (cap < c->pkt_len + len)
			cap *= 2U;
		if (cap > SDK_VORBIS_MAX_PACKET_BYTES)
			cap = SDK_VORBIS_MAX_PACKET_BYTES;
		c->pkt = (unsigned char *)sdk_vorbis_realloc(c->pkt, cap);
		if (!c->pkt)
			return fail(st, SDK_STATUS_NO_MEMORY);
		c->pkt_cap = cap;
	}
	memcpy(c->pkt + c->pkt_len, data, len);
	c->pkt_len += len;
	return NP_PACKET;
}

/* Reassemble the next packet from the current (or next) page. */
static int next_packet(struct sdk_vorbis_state *st, struct vorbis_codec *c,
                       struct sdk_audio_codec_io *io, ogg_packet *op)
{
	for (;;) {
		const unsigned char *hdr, *data;
		uint32_t nseg, seg, len, i;
		int complete = 0;
		int last = 1;
		int r;

		if (c->phase == PH_IGNORE) {
			io->consumed = io->input_length;
			return io->eof ? NP_END : NP_NEED_INPUT;
		}
		if (!c->page_active) {
			r = next_page(st, c, io);
			if (r != NP_PACKET)
				return r;
		}
		hdr = c->og.header;
		nseg = hdr[26];
		if (c->seg >= nseg) {
			c->page_active = 0U;
			if (ogg_page_eos(&c->og)) {
				/* Truncated packet, or EOS before audio. */
				if (c->carry || c->phase < PH_AUDIO)
					return fail(st, SDK_STATUS_IO_ERROR);
				c->phase = PH_TRAILER;
			}
			continue;
		}
		seg = c->seg;
		len = 0U;
		while (seg < nseg) {
			uint32_t v = hdr[27U + seg++];

			len += v;
			if (v < 255U) {
				complete = 1;
				break;
			}
		}
		data = c->og.body + c->body_off;
		c->seg = seg;
		c->body_off += len;
		for (i = seg; i < nseg; i++)
			if (hdr[27U + i] < 255U)
				last = 0;

		if (c->phase == PH_COMMENT) {
			if (!comment_feed(c, data, len))
				return fail(st, SDK_STATUS_IO_ERROR);
			c->carry = complete ? 0U : 1U;
			if (!complete)
				continue;
			if (c->cm_state != CM_DONE)
				return fail(st, SDK_STATUS_IO_ERROR);
			op->packet = (unsigned char *)empty_comment;
			op->bytes = (long)sizeof(empty_comment);
		} else if (c->phase == PH_ID && !complete) {
			/* The identification header must end on the BOS page. */
			return fail(st, is_vorbis_header(data, len, 1) ?
			                SDK_STATUS_IO_ERROR :
			                SDK_STATUS_UNSUPPORTED);
		} else if (c->carry || !complete) {
			r = append_packet(st, c, data, len);
			if (r != NP_PACKET)
				return r;
			c->carry = complete ? 0U : 1U;
			if (!complete)
				continue;
			op->packet = c->pkt;
			op->bytes = (long)c->pkt_len;
			c->pkt_len = 0U;
		} else {
			op->packet = (unsigned char *)data;
			op->bytes = (long)len;
		}
		op->b_o_s = c->packetno == 0;
		op->e_o_s = ogg_page_eos(&c->og) && seg == nseg;
		op->granulepos = last ? ogg_page_granulepos(&c->og) : -1;
		op->packetno = c->packetno++;
		return NP_PACKET;
	}
}

/* Bound every codebook before Tremor builds its decode tables. */
static int books_within_bounds(const vorbis_info *vi)
{
	const codec_setup_info *ci = (const codec_setup_info *)vi->codec_setup;
	int i;

	for (i = 0; i < ci->books; i++) {
		const static_codebook *b = ci->book_param[i];
		long used = 0, j;

		for (j = 0; j < b->entries; j++)
			if (b->lengthlist[j] > 0)
				used++;
		if (used > (long)SDK_VORBIS_MAX_BOOK_USED)
			return 0;
	}
	return 1;
}

static int handle_packet(struct sdk_vorbis_state *st, struct vorbis_codec *c,
                         ogg_packet *op, struct sdk_audio_codec_io *io)
{
	const unsigned char *p = op->packet;
	uint32_t len = (uint32_t)op->bytes;

	switch (c->phase) {
	case PH_ID:
		if (len < 7U || memcmp(p + 1, "vorbis", 6U) != 0)
			return fail(st, SDK_STATUS_UNSUPPORTED);   /* not Vorbis */
		if (p[0] != 1U)
			return fail(st, SDK_STATUS_IO_ERROR);      /* out of order */
		if (vorbis_synthesis_headerin(&c->vi, &c->vc, op) != 0 ||
		    c->seg != c->og.header[26])
			return fail(st, SDK_STATUS_IO_ERROR);
		if (c->vi.channels < 1 ||
		    c->vi.channels > (int)SDK_VORBIS_MAX_CHANNELS ||
		    c->vi.rate < (long)SDK_VORBIS_MIN_RATE ||
		    c->vi.rate > (long)SDK_VORBIS_MAX_RATE)
			return fail(st, SDK_STATUS_UNSUPPORTED);
		st->sample_rate = (uint32_t)c->vi.rate;
		st->channels = (uint32_t)c->vi.channels;
		c->phase = PH_COMMENT;
		return NP_PACKET;
	case PH_COMMENT:
		if (vorbis_synthesis_headerin(&c->vi, &c->vc, op) != 0)
			return fail(st, SDK_STATUS_INTERNAL_ERROR);
		c->phase = PH_SETUP;
		return NP_PACKET;
	case PH_SETUP:
		if (!is_vorbis_header(p, len, 5) ||
		    vorbis_synthesis_headerin(&c->vi, &c->vc, op) != 0)
			return fail(st, SDK_STATUS_IO_ERROR);
		if (!books_within_bounds(&c->vi))
			return fail(st, SDK_STATUS_UNSUPPORTED);
		if (vorbis_synthesis_init(&c->vd, &c->vi) != 0)
			return fail(st, SDK_STATUS_IO_ERROR);
		(void)vorbis_block_init(&c->vd, &c->vb);
		c->dsp_ready = 1U;
		c->phase = PH_AUDIO;
		return NP_PACKET;
	default:
		break;
	}
	/* Audio. A zero-length packet carries nothing and is skipped. */
	if (len == 0U) {
		c->packetno--;
		return NP_PACKET;
	}
	if (vorbis_synthesis(&c->vb, op) != 0 ||
	    vorbis_synthesis_blockin(&c->vd, &c->vb) != 0)
		return fail(st, SDK_STATUS_IO_ERROR);
	io->frames++;
	return NP_PACKET;
}

static int16_t clip16(ogg_int32_t v)
{
	v >>= 9;
	if (v > 32767)
		return 32767;
	if (v < -32768)
		return -32768;
	return (int16_t)v;
}

static void write_pcm(struct sdk_audio_codec_io *io, ogg_int32_t **pcm,
                      uint32_t channels, uint32_t frames)
{
	uint32_t pos = (io->pcm_write + io->produced) % io->pcm_capacity;
	uint32_t i, ch;

	for (i = 0U; i < frames; i++) {
		for (ch = 0U; ch < channels; ch++) {
			uint16_t v = (uint16_t)clip16(pcm[ch][i]);

			io->pcm[pos] = (uint8_t)(v >> 8);
			if (++pos == io->pcm_capacity)
				pos = 0U;
			io->pcm[pos] = (uint8_t)v;
			if (++pos == io->pcm_capacity)
				pos = 0U;
		}
	}
	io->produced += frames * channels * 2U;
}

static void decode_loop(struct sdk_vorbis_state *st,
                        struct sdk_audio_codec_io *io)
{
	struct vorbis_codec *c = (struct vorbis_codec *)st->codec;

	for (;;) {
		ogg_packet op;
		uint32_t room = io->pcm_free - io->produced;
		int r;

		if (io->produced != 0U) {
			uint32_t budget_left = io->produced < io->pcm_budget ?
			                       io->pcm_budget - io->produced : 0U;

			if (budget_left < room)
				room = budget_left;
		}
		if (c->dsp_ready) {
			ogg_int32_t **pcm;
			int n = vorbis_synthesis_pcmout(&c->vd, &pcm);

			if (n > 0) {
				uint32_t frames = room / (st->channels * 2U);

				if (frames == 0U)
					return;             /* PCM ring or budget full */
				if (frames > (uint32_t)n)
					frames = (uint32_t)n;
				write_pcm(io, pcm, st->channels, frames);
				(void)vorbis_synthesis_read(&c->vd, (int)frames);
				continue;
			}
		}
		memset(&op, 0, sizeof(op));
		r = next_packet(st, c, io, &op);
		if (r == NP_PACKET)
			r = handle_packet(st, c, &op, io);
		switch (r) {
		case NP_PACKET:
			continue;
		case NP_NEED_INPUT:
			io->starved = 1;
			return;
		case NP_END:
			if (c->phase == PH_IGNORE && !io->eof)
				continue;   /* discard trailing data until EOF */
			st->done = 1U;
			io->complete = 1;
			return;
		case NP_SECOND_STREAM:
			st->pending_status = SDK_STATUS_UNSUPPORTED;
			io->consumed = io->input_length;
			return;
		default:
			return;
		}
	}
}

static void decode_run(struct sdk_vorbis_state *st,
                       struct sdk_audio_codec_io *io)
{
	if (!st->codec) {
		struct vorbis_codec *c =
			(struct vorbis_codec *)sdk_vorbis_calloc(1U, sizeof(*c));

		ogg_sync_init(&c->oy);
		vorbis_info_init(&c->vi);
		vorbis_comment_init(&c->vc);
		st->codec = c;
	}
	decode_loop(st, io);
}

void sdk_vorbis_init(struct sdk_vorbis_state *st)
{
	memset(st, 0, sizeof(*st));
	sdk_vorbis_heap_init(&st->heap, SDK_VORBIS_REGION_BYTES,
	                     SDK_VORBIS_ALLOC_LIMIT);
}

uint16_t sdk_vorbis_decode(struct sdk_vorbis_state *st,
                           struct sdk_audio_codec_io *io)
{
	jmp_buf alloc_failed;

	io->consumed = 0U;
	io->produced = 0U;
	io->frames = 0U;
	io->starved = 0;
	io->complete = 0;
	if (st->status != SDK_STATUS_OK)
		return st->status;
	if (st->pending_status != SDK_STATUS_OK || st->done) {
		/* Nothing more is decoded; input is discarded. */
		io->consumed = io->input_length;
		io->complete = st->done;
		if (st->pending_status != SDK_STATUS_OK && io->pcm_unread == 0U)
			st->status = st->pending_status;
		return st->status;
	}
	if (!io->pcm || io->pcm_capacity == 0U)
		return SDK_STATUS_BAD_REQUEST;

	/* libogg and Tremor do not check most allocations: an arena failure
	 * longjmp()s back here and the whole arena is dropped below. */
	sdk_vorbis_heap_select(&st->heap, &alloc_failed);
	if (setjmp(alloc_failed) == 0)
		decode_run(st, io);
	else
		st->status = SDK_STATUS_NO_MEMORY;
	sdk_vorbis_heap_select(0, 0);

	if (st->status != SDK_STATUS_OK || st->pending_status != SDK_STATUS_OK ||
	    st->done)
		sdk_vorbis_release(st);
	/* A second logical stream faults only once every PCM byte of the
	 * first has been published (earlier calls) and read by the client. */
	if (st->status == SDK_STATUS_OK &&
	    st->pending_status != SDK_STATUS_OK &&
	    io->produced == 0U && io->pcm_unread == 0U)
		st->status = st->pending_status;
	return st->status;
}

int sdk_vorbis_holds_memory(const struct sdk_vorbis_state *st)
{
	return st && sdk_vorbis_heap_regions(&st->heap) != 0U;
}

void sdk_vorbis_release(struct sdk_vorbis_state *st)
{
	if (!st)
		return;
	sdk_vorbis_heap_release(&st->heap);
	st->codec = 0;
}

void sdk_vorbis_forget(struct sdk_vorbis_state *st)
{
	if (!st)
		return;
	sdk_vorbis_heap_forget(&st->heap);
	st->codec = 0;
}
