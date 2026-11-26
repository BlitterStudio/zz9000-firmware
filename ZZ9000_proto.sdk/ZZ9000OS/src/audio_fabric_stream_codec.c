/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "audio_fabric.h"
#include "audio_fabric_codec.h"
#include "sdk_audio_stream.h"
#include "sdk_compression.h"
#include "xil_cache.h"

/* A FLAC frame can hold 65535 stereo samples at up to 24 bits. Leave
 * enough input room for that frame and the next sync header; PCM is
 * always S16. Power-of-two PCM geometry tolerates cursor wrap. */
#define FLAC_INPUT_BYTES (512U * 1024U)
#define FLAC_PCM_BYTES   (256U * 1024U)
#define CODEC_PCM_BYTES  (32U * 1024U)
#define CODEC_COPY_BYTES  (16U * 1024U)
#define CODEC_PASS_BYTES  (MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t))

struct lease_decoder {
	struct SDKAudioStream stream;
	uint8_t *storage;
	uint32_t capacity;
};

static struct lease_decoder decoders[AUDIO_FABRIC_LEASE_SLOTS];

static struct lease_decoder *decoder(uint32_t slot)
{
	return &decoders[slot - AUDIO_FABRIC_SLOT_MAILBOX];
}

static void release_decoder(struct SDKAudioStream *s)
{
	if (s->codec == SDK_AUDIO_CODEC_FLAC)
		sdk_flac_release(&s->flac);
	else if (s->codec == SDK_AUDIO_CODEC_VORBIS)
		sdk_vorbis_release(&s->vorbis);
	s->codec = SDK_AUDIO_RING_CODEC_NONE;
}

static void codec_end(uint32_t slot)
{
	struct lease_decoder *d = decoder(slot);

	release_decoder(&d->stream);
	if (d->storage != NULL)
		sdk_decode_heap_free(d->storage);
	d->storage = NULL;
	d->capacity = 0U;
}

static void codec_begin(uint32_t slot, uint32_t codec, uint32_t param)
{
	struct lease_decoder *d = decoder(slot);
	struct SDKAudioStream *s = &d->stream;

	uint32_t input_bytes = codec == SDK_AUDIO_CODEC_FLAC ? FLAC_INPUT_BYTES :
		(codec == SDK_AUDIO_RING_CODEC_PCM ? 0U : CODEC_COPY_BYTES);
	uint32_t pcm_bytes = codec == SDK_AUDIO_CODEC_FLAC ? FLAC_PCM_BYTES : CODEC_PCM_BYTES;
	uint32_t capacity = input_bytes + pcm_bytes;

	release_decoder(s);
	/* Keep the FIFO allocation across a restart of the same geometry. */
	if (d->capacity != capacity) {
		if (d->storage != NULL)
			sdk_decode_heap_free(d->storage);
		d->storage = sdk_decode_heap_alloc(capacity);
		d->capacity = d->storage != NULL ? capacity : 0U;
	}
	memset(s, 0, sizeof(*s));
	s->codec = codec;
	if (d->storage == NULL) {
		s->faulted = 1;
		s->fault_status = SDK_STATUS_NO_MEMORY;
		return;
	}
	s->mp3_ring_addr = (uintptr_t)d->storage;
	s->mp3_capacity = input_bytes;
	s->pcm_ring_addr = (uintptr_t)(d->storage + input_bytes);
	s->pcm_capacity = pcm_bytes;
	s->pcm_local = 1U;
	s->sample_format = SDK_AUDIO_SAMPLE_FORMAT_S16LE;
	s->high_water_bytes = CODEC_PASS_BYTES;
	if (codec == SDK_AUDIO_RING_CODEC_PCM) {
		audio_stream_set_format(s, param != 0U ? param : 44100U, 2U);
	} else {
		(void)audio_reduce_request(&s->reduce, 96000U, 0U);
		if (codec == SDK_AUDIO_CODEC_FLAC)
			sdk_flac_init(&s->flac, s->sample_format);
		else if (codec == SDK_AUDIO_CODEC_VORBIS)
			sdk_vorbis_init(&s->vorbis, s->sample_format);
		else
			mp3dec_init(&s->decoder);
		s->backend_starved = codec != SDK_AUDIO_CODEC_MP3;
	}
}

static uint32_t copy_input(struct SDKAudioStream *s,
	const struct fabric_codec_input *in)
{
	uint8_t *buffer = (uint8_t *)s->mp3_ring_addr;
	uint32_t n = s->mp3_capacity - s->input_length;
	uint32_t offset, first;
	uint8_t *dst;

	if (n > in->available)
		n = in->available;
	if (n > CODEC_COPY_BYTES)
		n = CODEC_COPY_BYTES;
	if (n == 0U)
		return 0U;
	if (s->input_offset > s->mp3_capacity - s->input_length - n) {
		memmove(buffer, buffer + s->input_offset, s->input_length);
		s->input_offset = 0U;
	}
	offset = (uint32_t)(in->cursor % in->capacity);
	first = in->capacity - offset;
	if (first > n)
		first = n;
	dst = buffer + s->input_offset + s->input_length;
	Xil_DCacheInvalidateRange((INTPTR)(in->ring + offset), first);
	memcpy(dst, in->ring + offset, first);
	if (first < n) {
		Xil_DCacheInvalidateRange((INTPTR)in->ring, n - first);
		memcpy(dst + first, in->ring, n - first);
	}
	s->input_length += n;
	return n;
}

static uint32_t copy_pcm(struct SDKAudioStream *s,
	const struct fabric_codec_input *in)
{
	uint32_t n = audio_stream_pcm_free(s);
	uint32_t offset, first;

	if (n > in->available)
		n = in->available;
	if (n > CODEC_COPY_BYTES)
		n = CODEC_COPY_BYTES;
	n &= ~3U;
	if (n != 0U) {
		offset = (uint32_t)(in->cursor % in->capacity);
		first = in->capacity - offset;
		if (first > n)
			first = n;
		Xil_DCacheInvalidateRange((INTPTR)(in->ring + offset), first);
		audio_stream_pcm_write(s, in->ring + offset, first);
		if (first < n) {
			Xil_DCacheInvalidateRange((INTPTR)in->ring, n - first);
			audio_stream_pcm_write(s, in->ring, n - first);
		}
		s->pcm_ready_total = s->pcm_written_total;
		s->frames_decoded = 1U;
	}
	if (in->end && in->available - n < 4U) {
		s->eof = 1;
		s->decode_complete = 1;
		s->faulted = in->available != n;
	}
	return n;
}

static uint32_t decode_error(uint16_t status)
{
	if (status == SDK_STATUS_UNSUPPORTED)
		return SDK_AUDIO_RING_DECODE_ERR_UNSUPPORTED;
	if (status == SDK_STATUS_NO_MEMORY)
		return SDK_AUDIO_RING_DECODE_ERR_NO_MEMORY;
	return SDK_AUDIO_RING_DECODE_ERR_CORRUPT;
}

static struct fabric_codec_view codec_step(uint32_t slot,
	const struct fabric_codec_input *in)
{
	struct lease_decoder *d = decoder(slot);
	struct SDKAudioStream *s = &d->stream;
	struct fabric_codec_view v = {0};

	if (!s->faulted && s->codec == SDK_AUDIO_RING_CODEC_PCM) {
		v.copied = copy_pcm(s, in);
	} else if (!s->faulted &&
	           (!s->decode_complete || s->codec == SDK_AUDIO_CODEC_MP3)) {
		v.copied = copy_input(s, in);
		s->eof = in->end && v.copied == in->available;
		if (v.copied != 0U || s->eof || s->output_terminal ||
		    !s->backend_starved) {
			uint32_t before_input = s->input_length;
			uint32_t before_pcm = s->pcm_written_total;

			audio_stream_decode(s);
			if (s->codec == SDK_AUDIO_CODEC_MP3 && s->eof &&
			    !s->decode_complete && audio_stream_pcm_free(s) >= CODEC_PASS_BYTES &&
			    before_input == s->input_length && before_pcm == s->pcm_written_total)
				s->faulted = 1;
			/* A complete FLAC frame and its lookahead fit the FIFO.
			 * No progress with a full input window is malformed data. */
			if (s->backend_starved && s->input_length == s->mp3_capacity)
				s->faulted = 1;
		}
	}
	v.rate = s->sample_rate;
	v.native_rate = s->reduce.in_hz;
	v.channels = s->channels;
	v.frames = v.channels != 0U ? audio_stream_pcm_used(s) / (2U * v.channels) : 0U;
	v.complete = s->faulted || (s->eof && s->decode_complete);
	v.tail = v.complete || s->output_terminal;
	if (s->faulted)
		v.error = decode_error(s->fault_status);
	else if (v.complete && s->frames_decoded == 0U)
		v.error = s->codec == SDK_AUDIO_CODEC_MP3 && s->bytes_consumed != 0U
			? SDK_AUDIO_RING_DECODE_ERR_CORRUPT : SDK_AUDIO_RING_DECODE_ERR_EMPTY;
	return v;
}

static void codec_read_stereo(uint32_t slot, int16_t *out, uint32_t frames)
{
	struct SDKAudioStream *s = &decoder(slot)->stream;
	const uint8_t *pcm = (const uint8_t *)s->pcm_ring_addr;
	uint32_t offset = s->pcm_consumed_total & (s->pcm_capacity - 1U);
	uint32_t n = frames * 2U * s->channels;
	uint32_t first = s->pcm_capacity - offset;

	if (first > n)
		first = n;
	memcpy(out, pcm + offset, first);
	if (first < n)
		memcpy((uint8_t *)out + first, pcm, n - first);
	if (s->channels == 1U) {
		for (uint32_t i = frames; i != 0U; i--) {
			int16_t mono = out[i - 1U];

			out[i * 2U - 2U] = mono;
			out[i * 2U - 1U] = mono;
		}
	}
	s->pcm_consumed_total += n;
}

const struct fabric_codec_ops audio_fabric_stream_codec_ops = {
	.begin = codec_begin,
	.end = codec_end,
	.step = codec_step,
	.read_stereo = codec_read_stereo,
};
