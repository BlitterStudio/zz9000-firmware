/*
 * pl_mpeg backend: MPEG-1 video and optional Layer-II audio carried in one
 * MPEG Program Stream demux.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_video_backend.h"
#include "sdk_media_timeline.h"

#include <stdlib.h>
#include <string.h>

#ifndef SDK_VIDEO_HOST_TEST
#include "sdk_compression.h"
#include "sdk_decode_reclaim.h"
#include "sdk_smp_lock.h"
#include "xil_cache.h"
#endif

#define SDK_VIDEO_CODEC_MPEG1 1U
#define SDK_VIDEO_CONTAINER_MPEG_PS 1U
#define PLM_INPUT_CAPACITY (256U * 1024U)
#define PLM_VIDEO_CAPACITY (256U * 1024U)
#define PLM_AUDIO_CAPACITY (64U * 1024U)
#define PLM_ES_ANCHORS 128U
#define PLM_PCM_ANCHORS 64U
#define PLM_NO_STDIO 1

#ifdef SDK_VIDEO_HOST_TEST
#define PLM_MALLOC(size) malloc(size)
#define PLM_REALLOC(ptr, size) realloc((ptr), (size))
#define PLM_FREE(ptr) free(ptr)
#else
static void *sdk_video_heap_realloc(void *ptr, size_t size)
{
	void *resized;

	if (!ptr)
		return sdk_decode_heap_alloc(size);
	if (smp_cpu_id() != 1)
		return realloc(ptr, size);

	sdk_decode_untrack(ptr);
	sdk_decode_flush_table();
	resized = realloc(ptr, size);
	if (!resized) {
		/* realloc failure preserves the old allocation. */
		sdk_decode_track(ptr);
		sdk_decode_flush_table();
		return 0;
	}
	sdk_decode_track(resized);
	sdk_decode_flush_table();
	return resized;
}

#define PLM_MALLOC(size) sdk_decode_heap_alloc(size)
#define PLM_REALLOC(ptr, size) sdk_video_heap_realloc((ptr), (size))
#define PLM_FREE(ptr) sdk_decode_heap_free(ptr)
#endif

#define PL_MPEG_IMPLEMENTATION
#include "third_party/pl_mpeg/pl_mpeg.h"

struct plmpeg_es_anchor {
	uint64_t start;
	uint64_t raw_pts;
};

struct plmpeg_pcm_anchor {
	uint64_t start;
	uint64_t end;
	uint64_t pts;
	uint32_t flags;
};

struct sdk_video_plmpeg {
	plm_buffer_t *input;
	plm_demux_t *demux;
	plm_buffer_t *video_input;
	plm_video_t *video;
	plm_buffer_t *audio_input;
	plm_audio_t *audio;
	plm_packet_t *pending_packet;
	struct SDKVideoMediaConfig media;
	struct SDKMediaTimeline timeline;
	struct plmpeg_es_anchor video_anchor[PLM_ES_ANCHORS];
	struct plmpeg_es_anchor audio_anchor[PLM_ES_ANCHORS];
	struct plmpeg_pcm_anchor pcm_anchor[PLM_PCM_ANCHORS];
	uint32_t video_anchor_count;
	uint32_t audio_anchor_count;
	uint32_t pcm_anchor_count;
	uint64_t video_written;
	uint64_t audio_written;
	uint64_t pcm_produced;
	uint64_t pcm_acknowledged;
	uint64_t current_audio_pts;
	uint64_t first_audio_pts;
	uint64_t audio_tail_pts;
	uint64_t last_raw_pts;
	uint32_t video_frames;
	uint32_t audio_frames;
	uint32_t backpressure_events;
	uint32_t media_flags;
	uint32_t media_event_flags;
	uint64_t audio_probe_pts[4];
	uint8_t audio_probe_last[4];
	uint8_t audio_probe_have_last[4];
	int selected_audio_type;
	int selected_audio_prefix;
	uint64_t selected_audio_prefix_pts;
	int routing_started;
	int media_configured;
	int input_eof;
	int video_eof;
	int audio_eof;
	int video_done;
	int audio_done;
	int route_blocked;
	int pcm_backpressure;
	int failed;
};

static uint64_t packet_raw_pts(const plm_packet_t *packet)
{
	double scaled;

	if (!packet || packet->pts == PLM_PACKET_INVALID_TS ||
	    packet->pts < 0.0)
		return SDK_MEDIA_TIMELINE_NO_PTS;
	scaled = packet->pts * 90000.0;
	if (scaled >= (double)SDK_MEDIA_TIMELINE_PTS_MODULUS)
		return (uint64_t)(scaled + 0.5) &
			(SDK_MEDIA_TIMELINE_PTS_MODULUS - 1U);
	return (uint64_t)(scaled + 0.5);
}

static int packet_contains_mp2(
	struct sdk_video_plmpeg *decoder, const plm_packet_t *packet,
	int *prefix_sync, uint64_t *prefix_pts)
{
	uint32_t index;
	size_t i;

	if (!decoder || !packet || !prefix_sync || !prefix_pts ||
	    packet->type < PLM_DEMUX_PACKET_AUDIO_1 ||
	    packet->type > PLM_DEMUX_PACKET_AUDIO_4)
		return 0;
	index = (uint32_t)(packet->type - PLM_DEMUX_PACKET_AUDIO_1);
	*prefix_sync = 0;
	*prefix_pts = SDK_MEDIA_TIMELINE_NO_PTS;
	if (packet->length != 0U &&
	    decoder->audio_probe_have_last[index] &&
	    decoder->audio_probe_last[index] == 0xffU &&
	    (packet->data[0] & 0xfeU) == 0xfcU) {
		*prefix_sync = 1;
		*prefix_pts = decoder->audio_probe_pts[index];
		return 1;
	}
	for (i = 0U; i + 1U < packet->length; i++) {
		if (packet->data[i] == 0xffU &&
		    (packet->data[i + 1U] & 0xfeU) == 0xfcU)
			return 1;
	}
	if (packet->length != 0U) {
		decoder->audio_probe_last[index] =
			packet->data[packet->length - 1U];
		decoder->audio_probe_have_last[index] = 1U;
		decoder->audio_probe_pts[index] = packet_raw_pts(packet);
	}
	return 0;
}

#ifdef SDK_VIDEO_HOST_TEST
int sdk_video_plmpeg_test_boundary_mp2_probe(void)
{
	struct sdk_video_plmpeg decoder;
	plm_packet_t first;
	plm_packet_t second;
	uint8_t first_bytes[] = {0x12U, 0xffU};
	uint8_t second_bytes[] = {0xfcU, 0x00U};
	uint64_t prefix_pts;
	int prefix_sync;

	memset(&decoder, 0, sizeof(decoder));
	memset(&first, 0, sizeof(first));
	first.type = PLM_DEMUX_PACKET_AUDIO_1;
	first.pts = 1.0;
	first.length = sizeof(first_bytes);
	first.data = first_bytes;
	if (packet_contains_mp2(
	        &decoder, &first, &prefix_sync, &prefix_pts))
		return 0;
	memset(&second, 0, sizeof(second));
	second.type = PLM_DEMUX_PACKET_AUDIO_1;
	second.pts = 2.0;
	second.length = sizeof(second_bytes);
	second.data = second_bytes;
	if (!packet_contains_mp2(
	        &decoder, &second, &prefix_sync, &prefix_pts) ||
	    !prefix_sync || prefix_pts != 90000U)
		return 0;
	/* Probe tails are kept independently per candidate stream. */
	second.type = PLM_DEMUX_PACKET_AUDIO_2;
	if (packet_contains_mp2(
	        &decoder, &second, &prefix_sync, &prefix_pts))
		return 0;
	return 1;
}
#endif

static uint64_t buffer_consumed(uint64_t written, plm_buffer_t *buffer)
{
	size_t remaining = plm_buffer_get_remaining(buffer);

	return remaining <= written ? written - remaining : 0U;
}

static void prune_es_anchors(struct plmpeg_es_anchor *anchor,
	                         uint32_t *count, uint64_t consumed)
{
	uint32_t remove = 0U;

	/* Keep the newest anchor at/before the cursor for output attribution. */
	while (remove + 1U < *count &&
	       anchor[remove + 1U].start < consumed)
		remove++;
	if (remove != 0U) {
		memmove(anchor, anchor + remove,
		        (*count - remove) * sizeof(*anchor));
		*count -= remove;
	}
}

static int append_es_anchor(struct plmpeg_es_anchor *anchor,
	                        uint32_t *count, uint64_t start,
	                        uint64_t raw_pts)
{
	if (raw_pts == SDK_MEDIA_TIMELINE_NO_PTS)
		return 1;
	if (*count >= PLM_ES_ANCHORS)
		return 0;
	anchor[*count].start = start;
	anchor[*count].raw_pts = raw_pts;
	(*count)++;
	return 1;
}

static uint64_t take_es_pts(struct plmpeg_es_anchor *anchor,
	                        uint32_t *count, uint64_t consumed)
{
	uint64_t raw_pts = SDK_MEDIA_TIMELINE_NO_PTS;
	uint32_t used = 0U;

	while (used < *count && anchor[used].start < consumed) {
		raw_pts = anchor[used].raw_pts;
		used++;
	}
	if (used != 0U) {
		memmove(anchor, anchor + used,
		        (*count - used) * sizeof(*anchor));
		*count -= used;
	}
	return raw_pts;
}

static int route_packet(struct sdk_video_plmpeg *decoder,
	                    plm_packet_t *packet)
{
	plm_buffer_t *destination = 0;
	struct plmpeg_es_anchor *anchors = 0;
	uint32_t *anchor_count = 0;
	uint64_t *written = 0;
	uint32_t capacity = 0U;
	uint32_t routed_length;
	uint64_t consumed;
	uint64_t raw_pts;
	uint64_t prefix_pts = SDK_MEDIA_TIMELINE_NO_PTS;
	uint8_t prefix_byte = 0xffU;
	int prefix_sync = 0;

	if (packet->type == PLM_DEMUX_PACKET_VIDEO_1) {
		destination = decoder->video->buffer;
		anchors = decoder->video_anchor;
		anchor_count = &decoder->video_anchor_count;
		written = &decoder->video_written;
		capacity = PLM_VIDEO_CAPACITY;
	} else if (decoder->media_configured &&
	           decoder->media.audio_codec == SDK_VIDEO_MEDIA_AUDIO_MP2 &&
	           packet->type >= PLM_DEMUX_PACKET_AUDIO_1 &&
	           packet->type <= PLM_DEMUX_PACKET_AUDIO_4) {
		if (decoder->selected_audio_type == 0) {
			if (!packet_contains_mp2(
				    decoder, packet, &prefix_sync, &prefix_pts))
				return 1;
			decoder->selected_audio_type = packet->type;
			decoder->selected_audio_prefix = prefix_sync;
			decoder->selected_audio_prefix_pts = prefix_pts;
		}
		if (packet->type != decoder->selected_audio_type)
			return 1;
		prefix_sync = decoder->selected_audio_prefix;
		prefix_pts = decoder->selected_audio_prefix_pts;
		destination = decoder->audio->buffer;
		anchors = decoder->audio_anchor;
		anchor_count = &decoder->audio_anchor_count;
		written = &decoder->audio_written;
		capacity = PLM_AUDIO_CAPACITY;
	} else {
		return 1;
	}

	routed_length = (uint32_t)packet->length +
		(prefix_sync ? 1U : 0U);
	if (plm_buffer_get_remaining(destination) + routed_length > capacity)
		return 0;
	if (decoder->media_configured) {
		consumed = buffer_consumed(*written, destination);
		prune_es_anchors(anchors, anchor_count, consumed);
		raw_pts = packet_raw_pts(packet);
		if (prefix_sync && prefix_pts != SDK_MEDIA_TIMELINE_NO_PTS)
			raw_pts = prefix_pts;
		sdk_media_timeline_seed_origin(&decoder->timeline, raw_pts);
		if ((raw_pts != SDK_MEDIA_TIMELINE_NO_PTS &&
		     *anchor_count >= PLM_ES_ANCHORS) ||
		    !append_es_anchor(
			    anchors, anchor_count, *written, raw_pts))
			return 0;
	}
	if (prefix_sync &&
	    plm_buffer_write(destination, &prefix_byte, 1U) != 1U) {
		decoder->failed = 1;
		return 0;
	}
	if (plm_buffer_write(destination, packet->data, packet->length) !=
	    packet->length) {
		decoder->failed = 1;
		return 0;
	}
	*written += routed_length;
	if (decoder->audio && destination == decoder->audio->buffer) {
		decoder->selected_audio_prefix = 0;
		decoder->selected_audio_prefix_pts =
			SDK_MEDIA_TIMELINE_NO_PTS;
	}
	return 1;
}

static void signal_es_end(struct sdk_video_plmpeg *decoder)
{
	if (!decoder->input_eof || decoder->pending_packet ||
	    !plm_demux_has_ended(decoder->demux))
		return;
	if (!decoder->video_eof) {
		plm_buffer_signal_end(decoder->video->buffer);
		decoder->video->buffer->has_ended = TRUE;
		decoder->video_eof = 1;
	}
	if (decoder->media_configured &&
	    decoder->media.audio_codec == SDK_VIDEO_MEDIA_AUDIO_MP2 &&
	    !decoder->audio_eof) {
		plm_buffer_signal_end(decoder->audio->buffer);
		decoder->audio->buffer->has_ended = TRUE;
		decoder->audio_eof = 1;
	}
}

static void plmpeg_route(struct sdk_video_plmpeg *decoder)
{
	plm_packet_t *packet;

	if (!decoder || decoder->failed)
		return;
	decoder->route_blocked = 0;
	if (decoder->pending_packet) {
		if (!route_packet(decoder, decoder->pending_packet)) {
			decoder->route_blocked = !decoder->failed;
			return;
		}
		decoder->pending_packet = 0;
	}
	while ((packet = plm_demux_decode(decoder->demux)) != 0) {
		if (!route_packet(decoder, packet)) {
			if (!decoder->failed) {
				/* packet->data remains owned by demux/input until the next
				 * demux call. Reject writes while retaining this pointer. */
				decoder->pending_packet = packet;
				decoder->route_blocked = 1;
			}
			return;
		}
	}
	signal_es_end(decoder);
}

static void plmpeg_load_es(plm_buffer_t *buffer, void *user)
{
	struct sdk_video_plmpeg *decoder = (struct sdk_video_plmpeg *)user;

	(void)buffer;
	if (!decoder || !decoder->routing_started)
		return;
	plmpeg_route(decoder);
	/* pl_mpeg's append-buffer end marker stores the pre-compaction length.
	 * If a decoder consumes/discards the final bytes, length becomes zero
	 * while total_size retains the old value and has_ended would otherwise
	 * never become true. */
	if (plm_buffer_get_remaining(buffer) == 0U &&
	    ((decoder->video_eof && buffer == decoder->video->buffer) ||
	     (decoder->audio_eof && decoder->audio &&
	      buffer == decoder->audio->buffer)))
		buffer->has_ended = TRUE;
}

static void *plmpeg_create(void)
{
	struct sdk_video_plmpeg *decoder =
		(struct sdk_video_plmpeg *)PLM_MALLOC(sizeof(*decoder));

	if (!decoder)
		return 0;
	memset(decoder, 0, sizeof(*decoder));
	decoder->first_audio_pts = SDK_VIDEO_MEDIA_NO_PTS;
	decoder->current_audio_pts = SDK_VIDEO_MEDIA_NO_PTS;
	decoder->audio_tail_pts = SDK_VIDEO_MEDIA_NO_PTS;
	decoder->last_raw_pts = SDK_VIDEO_MEDIA_NO_PTS;
	sdk_media_timeline_init(&decoder->timeline);
	decoder->input = plm_buffer_create_with_capacity(PLM_INPUT_CAPACITY);
	if (!decoder->input)
		goto fail;
	decoder->demux = plm_demux_create(decoder->input, 1);
	if (!decoder->demux)
		goto fail;
	decoder->video_input =
		plm_buffer_create_with_capacity(PLM_VIDEO_CAPACITY);
	if (!decoder->video_input)
		goto fail;
	plm_buffer_set_load_callback(decoder->video_input, plmpeg_load_es,
	                             decoder);
	decoder->video = plm_video_create_with_buffer(decoder->video_input, 1);
	if (!decoder->video)
		goto fail;
	/* video owns video_input from here. */
	decoder->video_input = 0;
	return decoder;

fail:
	if (decoder->video)
		plm_video_destroy(decoder->video);
	else if (decoder->video_input)
		plm_buffer_destroy(decoder->video_input);
	if (decoder->demux)
		plm_demux_destroy(decoder->demux);
	else if (decoder->input)
		plm_buffer_destroy(decoder->input);
	PLM_FREE(decoder);
	return 0;
}

static void plmpeg_destroy(void *opaque)
{
	struct sdk_video_plmpeg *decoder = (struct sdk_video_plmpeg *)opaque;

	if (!decoder)
		return;
	if (decoder->audio)
		plm_audio_destroy(decoder->audio);
	else if (decoder->audio_input)
		plm_buffer_destroy(decoder->audio_input);
	plm_video_destroy(decoder->video);
	plm_demux_destroy(decoder->demux);
	PLM_FREE(decoder);
}

static int plmpeg_write(void *opaque, const uint8_t *src, uint32_t length,
	                    int eof, uint32_t *accepted)
{
	struct sdk_video_plmpeg *decoder = (struct sdk_video_plmpeg *)opaque;
	size_t written = 0U;

	if (accepted)
		*accepted = 0U;
	if (!decoder || decoder->failed || (length != 0U && !src) ||
	    decoder->input_eof)
		return SDK_VIDEO_BACKEND_WRITE_ERROR;
	if (decoder->pending_packet ||
	    plm_buffer_get_remaining(decoder->input) + length >
		    PLM_INPUT_CAPACITY)
		return SDK_VIDEO_BACKEND_WRITE_BACKPRESSURE;
	if (length != 0U)
		decoder->routing_started = 1;
	if (eof)
		decoder->routing_started = 1;
	if (length != 0U)
		written = plm_buffer_write(decoder->input,
		                           (uint8_t *)(uintptr_t)src, length);
	if (written != length) {
		decoder->failed = 1;
		return SDK_VIDEO_BACKEND_WRITE_ERROR;
	}
	if (eof) {
		plm_buffer_signal_end(decoder->input);
		decoder->input->has_ended = TRUE;
		decoder->input_eof = 1;
	}
	if (accepted)
		*accepted = (uint32_t)written;
	return SDK_VIDEO_BACKEND_WRITE_OK;
}

static int plmpeg_get_info(void *opaque, struct SDKVideoDecoderInfo *info)
{
	struct sdk_video_plmpeg *decoder = (struct sdk_video_plmpeg *)opaque;
	double rate;

	if (!decoder || !info || !plm_video_has_header(decoder->video))
		return 0;
	memset(info, 0, sizeof(*info));
	info->width = (uint32_t)plm_video_get_width(decoder->video);
	info->height = (uint32_t)plm_video_get_height(decoder->video);
	rate = plm_video_get_framerate(decoder->video);
	if (rate > 0.0 && rate < 4294967.0)
		info->frame_rate_milli = (uint32_t)(rate * 1000.0 + 0.5);
	return info->width != 0U && info->height != 0U;
}

static void video_rate(const struct sdk_video_plmpeg *decoder,
	                   uint64_t *units, uint32_t *per_second)
{
	double rate = plm_video_get_framerate(decoder->video);

	*units = 1U;
	*per_second = 25U;
	if (rate > 23.9 && rate < 24.0) {
		*units = 1001U;
		*per_second = 24000U;
	} else if (rate > 24.9 && rate < 25.1) {
		*per_second = 25U;
	} else if (rate > 29.9 && rate < 30.0) {
		*units = 1001U;
		*per_second = 30000U;
	} else if (rate > 29.9 && rate < 30.1) {
		*per_second = 30U;
	} else if (rate > 49.9 && rate < 50.1) {
		*per_second = 50U;
	} else if (rate > 59.8 && rate < 60.0) {
		*units = 1001U;
		*per_second = 60000U;
	} else if (rate > 59.9 && rate < 60.1) {
		*per_second = 60U;
	}
}

static uint32_t timeline_media_flags(uint32_t timeline_flags)
{
	uint32_t flags = 0U;

	if (timeline_flags & SDK_MEDIA_TIMELINE_DERIVED)
		flags |= SDK_VIDEO_MEDIA_FLAG_DERIVED_TIME;
	if (timeline_flags & SDK_MEDIA_TIMELINE_DISCONTINUITY)
		flags |= SDK_VIDEO_MEDIA_FLAG_DISCONTINUITY;
	if (timeline_flags & SDK_MEDIA_TIMELINE_REBASED)
		flags |= SDK_VIDEO_MEDIA_FLAG_REBASED;
	return flags;
}

static int pcm_has_space(struct sdk_video_plmpeg *decoder, uint32_t bytes)
{
	uint64_t queued = decoder->pcm_produced - decoder->pcm_acknowledged;

	if (decoder->pcm_backpressure) {
		if (queued > decoder->media.pcm_low_water_bytes)
			return 0;
		decoder->pcm_backpressure = 0;
		decoder->media_flags &=
			~SDK_VIDEO_MEDIA_FLAG_BACKPRESSURE;
	}
	if (queued + bytes > decoder->media.pcm_high_water_bytes ||
	    queued + bytes > decoder->media.pcm_ring_capacity) {
		decoder->pcm_backpressure = 1;
		decoder->media_flags |= SDK_VIDEO_MEDIA_FLAG_BACKPRESSURE;
		decoder->backpressure_events++;
		return 0;
	}
	return 1;
}

static int16_t sample_to_s16(float sample)
{
	if (sample >= 1.0f)
		return 32767;
	if (sample <= -1.0f)
		return -32768;
	return (int16_t)(sample * 32768.0f);
}

static void flush_pcm(struct sdk_video_plmpeg *decoder,
	                  uint32_t offset, uint32_t bytes)
{
#ifndef SDK_VIDEO_HOST_TEST
	uint32_t first = decoder->media.pcm_ring_capacity - offset;

	if (first > bytes)
		first = bytes;
	Xil_DCacheFlushRange(
		(INTPTR)(uintptr_t)(decoder->media.pcm_ring + offset), first);
	if (bytes > first)
		Xil_DCacheFlushRange(
			(INTPTR)(uintptr_t)decoder->media.pcm_ring, bytes - first);
#else
	(void)decoder;
	(void)offset;
	(void)bytes;
#endif
}

static void write_pcm(struct sdk_video_plmpeg *decoder,
	                  const plm_samples_t *samples)
{
	const uint32_t bytes =
		PLM_AUDIO_SAMPLES_PER_FRAME * SDK_VIDEO_MEDIA_PCM_FRAME_BYTES;
	uint32_t offset =
		(uint32_t)(decoder->pcm_produced %
		           decoder->media.pcm_ring_capacity);
	uint32_t at = offset;
	uint32_t i;

	for (i = 0U; i < PLM_AUDIO_SAMPLES_PER_FRAME * 2U; i++) {
		int16_t value = sample_to_s16(samples->interleaved[i]);

		decoder->media.pcm_ring[at++] =
			(uint8_t)((uint16_t)value >> 8);
		if (at == decoder->media.pcm_ring_capacity)
			at = 0U;
		decoder->media.pcm_ring[at++] = (uint8_t)value;
		if (at == decoder->media.pcm_ring_capacity)
			at = 0U;
	}
	flush_pcm(decoder, offset, bytes);
}

static void append_pcm_anchor(struct sdk_video_plmpeg *decoder,
	                          uint64_t pts, uint32_t flags)
{
	struct plmpeg_pcm_anchor *anchor;

	if (decoder->pcm_anchor_count >= PLM_PCM_ANCHORS) {
		decoder->failed = 1;
		return;
	}
	anchor = &decoder->pcm_anchor[decoder->pcm_anchor_count++];
	anchor->start = decoder->pcm_produced;
	anchor->end = decoder->pcm_produced +
		PLM_AUDIO_SAMPLES_PER_FRAME * SDK_VIDEO_MEDIA_PCM_FRAME_BYTES;
	anchor->pts = pts;
	anchor->flags = flags;
}

static void prune_pcm_anchors(struct sdk_video_plmpeg *decoder)
{
	uint32_t remove = 0U;

	while (remove < decoder->pcm_anchor_count &&
	       decoder->pcm_anchor[remove].end <= decoder->pcm_acknowledged)
		remove++;
	if (remove != 0U) {
		memmove(decoder->pcm_anchor, decoder->pcm_anchor + remove,
		        (decoder->pcm_anchor_count - remove) *
			        sizeof(decoder->pcm_anchor[0]));
		decoder->pcm_anchor_count -= remove;
	}
}

static uint64_t unread_audio_pts(const struct sdk_video_plmpeg *decoder)
{
	const struct plmpeg_pcm_anchor *anchor;
	uint64_t frames;
	uint32_t sample_rate;

	if (decoder->pcm_acknowledged == decoder->pcm_produced)
		return decoder->audio_tail_pts;
	if (decoder->pcm_anchor_count == 0U)
		return SDK_VIDEO_MEDIA_NO_PTS;
	anchor = &decoder->pcm_anchor[0];
	if (decoder->pcm_acknowledged < anchor->start ||
	    decoder->pcm_acknowledged >= anchor->end)
		return SDK_VIDEO_MEDIA_NO_PTS;
	sample_rate = (uint32_t)plm_audio_get_samplerate(decoder->audio);
	if (sample_rate == 0U)
		return anchor->pts;
	frames = (decoder->pcm_acknowledged - anchor->start) /
		SDK_VIDEO_MEDIA_PCM_FRAME_BYTES;
	return anchor->pts + frames * UINT64_C(90000) / sample_rate;
}

static int decode_audio(struct sdk_video_plmpeg *decoder)
{
	const uint32_t block_bytes =
		PLM_AUDIO_SAMPLES_PER_FRAME * SDK_VIDEO_MEDIA_PCM_FRAME_BYTES;
	struct SDKMediaTimelineResult timing;
	plm_samples_t *samples;
	uint64_t raw_pts;
	uint64_t consumed;
	uint32_t sample_rate;

	if (!decoder->media_configured ||
	    decoder->media.audio_codec != SDK_VIDEO_MEDIA_AUDIO_MP2 ||
	    decoder->audio_done)
		return SDK_VIDEO_BACKEND_NEED_INPUT;
	if (decoder->audio_eof &&
	    plm_buffer_get_remaining(decoder->audio->buffer) == 0U)
		decoder->audio->buffer->has_ended = TRUE;
	if (!pcm_has_space(decoder, block_bytes))
		return SDK_VIDEO_BACKEND_BACKPRESSURE;
	samples = plm_audio_decode(decoder->audio);
	if (!samples) {
		if (decoder->failed)
			return SDK_VIDEO_BACKEND_ERROR;
		if (decoder->audio_eof && plm_audio_has_ended(decoder->audio)) {
			decoder->audio_done = 1;
			decoder->media_flags |= SDK_VIDEO_MEDIA_FLAG_AUDIO_DONE;
		}
		return SDK_VIDEO_BACKEND_NEED_INPUT;
	}
	consumed = buffer_consumed(
		decoder->audio_written, decoder->audio->buffer);
	raw_pts = take_es_pts(
		decoder->audio_anchor, &decoder->audio_anchor_count, consumed);
	sample_rate = (uint32_t)plm_audio_get_samplerate(decoder->audio);
	if (sample_rate == 0U ||
	    !sdk_media_timeline_map(
		    &decoder->timeline, SDK_MEDIA_TIMELINE_AUDIO, raw_pts,
		    PLM_AUDIO_SAMPLES_PER_FRAME, sample_rate, &timing)) {
		decoder->failed = 1;
		return SDK_VIDEO_BACKEND_ERROR;
	}
	write_pcm(decoder, samples);
	append_pcm_anchor(decoder, timing.pts,
	                  timeline_media_flags(timing.flags));
	if (decoder->failed)
		return SDK_VIDEO_BACKEND_ERROR;
	decoder->current_audio_pts = timing.pts;
	if (decoder->first_audio_pts == SDK_VIDEO_MEDIA_NO_PTS)
		decoder->first_audio_pts = timing.pts;
	decoder->last_raw_pts = timing.raw_pts;
	decoder->media_flags |= SDK_VIDEO_MEDIA_FLAG_AUDIO_READY;
	decoder->media_event_flags |= timeline_media_flags(timing.flags);
	decoder->pcm_produced += block_bytes;
	if (!sdk_media_timeline_peek_next(
		    &decoder->timeline, SDK_MEDIA_TIMELINE_AUDIO,
		    PLM_AUDIO_SAMPLES_PER_FRAME, sample_rate,
		    &decoder->audio_tail_pts)) {
		decoder->failed = 1;
		return SDK_VIDEO_BACKEND_ERROR;
	}
	decoder->audio_frames++;
	return SDK_VIDEO_BACKEND_PROGRESS;
}

static int plmpeg_decode(void *opaque, struct SDKVideoDecodedFrame *out)
{
	struct sdk_video_plmpeg *decoder = (struct sdk_video_plmpeg *)opaque;
	struct SDKMediaTimelineResult timing;
	plm_frame_t *frame;
	uint64_t raw_pts;
	uint64_t consumed;
	uint64_t units;
	uint32_t per_second;
	int audio_result;

	if (!decoder || !out || decoder->failed)
		return SDK_VIDEO_BACKEND_ERROR;
	if (decoder->media_configured) {
		audio_result = decode_audio(decoder);
		if (audio_result == SDK_VIDEO_BACKEND_PROGRESS ||
		    audio_result == SDK_VIDEO_BACKEND_BACKPRESSURE ||
		    audio_result == SDK_VIDEO_BACKEND_ERROR)
			return audio_result;
	}
	if (decoder->video_eof &&
	    plm_buffer_get_remaining(decoder->video->buffer) == 0U)
		decoder->video->buffer->has_ended = TRUE;
	frame = plm_video_decode(decoder->video);
	if (!frame) {
		if (decoder->failed)
			return SDK_VIDEO_BACKEND_ERROR;
		if (decoder->route_blocked)
			return SDK_VIDEO_BACKEND_BACKPRESSURE;
		if (decoder->video_eof && plm_video_has_ended(decoder->video))
			decoder->video_done = 1;
		if (decoder->media_configured && decoder->input_eof &&
		    decoder->video_done && decoder->audio_done) {
			if (decoder->video_frames == 0U)
				return SDK_VIDEO_BACKEND_ERROR;
			if (decoder->media.audio_codec ==
			    SDK_VIDEO_MEDIA_AUDIO_MP2) {
				if (decoder->selected_audio_type == 0)
					return SDK_VIDEO_BACKEND_UNSUPPORTED;
				if (decoder->audio_frames == 0U)
					return SDK_VIDEO_BACKEND_ERROR;
			}
			return SDK_VIDEO_BACKEND_DONE;
		}
		if (decoder->media_configured && decoder->input_eof)
			return SDK_VIDEO_BACKEND_PROGRESS;
		if (!decoder->media_configured && decoder->video_done)
			return SDK_VIDEO_BACKEND_DONE;
		return SDK_VIDEO_BACKEND_NEED_INPUT;
	}

	memset(out, 0, sizeof(*out));
	out->width = frame->width;
	out->height = frame->height;
	out->y_pitch = frame->y.width;
	out->chroma_pitch = frame->cb.width;
	out->y = frame->y.data;
	out->cb = frame->cb.data;
	out->cr = frame->cr.data;
	if (frame->time > 0.0 && frame->time < 4294967.0)
		out->time_millis = (uint32_t)(frame->time * 1000.0 + 0.5);
	if (decoder->media_configured) {
		consumed = buffer_consumed(
			decoder->video_written, decoder->video->buffer);
		raw_pts = take_es_pts(
			decoder->video_anchor, &decoder->video_anchor_count,
			consumed);
		video_rate(decoder, &units, &per_second);
		if (!decoder->timeline.track[
			    SDK_MEDIA_TIMELINE_VIDEO].have_current) {
			if (!sdk_media_timeline_map(
				    &decoder->timeline,
				    SDK_MEDIA_TIMELINE_VIDEO, raw_pts,
				    units, per_second, &timing)) {
				decoder->failed = 1;
				return SDK_VIDEO_BACKEND_ERROR;
			}
		} else if (!sdk_media_timeline_map(
			           &decoder->timeline,
			           SDK_MEDIA_TIMELINE_VIDEO,
			           SDK_MEDIA_TIMELINE_NO_PTS,
			           units, per_second, &timing)) {
			decoder->failed = 1;
			return SDK_VIDEO_BACKEND_ERROR;
		} else if (raw_pts != SDK_MEDIA_TIMELINE_NO_PTS) {
			struct SDKMediaTimelineResult observed;

			if (!sdk_media_timeline_observe_ordered(
				    &decoder->timeline,
				    SDK_MEDIA_TIMELINE_VIDEO, raw_pts,
				    &observed)) {
				decoder->failed = 1;
				return SDK_VIDEO_BACKEND_ERROR;
			}
			timing.pts = observed.pts;
			timing.origin = observed.origin;
			timing.raw_pts = observed.raw_pts;
			timing.flags |= observed.flags;
		}
		out->media_pts = timing.pts;
		out->raw_pts = timing.raw_pts;
		out->media_flags = timeline_media_flags(timing.flags);
		decoder->last_raw_pts = timing.raw_pts;
	} else {
		out->media_pts = SDK_VIDEO_MEDIA_NO_PTS;
		out->raw_pts = SDK_VIDEO_MEDIA_NO_PTS;
	}
	decoder->video_frames++;
	return SDK_VIDEO_BACKEND_FRAME;
}

static int plmpeg_configure_media(
	void *opaque, const struct SDKVideoMediaConfig *config)
{
	struct sdk_video_plmpeg *decoder = (struct sdk_video_plmpeg *)opaque;

	if (!decoder || !config || decoder->media_configured ||
	    (config->audio_codec != SDK_VIDEO_MEDIA_AUDIO_NONE &&
	     config->audio_codec != SDK_VIDEO_MEDIA_AUDIO_MP2))
		return 0;
	if (config->audio_codec == SDK_VIDEO_MEDIA_AUDIO_NONE) {
		if (config->pcm_ring ||
		    config->pcm_ring_capacity != 0U ||
		    config->pcm_low_water_bytes != 0U ||
		    config->pcm_high_water_bytes != 0U)
			return 0;
		decoder->media = *config;
		decoder->media_configured = 1;
		decoder->audio_eof = 1;
		decoder->audio_done = 1;
		return 1;
	}
	if (
	    !config->pcm_ring ||
	    config->pcm_ring_capacity <
		    PLM_AUDIO_SAMPLES_PER_FRAME *
			    SDK_VIDEO_MEDIA_PCM_FRAME_BYTES ||
	    config->pcm_ring_capacity > SDK_VIDEO_MEDIA_MAX_PCM_RING ||
	    (config->pcm_ring_capacity &
	     (SDK_VIDEO_MEDIA_PCM_FRAME_BYTES - 1U)) != 0U ||
	    config->pcm_low_water_bytes >=
		    config->pcm_high_water_bytes ||
	    config->pcm_high_water_bytes <
		    PLM_AUDIO_SAMPLES_PER_FRAME *
			    SDK_VIDEO_MEDIA_PCM_FRAME_BYTES ||
	    config->pcm_high_water_bytes >
		    config->pcm_ring_capacity ||
	    (config->pcm_low_water_bytes &
	     (SDK_VIDEO_MEDIA_PCM_FRAME_BYTES - 1U)) != 0U ||
	    (config->pcm_high_water_bytes &
	     (SDK_VIDEO_MEDIA_PCM_FRAME_BYTES - 1U)) != 0U)
		return 0;
	decoder->audio_input =
		plm_buffer_create_with_capacity(PLM_AUDIO_CAPACITY);
	if (!decoder->audio_input)
		return 0;
	plm_buffer_set_load_callback(decoder->audio_input, plmpeg_load_es,
	                             decoder);
	decoder->audio =
		plm_audio_create_with_buffer(decoder->audio_input, 1);
	if (!decoder->audio) {
		plm_buffer_destroy(decoder->audio_input);
		decoder->audio_input = 0;
		return 0;
	}
	decoder->audio_input = 0;
	decoder->media = *config;
	decoder->media_configured = 1;
	return 1;
}

static int plmpeg_get_media_info(
	void *opaque, struct SDKVideoMediaInfo *info)
{
	struct sdk_video_plmpeg *decoder = (struct sdk_video_plmpeg *)opaque;

	if (!decoder || !info || !decoder->media_configured)
		return 0;
	memset(info, 0, sizeof(*info));
	info->sample_rate = decoder->audio
		? (uint32_t)plm_audio_get_samplerate(decoder->audio) : 0U;
	if (decoder->media.audio_codec == SDK_VIDEO_MEDIA_AUDIO_MP2) {
		info->channels = 2U;
		info->sample_format = SDK_VIDEO_MEDIA_SAMPLE_S16BE;
	}
	info->pcm_produced = decoder->pcm_produced;
	info->pcm_acknowledged = decoder->pcm_acknowledged;
	info->audio_pts = unread_audio_pts(decoder);
	info->current_audio_pts = decoder->current_audio_pts;
	info->first_audio_pts = decoder->first_audio_pts;
	info->pts_origin = decoder->timeline.origin;
	info->raw_pts = decoder->last_raw_pts;
	info->audio_frames = decoder->audio_frames;
	info->backpressure_events = decoder->backpressure_events;
	info->flags = decoder->media_flags | decoder->media_event_flags;
	decoder->media_event_flags = 0U;
	return 1;
}

static int plmpeg_ack_media(void *opaque, uint64_t acknowledged)
{
	struct sdk_video_plmpeg *decoder = (struct sdk_video_plmpeg *)opaque;

	if (!decoder || !decoder->media_configured ||
	    decoder->media.audio_codec != SDK_VIDEO_MEDIA_AUDIO_MP2 ||
	    acknowledged < decoder->pcm_acknowledged ||
	    acknowledged > decoder->pcm_produced ||
	    (acknowledged & (SDK_VIDEO_MEDIA_PCM_FRAME_BYTES - 1U)) != 0U)
		return 0;
	decoder->pcm_acknowledged = acknowledged;
	prune_pcm_anchors(decoder);
	if (decoder->pcm_produced - decoder->pcm_acknowledged <=
	    decoder->media.pcm_low_water_bytes) {
		decoder->pcm_backpressure = 0;
		decoder->media_flags &= ~SDK_VIDEO_MEDIA_FLAG_BACKPRESSURE;
	}
	if (decoder->pcm_produced == decoder->pcm_acknowledged)
		decoder->media_flags &= ~SDK_VIDEO_MEDIA_FLAG_AUDIO_READY;
	return 1;
}

static const struct SDKVideoDecoderOps plmpeg_ops = {
	.codec = SDK_VIDEO_CODEC_MPEG1,
	.container = SDK_VIDEO_CONTAINER_MPEG_PS,
	.name = "pl_mpeg MPEG-1/PS",
	.create = plmpeg_create,
	.destroy = plmpeg_destroy,
	.write = plmpeg_write,
	.get_info = plmpeg_get_info,
	.decode = plmpeg_decode,
	.configure_media = plmpeg_configure_media,
	.get_media_info = plmpeg_get_media_info,
	.ack_media = plmpeg_ack_media,
};

const struct SDKVideoDecoderOps *sdk_video_backend_find(uint32_t codec,
	                                                     uint32_t container)
{
	/* Add future codec/container backends to this registry. */
	if (codec == plmpeg_ops.codec && container == plmpeg_ops.container)
		return &plmpeg_ops;
	return 0;
}
