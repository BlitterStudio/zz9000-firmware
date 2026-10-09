/*
 * Native FLAC backend for the codec-aware audio stream (U7). See
 * sdk_audio_flac.h for the unit-at-a-time contract with libFLAC.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_audio_flac.h"

#include "sdk_flac_alloc.h"
#include "sdk_mailbox.h"

#include "FLAC/stream_decoder.h"

#include <string.h>

enum {
	FLAC_PHASE_MAGIC = 0,   /* expecting "fLaC" + STREAMINFO */
	FLAC_PHASE_META,        /* skipping further metadata blocks */
	FLAC_PHASE_FRAMES,
	FLAC_PHASE_TRAILER,     /* all STREAMINFO samples decoded; ignore tail */
	FLAC_PHASE_END
};

enum {
	STEP_UNIT = 0,
	STEP_NEED_INPUT,
	STEP_NEED_PCM,
	STEP_END,
	STEP_FAIL
};

#define FLAC_STREAMINFO_UNIT  42U   /* "fLaC" + block header + 34 */
#define FLAC_META_TYPE_INVALID 127U

struct flac_frame_header {
	uint64_t number;
	uint32_t blocksize;
	uint32_t sample_rate;   /* 0: from STREAMINFO */
	uint32_t channels;
	uint32_t bits;          /* 0: from STREAMINFO */
	uint32_t variable;
	uint32_t length;
};

static const uint32_t flac_rate_table[12] = {
	0U, 88200U, 176400U, 192000U, 8000U, 16000U, 22050U, 24000U,
	32000U, 44100U, 48000U, 96000U
};

static const uint32_t flac_bits_table[8] = {
	0U, 8U, 12U, 0U, 16U, 20U, 24U, 32U
};

static uint8_t flac_crc8(const uint8_t *p, uint32_t len)
{
	uint32_t crc = 0U;
	uint32_t i, b;

	for (i = 0U; i < len; i++) {
		crc ^= p[i];
		for (b = 0U; b < 8U; b++)
			crc = (crc & 0x80U) ? ((crc << 1) ^ 0x07U) : (crc << 1);
		crc &= 0xFFU;
	}
	return (uint8_t)crc;
}

/* CRC-16 (poly 0x8005, MSB first) over a frame including its big-endian
 * footer is zero when the footer matches. */
static uint32_t flac_crc16(const uint8_t *p, uint32_t len)
{
	uint32_t crc = 0U;
	uint32_t i, b;

	for (i = 0U; i < len; i++) {
		crc ^= (uint32_t)p[i] << 8;
		for (b = 0U; b < 8U; b++)
			crc = (crc & 0x8000U) ? ((crc << 1) ^ 0x8005U) : (crc << 1);
		crc &= 0xFFFFU;
	}
	return crc;
}

static uint32_t be16(const uint8_t *p)
{
	return ((uint32_t)p[0] << 8) | p[1];
}

static uint32_t be24(const uint8_t *p)
{
	return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

/* RFC 9639 frame header. >0: header length, 0: incomplete, -1: invalid. */
static int parse_frame_header(const uint8_t *p, uint32_t len,
                              struct flac_frame_header *h)
{
	uint32_t bs_code, rate_code, ch_code, bits_code, extra, n, i;
	uint64_t num;
	uint8_t b;

	if (len < 2U)
		return 0;
	if (p[0] != 0xFFU || (p[1] & 0xFEU) != 0xF8U)
		return -1;
	if (len < 5U)
		return 0;
	bs_code = (uint32_t)p[2] >> 4;
	rate_code = p[2] & 0x0FU;
	ch_code = (uint32_t)p[3] >> 4;
	bits_code = ((uint32_t)p[3] >> 1) & 7U;
	if (bs_code == 0U || rate_code == 15U || ch_code > 10U ||
	    bits_code == 3U || (p[3] & 1U) != 0U)
		return -1;
	h->variable = p[1] & 1U;
	b = p[4];
	if (b < 0x80U) {
		num = b;
		extra = 0U;
	} else if ((b & 0xE0U) == 0xC0U) {
		num = b & 0x1FU;
		extra = 1U;
	} else if ((b & 0xF0U) == 0xE0U) {
		num = b & 0x0FU;
		extra = 2U;
	} else if ((b & 0xF8U) == 0xF0U) {
		num = b & 0x07U;
		extra = 3U;
	} else if ((b & 0xFCU) == 0xF8U) {
		num = b & 0x03U;
		extra = 4U;
	} else if ((b & 0xFEU) == 0xFCU) {
		num = b & 0x01U;
		extra = 5U;
	} else if (b == 0xFEU && h->variable) {
		num = 0U;
		extra = 6U;
	} else {
		return -1;
	}
	n = 5U;
	if (len < n + extra)
		return 0;
	for (i = 0U; i < extra; i++) {
		if ((p[n + i] & 0xC0U) != 0x80U)
			return -1;
		num = (num << 6) | (p[n + i] & 0x3FU);
	}
	n += extra;
	h->number = num;

	if (bs_code == 1U) {
		h->blocksize = 192U;
	} else if (bs_code <= 5U) {
		h->blocksize = 576U << (bs_code - 2U);
	} else if (bs_code == 6U) {
		if (len < n + 1U)
			return 0;
		h->blocksize = (uint32_t)p[n] + 1U;
		n += 1U;
	} else if (bs_code == 7U) {
		if (len < n + 2U)
			return 0;
		h->blocksize = be16(p + n) + 1U;
		n += 2U;
	} else {
		h->blocksize = 256U << (bs_code - 8U);
	}
	if (h->blocksize > 65535U)
		return -1;

	if (rate_code < 12U) {
		h->sample_rate = flac_rate_table[rate_code];
	} else if (rate_code == 12U) {
		if (len < n + 1U)
			return 0;
		h->sample_rate = (uint32_t)p[n] * 1000U;
		n += 1U;
	} else {
		if (len < n + 2U)
			return 0;
		h->sample_rate = be16(p + n) * (rate_code == 14U ? 10U : 1U);
		n += 2U;
	}
	if (rate_code >= 12U && h->sample_rate == 0U)
		return -1;
	h->channels = (ch_code < 8U) ? ch_code + 1U : 2U;
	h->bits = flac_bits_table[bits_code];
	if (len < n + 1U)
		return 0;
	if (flac_crc8(p, n) != p[n])
		return -1;
	h->length = n + 1U;
	return (int)h->length;
}

static int frame_matches_stream(const struct sdk_flac_state *st,
                                const struct flac_frame_header *h)
{
	return h->channels == st->channels &&
	       (h->bits == 0U || h->bits == st->bits_per_sample) &&
	       (h->sample_rate == 0U || h->sample_rate == st->sample_rate) &&
	       h->blocksize <= st->max_blocksize;
}

static int step_fail(struct sdk_flac_state *st, uint16_t status)
{
	st->status = status;
	return STEP_FAIL;
}

/* ---- libFLAC callbacks: serve exactly the granted unit ---- */

static FLAC__StreamDecoderReadStatus flac_read_cb(
	const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes,
	void *client_data)
{
	struct sdk_flac_state *st = (struct sdk_flac_state *)client_data;
	size_t take;

	(void)decoder;
	if (st->grant_len == 0U) {
		/* libFLAC wants bytes beyond the unit: the boundary was wrong
		 * or the stream is truncated. Stop the decoder; the unit fails. */
		st->overrun = 1U;
		*bytes = 0U;
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
	}
	take = *bytes < st->grant_len ? *bytes : st->grant_len;
	memcpy(buffer, st->grant, take);
	st->grant += take;
	st->grant_len -= (uint32_t)take;
	*bytes = take;
	return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderWriteStatus flac_write_cb(
	const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
	const FLAC__int32 *const buffer[], void *client_data)
{
	struct sdk_flac_state *st = (struct sdk_flac_state *)client_data;
	const uint32_t blocksize = frame->header.blocksize;
	const uint32_t channels = st->channels;
	const uint32_t cb = st->container_bytes;
	const uint32_t shift = cb * 8U - st->bits_per_sample;
	uint32_t pos = st->pcm_pos;
	uint32_t i, c, k;

	(void)decoder;
	if (st->cb_error != 0U)
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	if (frame->header.channels != channels ||
	    frame->header.bits_per_sample != st->bits_per_sample ||
	    frame->header.sample_rate != st->sample_rate ||
	    (uint64_t)blocksize * channels * cb > st->pcm_limit ||
	    st->frames_written != 0U) {
		st->cb_error = SDK_STATUS_IO_ERROR;
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	}
	for (i = 0U; i < blocksize; i++) {
		for (c = 0U; c < channels; c++) {
			uint32_t v = (uint32_t)buffer[c][i] << shift;

			if (pos + cb <= st->pcm_capacity) {
				for (k = 0U; k < cb; k++)
					st->pcm[pos + k] =
						(uint8_t)(v >> (8U * (cb - 1U - k)));
				pos += cb;
				if (pos == st->pcm_capacity)
					pos = 0U;
			} else {
				for (k = 0U; k < cb; k++) {
					st->pcm[pos] =
						(uint8_t)(v >> (8U * (cb - 1U - k)));
					if (++pos == st->pcm_capacity)
						pos = 0U;
				}
			}
		}
	}
	st->pcm_pos = pos;
	st->frames_written++;
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void flac_error_cb(const FLAC__StreamDecoder *decoder,
                          FLAC__StreamDecoderErrorStatus status,
                          void *client_data)
{
	struct sdk_flac_state *st = (struct sdk_flac_state *)client_data;

	(void)decoder;
	(void)status;
	/* CRC mismatch, lost sync, bad header, unparseable or out-of-bounds
	 * frames all fault the stream; nothing is concealed. */
	if (st->cb_error == 0U)
		st->cb_error = SDK_STATUS_IO_ERROR;
}

static uint16_t run_unit(struct sdk_flac_state *st, const uint8_t *unit,
                         uint32_t len)
{
	FLAC__StreamDecoder *dec = (FLAC__StreamDecoder *)st->decoder;
	FLAC__StreamDecoderState state;
	FLAC__bool ok;

	st->grant = unit;
	st->grant_len = len;
	st->overrun = 0U;
	st->cb_error = 0U;
	st->frames_written = 0U;
	sdk_flac_alloc_set_quota(&st->alloc_used, &st->alloc_peak,
	                         SDK_FLAC_ALLOC_LIMIT);
	ok = FLAC__stream_decoder_process_single(dec);
	state = FLAC__stream_decoder_get_state(dec);
	st->grant = 0;
	if (sdk_flac_alloc_failed() ||
	    state == FLAC__STREAM_DECODER_MEMORY_ALLOCATION_ERROR)
		return SDK_STATUS_NO_MEMORY;
	if (st->cb_error != 0U)
		return (uint16_t)st->cb_error;
	if (!ok || st->overrun != 0U ||
	    (state != FLAC__STREAM_DECODER_SEARCH_FOR_FRAME_SYNC &&
	     state != FLAC__STREAM_DECODER_READ_FRAME))
		return SDK_STATUS_IO_ERROR;
	return SDK_STATUS_OK;
}

static uint16_t create_decoder(struct sdk_flac_state *st)
{
	FLAC__StreamDecoder *dec;
	FLAC__StreamDecoderInitStatus init;

	sdk_flac_alloc_set_quota(&st->alloc_used, &st->alloc_peak,
	                         SDK_FLAC_ALLOC_LIMIT);
	dec = FLAC__stream_decoder_new();
	if (!dec)
		return SDK_STATUS_NO_MEMORY;
	(void)FLAC__stream_decoder_set_md5_checking(dec, false);
	(void)FLAC__stream_decoder_set_metadata_ignore_all(dec);
	init = FLAC__stream_decoder_init_stream(dec, flac_read_cb, 0, 0, 0, 0,
	                                        flac_write_cb, 0, flac_error_cb,
	                                        st);
	if (init != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
		int failed = sdk_flac_alloc_failed();

		FLAC__stream_decoder_delete(dec);
		return failed ? SDK_STATUS_NO_MEMORY : SDK_STATUS_INTERNAL_ERROR;
	}
	st->decoder = dec;
	return SDK_STATUS_OK;
}

/* "fLaC" + STREAMINFO: validate, apply the envelope, create the decoder. */
static int step_magic(struct sdk_flac_state *st, const uint8_t *in,
                      uint32_t len, int eof, uint32_t pcm_capacity,
                      uint32_t *consumed)
{
	const uint8_t *si;
	uint64_t packed, frame_bytes;
	uint32_t min_bs, max_bs, min_fs, max_fs, rate, channels, bits, cb;
	uint16_t status;

	if (len < 4U)
		return eof ? step_fail(st, SDK_STATUS_IO_ERROR) : STEP_NEED_INPUT;
	if (memcmp(in, "OggS", 4U) == 0 || memcmp(in, "ID3", 3U) == 0)
		return step_fail(st, SDK_STATUS_UNSUPPORTED);
	if (memcmp(in, "fLaC", 4U) != 0)
		return step_fail(st, SDK_STATUS_IO_ERROR);
	if (len < FLAC_STREAMINFO_UNIT)
		return eof ? step_fail(st, SDK_STATUS_IO_ERROR) : STEP_NEED_INPUT;
	if ((in[4] & 0x7FU) != 0U || be24(in + 5) != 34U)
		return step_fail(st, SDK_STATUS_IO_ERROR);
	si = in + 8;
	min_bs = be16(si);
	max_bs = be16(si + 2);
	min_fs = be24(si + 4);
	max_fs = be24(si + 7);
	packed = 0U;
	for (cb = 0U; cb < 8U; cb++)
		packed = (packed << 8) | si[10U + cb];
	rate = (uint32_t)(packed >> 44);
	channels = (uint32_t)((packed >> 41) & 7U) + 1U;
	bits = (uint32_t)((packed >> 36) & 31U) + 1U;
	if (max_bs == 0U || min_bs > max_bs || rate == 0U || bits < 4U ||
	    (min_fs != 0U && max_fs != 0U && min_fs > max_fs))
		return step_fail(st, SDK_STATUS_IO_ERROR);
	if (channels > SDK_FLAC_MAX_CHANNELS || bits > SDK_FLAC_MAX_BITS ||
	    rate < SDK_FLAC_MIN_RATE || rate > SDK_FLAC_MAX_RATE)
		return step_fail(st, SDK_STATUS_UNSUPPORTED);
	if (st->output_format == SDK_AUDIO_SAMPLE_FORMAT_S16BE) {
		if (bits > 16U)
			return step_fail(st, SDK_STATUS_UNSUPPORTED);
		cb = 2U;
	} else {
		cb = 4U;
	}
	frame_bytes = (uint64_t)max_bs * channels * cb;
	if (frame_bytes > pcm_capacity)
		return step_fail(st, SDK_STATUS_UNSUPPORTED);

	st->sample_rate = rate;
	st->channels = channels;
	st->bits_per_sample = bits;
	st->container_bytes = cb;
	st->max_blocksize = max_bs;
	st->total_samples = packed & 0xFFFFFFFFFULL;

	if (!st->decoder) {
		status = create_decoder(st);
		if (status != SDK_STATUS_OK)
			return step_fail(st, status);
	}
	/* libFLAC sees STREAMINFO as the only metadata block. */
	memcpy(st->header, in, FLAC_STREAMINFO_UNIT);
	st->header[4] |= 0x80U;
	st->pcm_limit = 0U;
	status = run_unit(st, st->header, FLAC_STREAMINFO_UNIT);
	if (status != SDK_STATUS_OK)
		return step_fail(st, status);
	st->phase = (in[4] & 0x80U) ? FLAC_PHASE_FRAMES : FLAC_PHASE_META;
	*consumed = FLAC_STREAMINFO_UNIT;
	return STEP_UNIT;
}

static int step_meta(struct sdk_flac_state *st, const uint8_t *in,
                     uint32_t len, int eof, uint32_t *consumed)
{
	uint32_t type;

	if (st->meta_remaining != 0U) {
		uint32_t take = len < st->meta_remaining ? len : st->meta_remaining;

		if (take == 0U)
			return eof ? step_fail(st, SDK_STATUS_IO_ERROR) :
			             STEP_NEED_INPUT;
		st->meta_remaining -= take;
		if (st->meta_remaining == 0U && st->meta_last)
			st->phase = FLAC_PHASE_FRAMES;
		*consumed = take;
		return STEP_UNIT;
	}
	if (len < 4U)
		return eof ? step_fail(st, SDK_STATUS_IO_ERROR) : STEP_NEED_INPUT;
	type = in[0] & 0x7FU;
	if (type == 0U || type == FLAC_META_TYPE_INVALID)
		return step_fail(st, SDK_STATUS_IO_ERROR);
	st->meta_last = (in[0] & 0x80U) ? 1U : 0U;
	st->meta_remaining = be24(in + 1);
	if (st->meta_remaining == 0U && st->meta_last)
		st->phase = FLAC_PHASE_FRAMES;
	*consumed = 4U;
	return STEP_UNIT;
}

static int step_finish(struct sdk_flac_state *st)
{
	if (st->total_samples != 0U &&
	    st->samples_decoded != st->total_samples)
		return step_fail(st, SDK_STATUS_IO_ERROR);   /* truncated */
	st->phase = FLAC_PHASE_END;
	return STEP_END;
}

/* Find the end of the frame at in[0]. >0: unit length, 0: undecided. */
static uint32_t frame_boundary(struct sdk_flac_state *st, const uint8_t *in,
                               uint32_t len, int eof, int drain,
                               const struct flac_frame_header *h)
{
	uint32_t i = st->scan_offset > h->length ? st->scan_offset : h->length;

	for (; i < len; i++) {
		struct flac_frame_header next;
		int r;

		if (in[i] != 0xFFU)
			continue;
		if (i + 1U >= len) {
			if (eof)
				break;
			st->scan_offset = i;
			return 0U;
		}
		if ((in[i + 1U] & 0xFEU) != 0xF8U)
			continue;
		r = parse_frame_header(in + i, len - i, &next);
		if (r == 0) {
			if (eof)
				continue;
			st->scan_offset = i;
			return 0U;
		}
		if (r < 0 || next.variable != h->variable ||
		    !frame_matches_stream(st, &next))
			continue;
		if (next.number != h->number +
		    (h->variable ? (uint64_t)h->blocksize : 1U))
			continue;
		st->scan_offset = i;
		return i;
	}
	if (eof)
		return len;
	if (drain && len > h->length + 2U && flac_crc16(in, len) == 0U) {
		st->scan_offset = len;
		return len;
	}
	st->scan_offset = len > h->length ? len : h->length;
	return 0U;
}

static int step_frame(struct sdk_flac_state *st, const uint8_t *in,
                      uint32_t len, int eof, int drain,
                      struct sdk_audio_codec_io *io, uint32_t room,
                      uint32_t *consumed, uint32_t *produced)
{
	struct flac_frame_header h;
	uint32_t unit, need;
	uint16_t status;
	int r;

	if (len == 0U)
		return eof ? step_finish(st) : STEP_NEED_INPUT;
	r = parse_frame_header(in, len, &h);
	if (r <= 0) {
		/* A tag or padding after the last STREAMINFO sample is
		 * ignored; anything else where a frame must start is
		 * corrupt (or, at EOF, truncated). */
		if (st->total_samples != 0U &&
		    st->samples_decoded == st->total_samples) {
			st->phase = FLAC_PHASE_TRAILER;
			*consumed = len;
			return STEP_UNIT;
		}
		if (r == 0 && !eof)
			return STEP_NEED_INPUT;
		return step_fail(st, SDK_STATUS_IO_ERROR);
	}
	if (!frame_matches_stream(st, &h) ||
	    (st->total_samples != 0U &&
	     st->samples_decoded + h.blocksize > st->total_samples))
		return step_fail(st, SDK_STATUS_IO_ERROR);
	unit = frame_boundary(st, in, len, eof, drain, &h);
	if (unit == 0U)
		return STEP_NEED_INPUT;
	need = h.blocksize * st->channels * st->container_bytes;
	if (need > io->pcm_capacity)
		return step_fail(st, SDK_STATUS_IO_ERROR);
	if (need > room)
		return STEP_NEED_PCM;

	st->pcm = io->pcm;
	st->pcm_capacity = io->pcm_capacity;
	st->pcm_pos = (io->pcm_write + io->produced) % io->pcm_capacity;
	st->pcm_limit = need;
	status = run_unit(st, in, unit);
	if (status != SDK_STATUS_OK)
		return step_fail(st, status);
	if (st->frames_written != 1U)
		return step_fail(st, SDK_STATUS_IO_ERROR);
	st->samples_decoded += h.blocksize;
	st->scan_offset = 0U;
	*consumed = unit - st->grant_len;
	*produced = need;
	return STEP_UNIT;
}

void sdk_flac_init(struct sdk_flac_state *st, uint32_t output_format)
{
	memset(st, 0, sizeof(*st));
	st->output_format = output_format;
	st->phase = FLAC_PHASE_MAGIC;
}

uint16_t sdk_flac_decode(struct sdk_flac_state *st,
                         struct sdk_audio_codec_io *io)
{
	io->consumed = 0U;
	io->produced = 0U;
	io->frames = 0U;
	io->starved = 0;
	io->complete = 0;
	if (st->status != SDK_STATUS_OK)
		return st->status;
	if (!io->pcm || io->pcm_capacity == 0U)
		return SDK_STATUS_BAD_REQUEST;

	for (;;) {
		const uint8_t *in = io->input + io->consumed;
		uint32_t len = io->input_length - io->consumed;
		uint32_t consumed = 0U;
		uint32_t produced = 0U;
		uint32_t room;
		int r;

		room = io->pcm_free - io->produced;
		if (io->produced != 0U) {
			uint32_t budget_left = io->produced < io->pcm_budget ?
			                       io->pcm_budget - io->produced : 0U;

			if (budget_left < room)
				room = budget_left;
		}
		switch (st->phase) {
		case FLAC_PHASE_MAGIC:
			r = step_magic(st, in, len, io->eof, io->pcm_capacity,
			               &consumed);
			break;
		case FLAC_PHASE_META:
			r = step_meta(st, in, len, io->eof, &consumed);
			break;
		case FLAC_PHASE_FRAMES:
			r = step_frame(st, in, len, io->eof, io->drain, io, room,
			               &consumed, &produced);
			break;
		case FLAC_PHASE_TRAILER:
			if (len != 0U) {
				consumed = len;
				r = STEP_UNIT;
			} else {
				r = io->eof ? step_finish(st) : STEP_NEED_INPUT;
			}
			break;
		default:
			r = STEP_END;
			break;
		}
		switch (r) {
		case STEP_UNIT:
			io->consumed += consumed;
			io->produced += produced;
			if (produced != 0U)
				io->frames++;
			continue;
		case STEP_NEED_INPUT:
			io->starved = 1;
			return SDK_STATUS_OK;
		case STEP_NEED_PCM:
			return SDK_STATUS_OK;
		case STEP_END:
			io->complete = 1;
			return SDK_STATUS_OK;
		default:
			return st->status;
		}
	}
}

void sdk_flac_release(struct sdk_flac_state *st)
{
	if (!st || !st->decoder)
		return;
	sdk_flac_alloc_set_quota(&st->alloc_used, &st->alloc_peak,
	                         SDK_FLAC_ALLOC_LIMIT);
	FLAC__stream_decoder_delete((FLAC__StreamDecoder *)st->decoder);
	st->decoder = 0;
}

void sdk_flac_forget(struct sdk_flac_state *st)
{
	if (!st)
		return;
	st->decoder = 0;
	st->alloc_used = 0U;
}
