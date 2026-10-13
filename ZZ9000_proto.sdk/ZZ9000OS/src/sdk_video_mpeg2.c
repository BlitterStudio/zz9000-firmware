/*
 * libmpeg2 + bounded PS/VOB demux backend: MPEG-1 and MPEG-2 video in a
 * Program Stream, with MP2 / DVD-LPCM / AC-3 audio.
 *
 * The backend implements the codec-neutral sdk_video_backend contract with
 * the sdk_media_timeline integer clock and an S16BE PCM ring, so media
 * sessions, YUY2 packing and the ZZPlay feed path stay codec-neutral.
 * Decoded surfaces are allocated from the card-resident decode heap and
 * stay card-local. MPEG audio uses the firmware's minimp3 decoder.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_video_backend.h"
#include "sdk_media_profile.h"
#include "sdk_media_timeline.h"
#include "sdk_dvd_ps.h"
#include "sdk_dvd_lpcm.h"
#include "sdk_dvd_ac3.h"
#include "sdk_mpeg2_neon.h"
#include "webm_parse.h"
#include "webm_window.h"

#include <stdlib.h>
#include <string.h>

#ifndef SDK_VIDEO_HOST_TEST
#include "sdk_compression.h"
#include "xil_cache.h"
#endif

#include "mpeg2.h"
#include "mp3/mp3.h"

/* One PES at a time is handed to libmpeg2 (a PES is at most 64 KiB). */
#define M2_ES_CAPACITY (64U * 1024U)
#define M2_MKV_WINDOW_CAP (WEBM_MAX_FRAME + (64U * 1024U))
#define M2_MKV_INPUT_AHEAD (256U * 1024U)
/* Video moved out of the demux ahead of decoding. A Program Stream can
 * carry a picture's audio well after it (non-DVD muxers lead video by
 * 0.5 s and more); without room for that much video, the demux could
 * not reach the audio until the video was decoded that far ahead. 1 MiB
 * holds about 0.85 s at DVD's 9.8 Mbit/s and several seconds of
 * MPEG-1. */
#define M2_VIDEO_FIFO_CAPACITY (1024U * 1024U)
#define M2_VIDEO_FIFO_WRAP 0xffffffffU
/* Compressed PES audio is moved out of the four-packet demux queue during
 * WRITE, just like video. This covers mux audio lag without decoding PCM. */
#define M2_AUDIO_FIFO_CAPACITY (256U * 1024U)
#define M2_AUDIO_FIFO_WRAP 0xffffffffU
#define M2_MP2_CAPACITY (64U * 1024U)
/* minimp3's largest stereo output is 4,608 bytes. */
#define M2_MP2_PCM_BYTES (MINIMP3_MAX_SAMPLES_PER_FRAME * 2U)
#define M2_PTS_SLOTS 64U
/* Largest visible picture either codec decodes. Keep this aligned with the
 * WebM envelope: either orientation fits a 1920-pixel side and 1920x1088
 * pixels. libmpeg2 rounds coded dimensions to 16-pixel macroblocks (and to
 * 32 lines for interlaced MPEG-2). The worst rounded 4:2:0 frame inside the
 * visible envelope is 1776x1216, or 3,239,424 bytes per frame buffer. */
#define M2_MAX_SIDE 1920U
#define M2_MAX_PIXELS (1920U * 1088U)
#define M2_MAX_CODED_PIXELS (1776U * 1216U)

#define M2_NO_PTS SDK_VIDEO_MEDIA_NO_PTS

#ifdef SDK_VIDEO_HOST_TEST
#define M2_MALLOC(size) malloc(size)
#define M2_FREE(ptr) free(ptr)
#else
/* sdk_decode_heap_alloc records the block for reclaim when it runs on
 * core 1, so these hooks must not track it again: reclaim frees every
 * slot, and a block recorded twice would be freed twice. */
static void *m2_alloc_hook(unsigned size, mpeg2_alloc_t reason)
{
	(void)reason;
	return sdk_decode_heap_alloc((size_t)size);
}

static int m2_free_hook(void *buf)
{
	sdk_decode_heap_free(buf);
	return 1;
}

#define M2_MALLOC(size) sdk_decode_heap_alloc(size)
#define M2_FREE(ptr) sdk_decode_heap_free(ptr)
#endif

struct m2_pts_slot {
	uint32_t tag;
	uint64_t pts;
	uint64_t dts;
	uint8_t used;
};

/* libmpeg2 0.5.1 recycles its four pictures[] entries. */
struct m2_picture_rate {
	const mpeg2_picture_t *picture;
	uint32_t frame_rate_milli;
};

/* A video PES in the FIFO: this header, then the payload padded to 8. */
struct m2_video_entry {
	uint64_t pts;
	uint64_t dts;
	uint32_t length;
	uint32_t reserved;
};

struct m2_audio_entry {
	uint64_t pts;
	uint64_t dts;
	uint32_t length;
	uint8_t stream_id;
	uint8_t substream_id;
	uint8_t kind;
	uint8_t header_bytes;
	uint8_t data[];
};

struct sdk_video_mpeg2 {
 	struct SDKDVDPSDemux demux;
 	mpeg2dec_t *decoder;
 	uint8_t *es;
 	uint8_t *video_fifo;         /* M2_VIDEO_FIFO_CAPACITY bytes */
 	uint32_t video_fifo_read;    /* offset of the oldest entry */
 	uint32_t video_fifo_write;   /* offset the next entry goes to */
 	uint32_t video_fifo_count;   /* entries queued */
	uint32_t fbuf_stride;
	uint8_t fbufs_bound;
	uint8_t *audio_fifo;         /* M2_AUDIO_FIFO_CAPACITY bytes */
	uint32_t audio_fifo_read;
	uint32_t audio_fifo_write;
	uint32_t audio_fifo_count;
	uint8_t *mp2_input;          /* M2_MP2_CAPACITY bytes of MPEG audio */
	uint32_t mp2_input_pos;      /* first unread byte */
	uint32_t mp2_input_len;      /* unread bytes */
	uint32_t mp2_rate;           /* rate of the first decoded frame */
	mp3dec_t mp2;
	struct SDKDVDLPCM lpcm;
	struct SDKDVDAC3 ac3;
	uint32_t ac3_packet_offset;  /* payload bytes of the head packet staged */
	uint32_t lpcm_packet_offset; /* payload bytes of the head packet emitted */
	uint32_t mp2_packet_offset;  /* payload bytes of the head packet written */
	struct SDKVideoMediaConfig media;
	struct SDKMediaTimeline timeline;
	struct m2_pts_slot pts_slot[M2_PTS_SLOTS];
	struct m2_picture_rate picture_rate[4];
	uint32_t next_tag;
	uint32_t width;
	uint32_t height;
	uint32_t frame_rate_milli;
	uint32_t previous_picture_rate_milli;
	uint64_t legacy_video_ticks;
	uint64_t legacy_video_remainder;
	uint32_t legacy_video_rate_milli;
	uint64_t pcm_produced;
	uint64_t pcm_acknowledged;
	uint64_t first_audio_pts;
	uint64_t last_audio_pts;
	uint64_t last_audio_pts_raw;
	uint32_t video_frames;
	uint32_t previous_picture_fields;
	uint32_t audio_frames;
	uint32_t backpressure_events;
	uint32_t malformed;
	uint32_t media_flags;        /* level bits: ready, backpressure, done */
	uint32_t media_event_flags;  /* audio timeline events, reported once */
	uint8_t audio_selected;
	uint8_t mp2_selected;
	uint8_t lpcm_selected;
	uint8_t ac3_selected;
	uint8_t media_configured;
	uint8_t input_eof;
	uint8_t video_end_signalled;
	uint8_t video_done;
	uint8_t pcm_backpressure;
	uint64_t audio_block_ack;
	uint32_t audio_block_checks;
	uint64_t stashed_audio_pts;
	uint64_t audio_clock_offset;
	uint32_t audio_drops_routed;
	uint64_t unclocked_samples;  /* audio decoded before the timeline origin */
	uint32_t unclocked_rate;
	uint8_t video_origin_seen;
	uint8_t video_seen;          /* a video PES reached libmpeg2 */
	struct webm_demux mkv_demux;
	struct webm_window mkv_win;
	uint8_t *mkv_block;
	struct webm_block mkv_audio_pending;
	uint32_t mkv_expect_codec;
	uint32_t mkv_video_track;
	uint32_t mkv_audio_track;
	uint32_t mkv_audio_offset;
	uint32_t mkv_pcm_rate;
	uint16_t mkv_pcm_channels;
	uint8_t mkv_container;
	uint8_t mkv_opened;
	uint8_t mkv_unsupported;
	uint8_t mkv_input_wanted;
	uint8_t mkv_private_pending;
	uint8_t mkv_audio_pending_valid;
	uint8_t mkv_audio_pending_observed;
	uint8_t pcmle_selected;
	uint8_t skip_late;           /* this decode skips B-pictures */
	uint8_t skip_to_intra;       /* discard dependent pictures until a replacement I */
	const mpeg2_picture_t *recovery_picture;
	uint32_t skipped_media_flags; /* timeline events of skipped pictures */
	uint8_t mpeg1;
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

	if (pending >= d->media.pcm_high_water_bytes)
		return 0U;
	return (uint32_t)(d->media.pcm_high_water_bytes - pending);
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

/* Raise the sticky backpressure flag; it releases when the client's
 * acknowledgements leave room for the largest unit a decode writes at
 * once (an AC-3 frame), or drain the ring to the low-water mark. Clients
 * decode only when they want the next picture, so a ring left to drain to
 * the low-water mark before refilling would run their audio queue down to
 * the point where they present pictures early to get audio decoded. */
#define M2_PCM_REFILL_BYTES SDK_DVD_AC3_FRAME_PCM_BYTES

static void raise_backpressure(struct sdk_video_mpeg2 *d)
{
	if (!d->pcm_backpressure)
		d->backpressure_events++;
	d->pcm_backpressure = 1U;
	d->media_flags |= SDK_VIDEO_MEDIA_FLAG_BACKPRESSURE;
}

/* Write whole stereo frames into the PCM ring. Callers check for room
 * first: what does not fit is not written. */
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
		raise_backpressure(d);
		return 0U;
	}
	if (bytes > free_bytes)
		bytes = free_bytes & ~3U;
	ring_write(d, src, bytes);
	d->media_flags |= SDK_VIDEO_MEDIA_FLAG_AUDIO_READY;
	return bytes;
}

/* Timeline transition flags in the backend's media-flag vocabulary. */
static uint32_t timeline_media_flags(uint32_t flags)
{
	uint32_t mapped = 0U;

	if ((flags & SDK_MEDIA_TIMELINE_DERIVED) != 0U)
		mapped |= SDK_VIDEO_MEDIA_FLAG_DERIVED_TIME;
	if ((flags & SDK_MEDIA_TIMELINE_DISCONTINUITY) != 0U)
		mapped |= SDK_VIDEO_MEDIA_FLAG_DISCONTINUITY;
	if ((flags & SDK_MEDIA_TIMELINE_REBASED) != 0U)
		mapped |= SDK_VIDEO_MEDIA_FLAG_REBASED;
	return mapped;
}

/* Advance the audio timeline by the units just decoded into the ring, the
 * same way the MPEG-1 backend does: the packet PTS anchors the track but
 * never moves it inside the 1 s discontinuity window, so without per-unit
 * advance the reported audio clock would stay at the first packet. */
static void advance_audio_timeline(struct sdk_video_mpeg2 *d,
                                   uint64_t samples, uint32_t rate)
{
	struct SDKMediaTimelineResult result;

	if (rate == 0U || samples == 0U)
		return;
	if (!d->timeline.have_origin) {
		/* Decoded while the first audio PTS waits for the video
		 * origin: the clock covers these once the track is anchored
		 * (clock_unclocked_audio), whatever the input chunking. */
		d->unclocked_samples += samples;
		d->unclocked_rate = rate;
		return;
	}
	if (!sdk_media_timeline_map(&d->timeline, SDK_MEDIA_TIMELINE_AUDIO,
	                            SDK_MEDIA_TIMELINE_NO_PTS, samples,
	                            rate, &result))
		return;
	if (result.pts >= result.origin)
		d->last_audio_pts = result.pts - result.origin;
	else
		d->last_audio_pts = 0U;
}

static void clock_unclocked_audio(struct sdk_video_mpeg2 *d)
{
	uint64_t samples = d->unclocked_samples;

	if (samples == 0U || !d->timeline.have_origin)
		return;
	d->unclocked_samples = 0U;
	advance_audio_timeline(d, samples, d->unclocked_rate);
}

/* Dropped audio shortened the track by a real span of program time. When
 * the first packet after the drop run is routed, move the audio clock to
 * that packet's content time and fold the skipped span into the reported
 * first audio PTS, so clients deriving clock = first + played stay glued
 * to the content that actually sounds. */
static void resync_audio_after_drops(struct sdk_video_mpeg2 *d,
                                      uint64_t packet_pts)
{
	uint64_t first_dropped;
	uint64_t last_dropped;

	if (d->demux.audio_packets_dropped == d->audio_drops_routed)
		return;
	d->audio_drops_routed = d->demux.audio_packets_dropped;
	if (!d->demux.first_dropped_audio_pts_valid ||
	    !d->demux.last_dropped_audio_pts_valid)
		return;
	first_dropped = d->demux.first_dropped_audio_pts;
	last_dropped = d->demux.last_dropped_audio_pts;
	if (packet_pts == SDK_DVD_PS_NO_TS || packet_pts <= last_dropped) {
		/* Kept packets queued before the drop run: the gap is still
		 * ahead; resync on the later packet that crosses it. */
		d->audio_drops_routed =
			d->demux.audio_packets_dropped - 1U;
		return;
	}
	{
		uint64_t gap = packet_pts - first_dropped;

		if (gap > SDK_MEDIA_TIMELINE_PTS_MODULUS / 2U)
			return;
		advance_audio_timeline(d, gap * 48000U / 90000U, 48000U);
		d->audio_clock_offset += gap;
		d->media_event_flags |= SDK_VIDEO_MEDIA_FLAG_DISCONTINUITY |
		                        SDK_VIDEO_MEDIA_FLAG_REBASED;
	}
}
/* ---------------------------------------------------------------- audio */

/* minimp3's frame header size: a complete frame followed by fewer bytes
 * than this cannot be confirmed against the next header. */
#define M2_MPA_HEADER_BYTES 4U

/* minimp3's hdr_valid: sync, a known version and layer, and a usable
 * bitrate and sample-rate index. */
static int mpa_header_valid(const uint8_t *h)
{
	return h[0] == 0xffU &&
	       ((h[1] & 0xf0U) == 0xf0U || (h[1] & 0xfeU) == 0xe2U) &&
	       ((h[1] >> 1) & 3U) != 0U &&
	       (h[2] >> 4) != 15U &&
	       ((h[2] >> 2) & 3U) != 3U;
}

/* Decode MPEG audio frames while the ring has room for a whole one: a
 * frame decoded into less room would lose its tail. Until the input ends
 * only a frame minimp3 confirms complete is decoded: it would skip a frame
 * split across packets as junk. At the end (and when a full input holds no
 * confirmable frame) junk up to the next frame minimp3 confirms is
 * dropped, then minimp3 gets everything and skips what is not a frame,
 * and a tail it cannot decode is dropped. */
static void pump_mp2(struct sdk_video_mpeg2 *d, int at_end)
{
	static mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
	static uint8_t swap[M2_MP2_PCM_BYTES];

	while (!d->pcm_backpressure && d->mp2_input_len != 0U) {
		const uint8_t *in = d->mp2_input + d->mp2_input_pos;
		mp3dec_frame_info_t info;
		uint32_t consumed;
		uint32_t frames;
		uint32_t i;
		int size;
		int samples;

		if (ring_free(d) < sizeof(swap)) {
			raise_backpressure(d);
			break;
		}
		if (!mp3_probe_complete_frame(&d->mp2, in,
		                              (int)d->mp2_input_len, &size)) {
			uint32_t skip;

			if (!at_end && d->mp2_input_len < M2_MP2_CAPACITY)
				break;
			/* minimp3 confirms a frame with nothing after it only
			 * at offset 0: drop the junk before such a frame. */
			for (skip = 1U; skip + M2_MPA_HEADER_BYTES <
			                d->mp2_input_len; skip++) {
				if (mpa_header_valid(in + skip) &&
				    mp3_probe_complete_frame(
					    &d->mp2, in + skip,
					    (int)(d->mp2_input_len - skip),
					    &size))
					break;
			}
			if (skip + M2_MPA_HEADER_BYTES < d->mp2_input_len) {
				d->mp2_input_pos += skip;
				d->mp2_input_len -= skip;
				continue;
			}
			size = (int)d->mp2_input_len;
		} else if ((uint32_t)size + M2_MPA_HEADER_BYTES <=
		           d->mp2_input_len) {
			/* The next header is there: let minimp3 sync on it,
			 * which also skips junk ahead of the first frame. */
			size = (int)d->mp2_input_len;
		}
		memset(&info, 0, sizeof(info));
		samples = mp3dec_decode_frame(&d->mp2, in, size, pcm, &info);
		consumed = info.frame_bytes > 0 ? (uint32_t)info.frame_bytes
		                                : 0U;
		if (consumed == 0U || consumed > d->mp2_input_len)
			consumed = d->mp2_input_len;
		d->mp2_input_pos += consumed;
		d->mp2_input_len -= consumed;
		if (d->mp2_input_len == 0U)
			d->mp2_input_pos = 0U;
		if (samples <= 0 || info.hz <= 0 ||
		    (info.channels != 1 && info.channels != 2))
			continue;
		/* The ring and the timeline run at the first frame's rate. */
		if (d->mp2_rate == 0U)
			d->mp2_rate = (uint32_t)info.hz;
		else if ((uint32_t)info.hz != d->mp2_rate)
			continue;
		frames = (uint32_t)samples;
		for (i = 0U; i < frames; i++) {
			uint16_t left = (uint16_t)pcm[i * (uint32_t)info.channels];
			uint16_t right = info.channels == 2
				? (uint16_t)pcm[i * 2U + 1U] : left;

			swap[4U * i] = (uint8_t)(left >> 8);
			swap[4U * i + 1U] = (uint8_t)left;
			swap[4U * i + 2U] = (uint8_t)(right >> 8);
			swap[4U * i + 3U] = (uint8_t)right;
		}
		ring_emit(d, swap, frames * 4U);
		advance_audio_timeline(d, frames, d->mp2_rate);
		d->audio_frames++;
	}
}

/* Append the packet to the MPEG audio input as room allows, decoding as
 * it goes: a packet near the 64 KiB PES limit never fits beside the
 * partial frame the input keeps. Returns 0 (packet stays queued, resumed
 * at mp2_packet_offset) when the input is full behind a full ring. */
static uint32_t route_mp2(struct sdk_video_mpeg2 *d,
                          const struct m2_audio_entry *packet)
{
	while (d->mp2_packet_offset < packet->length) {
		uint32_t n = packet->length - d->mp2_packet_offset;
		uint32_t room;

		if (d->mp2_input_pos != 0U) {
			memmove(d->mp2_input, d->mp2_input + d->mp2_input_pos,
			        d->mp2_input_len);
			d->mp2_input_pos = 0U;
		}
		room = M2_MP2_CAPACITY - d->mp2_input_len;
		if (room == 0U)
			return 0U;
		if (n > room)
			n = room;
		memcpy(d->mp2_input + d->mp2_input_len,
		       packet->data + d->mp2_packet_offset, n);
		d->mp2_input_len += n;
		d->mp2_packet_offset += n;
		pump_mp2(d, 0);
	}
	d->mp2_packet_offset = 0U;
	return 1U;
}

/* Copy the packet's whole PCM frames into the ring as room allows, mono
 * doubled to stereo. Returns 0 (packet stays queued, resumed at
 * lpcm_packet_offset) when the ring is full. */
static uint32_t route_lpcm(struct sdk_video_mpeg2 *d,
                           const struct m2_audio_entry *packet)
{
	static uint8_t pcm[4096U];
	const uint8_t *payload = packet->data + SDK_DVD_LPCM_HEADER_BYTES;
	uint32_t frame_bytes;
	uint32_t length;

	if (packet->length <= SDK_DVD_LPCM_HEADER_BYTES ||
	    !sdk_dvd_lpcm_header(&d->lpcm, packet->data))
		return 1U;
	frame_bytes = 2U * d->lpcm.channels;
	length = packet->length - SDK_DVD_LPCM_HEADER_BYTES;
	length -= length % frame_bytes;
	while (d->lpcm_packet_offset < length) {
		uint32_t frames = (length - d->lpcm_packet_offset) /
		                  frame_bytes;
		uint32_t room = ring_free(d) / 4U;
		int32_t i;

		if (d->pcm_backpressure || room == 0U) {
			raise_backpressure(d);
			return 0U;
		}
		if (frames > room)
			frames = room;
		if (frames > sizeof(pcm) / 4U)
			frames = sizeof(pcm) / 4U;
		sdk_dvd_lpcm_unpack(&d->lpcm,
		                    payload + d->lpcm_packet_offset,
		                    frames * frame_bytes, pcm,
		                    frames * frame_bytes);
		/* Dual-mono expansion keeps the ring stereo; working
		 * backwards never overwrites an unread sample. */
		if (d->lpcm.channels == 1U) {
			for (i = (int32_t)frames - 1; i >= 0; i--) {
				pcm[4 * i + 3] = pcm[2 * i + 1];
				pcm[4 * i + 2] = pcm[2 * i];
				pcm[4 * i + 1] = pcm[2 * i + 1];
				pcm[4 * i]     = pcm[2 * i];
			}
		}
		ring_emit(d, pcm, frames * 4U);
		advance_audio_timeline(d, frames,
		                       SDK_DVD_LPCM_SAMPLE_RATE_HZ);
		d->lpcm_packet_offset += frames * frame_bytes;
	}
	d->lpcm_packet_offset = 0U;
	/* LPCM has no codec frames: a packet with any whole sample frame
	 * counts as one audio unit. */
	if (length != 0U)
		d->audio_frames++;
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
			raise_backpressure(d);
			return 0U;
		}
		bytes = sdk_dvd_ac3_decode(&d->ac3, pcm, sizeof(pcm));
		if (bytes != 0U) {
			ring_emit(d, pcm, bytes);
			advance_audio_timeline(
				d, bytes / 4U, d->ac3.sample_rate);
			d->audio_frames++;
		}
	}
	return 1U;
}

/* Stage the packet payload in pieces the AC-3 stage can hold, decoding as
 * it goes. Returns 0 (packet stays queued, resumed at ac3_packet_offset)
 * when the PCM ring is full. */
static uint32_t route_ac3(struct sdk_video_mpeg2 *d,
                          const struct m2_audio_entry *packet)
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


/* Returns 0 when the packet must stay queued (behind a full ring, or a
 * full MPEG audio input). */
static uint32_t route_audio(struct sdk_video_mpeg2 *d,
                            const struct m2_audio_entry *packet)
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
		/* Media-space audio timestamps, rebased by the same timeline
		 * origin as the video clock (the origin is the first VIDEO
		 * PTS; until one is seen the packet's PTS is stashed so a
		 * leading audio track cannot skew the video clock). */
		if (d->video_origin_seen) {
			struct SDKMediaTimelineResult observed;
			uint64_t media_pts = 0U;

			resync_audio_after_drops(d, packet->pts);
			if (sdk_media_timeline_observe_ordered(
				    &d->timeline, SDK_MEDIA_TIMELINE_AUDIO,
				    packet->pts, &observed)) {
				/* A jump the clock cannot absorb rebases the
				 * audio track: tell the client once. */
				d->media_event_flags |=
					timeline_media_flags(observed.flags);
				if (observed.pts >= observed.origin)
					media_pts = observed.pts -
					            observed.origin;
			}
			if (d->first_audio_pts == M2_NO_PTS)
				d->first_audio_pts = media_pts;
			d->last_audio_pts_raw = packet->pts;
		} else if (d->stashed_audio_pts == SDK_DVD_PS_NO_TS &&
		           (sdk_dvd_ps_peek_video(&d->demux) != 0 ||
		            d->video_fifo_count != 0U ||
		            !d->demux.eof || d->demux.staged != 0U)) {
			/* Video is coming (or may still be in the unparsed
			 * input); hold the first audio PTS until the video
			 * origin seeds the timeline. Later packets before that
			 * seed are deliberately not observed: the stashed first
			 * PTS anchors the track the moment the seed exists. */
			d->stashed_audio_pts = packet->pts;
		} else if (d->stashed_audio_pts == SDK_DVD_PS_NO_TS) {
			/* Audio-only stream (input ended and fully parsed, no
			 * video packet queued): audio seeds the timeline. */
			struct SDKMediaTimelineResult observed;

			if (sdk_media_timeline_observe_ordered(
				    &d->timeline, SDK_MEDIA_TIMELINE_AUDIO,
				    packet->pts, &observed) &&
			    observed.pts >= observed.origin &&
			    d->first_audio_pts == M2_NO_PTS)
				d->first_audio_pts =
					observed.pts - observed.origin;
			d->last_audio_pts_raw = packet->pts;
		}
	}
	if (packet->kind == SDK_DVD_PS_MP2)
		return route_mp2(d, packet);
	if (packet->kind == SDK_DVD_PS_LPCM)
		return route_lpcm(d, packet);
	return route_ac3(d, packet);
}

/* Decide when the demux may drop audio packets its queue cannot hold.
 * Dropping is the escape hatch that keeps video flowing while the PCM
 * ring is full, but it must never trigger while the client still
 * acknowledges PCM (a slow-but-alive sink simply waits): only a ring
 * that has stayed full across many checks with no acknowledgement
 * progress - an output that never started or has stopped - justifies
 * losing audio rather than freezing the picture. */
#define M2_AUDIO_DROP_CHECKS 64U

static void update_audio_drop_policy(struct sdk_video_mpeg2 *d)
{
	if (!d->media_configured || d->video_done || !d->pcm_backpressure ||
	    !d->audio_selected || d->pcm_acknowledged == 0U ||
	    d->demux.audio.count < SDK_DVD_PS_QUEUE_DEPTH) {
		d->audio_block_checks = 0U;
		d->demux.drop_audio_when_full = 0U;
		return;
	}
	if (d->pcm_acknowledged != d->audio_block_ack) {
		d->audio_block_ack = d->pcm_acknowledged;
		d->audio_block_checks = 0U;
		return;
	}
	if (d->audio_block_checks < M2_AUDIO_DROP_CHECKS)
		d->audio_block_checks++;
	if (d->audio_block_checks >= M2_AUDIO_DROP_CHECKS)
		d->demux.drop_audio_when_full = 1U;
}

/* Drain the audio queue: before the stream kind is selected this scans
 * for the configured kind (bounded skip of other candidate streams); after
 * selection it decodes every matching packet up to backpressure. Audio
 * left decodable behind a full ring (AC-3 frames staged, MPEG audio in its
 * input) is decoded first. */
static struct m2_audio_entry *audio_fifo_head(struct sdk_video_mpeg2 *d);
static void audio_fifo_pop(struct sdk_video_mpeg2 *d);
static void buffer_audio(struct sdk_video_mpeg2 *d);
static void route_audio_queue(struct sdk_video_mpeg2 *d)
{
	if (d->ac3_selected && !pump_ac3(d))
		return;
	if (d->mp2_selected)
		pump_mp2(d, 0);
	buffer_audio(d);
	while (!d->pcm_backpressure) {
		struct m2_audio_entry *packet = audio_fifo_head(d);

		if (!packet)
			break;
		if (!route_audio(d, packet))
			break;
		audio_fifo_pop(d);
		buffer_audio(d);
	}
}

/* --------------------------------------------------------------- video */

static int geometry_ok(uint32_t width, uint32_t height)
{
	uint32_t side;

	if (width == 0U || height == 0U)
		return 0;
	side = width > height ? width : height;
	return side <= M2_MAX_SIDE && width <= M2_MAX_PIXELS / height;
}

/* Frames report the picture size; the planes keep libmpeg2's coded
 * (macroblock-rounded) stride, and the rows below the picture are never
 * shown. */
static int bind_fbufs(struct sdk_video_mpeg2 *d,
                      const mpeg2_sequence_t *sequence)
{
	uint32_t width = sequence->picture_width;
	uint32_t height = sequence->picture_height;
	uint32_t stride = sequence->width;

	if (!geometry_ok(width, height))
		return 0;
	/* Frames are published and packed as 4:2:0; 4:2:2 and 4:4:4 MPEG-2
	 * (chroma as tall, or as wide, as luma) would be read with the wrong
	 * chroma geometry, so they are refused. */
	if (sequence->chroma_width != sequence->width / 2U ||
	    sequence->chroma_height != sequence->height / 2U)
		return 0;
	/* Sequence dimensions are macroblock-rounded. Bound them separately so a
	 * hostile header cannot make libmpeg2 allocate beyond the largest rounded
	 * frame admitted by geometry_ok(). */
	if (stride == 0U || sequence->height == 0U ||
	    stride > M2_MAX_SIDE || sequence->height > M2_MAX_SIDE ||
	    stride > M2_MAX_CODED_PIXELS / sequence->height)
		return 0;
	/* The session's display allocation is sized for its first sequence.
	 * A later geometry cannot be presented safely without renegotiation. */
	if (d->fbufs_bound)
		return d->fbuf_stride == stride && d->width == width &&
		       d->height == height;
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

/* Fields a displayed frame holds the screen for. libmpeg2 hands out a frame
 * once per frame picture or per pair of field pictures; a pair is two
 * one-field pictures, the second in display_picture_2nd. */
static uint32_t picture_display_fields(const mpeg2_info_t *info)
{
	const mpeg2_picture_t *picture = info->display_picture;
	uint32_t fields;

	if (!picture || picture->nb_fields == 0U)
		return 2U;
	fields = picture->nb_fields;
	if (info->display_picture_2nd)
		fields += info->display_picture_2nd->nb_fields;
	return fields;
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


static void picture_rate_store(struct sdk_video_mpeg2 *d,
                               const mpeg2_picture_t *picture)
{
	uint32_t i;

	if (!picture)
		return;
	for (i = 0U; i < sizeof(d->picture_rate) / sizeof(d->picture_rate[0]);
	     i++) {
		if (d->picture_rate[i].picture == picture ||
		    d->picture_rate[i].picture == 0) {
			d->picture_rate[i].picture = picture;
			d->picture_rate[i].frame_rate_milli = d->frame_rate_milli;
			return;
		}
	}
	/* The decoder has four recycled picture entries; this path is only a
	 * bounded fallback for an incompatible decoder implementation. */
	d->picture_rate[0].picture = picture;
	d->picture_rate[0].frame_rate_milli = d->frame_rate_milli;
}

static uint32_t picture_rate(const struct sdk_video_mpeg2 *d,
				     const mpeg2_picture_t *picture)
{
	uint32_t i;

	for (i = 0U; picture &&
	     i < sizeof(d->picture_rate) / sizeof(d->picture_rate[0]); i++)
		if (d->picture_rate[i].picture == picture &&
		    d->picture_rate[i].frame_rate_milli != 0U)
			return d->picture_rate[i].frame_rate_milli;
	return d->frame_rate_milli != 0U ? d->frame_rate_milli : 25000U;
}
/* Advance the video clock by the preceding picture's display fields and let a
 * tagged picture's raw PTS anchor it, exactly like the MPEG-1 backend:
 * observing raw PTS alone freezes the reported clock for up to the 1 s
 * discontinuity window, and a player pacing against that sees the picture
 * jump in ~1 s steps. Returns the picture's media time. */
static uint64_t clock_picture(struct sdk_video_mpeg2 *d,
                              const mpeg2_picture_t *picture,
                              uint32_t fields,
                              uint64_t *raw_pts, uint64_t *raw_dts,
                              uint32_t *timeline_flags)
{
	struct SDKMediaTimelineResult result;
	uint64_t media_pts = M2_NO_PTS;
	uint32_t previous_fields = d->previous_picture_fields;
	uint64_t step_units = (uint64_t)(previous_fields < 2U
		? 2U : previous_fields) * 500U;
	uint32_t per_second = d->previous_picture_rate_milli != 0U
		? d->previous_picture_rate_milli
		: (d->frame_rate_milli != 0U ? d->frame_rate_milli : 25000U);
	uint64_t scaled;

	picture_timestamps(d, picture, raw_pts, raw_dts);
	/* The slot ends after the preceding picture's duration. Keep a
	 * separate integer clock for VIDEO_SESSION's millisecond timestamp. */
	if (d->video_frames != 0U) {
		if (d->legacy_video_rate_milli != per_second) {
			d->legacy_video_remainder = 0U;
			d->legacy_video_rate_milli = per_second;
		}
		scaled = step_units * UINT64_C(90000) + d->legacy_video_remainder;
		d->legacy_video_ticks += scaled / per_second;
		d->legacy_video_remainder = scaled % per_second;
	}
	d->video_frames++;
	d->previous_picture_fields = fields;
	d->previous_picture_rate_milli = picture_rate(d, picture);
	if (!d->media_configured ||
	    !sdk_media_timeline_map(&d->timeline, SDK_MEDIA_TIMELINE_VIDEO,
	                            d->mkv_container ? *raw_pts
	                            : SDK_MEDIA_TIMELINE_NO_PTS,
	                            step_units, per_second, &result))
		return M2_NO_PTS;
	{
		uint64_t origin = result.origin != SDK_MEDIA_TIMELINE_NO_PTS
			? result.origin : 0U;

		if (result.pts >= origin)
			media_pts = result.pts - origin;
	}
	*timeline_flags |= timeline_media_flags(result.flags);
	if (!d->mkv_container && *raw_pts != M2_NO_PTS &&
	    sdk_media_timeline_observe_ordered(&d->timeline,
	                                       SDK_MEDIA_TIMELINE_VIDEO,
	                                       *raw_pts, &result)) {
		*timeline_flags |= timeline_media_flags(result.flags);
		if (result.pts >= result.origin)
			media_pts = result.pts - result.origin;
	}
	return media_pts;
}

static void fill_frame(struct sdk_video_mpeg2 *d,
                       const mpeg2_info_t *info,
                       struct SDKVideoDecodedFrame *frame)
{
	const mpeg2_picture_t *picture = info->display_picture;
	uint64_t raw_pts;
	uint64_t raw_dts;
	uint64_t media_pts;
	uint32_t timeline_flags = d->skipped_media_flags;
	uint32_t time_millis;

	media_pts = clock_picture(d, picture, picture_display_fields(info),
	                          &raw_pts, &raw_dts,
	                          &timeline_flags);
	time_millis = (uint32_t)(d->legacy_video_ticks / 90U);
	d->skipped_media_flags = 0U;
	memset(frame, 0, sizeof(*frame));
	frame->width = d->width;
	frame->height = d->height;
	frame->y_pitch = d->fbuf_stride;
	frame->chroma_pitch = d->fbuf_stride / 2U;
	frame->y = info->display_fbuf->buf[0];
	frame->cb = info->display_fbuf->buf[1];
	frame->cr = info->display_fbuf->buf[2];
	frame->time_millis = time_millis;
	frame->media_pts = media_pts;
	frame->raw_pts = raw_pts;
	frame->raw_dts = raw_dts;
	frame->media_flags = timeline_flags;
	frame->picture_flags = picture_flags_of(picture);
	frame->display_fields = picture_display_fields(info);
}

/* A picture libmpeg2 skipped (a late B-picture): its slot passes on the
 * clock, and its timeline events ride on the next frame returned. */
static void skip_picture(struct sdk_video_mpeg2 *d, const mpeg2_info_t *info)
{
	uint64_t raw_pts;
	uint64_t raw_dts;

	(void)clock_picture(d, info->display_picture,
	                    picture_display_fields(info), &raw_pts, &raw_dts,
	                    &d->skipped_media_flags);
}

static int picture_skipped(const mpeg2_info_t *info)
{
	return info->display_picture &&
	       (info->display_picture->flags & PIC_FLAG_SKIP) != 0U;
}

static int demux_drained(const struct sdk_video_mpeg2 *d)
{
	if (d->mkv_container)
		return d->input_eof && !d->mkv_audio_pending_valid;
	/* Audio still queued behind backpressure must not hold the video
	 * end hostage: the sequence-end flush has to reach libmpeg2 while
	 * queued audio waits for ring room the client may give later.
	 * Routing already ran this decode, so a non-empty audio queue here
	 * is stuck by definition. */
	return d->input_eof && d->demux.staged == 0U &&
	       d->demux.video.count == 0U && d->video_fifo_count == 0U &&
	       (d->demux.audio.count == 0U || d->pcm_backpressure);
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
			if (!bind_fbufs(d, info->sequence)) {
				d->failed = 1U;
				return -1;
			}
			d->frame_rate_milli = milli_from_period(
				info->sequence->frame_period);
			break;
		case STATE_PICTURE: {
			uint32_t coding = 0U;
			int skip = 0;

			picture_rate_store(d, info->current_picture);
			if (info->current_picture)
				coding = info->current_picture->flags &
					PIC_MASK_CODING_TYPE;
			/* Keep recovery latched until this I picture reaches display.
			 * libmpeg2 exposes it only while decoding a following reference,
			 * so that P must decode and become a valid successor. Before the I
			 * is selected, and for open-GOP B pictures after it, skip dependent
			 * pictures that would use the discarded reference chain. */
			if (d->skip_to_intra) {
				if (!d->recovery_picture) {
					if (coding == PIC_FLAG_CODING_TYPE_I)
						d->recovery_picture = info->current_picture;
					else
						skip = 1;
				} else if (coding == PIC_FLAG_CODING_TYPE_B) {
					skip = 1;
				}
			} else if (d->skip_late &&
			           coding == PIC_FLAG_CODING_TYPE_B) {
				skip = 1;
			}
			mpeg2_skip(d->decoder, skip);
			break;
		}
		case STATE_SLICE:
		case STATE_SLICE_1ST:
			if (info->display_fbuf) {
				if (!d->fbufs_bound) {
					d->failed = 1U;
					return -1;
				}
				if (picture_skipped(info) ||
				    (d->skip_to_intra &&
				     info->display_picture != d->recovery_picture)) {
					skip_picture(d, info);
				} else {
					fill_frame(d, info, out_frame);
					if (d->skip_to_intra) {
						d->skip_to_intra = 0U;
						d->recovery_picture = 0;
					}
					got_frame = 1;
				}
			}
			break;
		case STATE_END:
		case STATE_INVALID_END:
			/* A sequence end is also the boundary before another
			 * sequence. STATE_BUFFER after the EOF flush is terminal. */
			if (state == STATE_INVALID_END)
				d->malformed++;
			if (info->display_fbuf && d->fbufs_bound) {
				if (picture_skipped(info) ||
				    (d->skip_to_intra &&
				     info->display_picture != d->recovery_picture)) {
					skip_picture(d, info);
				} else {
					fill_frame(d, info, out_frame);
					if (d->skip_to_intra) {
						d->skip_to_intra = 0U;
						d->recovery_picture = 0;
					}
					got_frame = 1;
				}
			}
			if (got_frame)
				return 1;
			continue;
		case STATE_INVALID:
			d->malformed++;
			break;
		case STATE_BUFFER:
			if (d->video_end_signalled) {
				/* The end code is consumed and libmpeg2 still
				 * wants input: it reports STATE_END only after a
				 * sequence header, so a stream without video (or
				 * with no valid sequence) ends here. */
				d->video_done = 1U;
				return got_frame ? 1 : 0;
			}
			if (d->input_eof && demux_drained(d)) {
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
	d->audio_fifo_read = d->audio_fifo_write = 0U;
	d->audio_fifo_count = 0U;
	while (sdk_dvd_ps_peek_audio(&d->demux))
		sdk_dvd_ps_pop_audio(&d->demux, 0);
}

/* The media timeline origin comes from the first VIDEO PTS (like the
 * MPEG-1 seed_origin rule), so an audio track that starts earlier cannot
 * skew the video clock; the audio track follows in the timeline the moment
 * this seed exists. Video is buffered (and seeds the origin) before any
 * audio is routed, so the audio clock does not depend on how the input was
 * chunked. */
static void seed_origin(struct sdk_video_mpeg2 *d, uint64_t seed)
{
	sdk_media_timeline_seed_origin(&d->timeline, seed);
	d->video_origin_seen = 1U;
	if (d->stashed_audio_pts != SDK_DVD_PS_NO_TS) {
		struct SDKMediaTimelineResult observed;

		if (sdk_media_timeline_observe_ordered(
			    &d->timeline, SDK_MEDIA_TIMELINE_AUDIO,
			    d->stashed_audio_pts, &observed) &&
		    observed.pts >= observed.origin &&
		    d->first_audio_pts == M2_NO_PTS)
			d->first_audio_pts = observed.pts - observed.origin;
		d->stashed_audio_pts = SDK_DVD_PS_NO_TS;
	}
	clock_unclocked_audio(d);
}

/* The input ended and was parsed without a single video packet: the first
 * audio PTS, held for a video origin that never came, anchors the
 * audio-only stream itself. */
static void seed_audio_only_origin(struct sdk_video_mpeg2 *d)
{
	if (!d->video_origin_seen && !d->video_seen && d->input_eof &&
	    d->demux.staged == 0U && d->demux.video.count == 0U &&
	    d->audio_fifo_count == 0U &&
	    d->stashed_audio_pts != SDK_DVD_PS_NO_TS)
		seed_origin(d, d->stashed_audio_pts);
}

static uint32_t video_entry_bytes(uint32_t length)
{
	return (uint32_t)sizeof(struct m2_video_entry) + ((length + 7U) & ~7U);
}

/* The oldest entry, past the wrap point the writer left at the tail. */
static struct m2_video_entry *video_fifo_head(struct sdk_video_mpeg2 *d)
{
	struct m2_video_entry *entry;

	if (d->video_fifo_count == 0U)
		return 0;
	if (M2_VIDEO_FIFO_CAPACITY - d->video_fifo_read <
	    sizeof(struct m2_video_entry))
		d->video_fifo_read = 0U;
	entry = (struct m2_video_entry *)(d->video_fifo +
	                                  d->video_fifo_read);
	if (entry->length == M2_VIDEO_FIFO_WRAP) {
		d->video_fifo_read = 0U;
		entry = (struct m2_video_entry *)d->video_fifo;
	}
	return entry;
}

static void video_fifo_pop(struct sdk_video_mpeg2 *d)
{
	struct m2_video_entry *entry = video_fifo_head(d);

	if (!entry)
		return;
	d->video_fifo_read += video_entry_bytes(entry->length);
	if (--d->video_fifo_count == 0U)
		d->video_fifo_read = d->video_fifo_write = 0U;
}

/* Append a packet; 0 when the FIFO has no room for it. Entries never
 * straddle the end: one that does not fit there starts again at 0, and
 * the write offset never catches up with the read offset from behind. */
static int video_fifo_push(struct sdk_video_mpeg2 *d,
                           const struct SDKDVDPSPacket *packet)
{
	uint32_t need = video_entry_bytes(packet->length);
	uint32_t read = d->video_fifo_read;
	uint32_t write = d->video_fifo_write;
	uint32_t at;
	struct m2_video_entry *entry;

	if (d->video_fifo_count == 0U) {
		at = 0U;
	} else if (write >= read) {
		if (M2_VIDEO_FIFO_CAPACITY - write >= need) {
			at = write;
		} else if (need < read) {
			if (M2_VIDEO_FIFO_CAPACITY - write >=
			    sizeof(struct m2_video_entry))
				((struct m2_video_entry *)(d->video_fifo +
				                           write))->length =
					M2_VIDEO_FIFO_WRAP;
			at = 0U;
		} else {
			return 0;
		}
	} else if (read - write > need) {
		at = write;
	} else {
		return 0;
	}
	entry = (struct m2_video_entry *)(d->video_fifo + at);
	entry->pts = packet->pts;
	entry->dts = packet->dts;
	entry->length = packet->length;
	entry->reserved = 0U;
	memcpy(entry + 1, packet->data, packet->length);
	if (d->video_fifo_count == 0U)
		d->video_fifo_read = 0U;
	d->video_fifo_write = at + need;
	d->video_fifo_count++;
	return 1;
}

/* Move every queued video packet into the FIFO while it has room. Each pop
 * lets the demux parse further, so the audio a muxer placed after a run of
 * video reaches the audio queue without decoding that video first. */
static void buffer_video(struct sdk_video_mpeg2 *d)
{
	const struct SDKDVDPSPacket *packet;

	while ((packet = sdk_dvd_ps_peek_video(&d->demux)) != 0) {
		if (!video_fifo_push(d, packet))
			break;
		d->video_seen = 1U;
		if (!d->video_origin_seen && packet->pts != SDK_DVD_PS_NO_TS)
			seed_origin(d, packet->pts);
		sdk_dvd_ps_pop_video(&d->demux, 0);
		drop_audio(d);
	}
}
static uint32_t audio_entry_bytes(uint32_t length)
{
	return (uint32_t)sizeof(struct m2_audio_entry) + ((length + 7U) & ~7U);
}

static struct m2_audio_entry *audio_fifo_head(struct sdk_video_mpeg2 *d)
{
	struct m2_audio_entry *entry;

	if (d->audio_fifo_count == 0U)
		return 0;
	if (M2_AUDIO_FIFO_CAPACITY - d->audio_fifo_read <
	    sizeof(struct m2_audio_entry))
		d->audio_fifo_read = 0U;
	entry = (struct m2_audio_entry *)(d->audio_fifo + d->audio_fifo_read);
	if (entry->length == M2_AUDIO_FIFO_WRAP) {
		d->audio_fifo_read = 0U;
		entry = (struct m2_audio_entry *)d->audio_fifo;
	}
	return entry;
}

static void audio_fifo_pop(struct sdk_video_mpeg2 *d)
{
	struct m2_audio_entry *entry = audio_fifo_head(d);

	if (!entry)
		return;
	d->audio_fifo_read += audio_entry_bytes(entry->length);
	if (--d->audio_fifo_count == 0U)
		d->audio_fifo_read = d->audio_fifo_write = 0U;
}

static int audio_fifo_push(struct sdk_video_mpeg2 *d,
	const struct SDKDVDPSPacket *packet)
{
	uint32_t need = audio_entry_bytes(packet->length);
	uint32_t read = d->audio_fifo_read;
	uint32_t write = d->audio_fifo_write;
	uint32_t at;
	struct m2_audio_entry *entry;

	if (need > M2_AUDIO_FIFO_CAPACITY)
		return 0;
	if (d->audio_fifo_count == 0U) {
		at = 0U;
	} else if (write >= read) {
		if (M2_AUDIO_FIFO_CAPACITY - write >= need) {
			at = write;
		} else if (need < read) {
			if (M2_AUDIO_FIFO_CAPACITY - write >=
			    sizeof(struct m2_audio_entry))
				((struct m2_audio_entry *)(d->audio_fifo + write))->length =
					M2_AUDIO_FIFO_WRAP;
			at = 0U;
		} else {
			return 0;
		}
	} else if (read - write > need) {
		at = write;
	} else {
		return 0;
	}
	entry = (struct m2_audio_entry *)(d->audio_fifo + at);
	entry->pts = packet->pts;
	entry->dts = packet->dts;
	entry->length = packet->length;
	entry->stream_id = packet->stream_id;
	entry->substream_id = packet->substream_id;
	entry->kind = packet->kind;
	entry->header_bytes = packet->header_bytes;
	memcpy(entry->data, packet->data, packet->length);
	if (d->audio_fifo_count == 0U)
		d->audio_fifo_read = 0U;
	d->audio_fifo_write = at + need;
	d->audio_fifo_count++;
	return 1;
}

static void buffer_audio(struct sdk_video_mpeg2 *d)
{
	const struct SDKDVDPSPacket *packet;

	if (!d->media_configured) {
		drop_audio(d);
		return;
	}
	if (d->pcm_backpressure && d->pcm_acknowledged != 0U)
		return;
	while ((packet = sdk_dvd_ps_peek_audio(&d->demux)) != 0) {
		if (!audio_fifo_push(d, packet))
			break;
		sdk_dvd_ps_pop_audio(&d->demux, 0);
	}
}
/* Hand one buffered PES to libmpeg2 per call. mpeg2_tag_picture() latches
 * the tag onto the next picture the decoder PARSES, so a PTS-bearing PES
 * must be tagged and handed over in its own step: batching several pictures
 * into one mpeg2_buffer() would leave only the last tag set when parsing
 * runs. */
static int feed_video(struct sdk_video_mpeg2 *d)
{
	struct m2_video_entry *entry;
	uint32_t tag;

	buffer_video(d);
	/* Far-lag recovery may consume a GOP in one decode command. Keep moving
	 * audio PES and PCM while its video FIFO is being drained, rather than
	 * leaving later audio behind a bounded compressed-video queue. */
	if (d->media_configured)
		route_audio_queue(d);
	else
		drop_audio(d);
	entry = video_fifo_head(d);
	if (!entry)
		return 0;
	tag = d->next_tag++;
	if (entry->pts != SDK_DVD_PS_NO_TS ||
	    entry->dts != SDK_DVD_PS_NO_TS) {
		pts_slot_store(d, tag,
		               entry->pts == SDK_DVD_PS_NO_TS
		               ? M2_NO_PTS : entry->pts,
		               entry->dts == SDK_DVD_PS_NO_TS
		               ? M2_NO_PTS : entry->dts);
		mpeg2_tag_picture(d->decoder, tag, 0U);
	}
	/* libmpeg2 reads the buffer until it asks for more, and the FIFO
	 * entry is reused once popped: hand over a copy. */
	memcpy(d->es, entry + 1, entry->length);
	mpeg2_buffer(d->decoder, d->es, d->es + entry->length);
	video_fifo_pop(d);
	buffer_video(d);
	return 1;
}

/* ----------------------------------------------------------------- ops */

/* ----------------------------------------------------- Matroska input */

static const struct webm_track *mkv_video_track(const struct sdk_video_mpeg2 *d)
{
	unsigned i;

	for (i = 0U; i < d->mkv_demux.ntracks; i++)
		if (d->mkv_demux.tracks[i].type == 1U &&
		    d->mkv_demux.tracks[i].enabled &&
		    d->mkv_demux.tracks[i].codec == d->mkv_expect_codec)
			return &d->mkv_demux.tracks[i];
	return 0;
}

static int mkv_select_audio(struct sdk_video_mpeg2 *d)
{
	unsigned i;

	if (!d->media_configured ||
	    d->media.audio_codec == SDK_VIDEO_MEDIA_AUDIO_NONE)
		return 1;
	for (i = 0U; i < d->mkv_demux.ntracks; i++) {
		const struct webm_track *t = &d->mkv_demux.tracks[i];
		int match = (d->media.audio_codec == SDK_VIDEO_MEDIA_AUDIO_MP2 &&
		             t->codec == WEBM_CODEC_MPEG_AUDIO) ||
		            (d->media.audio_codec == SDK_VIDEO_MEDIA_AUDIO_AC3 &&
		             t->codec == WEBM_CODEC_AC3) ||
		            (d->media.audio_codec == SDK_VIDEO_MEDIA_AUDIO_LPCM &&
		             t->codec == WEBM_CODEC_PCM_LE);

		if (t->type != 2U || !t->enabled || !match)
			continue;
		if (t->codec == WEBM_CODEC_PCM_LE &&
		    (t->rate != 48000U ||
		     (t->channels != 1U && t->channels != 2U) ||
		     t->bit_depth != 16U))
			continue;
		d->mkv_audio_track = t->number;
		d->audio_selected = 1U;
		d->mp2_selected = t->codec == WEBM_CODEC_MPEG_AUDIO;
		d->ac3_selected = t->codec == WEBM_CODEC_AC3;
		d->pcmle_selected = t->codec == WEBM_CODEC_PCM_LE;
		d->mkv_pcm_rate = t->rate;
		d->mkv_pcm_channels = t->channels;
		return 1;
	}
	return 0;
}

static int mkv_finish_open(struct sdk_video_mpeg2 *d)
{
	const struct webm_track *video = mkv_video_track(d);
	const uint8_t *priv;

	if (!d->mkv_demux.is_matroska || !video ||
	    !geometry_ok(video->width, video->height))
		return 0;
	if (!mkv_select_audio(d))
		return 0;
	priv = webm_track_priv(&d->mkv_demux, video);
	if (video->priv_len != 0U && !priv)
		return 0;
	d->mkv_video_track = video->number;
	d->frame_rate_milli = webm_rate_milli(video->default_duration_ns);
	d->mkv_private_pending = video->priv_len != 0U;
	d->mkv_opened = 1U;
	webm_window_compact(&d->mkv_win);
	return 1;
}

static int mkv_try_open(struct sdk_video_mpeg2 *d)
{
	struct webm_io io;
	int rc;

	if (d->mkv_opened)
		return 1;
	d->mkv_win.cursor = d->mkv_win.start;
	webm_window_bind(&d->mkv_win, &io);
	rc = webm_open(&d->mkv_demux, &io, d->mkv_block, WEBM_MAX_FRAME);
	if (rc == 0) {
		if (mkv_finish_open(d))
			return 1;
		d->mkv_unsupported = 1U;
		return 0;
	}
	if (d->mkv_demux.error == WEBM_ERR_NEED) {
		if (webm_window_exhausted(&d->mkv_win, d->mkv_win.start))
			d->mkv_unsupported = 1U;
		return 0;
	}
	if (d->mkv_demux.error == WEBM_ERR_CODEC ||
	    d->mkv_demux.error == WEBM_ERR_LIMIT)
		d->mkv_unsupported = 1U;
	else
		d->failed = 1U;
	return 0;
}

static int mkv_next_block(struct sdk_video_mpeg2 *d, struct webm_block *blk)
{
	struct webm_demux saved;
	int rc;

	/* WEBM_NEXT_SKIPPED: input that had not arrived was dropped, so the
	 * old mark and snapshot are gone; take new ones. */
	do {
		webm_window_mark(&d->mkv_win);
		saved = d->mkv_demux;
		rc = webm_next(&d->mkv_demux, blk);
	} while (rc == WEBM_NEXT_SKIPPED);
	if (rc < 0 && d->mkv_demux.error == WEBM_ERR_NEED) {
		d->mkv_demux = saved;
		webm_window_rewind(&d->mkv_win);
		d->mkv_input_wanted = 1U;
		if (webm_window_exhausted(&d->mkv_win, d->mkv_win.mark))
			d->mkv_unsupported = 1U;
		return 0;
	}
	if (rc == 0) {
		d->input_eof = 1U;
		return 0;
	}
	if (rc < 0) {
		if (d->mkv_demux.error == WEBM_ERR_CODEC ||
		    d->mkv_demux.error == WEBM_ERR_LIMIT)
			d->mkv_unsupported = 1U;
		else
			d->failed = 1U;
		return -1;
	}
	d->mkv_input_wanted = 0U;
	return 1;
}

static void mkv_observe_audio(struct sdk_video_mpeg2 *d, uint64_t pts)
{
	struct SDKMediaTimelineResult observed;
	uint64_t media_pts = 0U;

	if (d->mkv_audio_pending_observed)
		return;
	d->mkv_audio_pending_observed = 1U;
	if (d->video_origin_seen) {
		if (sdk_media_timeline_observe_ordered(&d->timeline,
			SDK_MEDIA_TIMELINE_AUDIO, pts, &observed)) {
			d->media_event_flags |= timeline_media_flags(observed.flags);
			if (observed.pts >= observed.origin)
				media_pts = observed.pts - observed.origin;
		}
		if (d->first_audio_pts == M2_NO_PTS)
			d->first_audio_pts = media_pts;
		d->last_audio_pts_raw = pts;
	} else if (d->stashed_audio_pts == SDK_DVD_PS_NO_TS) {
		d->stashed_audio_pts = pts;
	}
}

static uint32_t mkv_route_mp2(struct sdk_video_mpeg2 *d,
	const uint8_t *data, uint32_t length)
{
	while (d->mkv_audio_offset < length) {
		uint32_t n = length - d->mkv_audio_offset;
		uint32_t room;

		if (d->mp2_input_pos != 0U) {
			memmove(d->mp2_input, d->mp2_input + d->mp2_input_pos,
				d->mp2_input_len);
			d->mp2_input_pos = 0U;
		}
		room = M2_MP2_CAPACITY - d->mp2_input_len;
		if (room == 0U)
			return 0U;
		if (n > room)
			n = room;
		memcpy(d->mp2_input + d->mp2_input_len,
		       data + d->mkv_audio_offset, n);
		d->mp2_input_len += n;
		d->mkv_audio_offset += n;
		pump_mp2(d, 0);
	}
	d->mkv_audio_offset = 0U;
	return 1U;
}

static uint32_t mkv_route_ac3(struct sdk_video_mpeg2 *d,
	const uint8_t *data, uint32_t length)
{
	while (d->mkv_audio_offset < length) {
		uint32_t taken;

		if (!pump_ac3(d))
			return 0U;
		taken = sdk_dvd_ac3_feed(&d->ac3, data + d->mkv_audio_offset,
					 length - d->mkv_audio_offset);
		if (taken == 0U) {
			d->malformed++;
			sdk_dvd_ac3_reset(&d->ac3);
			continue;
		}
		d->mkv_audio_offset += taken;
	}
	d->mkv_audio_offset = 0U;
	pump_ac3(d);
	return 1U;
}

static uint32_t mkv_route_pcmle(struct sdk_video_mpeg2 *d,
	const uint8_t *data, uint32_t length)
{
	static uint8_t pcm[4096U];
	uint32_t frame_bytes = 2U * d->mkv_pcm_channels;

	if (frame_bytes == 0U || length % frame_bytes != 0U)
		return 1U;
	while (d->mkv_audio_offset < length) {
		uint32_t frames = (length - d->mkv_audio_offset) / frame_bytes;
		uint32_t room = ring_free(d) / 4U;
		uint32_t i;

		if (d->pcm_backpressure || room == 0U) {
			raise_backpressure(d);
			return 0U;
		}
		if (frames > room)
			frames = room;
		if (frames > sizeof(pcm) / 4U)
			frames = sizeof(pcm) / 4U;
		for (i = 0U; i < frames; i++) {
			const uint8_t *in = data + d->mkv_audio_offset + i * frame_bytes;
			uint16_t left = (uint16_t)in[0] | ((uint16_t)in[1] << 8);
			uint16_t right = d->mkv_pcm_channels == 2U
				? (uint16_t)in[2] | ((uint16_t)in[3] << 8) : left;

			pcm[4U * i] = (uint8_t)(left >> 8);
			pcm[4U * i + 1U] = (uint8_t)left;
			pcm[4U * i + 2U] = (uint8_t)(right >> 8);
			pcm[4U * i + 3U] = (uint8_t)right;
		}
		ring_emit(d, pcm, frames * 4U);
		advance_audio_timeline(d, frames, 48000U);
		d->mkv_audio_offset += frames * frame_bytes;
	}
	d->mkv_audio_offset = 0U;
	d->audio_frames++;
	return 1U;
}

static const struct webm_track *mkv_audio_track(const struct sdk_video_mpeg2 *d)
{
	unsigned i;

	for (i = 0U; i < d->mkv_demux.ntracks; i++)
		if (d->mkv_demux.tracks[i].type == 2U &&
		    d->mkv_demux.tracks[i].number == d->mkv_audio_track)
			return &d->mkv_demux.tracks[i];
	return 0;
}

/* Returns one when the block has an anchor PTS, zero when a later lace
 * cannot be anchored safely, and -1 for malformed timing. Without a
 * DefaultDuration the demux gives each lace the same Block timecode. Do not
 * observe those duplicate anchors: decoding advances the audio timeline by
 * its decoded samples instead. */
static int mkv_audio_pts(const struct sdk_video_mpeg2 *d,
	const struct webm_block *blk, uint64_t *pts)
{
	const struct webm_track *audio = mkv_audio_track(d);
	uint64_t offset = 0U;

	if (!webm_pts90(blk->timecode, d->mkv_demux.scale_ns, pts))
		return -1;
	if (blk->lace_index != 0U) {
		if (!audio || audio->default_duration_ns == 0U)
			return 0;
		if (audio->default_duration_ns >
		    (UINT64_MAX - UINT64_C(500000)) /
		    (UINT64_C(90) * blk->lace_index))
			return -1;
		offset = (audio->default_duration_ns * UINT64_C(90) *
			  blk->lace_index + UINT64_C(500000)) /
			UINT64_C(1000000);
	}
	if (*pts > UINT64_MAX - offset)
		return -1;
	*pts += offset;
	return 1;
}

static int mkv_take_audio(struct sdk_video_mpeg2 *d)
{
	struct webm_block *blk = &d->mkv_audio_pending;
	uint64_t pts;
	uint32_t done;
	int have_pts;

	/* Do not anchor a block while its predecessors wait in the PCM ring.
	 * Once anchored, the block can take several calls to drain through the
	 * compressed and PCM queues; never observe that same anchor again. */
	if (d->pcm_backpressure)
		return 0;
	if (!d->mkv_audio_pending_observed) {
		have_pts = mkv_audio_pts(d, blk, &pts);
		if (have_pts < 0)
			return -1;
		if (have_pts != 0)
			mkv_observe_audio(d, pts);
		d->mkv_audio_pending_observed = 1U;
	}
	if (d->mp2_selected)
		done = mkv_route_mp2(d, blk->data, blk->size);
	else if (d->ac3_selected)
		done = mkv_route_ac3(d, blk->data, blk->size);
	else if (d->pcmle_selected)
		done = mkv_route_pcmle(d, blk->data, blk->size);
	else
		done = 1U;
	if (!done)
		return 0;
	d->mkv_audio_pending_valid = 0U;
	d->mkv_audio_pending_observed = 0U;
	return 1;
}
static int mkv_video_pts(const struct sdk_video_mpeg2 *d,
	const struct webm_block *blk, uint64_t *pts)
{
	const struct webm_track *video = mkv_video_track(d);
	uint64_t offset = 0U;
	uint32_t rate;

	if (!webm_pts90(blk->timecode, d->mkv_demux.scale_ns, pts))
		return 0;
	if (blk->lace_index == 0U)
		return 1;
	if (video && video->default_duration_ns != 0U)
		offset = (video->default_duration_ns * UINT64_C(90) *
			  blk->lace_index + UINT64_C(500000)) / UINT64_C(1000000);
	else {
		rate = d->frame_rate_milli != 0U ? d->frame_rate_milli : 25000U;
		offset = (UINT64_C(90000000) * blk->lace_index + rate / 2U) / rate;
	}
	if (*pts > UINT64_MAX - offset)
		return 0;
	*pts += offset;
	return 1;
}

static int mkv_feed_next(struct sdk_video_mpeg2 *d)
{
	struct webm_block blk;
	const uint8_t *priv;
	uint32_t tag;
	uint64_t pts;
	int rc;

	if (d->mkv_private_pending) {
		const struct webm_track *video = mkv_video_track(d);

		priv = webm_track_priv(&d->mkv_demux, video);
		mpeg2_buffer(d->decoder, (uint8_t *)(uintptr_t)priv,
			     (uint8_t *)(uintptr_t)priv + video->priv_len);
		d->mkv_private_pending = 0U;
		return 1;
	}
	if (d->mkv_audio_pending_valid)
		return mkv_take_audio(d);
	rc = mkv_next_block(d, &blk);
	if (rc <= 0)
		return rc;
	if (blk.track == d->mkv_video_track) {
		if (!mkv_video_pts(d, &blk, &pts))
			return -1;
		if (!d->video_origin_seen)
			seed_origin(d, pts);
		tag = d->next_tag++;
		pts_slot_store(d, tag, pts, M2_NO_PTS);
		mpeg2_tag_picture(d->decoder, tag, 0U);
		mpeg2_buffer(d->decoder, (uint8_t *)(uintptr_t)blk.data,
			     (uint8_t *)(uintptr_t)blk.data + blk.size);
		d->video_seen = 1U;
		return 1;
	}
	if (d->media_configured && d->mkv_audio_track != 0U &&
	    blk.track == d->mkv_audio_track) {
		d->mkv_audio_pending = blk;
		d->mkv_audio_pending_valid = 1U;
		return mkv_take_audio(d);
	}
	return 1;
}
static void *mpeg2_create(uint32_t pool_owner)
{
	struct sdk_video_mpeg2 *d;

	(void)pool_owner;
#ifndef SDK_VIDEO_HOST_TEST
	mpeg2_malloc_hooks(m2_alloc_hook, m2_free_hook);
#endif
	d = (struct sdk_video_mpeg2 *)M2_MALLOC(sizeof(struct sdk_video_mpeg2));
	if (!d)
		return 0;
	memset(d, 0, sizeof(*d));
	sdk_dvd_ps_init(&d->demux);
	d->stashed_audio_pts = SDK_DVD_PS_NO_TS;
	sdk_dvd_ac3_init(&d->ac3);
	sdk_media_timeline_init(&d->timeline);
	d->first_audio_pts = M2_NO_PTS;
	d->last_audio_pts = M2_NO_PTS;
	d->decoder = mpeg2_init();
	if (!d->decoder) {
		sdk_dvd_ac3_destroy(&d->ac3);
		M2_FREE(d);
		return 0;
	}
	/* mpeg2_init installed libmpeg2's C IDCT and motion compensation on
	 * its first call; the NEON kernels replace them once checked. */
	sdk_mpeg2_neon_install();
	d->es = (uint8_t *)M2_MALLOC(M2_ES_CAPACITY);
	d->video_fifo = (uint8_t *)M2_MALLOC(M2_VIDEO_FIFO_CAPACITY);
	d->audio_fifo = (uint8_t *)M2_MALLOC(M2_AUDIO_FIFO_CAPACITY);
	if (!d->es || !d->video_fifo || !d->audio_fifo) {
		M2_FREE(d->audio_fifo);
		M2_FREE(d->video_fifo);
		M2_FREE(d->es);
		mpeg2_close(d->decoder);
		sdk_dvd_ac3_destroy(&d->ac3);
		M2_FREE(d);
		return 0;
	}
	return d;
}

static void *mpeg1_create(uint32_t pool_owner)
{
	struct sdk_video_mpeg2 *d =
		(struct sdk_video_mpeg2 *)mpeg2_create(pool_owner);

	if (d)
		d->mpeg1 = 1U;
	return d;
}

static void mpeg2_destroy(void *decoder)
{
	struct sdk_video_mpeg2 *d = (struct sdk_video_mpeg2 *)decoder;

	if (!d)
		return;
	if (d->decoder)
		mpeg2_close(d->decoder);
	M2_FREE(d->mp2_input);
	webm_close(&d->mkv_demux);
	M2_FREE(d->mkv_block);
	M2_FREE(d->mkv_win.data);
	sdk_dvd_ac3_destroy(&d->ac3);
	M2_FREE(d->audio_fifo);
	M2_FREE(d->video_fifo);
	M2_FREE(d->es);
	M2_FREE(d);
}

static void *mkv_create(uint32_t codec, uint32_t pool_owner)
{
	struct sdk_video_mpeg2 *d = mpeg2_create(pool_owner);

	if (!d)
		return 0;
	d->mkv_block = (uint8_t *)M2_MALLOC(WEBM_MAX_FRAME);
	webm_window_init(&d->mkv_win,
		(uint8_t *)M2_MALLOC(M2_MKV_WINDOW_CAP), M2_MKV_WINDOW_CAP);
	if (!d->mkv_block || !d->mkv_win.data) {
		mpeg2_destroy(d);
		return 0;
	}
	d->mkv_container = 1U;
	d->mkv_expect_codec = codec == SDK_VIDEO_CODEC_MPEG1
		? WEBM_CODEC_MPEG1 : WEBM_CODEC_MPEG2;
	d->mpeg1 = codec == SDK_VIDEO_CODEC_MPEG1;
	return d;
}

static void *mkv_mpeg1_create(uint32_t pool_owner)
{
	return mkv_create(SDK_VIDEO_CODEC_MPEG1, pool_owner);
}

static void *mkv_mpeg2_create(uint32_t pool_owner)
{
	return mkv_create(SDK_VIDEO_CODEC_MPEG2, pool_owner);
}

static int mkv_write(void *decoder, const uint8_t *src, uint32_t length,
	int eof, uint32_t *accepted)
{
	struct sdk_video_mpeg2 *d = (struct sdk_video_mpeg2 *)decoder;
	uint32_t space;

	if (!d || d->failed || (!src && length != 0U))
		return SDK_VIDEO_BACKEND_WRITE_ERROR;
	if (d->mkv_unsupported)
		return SDK_VIDEO_BACKEND_WRITE_UNSUPPORTED;
	if (d->input_eof)
		return SDK_VIDEO_BACKEND_WRITE_ERROR;
	if (d->mkv_opened) {
		uint32_t ahead = d->mkv_win.start + d->mkv_win.filled -
			d->mkv_win.cursor;
		uint32_t room = ahead < M2_MKV_INPUT_AHEAD
			? M2_MKV_INPUT_AHEAD - ahead : 0U;

		if (length != 0U && room == 0U && !d->mkv_input_wanted)
			return SDK_VIDEO_BACKEND_WRITE_BACKPRESSURE;
		if (!d->mkv_input_wanted && length > room) {
			length = room;
			eof = 0;
		}
		if (length > webm_window_space(&d->mkv_win))
			webm_window_compact(&d->mkv_win);
	}
	space = webm_window_space(&d->mkv_win);
	if (length != 0U && space == 0U)
		return SDK_VIDEO_BACKEND_WRITE_BACKPRESSURE;
	if (length > space) {
		length = space;
		eof = 0;
	}
	(void)webm_window_append(&d->mkv_win, src, length);
	if (length != 0U)
		d->mkv_input_wanted = 0U;
	if (eof)
		d->mkv_win.eof = 1;
	if (accepted)
		*accepted = length;
	if (!d->mkv_opened)
		(void)mkv_try_open(d);
	if (d->mkv_unsupported)
		return SDK_VIDEO_BACKEND_WRITE_UNSUPPORTED;
	return d->failed ? SDK_VIDEO_BACKEND_WRITE_ERROR
			 : SDK_VIDEO_BACKEND_WRITE_OK;
}

static int audio_drained(struct sdk_video_mpeg2 *d);
static int mkv_decode(void *decoder, struct SDKVideoDecodedFrame *frame)
{
	struct sdk_video_mpeg2 *d = (struct sdk_video_mpeg2 *)decoder;
	int result;

	if (!d || !frame || d->failed)
		return SDK_VIDEO_BACKEND_ERROR;
	if (d->mkv_unsupported)
		return SDK_VIDEO_BACKEND_UNSUPPORTED;
	if (!d->mkv_opened && !mkv_try_open(d))
		return d->mkv_unsupported ? SDK_VIDEO_BACKEND_UNSUPPORTED :
			(d->failed || d->mkv_win.eof ? SDK_VIDEO_BACKEND_ERROR :
			 SDK_VIDEO_BACKEND_NEED_INPUT);
	if (d->media_configured) {
		if (d->ac3_selected && !pump_ac3(d))
			return SDK_VIDEO_BACKEND_BACKPRESSURE;
		if (d->mp2_selected)
			pump_mp2(d, d->input_eof);
	}
	for (;;) {
		result = parse_video(d, frame);
		if (result > 0)
			return SDK_VIDEO_BACKEND_FRAME;
		if (result < 0)
			return SDK_VIDEO_BACKEND_ERROR;
		result = mkv_feed_next(d);
		if (result < 0)
			return d->mkv_unsupported ? SDK_VIDEO_BACKEND_UNSUPPORTED :
				SDK_VIDEO_BACKEND_ERROR;
		if (result == 0) {
			if (d->input_eof && !d->video_done)
				continue;
			break;
		}
	}
	if (d->video_done && audio_drained(d)) {
		if ((d->video_frames == 0U && d->audio_frames == 0U) ||
		    (d->video_seen && d->video_frames == 0U))
			return SDK_VIDEO_BACKEND_ERROR;
		if (d->media_configured)
			d->media_flags |= SDK_VIDEO_MEDIA_FLAG_AUDIO_DONE;
		return SDK_VIDEO_BACKEND_DONE;
	}
	if (d->pcm_backpressure || d->mkv_audio_pending_valid)
		return SDK_VIDEO_BACKEND_BACKPRESSURE;
	return SDK_VIDEO_BACKEND_NEED_INPUT;
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
	update_audio_drop_policy(d);
	sdk_dvd_ps_write(&d->demux, src, length, &used, eof);
	/* Moving both PES kinds out of the demux lets WRITE reach the bounded
	 * compressed FIFOs without waiting for a clocked decode. */
	for (;;) {
		uint32_t more = 0U;

		buffer_video(d);
		buffer_audio(d);
		if (used == length)
			break;
		sdk_dvd_ps_write(&d->demux, src + used, length - used, &more,
		                 eof);
		if (more == 0U)
			break;
		used += more;
	}
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
	if (d->demux.audio.count != 0U || d->demux.staged != 0U ||
	    d->audio_fifo_count != 0U)
		return 0;
	if (d->mp2_selected) {
		/* Once the input has ended no later packet will pump the
		 * frames a full ring held back, and bytes short of a frame
		 * (ffmpeg's DVD MP2 ends with some) can never decode. */
		if (d->input_eof)
			pump_mp2(d, 1);
		return d->mp2_input_len == 0U;
	}
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
	update_audio_drop_policy(d);
	buffer_video(d);
	seed_audio_only_origin(d);
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
	if (d->video_done && audio_drained(d)) {
		/* A stream that ended without a single picture or audio frame
		 * (empty or unparseable), or whose video track never yielded
		 * a picture, failed. An audio-only stream plays out. */
		if ((d->video_frames == 0U && d->audio_frames == 0U) ||
		    (d->video_seen && d->video_frames == 0U))
			return SDK_VIDEO_BACKEND_ERROR;
		/* Every picture and every audio frame is decoded; PCM still
		 * in the ring plays out after DONE, as with WebM. Waiting for
		 * its acknowledgement would never end for a client that
		 * copies whole output periods until the stream is done. */
		if (d->media_configured)
			d->media_flags |= SDK_VIDEO_MEDIA_FLAG_AUDIO_DONE;
		return SDK_VIDEO_BACKEND_DONE;
	}
	if (d->input_eof && d->media_configured && !audio_drained(d)) {
		/* Audio packets are stuck behind a ring only the client can
		 * drain; video is done or starving. BUSY, not NEED_INPUT. */
		return SDK_VIDEO_BACKEND_BACKPRESSURE;
	}
	return SDK_VIDEO_BACKEND_NEED_INPUT;
}

/* DVD Program Stream audio: MP2, LPCM or AC-3, or none (a video-only or
 * muted file: the audio packets are dropped). */
static int mpeg2_audio_ok(uint32_t audio_codec)
{
	return audio_codec == SDK_VIDEO_MEDIA_AUDIO_NONE ||
	       audio_codec == SDK_VIDEO_MEDIA_AUDIO_MP2 ||
	       audio_codec == SDK_VIDEO_MEDIA_AUDIO_LPCM ||
	       audio_codec == SDK_VIDEO_MEDIA_AUDIO_AC3;
}

/* An MPEG-1 Program Stream carries MPEG audio only. */
static int mpeg1_audio_ok(uint32_t audio_codec)
{
	return audio_codec == SDK_VIDEO_MEDIA_AUDIO_NONE ||
	       audio_codec == SDK_VIDEO_MEDIA_AUDIO_MP2;
}

static int mpeg2_configure_media(void *decoder,
                                 const struct SDKVideoMediaConfig *config)
{
	struct sdk_video_mpeg2 *d = (struct sdk_video_mpeg2 *)decoder;

	if (!d || !config || d->media_configured)
		return 0;
	if (!(d->mkv_container ? mpeg2_audio_ok :
	      (d->mpeg1 ? mpeg1_audio_ok : mpeg2_audio_ok))(config->audio_codec))
		return 0;
	/* No audio: no ring, and route_audio never selects a stream, so
	 * every audio packet is dropped and the end waits on video only. */
	if (config->audio_codec == SDK_VIDEO_MEDIA_AUDIO_NONE) {
		if (config->pcm_ring || config->pcm_ring_capacity != 0U ||
		    config->pcm_low_water_bytes != 0U ||
		    config->pcm_high_water_bytes != 0U)
			return 0;
		d->media = *config;
		d->media_configured = 1U;
		return 1;
	}
	uint32_t pcm_unit = SDK_VIDEO_MEDIA_PCM_FRAME_BYTES;
	/* Decoding stops at the high-water mark, so it must hold the largest
	 * unit one decode writes at once (a whole AC-3 or MP2 frame), or the
	 * backpressure that unit raises would never release. */
	if (config->audio_codec == SDK_VIDEO_MEDIA_AUDIO_AC3)
		pcm_unit = SDK_DVD_AC3_FRAME_PCM_BYTES;
	else if (config->audio_codec == SDK_VIDEO_MEDIA_AUDIO_MP2)
		pcm_unit = M2_MP2_PCM_BYTES;
	if (!config->pcm_ring || config->pcm_ring_capacity < pcm_unit ||
	    config->pcm_ring_capacity > SDK_VIDEO_MEDIA_MAX_PCM_RING ||
	    (config->pcm_ring_capacity &
	     (SDK_VIDEO_MEDIA_PCM_FRAME_BYTES - 1U)) != 0U ||
	    config->pcm_low_water_bytes >= config->pcm_high_water_bytes ||
	    config->pcm_high_water_bytes < pcm_unit ||
	    config->pcm_high_water_bytes > config->pcm_ring_capacity ||
	    (config->pcm_low_water_bytes &
	     (SDK_VIDEO_MEDIA_PCM_FRAME_BYTES - 1U)) != 0U ||
	    (config->pcm_high_water_bytes &
	     (SDK_VIDEO_MEDIA_PCM_FRAME_BYTES - 1U)) != 0U)
		return 0;
	if (config->audio_codec == SDK_VIDEO_MEDIA_AUDIO_MP2) {
		d->mp2_input = (uint8_t *)M2_MALLOC(M2_MP2_CAPACITY);
		if (!d->mp2_input)
			return 0;
		mp3dec_init(&d->mp2);
	}
	d->media = *config;
	d->media_configured = 1U;
	if (d->mkv_opened && !mkv_select_audio(d)) {
		d->media_configured = 0U;
		M2_FREE(d->mp2_input);
		d->mp2_input = 0;
		return 0;
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
	if (d->media.audio_codec != SDK_VIDEO_MEDIA_AUDIO_NONE)
		info->sample_format = SDK_VIDEO_MEDIA_SAMPLE_S16BE;
	if (d->mp2_selected) {
		if (d->mp2_rate != 0U) {
			info->sample_rate = d->mp2_rate;
			info->channels = 2U;
		}
		} else if (d->pcmle_selected) {
			info->sample_rate = d->mkv_pcm_rate;
			info->channels = 2U;
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
	info->first_audio_pts = d->first_audio_pts == M2_NO_PTS
		? M2_NO_PTS
		: d->first_audio_pts + d->audio_clock_offset;
	info->pts_origin = 0U;
	info->raw_pts = d->last_audio_pts_raw;
	info->audio_frames = d->audio_frames;
	info->backpressure_events = d->backpressure_events;
	info->audio_packets_dropped = d->demux.audio_packets_dropped;
#ifdef SDK_VIDEO_HOST_TEST
	info->first_dropped_audio_pts =
		d->demux.first_dropped_audio_pts_valid
			? d->demux.first_dropped_audio_pts : M2_NO_PTS;
#endif
	info->flags = d->media_flags | d->media_event_flags;
	d->media_event_flags = 0U;
	return 1;
}

/* SDK_VIDEO_DECODE_SKIP_LATE is a short-lag B-picture skip. A held
 * SDK_VIDEO_DECODE_SKIP_TO_INTRA request remains latched until its replacement
 * I picture reaches display. A new request after that frame re-arms recovery. */
static void mpeg2_set_decode_flags(void *decoder, uint32_t flags)
{
	struct sdk_video_mpeg2 *d = (struct sdk_video_mpeg2 *)decoder;

	if (!d)
		return;
	d->skip_late = (flags & SDK_VIDEO_DECODE_SKIP_LATE) != 0U;
	if ((flags & SDK_VIDEO_DECODE_SKIP_TO_INTRA) != 0U) {
		if (!d->skip_to_intra) {
			d->skip_to_intra = 1U;
			d->recovery_picture = 0;
		}
	} else {
		d->skip_to_intra = 0U;
		d->recovery_picture = 0;
	}
}

static int mpeg2_ack_media(void *decoder, uint64_t acknowledged)
{
	struct sdk_video_mpeg2 *d = (struct sdk_video_mpeg2 *)decoder;

	if (!d || !d->media_configured)
		return 0;
	/* Acknowledgements only move forward, in whole stereo frames: a
	 * cursor moved back would let the decoder overwrite unplayed PCM. */
	if (acknowledged < d->pcm_acknowledged ||
	    acknowledged > d->pcm_produced ||
	    (acknowledged & (SDK_VIDEO_MEDIA_PCM_FRAME_BYTES - 1U)) != 0U)
		return 0;
	d->pcm_acknowledged = acknowledged;
	if (d->pcm_produced - d->pcm_acknowledged <=
	        d->media.pcm_low_water_bytes ||
	    ring_free(d) >= M2_PCM_REFILL_BYTES) {
		d->pcm_backpressure = 0U;
		d->media_flags &= ~SDK_VIDEO_MEDIA_FLAG_BACKPRESSURE;
	}
	if (d->pcm_produced == d->pcm_acknowledged)
		d->media_flags &= ~SDK_VIDEO_MEDIA_FLAG_AUDIO_READY;
	return 1;
}

static const struct SDKVideoDecoderOps mpeg1_ops = {
	.codec = SDK_VIDEO_CODEC_MPEG1,
	.container = SDK_VIDEO_CONTAINER_MPEG_PS,
	.name = "libmpeg2 MPEG-1/PS",
	.create = mpeg1_create,
	.destroy = mpeg2_destroy,
	.write = mpeg2_write,
	.get_info = mpeg2_get_info,
	.decode = mpeg2_decode,
	.configure_media = mpeg2_configure_media,
	.get_media_info = mpeg2_get_media_info,
	.ack_media = mpeg2_ack_media,
	.set_decode_flags = mpeg2_set_decode_flags,
	.geometry_ok = geometry_ok,
	.audio_ok = mpeg1_audio_ok,
};

static const struct SDKVideoDecoderOps mpeg2_ops = {
	.codec = SDK_VIDEO_CODEC_MPEG2,
	.container = SDK_VIDEO_CONTAINER_MPEG_PS,
	.name = "libmpeg2 MPEG-2/PS",
	.create = mpeg2_create,
	.destroy = mpeg2_destroy,
	.write = mpeg2_write,
	.get_info = mpeg2_get_info,
	.decode = mpeg2_decode,
	.configure_media = mpeg2_configure_media,
	.get_media_info = mpeg2_get_media_info,
	.ack_media = mpeg2_ack_media,
	.set_decode_flags = mpeg2_set_decode_flags,
	.geometry_ok = geometry_ok,
	.audio_ok = mpeg2_audio_ok,
};
static const struct SDKVideoDecoderOps mkv_mpeg1_ops = {
	.codec = SDK_VIDEO_CODEC_MPEG1,
	.container = SDK_VIDEO_CONTAINER_WEBM,
	.name = "libmpeg2 MPEG-1/Matroska",
	.create = mkv_mpeg1_create,
	.destroy = mpeg2_destroy,
	.write = mkv_write,
	.get_info = mpeg2_get_info,
	.decode = mkv_decode,
	.configure_media = mpeg2_configure_media,
	.get_media_info = mpeg2_get_media_info,
	.ack_media = mpeg2_ack_media,
	.set_decode_flags = mpeg2_set_decode_flags,
	.geometry_ok = geometry_ok,
	.audio_ok = mpeg2_audio_ok,
};

static const struct SDKVideoDecoderOps mkv_mpeg2_ops = {
	.codec = SDK_VIDEO_CODEC_MPEG2,
	.container = SDK_VIDEO_CONTAINER_WEBM,
	.name = "libmpeg2 MPEG-2/Matroska",
	.create = mkv_mpeg2_create,
	.destroy = mpeg2_destroy,
	.write = mkv_write,
	.get_info = mpeg2_get_info,
	.decode = mkv_decode,
	.configure_media = mpeg2_configure_media,
	.get_media_info = mpeg2_get_media_info,
	.ack_media = mpeg2_ack_media,
	.set_decode_flags = mpeg2_set_decode_flags,
	.geometry_ok = geometry_ok,
	.audio_ok = mpeg2_audio_ok,
};

const struct SDKVideoDecoderOps *sdk_video_backend_find(uint32_t codec,
	                                                     uint32_t container)
{
	if (container == SDK_VIDEO_CONTAINER_MPEG_PS) {
		if (codec == SDK_VIDEO_CODEC_MPEG1)
			return &mpeg1_ops;
		if (codec == SDK_VIDEO_CODEC_MPEG2)
			return &mpeg2_ops;
		return 0;
	}
	if (container == SDK_VIDEO_CONTAINER_WEBM) {
		if (codec == SDK_VIDEO_CODEC_MPEG1)
			return &mkv_mpeg1_ops;
		if (codec == SDK_VIDEO_CODEC_MPEG2)
			return &mkv_mpeg2_ops;
		if (codec == SDK_VIDEO_CODEC_VP8 || codec == SDK_VIDEO_CODEC_VP9)
			return sdk_video_webm_ops(codec);
	}
	return 0;
}

/* DVD host tests link this file without the WebM backend. The firmware
 * link provides the strong definition in sdk_video_webm.c. */
__attribute__((weak))
const struct SDKVideoDecoderOps *sdk_video_webm_ops(uint32_t codec)
{
	(void)codec;
	return 0;
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
