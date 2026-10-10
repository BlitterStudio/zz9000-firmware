/*
 * libmpeg2 + bounded PS/VOB demux backend: DVD-resolution MPEG-2 video with
 * MP2 / DVD-LPCM / AC-3 audio for the U11 feasibility experiment.
 *
 * The backend reuses the codec-neutral sdk_video_backend contract, the
 * sdk_media_timeline integer clock and the S16BE PCM ring conventions of
 * the pl_mpeg backend, so media sessions, YUY2 packing and the ZZPlay feed
 * path stay codec-neutral. Decoded surfaces are allocated from the
 * card-resident decode heap and stay card-local.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_video_backend.h"
#include "sdk_media_profile.h"
#include "sdk_media_timeline.h"
#include "sdk_dvd_ps.h"
#include "sdk_dvd_lpcm.h"
#include "sdk_dvd_ac3.h"

#include <stdlib.h>
#include <string.h>

#ifndef SDK_VIDEO_HOST_TEST
#include "sdk_compression.h"
#include "sdk_decode_reclaim.h"
#include "sdk_smp_lock.h"
#include "xil_cache.h"
#endif

#include "mpeg2.h"

#define M2_ES_CAPACITY (256U * 1024U)
#define M2_MP2_CAPACITY (64U * 1024U)
#define M2_PTS_SLOTS 64U
#define M2_MAX_WIDTH 720U
#define M2_MAX_HEIGHT 576U
#define M2_MAX_STRIDE 736U

#define M2_NO_PTS SDK_VIDEO_MEDIA_NO_PTS

#define PLM_NO_STDIO 1
#ifdef SDK_VIDEO_HOST_TEST
#define M2_MALLOC(size) malloc(size)
#define M2_FREE(ptr) free(ptr)
#else
static void *m2_alloc(size_t size)
{
	void *ptr = sdk_decode_heap_alloc(size);

	if (ptr)
		sdk_decode_track(ptr);
	return ptr;
}

static void m2_free(void *ptr)
{
	if (!ptr)
		return;
	sdk_decode_untrack(ptr);
	sdk_decode_heap_free(ptr);
}

static void *m2_alloc_hook(unsigned size, mpeg2_alloc_t reason)
{
	(void)reason;
	return m2_alloc((size_t)size);
}

static int m2_free_hook(void *buf)
{
	m2_free(buf);
	return 1;
}

#define M2_MALLOC(size) m2_alloc(size)
#define M2_FREE(ptr) m2_free(ptr)
#endif

/* The pl_mpeg translation unit (compiled with PL_MPEG_IMPLEMENTATION in
 * sdk_video_plmpeg.c) provides the MP2 elementary decoder symbols; this
 * backend consumes the public API only, so the two backends never
 * duplicate the pl_mpeg implementation. */
#define PLM_NO_STDIO 1
#include "third_party/pl_mpeg/pl_mpeg.h"


struct m2_pts_slot {
	uint32_t tag;
	uint64_t pts;
	uint64_t dts;
	uint8_t used;
};

struct sdk_video_mpeg2 {
	struct SDKDVDPSDemux demux;
	mpeg2dec_t *decoder;
	uint8_t *es;
	uint32_t es_len;
	uint32_t fbuf_stride;
	uint8_t fbufs_bound;
	plm_buffer_t *mp2_input;
	plm_audio_t *mp2;
	struct SDKDVDLPCM lpcm;
	struct SDKDVDAC3 ac3;
	uint32_t ac3_packet_offset;  /* payload bytes of the head packet staged */
	struct SDKVideoMediaConfig media;
	struct SDKMediaTimeline timeline;
	struct m2_pts_slot pts_slot[M2_PTS_SLOTS];
	uint32_t next_tag;
	uint32_t width;
	uint32_t height;
	uint32_t frame_rate_milli;
	uint64_t pcm_produced;
	uint64_t pcm_acknowledged;
	uint64_t first_audio_pts;
	uint64_t last_audio_pts;
	uint32_t video_frames;
	uint32_t audio_frames;
	uint32_t backpressure_events;
	uint32_t malformed;
	uint32_t media_flags;
	uint8_t audio_selected;
	uint8_t mp2_selected;
	uint8_t lpcm_selected;
	uint8_t ac3_selected;
	uint8_t media_configured;
	uint8_t input_eof;
	uint8_t video_end_signalled;
	uint8_t video_done;
	uint8_t pcm_backpressure;
	uint8_t failed;
};

/* ------------------------------------------------------------------ pts */

static struct m2_pts_slot *pts_slot_for_store(struct sdk_video_mpeg2 *d,
                                              uint32_t tag)
{
	uint32_t oldest = 0;
	uint32_t i;

	for (i = 0U; i < M2_PTS_SLOTS; i++) {
		if (d->pts_slot[i].used && d->pts_slot[i].tag == tag)
			return &d->pts_slot[i];
		if (!d->pts_slot[i].used)
			return &d->pts_slot[i];
		if (d->pts_slot[i].tag < d->pts_slot[oldest].tag)
			oldest = i;
	}
	return &d->pts_slot[oldest];
}

static const struct m2_pts_slot *pts_slot_find(
	const struct sdk_video_mpeg2 *d, uint32_t tag)
{
	uint32_t i;

	for (i = 0U; i < M2_PTS_SLOTS; i++)
		if (d->pts_slot[i].used && d->pts_slot[i].tag == tag)
			return &d->pts_slot[i];
	return 0;
}

static void pts_slot_store(struct sdk_video_mpeg2 *d, uint32_t tag,
                           uint64_t pts, uint64_t dts)
{
	struct m2_pts_slot *slot = pts_slot_for_store(d, tag);

	slot->tag = tag;
	slot->pts = pts;
	slot->dts = dts;
	slot->used = 1U;
}

/* -------------------------------------------------------------- pcm ring */

static uint32_t ring_free(const struct sdk_video_mpeg2 *d)
{
	uint64_t pending = d->pcm_produced - d->pcm_acknowledged;

	if (pending >= d->media.pcm_ring_capacity)
		return 0U;
	return (uint32_t)(d->media.pcm_ring_capacity - pending);
}

static void ring_write(struct sdk_video_mpeg2 *d, const uint8_t *src,
                       uint32_t bytes)
{
	uint32_t offset = (uint32_t)(d->pcm_produced %
	                             d->media.pcm_ring_capacity);
	uint32_t first = d->media.pcm_ring_capacity - offset;

	if (first > bytes)
		first = bytes;
	memcpy(d->media.pcm_ring + offset, src, first);
	memcpy(d->media.pcm_ring, src + first, bytes - first);
#ifndef SDK_VIDEO_HOST_TEST
	Xil_DCacheFlushRange(
		(INTPTR)(uintptr_t)(d->media.pcm_ring + offset), first);
	if (bytes - first != 0U)
		Xil_DCacheFlushRange(
			(INTPTR)(uintptr_t)d->media.pcm_ring, bytes - first);
#endif
	d->pcm_produced += bytes;
}

/* Emit decoded units (whole stereo frames only) into the PCM ring. The
 * sticky backpressure flag releases under the low-water mark. */
static uint32_t ring_emit(struct sdk_video_mpeg2 *d, const uint8_t *src,
                          uint32_t bytes)
{
	uint32_t free_bytes;

	if (bytes == 0U)
		return 0U;
	if (d->pcm_backpressure)
		return 0U;
	free_bytes = ring_free(d);
	if (free_bytes < 4U) {
		d->pcm_backpressure = 1U;
		d->backpressure_events++;
		d->media_flags |= SDK_VIDEO_MEDIA_FLAG_BACKPRESSURE;
		return 0U;
	}
	if (bytes > free_bytes)
		bytes = free_bytes & ~3U;
	ring_write(d, src, bytes);
	d->media_flags |= SDK_VIDEO_MEDIA_FLAG_AUDIO_READY;
	return bytes;
}

/* ---------------------------------------------------------------- audio */

static void pump_mp2(struct sdk_video_mpeg2 *d)
{
	static uint8_t swap[PLM_AUDIO_SAMPLES_PER_FRAME * 4U];

	while (!d->pcm_backpressure && !plm_audio_has_ended(d->mp2)) {
		plm_samples_t *samples = plm_audio_decode(d->mp2);
		uint32_t bytes;
		uint32_t i;

		if (!samples || samples->count == 0U)
			break;
		bytes = samples->count * 4U;
		if (bytes > sizeof(swap))
			bytes = sizeof(swap);
		for (i = 0U; i < bytes / 2U; i++) {
			float sample = samples->interleaved[i];
			int16_t value;

			if (sample >= 1.0f)
				value = 32767;
			else if (sample <= -1.0f)
				value = -32768;
			else
				value = (int16_t)(sample * 32768.0f);
			swap[2U * i] = (uint8_t)((uint16_t)value >> 8);
			swap[2U * i + 1U] = (uint8_t)value;
		}
		if (ring_emit(d, swap, bytes) == 0U)
			break;
		d->audio_frames++;
	}
}

static uint32_t route_mp2(struct sdk_video_mpeg2 *d,
                          const struct SDKDVDPSPacket *packet)
{
	size_t remaining = plm_buffer_get_remaining(d->mp2_input);

	if (remaining + packet->length > M2_MP2_CAPACITY)
		return 0U;
	if (plm_buffer_write(d->mp2_input,
	                     (uint8_t *)packet->data,
	                     packet->length) != packet->length) {
		d->failed = 1U;
		return 0U;
	}
	pump_mp2(d);
	return 1U;
}

static uint32_t route_lpcm(struct sdk_video_mpeg2 *d,
                           const struct SDKDVDPSPacket *packet)
{
	static uint8_t pcm[SDK_DVD_PS_PES_MAX * 2U];
	uint32_t bytes;

	if (packet->length <= SDK_DVD_LPCM_HEADER_BYTES)
		return 1U;
	if (!sdk_dvd_lpcm_header(&d->lpcm, packet->data))
		return 1U;
	bytes = sdk_dvd_lpcm_unpack(
		&d->lpcm, packet->data + SDK_DVD_LPCM_HEADER_BYTES,
		packet->length - SDK_DVD_LPCM_HEADER_BYTES,
		pcm, sizeof(pcm) / 2U);
	if (bytes == 0U)
		return 1U;
	if (d->lpcm.channels == 2U) {
		if (ring_emit(d, pcm, bytes) != 0U)
			d->audio_frames++;
	} else {
		/* Dual-mono expansion keeps the ring stereo.
		 * Unpack directly in reverse so in-place expansion never
		 * overwrites unread samples. */
		uint32_t frames = bytes / 2U;
		int32_t i;

		if (frames * 4U > sizeof(pcm))
			frames = sizeof(pcm) / 4U;
		for (i = (int32_t)frames - 1; i >= 0; i--) {
			pcm[4 * i + 3] = pcm[2 * i + 1];
			pcm[4 * i + 2] = pcm[2 * i];
			pcm[4 * i + 1] = pcm[2 * i + 1];
			pcm[4 * i]     = pcm[2 * i];
		}
		if (ring_emit(d, pcm, 4U * frames) != 0U)
			d->audio_frames++;
	}
	return 1U;
}

/* Decode every complete staged AC-3 frame, one frame at a time, while the
 * PCM ring has room for a whole frame. A pack can carry several frames
 * (about 2.6 at 192 kbit/s), so the stage never backs up as long as the
 * client acknowledges. Returns 0 when the ring stopped it with a frame
 * still staged; backpressure is raised so routing pauses until the ack. */
static uint32_t pump_ac3(struct sdk_video_mpeg2 *d)
{
	static uint8_t pcm[SDK_DVD_AC3_FRAME_PCM_BYTES];

	while (sdk_dvd_ac3_frame_ready(&d->ac3)) {
		uint32_t bytes;

		/* ring_emit refuses everything while backpressure is set, so
		 * a decoded frame would be lost: wait for the ack instead. */
		if (d->pcm_backpressure ||
		    ring_free(d) < SDK_DVD_AC3_FRAME_PCM_BYTES) {
			if (!d->pcm_backpressure)
				d->backpressure_events++;
			d->pcm_backpressure = 1U;
			d->media_flags |= SDK_VIDEO_MEDIA_FLAG_BACKPRESSURE;
			return 0U;
		}
		bytes = sdk_dvd_ac3_decode(&d->ac3, pcm, sizeof(pcm));
		if (bytes != 0U) {
			ring_emit(d, pcm, bytes);
			d->audio_frames++;
		}
	}
	return 1U;
}

/* Stage the packet payload in pieces the AC-3 stage can hold, decoding as
 * it goes. Returns 0 (packet stays queued, resumed at ac3_packet_offset)
 * when the PCM ring is full. */
static uint32_t route_ac3(struct sdk_video_mpeg2 *d,
                          const struct SDKDVDPSPacket *packet)
{
	const uint8_t *payload = packet->data + SDK_DVD_AC3_HEADER_BYTES;
	uint32_t length;

	if (packet->length <= SDK_DVD_AC3_HEADER_BYTES)
		return 1U;
	length = packet->length - SDK_DVD_AC3_HEADER_BYTES;
	while (d->ac3_packet_offset < length) {
		uint32_t taken;

		if (!pump_ac3(d))
			return 0U;
		taken = sdk_dvd_ac3_feed(&d->ac3, payload + d->ac3_packet_offset,
		                         length - d->ac3_packet_offset);
		if (taken == 0U) {
			/* Stage full without a whole frame in it: not AC-3
			 * the decoder can use. Drop it and resync. */
			d->malformed++;
			sdk_dvd_ac3_reset(&d->ac3);
			continue;
		}
		d->ac3_packet_offset += taken;
	}
	d->ac3_packet_offset = 0U;
	pump_ac3(d);
	return 1U;
}

/* Returns 0 when the packet must stay queued (AC-3 behind a full ring). */
static uint32_t route_audio(struct sdk_video_mpeg2 *d,
                            const struct SDKDVDPSPacket *packet)
{
	if (!d->audio_selected) {
		if (packet->kind == SDK_DVD_PS_MP2 &&
		    d->media.audio_codec == SDK_VIDEO_MEDIA_AUDIO_MP2) {
			d->mp2_selected = 1U;
			d->audio_selected = 1U;
		} else if (packet->kind == SDK_DVD_PS_LPCM &&
		           d->media.audio_codec ==
		           SDK_VIDEO_MEDIA_AUDIO_LPCM) {
			d->lpcm_selected = 1U;
			d->audio_selected = 1U;
		} else if (packet->kind == SDK_DVD_PS_AC3 &&
		           d->media.audio_codec == SDK_VIDEO_MEDIA_AUDIO_AC3) {
			d->ac3_selected = 1U;
			d->audio_selected = 1U;
		} else {
			return 1U;
		}
	}
	if ((packet->kind == SDK_DVD_PS_MP2 && !d->mp2_selected) ||
	    (packet->kind == SDK_DVD_PS_LPCM && !d->lpcm_selected) ||
	    (packet->kind == SDK_DVD_PS_AC3 && !d->ac3_selected))
		return 1U;
	if (packet->pts != SDK_DVD_PS_NO_TS) {
		if (d->first_audio_pts == M2_NO_PTS)
			d->first_audio_pts = packet->pts;
		d->last_audio_pts = packet->pts;
	}
	if (packet->kind == SDK_DVD_PS_MP2)
		route_mp2(d, packet);
	else if (packet->kind == SDK_DVD_PS_LPCM)
		route_lpcm(d, packet);
	else
		return route_ac3(d, packet);
	return 1U;
}

/* Drain the audio queue: before the stream kind is selected this scans
 * for the configured kind (bounded skip of other candidate streams); after
 * selection it decodes every matching packet up to backpressure. AC-3
 * frames left staged behind a full ring are decoded first. */
static void route_audio_queue(struct sdk_video_mpeg2 *d)
{
	if (d->ac3_selected && !pump_ac3(d))
		return;
	while (!d->pcm_backpressure) {
		const struct SDKDVDPSPacket *packet =
			sdk_dvd_ps_peek_audio(&d->demux);
		if (!packet)
			break;
		if ((packet->kind == SDK_DVD_PS_MP2 ||
		     packet->kind == SDK_DVD_PS_LPCM ||
		     packet->kind == SDK_DVD_PS_AC3) &&
		    !route_audio(d, packet))
			break;
		sdk_dvd_ps_pop_audio(&d->demux, 0);
	}
}

/* --------------------------------------------------------------- video */

static int bind_fbufs(struct sdk_video_mpeg2 *d, uint32_t width,
                      uint32_t height)
{
	uint32_t stride = (width + 15U) & ~15U;

	if (width == 0U || height == 0U || stride > M2_MAX_STRIDE ||
	    height > M2_MAX_HEIGHT)
		return 0;
	if (d->fbufs_bound && d->fbuf_stride == stride &&
	    d->width == width && d->height == height)
		return 1;
	mpeg2_stride(d->decoder, (int)stride);
	d->fbuf_stride = stride;
	d->width = width;
	d->height = height;
	d->fbufs_bound = 1U;
	return 1;
}

static uint32_t milli_from_period(uint32_t frame_period)
{
	uint64_t milli;

	if (frame_period == 0U)
		return 0U;
	milli = (UINT64_C(27000000000) + (frame_period / 2U)) / frame_period;
	if (milli > 0xffffffffU)
		return 0xffffffffU;
	return (uint32_t)milli;
}

static uint32_t picture_display_fields(const mpeg2_picture_t *picture)
{
	if (!picture || picture->nb_fields == 0U)
		return 2U;
	return picture->nb_fields;
}

static uint32_t picture_flags_of(const mpeg2_picture_t *picture)
{
	uint32_t flags = 0U;

	if (!picture)
		return 0U;
	if ((picture->flags & PIC_FLAG_PROGRESSIVE_FRAME) != 0U)
		flags |= SDK_VIDEO_PICTURE_FLAG_PROGRESSIVE;
	if ((picture->flags & PIC_FLAG_TOP_FIELD_FIRST) != 0U)
		flags |= SDK_VIDEO_PICTURE_FLAG_TFF;
	else if ((picture->flags & PIC_FLAG_PROGRESSIVE_FRAME) == 0U)
		flags |= SDK_VIDEO_PICTURE_FLAG_BFF;
	if ((picture->flags & PIC_FLAG_REPEAT_FIRST_FIELD) != 0U)
		flags |= SDK_VIDEO_PICTURE_FLAG_REPEAT;
	return flags;
}

static void picture_timestamps(const struct sdk_video_mpeg2 *d,
                               const mpeg2_picture_t *picture,
                               uint64_t *raw_pts, uint64_t *raw_dts)
{
	const struct m2_pts_slot *slot;

	*raw_pts = M2_NO_PTS;
	*raw_dts = M2_NO_PTS;
	if (picture && (picture->flags & PIC_FLAG_TAGS) != 0U) {
		slot = pts_slot_find(d, picture->tag);
		if (slot) {
			*raw_pts = slot->pts;
			*raw_dts = slot->dts;
		}
	}
}

static void fill_frame(struct sdk_video_mpeg2 *d,
                       const mpeg2_info_t *info,
                       struct SDKVideoDecodedFrame *frame)
{
	const mpeg2_picture_t *picture = info->display_picture;
	struct SDKMediaTimelineResult result;
	uint64_t raw_pts;
	uint64_t raw_dts;
	uint64_t media_pts = M2_NO_PTS;
	uint32_t ms_per_frame = d->frame_rate_milli != 0U
		? (1000000U + d->frame_rate_milli / 2U) /
			d->frame_rate_milli
		: 40U;

	picture_timestamps(d, picture, &raw_pts, &raw_dts);
	if (raw_pts != M2_NO_PTS &&
	    sdk_media_timeline_observe_ordered(
		    (struct SDKMediaTimeline *)&d->timeline,
		    SDK_MEDIA_TIMELINE_VIDEO, raw_pts, &result) &&
	    result.pts >= result.origin)
		media_pts = result.pts - result.origin;
	memset(frame, 0, sizeof(*frame));
	frame->width = d->width;
	frame->height = d->height;
	frame->y_pitch = d->fbuf_stride;
	frame->chroma_pitch = d->fbuf_stride / 2U;
	frame->y = info->display_fbuf->buf[0];
	frame->cb = info->display_fbuf->buf[1];
	frame->cr = info->display_fbuf->buf[2];
	frame->time_millis = d->video_frames * ms_per_frame / 1000U;
	frame->media_pts = media_pts;
	frame->raw_pts = raw_pts;
	frame->raw_dts = raw_dts;
	frame->picture_flags = picture_flags_of(picture);
	frame->display_fields = picture_display_fields(picture);
	d->video_frames++;
}

static int demux_drained(const struct sdk_video_mpeg2 *d)
{
	return d->input_eof && d->demux.staged == 0U &&
	       d->demux.video.count == 0U && d->demux.audio.count == 0U;
}

static int parse_video(struct sdk_video_mpeg2 *d,
                       struct SDKVideoDecodedFrame *out_frame)
{
	const mpeg2_info_t *info = mpeg2_info(d->decoder);
	mpeg2_state_t state;
	int got_frame = 0;

	for (;;) {
		state = mpeg2_parse(d->decoder);
		switch (state) {
		case STATE_SEQUENCE:
		case STATE_SEQUENCE_MODIFIED:
			if (!bind_fbufs(d, info->sequence->width,
			                info->sequence->height)) {
				d->failed = 1U;
				return -1;
			}
			d->frame_rate_milli = milli_from_period(
				info->sequence->frame_period);
			break;
		case STATE_SLICE:
		case STATE_SLICE_1ST:
			if (info->display_fbuf) {
				if (!d->fbufs_bound) {
					d->failed = 1U;
					return -1;
				}
				fill_frame(d, info, out_frame);
				got_frame = 1;
			}
			break;
		case STATE_END:
		case STATE_INVALID_END:
			d->video_done = 1U;
			if (state == STATE_INVALID_END)
				d->malformed++;
			if (info->display_fbuf && d->fbufs_bound) {
				fill_frame(d, info, out_frame);
				got_frame = 1;
			}
			return got_frame ? 1 : 0;
		case STATE_INVALID:
			d->malformed++;
			break;
		case STATE_BUFFER:
			if (!d->video_end_signalled && d->input_eof &&
			    demux_drained(d)) {
				static const uint8_t seq_end[4] = {
					0x00, 0x00, 0x01, 0xb7
				};
				mpeg2_buffer(d->decoder,
				             (uint8_t *)seq_end,
				             (uint8_t *)seq_end + sizeof(seq_end));
				d->video_end_signalled = 1U;
				continue;
			}
			return got_frame ? 1 : 0;
		default:
			break;
		}
		if (got_frame)
			return 1;
	}
}

/* Video-only session: nobody consumes audio, and queued packets would
 * fill the demux until every write reports backpressure nothing can
 * relieve, or hold back the end of the stream. Each pop re-pumps the
 * demux, so this runs wherever video is taken too. */
static void drop_audio(struct sdk_video_mpeg2 *d)
{
	if (d->media_configured)
		return;
	while (sdk_dvd_ps_peek_audio(&d->demux))
		sdk_dvd_ps_pop_audio(&d->demux, 0);
}

/* Hand one queued PES to libmpeg2 per call. mpeg2_tag_picture() latches the
 * tag onto the next picture the decoder PARSES, so a PTS-bearing PES must
 * be tagged and handed over in its own step: batching several pictures into
 * one mpeg2_buffer() would leave only the last tag set when parsing runs. */
static int feed_video(struct sdk_video_mpeg2 *d)
{
	const struct SDKDVDPSPacket *packet;
	uint32_t tag;

	packet = sdk_dvd_ps_peek_video(&d->demux);
	if (!packet)
		return 0;
	if (packet->length > M2_ES_CAPACITY - d->es_len)
		return 0;
	tag = d->next_tag++;
	if (packet->pts != SDK_DVD_PS_NO_TS ||
	    packet->dts != SDK_DVD_PS_NO_TS) {
		pts_slot_store(d, tag,
		               packet->pts == SDK_DVD_PS_NO_TS
		               ? M2_NO_PTS : packet->pts,
		               packet->dts == SDK_DVD_PS_NO_TS
		               ? M2_NO_PTS : packet->dts);
		mpeg2_tag_picture(d->decoder, tag, 0U);
	}
	memcpy(d->es + d->es_len, packet->data, packet->length);
	d->es_len += packet->length;
	mpeg2_buffer(d->decoder, d->es, d->es + d->es_len);
	d->es_len = 0U;
	sdk_dvd_ps_pop_video(&d->demux, 0);
	drop_audio(d);
	return 1;
}

/* ----------------------------------------------------------------- ops */

static void *mpeg2_create(void)
{
	struct sdk_video_mpeg2 *d;

#ifndef SDK_VIDEO_HOST_TEST
	mpeg2_malloc_hooks(m2_alloc_hook, m2_free_hook);
#endif
	d = (struct sdk_video_mpeg2 *)M2_MALLOC(sizeof(struct sdk_video_mpeg2));
	if (!d)
		return 0;
	memset(d, 0, sizeof(*d));
	sdk_dvd_ps_init(&d->demux);
	sdk_dvd_ac3_init(&d->ac3);
	sdk_media_timeline_init(&d->timeline);
	d->first_audio_pts = M2_NO_PTS;
	d->last_audio_pts = M2_NO_PTS;
	d->decoder = mpeg2_init();
	if (!d->decoder) {
		M2_FREE(d);
		return 0;
	}
	d->es = (uint8_t *)M2_MALLOC(M2_ES_CAPACITY);
	if (!d->es) {
		mpeg2_close(d->decoder);
		M2_FREE(d);
		return 0;
	}
	return d;
}

static void mpeg2_destroy(void *decoder)
{
	struct sdk_video_mpeg2 *d = (struct sdk_video_mpeg2 *)decoder;

	if (!d)
		return;
	if (d->decoder)
		mpeg2_close(d->decoder);
	if (d->mp2)
		plm_audio_destroy(d->mp2);
	if (d->mp2_input)
		plm_buffer_destroy(d->mp2_input);
	sdk_dvd_ac3_destroy(&d->ac3);
	M2_FREE(d->es);
	M2_FREE(d);
}

static int mpeg2_write(void *decoder, const uint8_t *src, uint32_t length,
                       int eof, uint32_t *accepted)
{
	struct sdk_video_mpeg2 *d = (struct sdk_video_mpeg2 *)decoder;
	uint32_t used = 0U;

	if (!d || d->failed || (!src && length != 0U))
		return SDK_VIDEO_BACKEND_WRITE_ERROR;
	if (d->input_eof)
		return SDK_VIDEO_BACKEND_WRITE_ERROR;
	sdk_dvd_ps_write(&d->demux, src, length, &used, eof);
	if (eof && used == length)
		d->input_eof = 1U;
	if (accepted)
		*accepted = used;
	/* The stream contract treats BACKPRESSURE as "nothing consumed";
	 * partial acceptance is plain success so the client keeps feeding. */
	if (used == 0U && length != 0U)
		return SDK_VIDEO_BACKEND_WRITE_BACKPRESSURE;
	return SDK_VIDEO_BACKEND_WRITE_OK;
}

static int mpeg2_get_info(void *decoder, struct SDKVideoDecoderInfo *info)
{
	struct sdk_video_mpeg2 *d = (struct sdk_video_mpeg2 *)decoder;

	if (!d || !info || !d->fbufs_bound)
		return 0;
	info->width = d->width;
	info->height = d->height;
	info->frame_rate_milli = d->frame_rate_milli;
	return 1;
}

static int audio_drained(struct sdk_video_mpeg2 *d)
{
	if (!d->media_configured || !d->audio_selected)
		return 1;
	if (d->demux.audio.count != 0U || d->demux.staged != 0U)
		return 0;
	if (d->mp2_selected)
		return plm_audio_has_ended(d->mp2) ||
		       plm_buffer_get_remaining(d->mp2_input) == 0U;
	/* A trailing partial frame can never complete once input ended. */
	if (d->ac3_selected)
		return !sdk_dvd_ac3_frame_ready(&d->ac3);
	return 1U;
}

static int mpeg2_decode(void *decoder, struct SDKVideoDecodedFrame *frame)
{
	struct sdk_video_mpeg2 *d = (struct sdk_video_mpeg2 *)decoder;
	int result;

	if (!d || !frame || d->failed)
		return SDK_VIDEO_BACKEND_ERROR;
	if (d->media_configured)
		route_audio_queue(d);
	else
		drop_audio(d);
	for (;;) {
		result = parse_video(d, frame);
		if (result > 0)
			return SDK_VIDEO_BACKEND_FRAME;
		if (result < 0)
			return SDK_VIDEO_BACKEND_ERROR;
		if (!feed_video(d))
			break;
	}
	if (d->video_done && audio_drained(d) &&
	    (!d->media_configured ||
	     d->pcm_produced == d->pcm_acknowledged)) {
		if (d->media_configured)
			d->media_flags |= SDK_VIDEO_MEDIA_FLAG_AUDIO_DONE;
		return SDK_VIDEO_BACKEND_DONE;
	}
	return SDK_VIDEO_BACKEND_NEED_INPUT;
}

/* DVD Program Stream audio: MP2, LPCM or AC-3. A media session always
 * carries one; video-only playback uses a legacy session. */
static int mpeg2_audio_ok(uint32_t audio_codec)
{
	return audio_codec == SDK_VIDEO_MEDIA_AUDIO_MP2 ||
	       audio_codec == SDK_VIDEO_MEDIA_AUDIO_LPCM ||
	       audio_codec == SDK_VIDEO_MEDIA_AUDIO_AC3;
}

static int mpeg2_configure_media(void *decoder,
                                 const struct SDKVideoMediaConfig *config)
{
	struct sdk_video_mpeg2 *d = (struct sdk_video_mpeg2 *)decoder;

	if (!d || !config || d->media_configured)
		return 0;
	if (!mpeg2_audio_ok(config->audio_codec))
		return 0;
	if (!config->pcm_ring || config->pcm_ring_capacity == 0U ||
	    config->pcm_ring_capacity > SDK_VIDEO_MEDIA_MAX_PCM_RING ||
	    config->pcm_low_water_bytes >= config->pcm_ring_capacity)
		return 0;
	/* AC-3 decodes whole frames only: a ring that cannot hold one would
	 * never emit audio and never drain. */
	if (config->audio_codec == SDK_VIDEO_MEDIA_AUDIO_AC3 &&
	    config->pcm_ring_capacity < SDK_DVD_AC3_FRAME_PCM_BYTES)
		return 0;
	d->media = *config;
	d->media_configured = 1U;
	if (config->audio_codec == SDK_VIDEO_MEDIA_AUDIO_MP2) {
		d->mp2_input = plm_buffer_create_with_capacity(
			M2_MP2_CAPACITY);
		if (!d->mp2_input)
			return 0;
		d->mp2 = plm_audio_create_with_buffer(d->mp2_input, 0);
		if (!d->mp2) {
			plm_buffer_destroy(d->mp2_input);
			d->mp2_input = 0;
			return 0;
		}
	}
	return 1;
}

static int mpeg2_get_media_info(void *decoder,
                                struct SDKVideoMediaInfo *info)
{
	struct sdk_video_mpeg2 *d = (struct sdk_video_mpeg2 *)decoder;

	if (!d || !info || !d->media_configured)
		return 0;
	memset(info, 0, sizeof(*info));
	info->sample_format = SDK_VIDEO_MEDIA_SAMPLE_S16BE;
	if (d->mp2_selected) {
		if (plm_audio_has_header(d->mp2)) {
			info->sample_rate =
				(uint32_t)plm_audio_get_samplerate(d->mp2);
			info->channels = 2U;
		}
	} else if (d->lpcm_selected) {
		info->sample_rate = SDK_DVD_LPCM_SAMPLE_RATE_HZ;
		info->channels = 2U;
	} else if (d->ac3_selected) {
		info->sample_rate = d->ac3.sample_rate;
		info->channels = 2U;
	}
	info->pcm_produced = d->pcm_produced;
	info->pcm_acknowledged = d->pcm_acknowledged;
	info->audio_pts = d->last_audio_pts;
	info->current_audio_pts = d->last_audio_pts;
	info->first_audio_pts = d->first_audio_pts;
	info->pts_origin = 0U;
	info->raw_pts = d->last_audio_pts;
	info->audio_frames = d->audio_frames;
	info->backpressure_events = d->backpressure_events;
	info->flags = d->media_flags;
	return 1;
}

static int mpeg2_ack_media(void *decoder, uint64_t acknowledged)
{
	struct sdk_video_mpeg2 *d = (struct sdk_video_mpeg2 *)decoder;

	if (!d || !d->media_configured)
		return 0;
	if (acknowledged > d->pcm_produced)
		return 0;
	d->pcm_acknowledged = acknowledged;
	if (d->pcm_produced - d->pcm_acknowledged <=
	    d->media.pcm_low_water_bytes) {
		d->pcm_backpressure = 0U;
		d->media_flags &= ~SDK_VIDEO_MEDIA_FLAG_BACKPRESSURE;
	}
	return 1;
}

static const struct SDKVideoDecoderOps mpeg2_ops = {
	.codec = SDK_VIDEO_CODEC_MPEG2,
	.container = SDK_VIDEO_CONTAINER_MPEG_PS,
	.name = "libmpeg2 MPEG-2/VOB",
	.create = mpeg2_create,
	.destroy = mpeg2_destroy,
	.write = mpeg2_write,
	.get_info = mpeg2_get_info,
	.decode = mpeg2_decode,
	.configure_media = mpeg2_configure_media,
	.get_media_info = mpeg2_get_media_info,
	.ack_media = mpeg2_ack_media,
};

const struct SDKVideoDecoderOps *sdk_video_mpeg2_backend_ops(void)
{
	return &mpeg2_ops;
}

#ifdef SDK_VIDEO_HOST_TEST
int sdk_video_mpeg2_test_milli_from_period(
	uint32_t frame_period, uint32_t *milli)
{
	if (!milli)
		return 0;
	*milli = milli_from_period(frame_period);
	return 1;
}
#endif
