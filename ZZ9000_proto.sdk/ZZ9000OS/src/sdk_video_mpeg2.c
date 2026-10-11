/*
 * libmpeg2 + bounded PS/VOB demux backend: MPEG-1 and MPEG-2 video up to
 * 800x600 in a Program Stream, with MP2 / DVD-LPCM / AC-3 audio.
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
/* Video moved out of the demux ahead of decoding. A Program Stream can
 * carry a picture's audio well after it (non-DVD muxers lead video by
 * 0.5 s and more); without room for that much video, the demux could
 * not reach the audio until the video was decoded that far ahead. 1 MiB
 * holds about 0.85 s at DVD's 9.8 Mbit/s and several seconds of
 * MPEG-1. */
#define M2_VIDEO_FIFO_CAPACITY (1024U * 1024U)
#define M2_VIDEO_FIFO_WRAP 0xffffffffU
#define M2_MP2_CAPACITY (64U * 1024U)
#define M2_PTS_SLOTS 64U
/* Largest picture either codec decodes. libmpeg2 rounds the coded size up
 * to whole macroblocks (16 lines, 32 for interlaced MPEG-2), so the frame
 * buffers can be up to 800x608. */
#define M2_MAX_WIDTH 800U
#define M2_MAX_HEIGHT 600U

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

/* A video PES in the FIFO: this header, then the payload padded to 8. */
struct m2_video_entry {
	uint64_t pts;
	uint64_t dts;
	uint32_t length;
	uint32_t reserved;
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
	uint32_t next_tag;
	uint32_t width;
	uint32_t height;
	uint32_t frame_rate_milli;
	uint64_t pcm_produced;
	uint64_t pcm_acknowledged;
	uint64_t first_audio_pts;
	uint64_t last_audio_pts;
	uint64_t last_audio_pts_raw;
	uint32_t video_frames;
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
	uint8_t skip_late;           /* this decode skips B-pictures */
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
	uint64_t last_dropped;

	if (d->demux.audio_packets_dropped == d->audio_drops_routed)
		return;
	d->audio_drops_routed = d->demux.audio_packets_dropped;
	if (!d->demux.last_dropped_audio_pts_valid)
		return;
	last_dropped = d->demux.last_dropped_audio_pts;
	if (packet_pts == SDK_DVD_PS_NO_TS || packet_pts <= last_dropped) {
		/* Kept packets queued before the drop run: the gap is still
		 * ahead; resync on the later packet that crosses it. */
		d->audio_drops_routed =
			d->demux.audio_packets_dropped - 1U;
		return;
	}
	{
		uint64_t gap = packet_pts - last_dropped;

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
	static uint8_t swap[MINIMP3_MAX_SAMPLES_PER_FRAME * 2U];

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
                          const struct SDKDVDPSPacket *packet)
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
                           const struct SDKDVDPSPacket *packet)
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


/* Returns 0 when the packet must stay queued (behind a full ring, or a
 * full MPEG audio input). */
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
static void route_audio_queue(struct sdk_video_mpeg2 *d)
{
	if (d->ac3_selected && !pump_ac3(d))
		return;
	if (d->mp2_selected)
		pump_mp2(d, 0);
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

static int geometry_ok(uint32_t width, uint32_t height)
{
	return width != 0U && height != 0U &&
	       width <= M2_MAX_WIDTH && height <= M2_MAX_HEIGHT;
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

/* Advance the video clock one picture (its display fields) and let a
 * tagged picture's raw PTS anchor it, exactly like the MPEG-1 backend:
 * observing raw PTS alone freezes the reported clock for up to the 1 s
 * discontinuity window, and a player pacing against that sees the picture
 * jump in ~1 s steps. Returns the picture's media time. */
static uint64_t clock_picture(struct sdk_video_mpeg2 *d,
                              const mpeg2_picture_t *picture,
                              uint64_t *raw_pts, uint64_t *raw_dts,
                              uint32_t *timeline_flags)
{
	struct SDKMediaTimelineResult result;
	uint64_t media_pts = M2_NO_PTS;
	uint32_t fields = picture_display_fields(picture);
	uint64_t step_units = (uint64_t)(fields < 2U ? 2U : fields) * 500U;
	uint32_t per_second = d->frame_rate_milli != 0U
		? d->frame_rate_milli : 25000U;

	picture_timestamps(d, picture, raw_pts, raw_dts);
	d->video_frames++;
	if (!d->media_configured ||
	    !sdk_media_timeline_map(&d->timeline, SDK_MEDIA_TIMELINE_VIDEO,
	                            SDK_MEDIA_TIMELINE_NO_PTS, step_units,
	                            per_second, &result))
		return M2_NO_PTS;
	{
		uint64_t origin = result.origin != SDK_MEDIA_TIMELINE_NO_PTS
			? result.origin : 0U;

		/* Before the first PTS the derived clock starts at 0; the
		 * first tagged picture re-anchors it below. */
		if (result.pts >= origin)
			media_pts = result.pts - origin;
	}
	*timeline_flags |= timeline_media_flags(result.flags);
	if (*raw_pts != M2_NO_PTS &&
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
	uint32_t ms_per_frame = d->frame_rate_milli != 0U
		? (1000000U + d->frame_rate_milli / 2U) /
			d->frame_rate_milli
		: 40U;
	/* Time of this picture's slot: pictures counted before it, skipped
	 * ones included. */
	uint32_t time_millis = d->video_frames * ms_per_frame / 1000U;

	media_pts = clock_picture(d, picture, &raw_pts, &raw_dts,
	                          &timeline_flags);
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
	frame->display_fields = picture_display_fields(picture);
}

/* A picture libmpeg2 skipped (a late B-picture): its slot passes on the
 * clock, and its timeline events ride on the next frame returned. */
static void skip_picture(struct sdk_video_mpeg2 *d, const mpeg2_info_t *info)
{
	uint64_t raw_pts;
	uint64_t raw_dts;

	(void)clock_picture(d, info->display_picture, &raw_pts, &raw_dts,
	                    &d->skipped_media_flags);
}

static int picture_skipped(const mpeg2_info_t *info)
{
	return info->display_picture &&
	       (info->display_picture->flags & PIC_FLAG_SKIP) != 0U;
}

static int demux_drained(const struct sdk_video_mpeg2 *d)
{
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
		case STATE_PICTURE:
			/* Late video: B-pictures go undecoded (no picture
			 * predicts from them) until a reference picture is
			 * returned; set per picture, both fields alike. */
			mpeg2_skip(d->decoder,
			           d->skip_late && info->current_picture &&
			           (info->current_picture->flags &
			            PIC_MASK_CODING_TYPE) ==
			                   PIC_FLAG_CODING_TYPE_B);
			break;
		case STATE_SLICE:
		case STATE_SLICE_1ST:
			if (info->display_fbuf) {
				if (!d->fbufs_bound) {
					d->failed = 1U;
					return -1;
				}
				if (picture_skipped(info)) {
					skip_picture(d, info);
				} else {
					fill_frame(d, info, out_frame);
					got_frame = 1;
				}
			}
			break;
		case STATE_END:
		case STATE_INVALID_END:
			d->video_done = 1U;
			if (state == STATE_INVALID_END)
				d->malformed++;
			if (info->display_fbuf && d->fbufs_bound) {
				if (picture_skipped(info)) {
					skip_picture(d, info);
				} else {
					fill_frame(d, info, out_frame);
					got_frame = 1;
				}
			}
			return got_frame ? 1 : 0;
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
		M2_FREE(d);
		return 0;
	}
	d->es = (uint8_t *)M2_MALLOC(M2_ES_CAPACITY);
	d->video_fifo = (uint8_t *)M2_MALLOC(M2_VIDEO_FIFO_CAPACITY);
	if (!d->es || !d->video_fifo) {
		M2_FREE(d->video_fifo);
		M2_FREE(d->es);
		mpeg2_close(d->decoder);
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
	sdk_dvd_ac3_destroy(&d->ac3);
	M2_FREE(d->video_fifo);
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
	update_audio_drop_policy(d);
	sdk_dvd_ps_write(&d->demux, src, length, &used, eof);
	/* Buffering video lets the demux parse further: offer it the rest
	 * of this write as long as it takes more. */
	for (;;) {
		uint32_t more = 0U;

		buffer_video(d);
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
	if (d->demux.audio.count != 0U || d->demux.staged != 0U)
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
	if (!(d->mpeg1 ? mpeg1_audio_ok : mpeg2_audio_ok)(config->audio_codec))
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
	if (!config->pcm_ring || config->pcm_ring_capacity == 0U ||
	    config->pcm_ring_capacity > SDK_VIDEO_MEDIA_MAX_PCM_RING ||
	    config->pcm_low_water_bytes >= config->pcm_ring_capacity)
		return 0;
	/* AC-3 decodes whole frames only: a ring that cannot hold one would
	 * never emit audio and never drain. */
	if (config->audio_codec == SDK_VIDEO_MEDIA_AUDIO_AC3 &&
	    config->pcm_ring_capacity < SDK_DVD_AC3_FRAME_PCM_BYTES)
		return 0;
	if (config->audio_codec == SDK_VIDEO_MEDIA_AUDIO_MP2) {
		d->mp2_input = (uint8_t *)M2_MALLOC(M2_MP2_CAPACITY);
		if (!d->mp2_input)
			return 0;
		mp3dec_init(&d->mp2);
	}
	d->media = *config;
	d->media_configured = 1U;
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
	info->flags = d->media_flags | d->media_event_flags;
	d->media_event_flags = 0U;
	return 1;
}

/* SDK_VIDEO_DECODE_SKIP_LATE comes with every decode the client wants a
 * late picture skipped for; the next decode without it decodes all. */
static void mpeg2_set_decode_flags(void *decoder, uint32_t flags)
{
	struct sdk_video_mpeg2 *d = (struct sdk_video_mpeg2 *)decoder;

	if (d)
		d->skip_late = (flags & SDK_VIDEO_DECODE_SKIP_LATE) != 0U;
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
	if (container == SDK_VIDEO_CONTAINER_WEBM &&
	    (codec == SDK_VIDEO_CODEC_VP8 || codec == SDK_VIDEO_CODEC_VP9))
		return sdk_video_webm_ops(codec);
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
