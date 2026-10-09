/*
 * WebM media-session backend: bounded demux, libvpx VP8/VP9 on core 1,
 * Opus or Tremor into the session PCM ring. Timestamps are converted to
 * the 90 kHz media clock with TimestampScale. Decoder allocations go
 * through the tracked arena (30 MB cap); compressed input is a separate
 * 4 MB window. Anything outside the tested subset fails closed.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdk_video_backend.h"
#include "webm_parse.h"
#include "webm_window.h"
#include "sdk_media_profile.h"
#include "sdk_media_timeline.h"
#include "sdk_audio_vorbis.h"

#include <string.h>

#ifndef SDK_VIDEO_HOST_TEST
#include "sdk_compression.h"
#include "sdk_decode_reclaim.h"
#include "sdk_vorbis_alloc.h"
#include "xil_cache.h"
#define WEBM_ALLOC(n) sdk_decode_heap_alloc(n)
#define WEBM_FREE(p) sdk_decode_heap_free(p)
#else
#include <stdlib.h>
#define WEBM_ALLOC(n) malloc(n)
#define WEBM_FREE(p) free(p)
#endif

#include <vpx/vpx_decoder.h>
#include <vpx/vp8dx.h>
#include <opus/opus.h>
#include "tremor/ivorbiscodec.h"
#include "tremor/codec_internal.h"

#define WEBM_WINDOW_SLACK (64U * 1024U)
#define WEBM_HEAP_REGION (8U * 1024U * 1024U)
#define WEBM_HEAP_LIMIT (30U * 1024U * 1024U)
#define WEBM_PCM_ANCHORS 128U
#define WEBM_SKIP_BUDGET 128U
#define WEBM_HELD_AUDIO (8U * 1024U)
#define WEBM_OPUS_MAX_SAMPLES 5760
#define WEBM_RATE_MIN 8000U
#define WEBM_RATE_MAX 96000U

struct webm_pcm_anchor {
	uint64_t start;
	uint64_t end;
	uint64_t pts;
};

struct sdk_video_webm {
	struct webm_demux demux;
	struct webm_window win;
	uint8_t *block;
	uint32_t expect_codec;
	uint32_t decode_flags;
	uint8_t opened;
	uint8_t failed;
	uint8_t unsupported;
	uint8_t nomem;
	uint8_t media_configured;
	uint8_t video_done;
	uint8_t audio_done;
	uint8_t vpx_ready;
	uint8_t vorbis_ready;
	uint8_t have_held_audio;
	uint32_t width;
	uint32_t height;
	uint32_t frame_rate_milli;
	uint32_t video_frames;
	uint32_t audio_frames;
	uint32_t sample_rate;
	uint32_t channels;
	uint32_t backpressure_events;
	uint32_t media_flags;
	uint32_t media_event_flags;
	uint8_t pcm_backpressure;
	uint8_t have_stashed_video;
	struct SDKVideoMediaConfig media;
	struct SDKMediaTimeline timeline;
	vpx_codec_ctx_t vpx;
	OpusDecoder *opus;
	uint32_t opus_skip_left;
	vorbis_info vi;
	vorbis_comment vc;
	vorbis_dsp_state vd;
	vorbis_block vb;
	uint64_t pcm_produced;
	uint64_t pcm_acknowledged;
	uint64_t current_audio_pts;
	uint64_t first_audio_pts;
	uint64_t audio_tail_pts;
	uint64_t last_raw_pts;
	uint64_t last_video_pts;
	struct webm_block stashed_video;
	struct webm_pcm_anchor pcm_anchor[WEBM_PCM_ANCHORS];
	uint32_t pcm_anchor_count;
	uint8_t held_audio[WEBM_HELD_AUDIO];
	uint32_t held_audio_len;
	int64_t held_audio_tc;
#ifndef SDK_VIDEO_HOST_TEST
	struct sdk_vorbis_heap heap;
	jmp_buf alloc_fail;
	uint8_t heap_inited;
#endif
};

#ifndef SDK_VIDEO_HOST_TEST
static void heap_select(struct sdk_video_webm *d)
{
	if (!d->heap_inited) {
		sdk_vorbis_heap_init(&d->heap, WEBM_HEAP_REGION, WEBM_HEAP_LIMIT);
		d->heap_inited = 1U;
	}
	sdk_vorbis_heap_select(&d->heap, &d->alloc_fail);
}

static void heap_clear(void)
{
	sdk_vorbis_heap_select(0, 0);
}
#else
/* Host tests allocate with malloc; only the deselect call sites remain. */
static void heap_clear(void)
{
}
#endif

static int books_ok(const vorbis_info *vi)
{
	const codec_setup_info *ci;
	int i;

	if (!vi || !vi->codec_setup)
		return 0;
	ci = (const codec_setup_info *)vi->codec_setup;
	for (i = 0; i < ci->books; i++) {
		const static_codebook *b = ci->book_param[i];
		long used = 0;
		long j;

		if (!b || !b->lengthlist)
			return 0;
		for (j = 0; j < b->entries; j++)
			if (b->lengthlist[j] > 0)
				used++;
		if (used > (long)SDK_VORBIS_MAX_BOOK_USED)
			return 0;
	}
	return 1;
}

static const struct webm_track *video_track(const struct sdk_video_webm *d)
{
	unsigned i;

	for (i = 0; i < d->demux.ntracks; i++)
		if (d->demux.tracks[i].type == 1)
			return &d->demux.tracks[i];
	return 0;
}

static const struct webm_track *audio_track(const struct sdk_video_webm *d)
{
	unsigned i;

	for (i = 0; i < d->demux.ntracks; i++)
		if (d->demux.tracks[i].type == 2)
			return &d->demux.tracks[i];
	return 0;
}

static int init_audio(struct sdk_video_webm *d)
{
	const struct webm_track *t = audio_track(d);
	const uint8_t *priv;
	uint32_t want = d->media.audio_codec;

	if (want == SDK_VIDEO_MEDIA_AUDIO_NONE) {
		d->audio_done = 1U;
		d->media_flags |= SDK_VIDEO_MEDIA_FLAG_AUDIO_DONE;
		return 1;
	}
	if (!t)
		return 0;
	if (want == SDK_VIDEO_MEDIA_AUDIO_OPUS && t->codec != WEBM_CODEC_OPUS)
		return 0;
	if (want == SDK_VIDEO_MEDIA_AUDIO_VORBIS &&
	    t->codec != WEBM_CODEC_VORBIS)
		return 0;
	priv = webm_track_priv(&d->demux, t);
	if (t->codec == WEBM_CODEC_OPUS) {
		int err = 0;
		int channels;

		if (!priv || t->priv_len < 19U ||
		    memcmp(priv, "OpusHead", 8) != 0 || priv[8] != 1U)
			return 0;
		channels = priv[9];
		if (channels < 1 || channels > 2 || priv[18] != 0U)
			return 0;
		d->channels = (uint32_t)channels;
		d->sample_rate = 48000U;
		d->opus_skip_left = (uint32_t)priv[10] | ((uint32_t)priv[11] << 8);
		d->opus = opus_decoder_create(48000, channels, &err);
		return d->opus != 0 && err == OPUS_OK;
	}
	{
		const uint8_t *pkt[3];
		uint32_t len[3];
		int i;

		if (!webm_xiph_split(priv, t->priv_len, pkt, len))
			return 0;
		vorbis_info_init(&d->vi);
		vorbis_comment_init(&d->vc);
		for (i = 0; i < 3; i++) {
			ogg_packet op;

			memset(&op, 0, sizeof(op));
			op.packet = (unsigned char *)(uintptr_t)pkt[i];
			op.bytes = (long)len[i];
			op.b_o_s = i == 0;
			op.packetno = i;
			if (vorbis_synthesis_headerin(&d->vi, &d->vc, &op) != 0)
				return 0;
		}
		if (d->vi.channels < 1 || d->vi.channels > 2 ||
		    d->vi.rate < (long)WEBM_RATE_MIN ||
		    d->vi.rate > (long)WEBM_RATE_MAX || !books_ok(&d->vi))
			return 0;
		if (vorbis_synthesis_init(&d->vd, &d->vi) != 0)
			return 0;
		if (vorbis_block_init(&d->vd, &d->vb) != 0)
			return 0;
		d->vorbis_ready = 1U;
		d->channels = (uint32_t)d->vi.channels;
		d->sample_rate = (uint32_t)d->vi.rate;
		return 1;
	}
}

static int init_vpx(struct sdk_video_webm *d)
{
	vpx_codec_dec_cfg_t cfg;
	vpx_codec_iface_t *iface;
	const struct webm_track *t = video_track(d);

	if (!t)
		return 0;
	memset(&cfg, 0, sizeof(cfg));
	cfg.threads = 1;
	cfg.w = t->width;
	cfg.h = t->height;
	iface = t->codec == WEBM_CODEC_VP9 ? vpx_codec_vp9_dx()
					    : vpx_codec_vp8_dx();
	if (vpx_codec_dec_init(&d->vpx, iface, &cfg, 0) != VPX_CODEC_OK)
		return 0;
	d->vpx_ready = 1U;
	d->width = t->width;
	d->height = t->height;
	d->frame_rate_milli = webm_rate_milli(t->default_duration_ns);
	return 1;
}

static int finish_open(struct sdk_video_webm *d)
{
	const struct webm_track *t = video_track(d);
	uint32_t expect = d->expect_codec == SDK_VIDEO_CODEC_VP9
		? WEBM_CODEC_VP9 : WEBM_CODEC_VP8;

	if (!t || t->codec != expect ||
	    !webm_size_allowed(t->width, t->height)) {
		d->unsupported = 1U;
		return 0;
	}
	if (!init_vpx(d) || !init_audio(d)) {
		d->unsupported = 1U;
		return 0;
	}
	d->opened = 1U;
	webm_window_compact(&d->win);
	return 1;
}

static int try_open(struct sdk_video_webm *d)
{
	struct webm_io io;
	int rc;

	if (d->opened)
		return 1;
	if (d->unsupported || d->failed)
		return 0;
	/* Until the header parses, every attempt starts from the first byte;
	 * webm_write does not compact before then. */
	d->win.cursor = d->win.start;
	webm_window_bind(&d->win, &io);
	rc = webm_open(&d->demux, &io, d->block, WEBM_MAX_FRAME);
	if (rc == 0)
		return finish_open(d);
	if (d->demux.error == WEBM_ERR_NEED)
		return 0;
	if (d->demux.error == WEBM_ERR_CODEC ||
	    d->demux.error == WEBM_ERR_LIMIT)
		d->unsupported = 1U;
	else
		d->failed = 1U;
	return 0;
}

static int pull_block(struct sdk_video_webm *d, struct webm_block *blk)
{
	struct webm_demux saved;
	int rc;

	webm_window_mark(&d->win);
	saved = d->demux;
	rc = webm_next(&d->demux, blk);
	if (rc < 0 && d->demux.error == WEBM_ERR_NEED) {
		d->demux = saved;
		webm_window_rewind(&d->win);
		return 0;
	}
	if (rc == 0) {
		d->video_done = 1U;
		if (!d->have_held_audio)
			d->audio_done = 1U;
		return 0;
	}
	if (rc < 0) {
		if (d->demux.error == WEBM_ERR_CODEC ||
		    d->demux.error == WEBM_ERR_LIMIT)
			d->unsupported = 1U;
		else
			d->failed = 1U;
		return -1;
	}
	return 1;
}

static void flush_pcm(struct sdk_video_webm *d, uint32_t offset, uint32_t bytes)
{
#ifndef SDK_VIDEO_HOST_TEST
	uint32_t first = d->media.pcm_ring_capacity - offset;

	if (first > bytes)
		first = bytes;
	Xil_DCacheFlushRange(
		(INTPTR)(uintptr_t)(d->media.pcm_ring + offset), first);
	if (bytes > first)
		Xil_DCacheFlushRange(
			(INTPTR)(uintptr_t)d->media.pcm_ring, bytes - first);
#else
	(void)d;
	(void)offset;
	(void)bytes;
#endif
}

static int pcm_room(struct sdk_video_webm *d, uint32_t bytes)
{
	uint64_t queued = d->pcm_produced - d->pcm_acknowledged;

	if (d->pcm_backpressure) {
		if (queued > d->media.pcm_low_water_bytes)
			return 0;
		d->pcm_backpressure = 0U;
		d->media_flags &= ~SDK_VIDEO_MEDIA_FLAG_BACKPRESSURE;
	}
	if (queued + bytes > d->media.pcm_high_water_bytes ||
	    queued + bytes > d->media.pcm_ring_capacity) {
		d->pcm_backpressure = 1U;
		d->media_flags |= SDK_VIDEO_MEDIA_FLAG_BACKPRESSURE;
		d->backpressure_events++;
		return 0;
	}
	return 1;
}

static void write_interleaved(struct sdk_video_webm *d, const int16_t *pcm,
			      uint32_t frames)
{
	uint32_t samples = frames * d->channels;
	uint32_t bytes = samples * 2U;
	uint32_t offset = (uint32_t)(d->pcm_produced % d->media.pcm_ring_capacity);
	uint32_t at = offset;
	uint32_t i;

	for (i = 0U; i < samples; i++) {
		uint16_t v = (uint16_t)pcm[i];

		d->media.pcm_ring[at++] = (uint8_t)(v >> 8);
		if (at == d->media.pcm_ring_capacity)
			at = 0U;
		d->media.pcm_ring[at++] = (uint8_t)v;
		if (at == d->media.pcm_ring_capacity)
			at = 0U;
	}
	flush_pcm(d, offset, bytes);
}

static int note_audio(struct sdk_video_webm *d, uint64_t raw_pts, uint32_t frames)
{
	struct SDKMediaTimelineResult timing;
	struct webm_pcm_anchor *anchor;

	if (d->pcm_anchor_count >= WEBM_PCM_ANCHORS) {
		d->failed = 1U;
		return 0;
	}
	if (!sdk_media_timeline_map(
		    &d->timeline, SDK_MEDIA_TIMELINE_AUDIO, raw_pts, frames,
		    d->sample_rate, &timing)) {
		d->failed = 1U;
		return 0;
	}
	anchor = &d->pcm_anchor[d->pcm_anchor_count++];
	anchor->start = d->pcm_produced;
	anchor->end = d->pcm_produced + (uint64_t)frames * d->channels * 2U;
	anchor->pts = timing.pts;
	d->pcm_produced = anchor->end;
	d->current_audio_pts = timing.pts;
	if (d->first_audio_pts == SDK_VIDEO_MEDIA_NO_PTS)
		d->first_audio_pts = timing.pts;
	d->last_raw_pts = timing.raw_pts;
	d->media_flags |= SDK_VIDEO_MEDIA_FLAG_AUDIO_READY;
	d->media_event_flags |=
		((timing.flags & SDK_MEDIA_TIMELINE_DERIVED)
			 ? SDK_VIDEO_MEDIA_FLAG_DERIVED_TIME : 0U) |
		((timing.flags & SDK_MEDIA_TIMELINE_DISCONTINUITY)
			 ? SDK_VIDEO_MEDIA_FLAG_DISCONTINUITY : 0U) |
		((timing.flags & SDK_MEDIA_TIMELINE_REBASED)
			 ? SDK_VIDEO_MEDIA_FLAG_REBASED : 0U);
	if (!sdk_media_timeline_peek_next(
		    &d->timeline, SDK_MEDIA_TIMELINE_AUDIO, frames,
		    d->sample_rate, &d->audio_tail_pts)) {
		d->failed = 1U;
		return 0;
	}
	d->audio_frames++;
	return 1;
}

static int emit_opus(struct sdk_video_webm *d, const uint8_t *data,
		     uint32_t size, int64_t tc)
{
	int16_t pcm[WEBM_OPUS_MAX_SAMPLES * 2];
	int samples;
	int got;
	uint32_t frames;
	uint32_t bytes;
	uint64_t raw;
	uint32_t skip;

	samples = opus_packet_get_nb_samples(data, (opus_int32)size, 48000);
	if (samples <= 0 || samples > WEBM_OPUS_MAX_SAMPLES)
		return -1;
	bytes = (uint32_t)samples * d->channels * 2U;
	if (bytes > d->media.pcm_ring_capacity)
		return -1;
	if (!pcm_room(d, bytes)) {
		if (size > WEBM_HELD_AUDIO)
			return -1;
		memcpy(d->held_audio, data, size);
		d->held_audio_len = size;
		d->held_audio_tc = tc;
		d->have_held_audio = 1U;
		return 0;
	}
	got = opus_decode(d->opus, data, (opus_int32)size, pcm, samples, 0);
	if (got < 0)
		return -1;
	frames = (uint32_t)got;
	skip = d->opus_skip_left;
	if (skip > frames)
		skip = frames;
	d->opus_skip_left -= skip;
	if (frames > skip) {
		if (!webm_pts90(tc, d->demux.scale_ns, &raw))
			return -1;
		write_interleaved(d, pcm + skip * d->channels, frames - skip);
		if (!note_audio(d, raw, frames - skip))
			return -1;
	}
	return 1;
}

static int16_t tremor_s16(ogg_int32_t v)
{
	v >>= 9;
	if (v > 32767)
		return 32767;
	if (v < -32768)
		return -32768;
	return (int16_t)v;
}

static int drain_vorbis(struct sdk_video_webm *d, uint64_t raw, int have_raw)
{
	for (;;) {
		ogg_int32_t **pcm;
		int n = vorbis_synthesis_pcmout(&d->vd, &pcm);
		uint32_t frame_bytes;
		uint64_t queued;
		uint32_t room;
		uint32_t frames;
		uint32_t i;
		uint32_t ch;
		int16_t interleaved[2048 * 2];
		uint32_t chunk;

		if (n <= 0)
			return 1;
		frame_bytes = d->channels * 2U;
		queued = d->pcm_produced - d->pcm_acknowledged;
		if (d->pcm_backpressure && queued > d->media.pcm_low_water_bytes)
			return 0;
		if (queued >= d->media.pcm_high_water_bytes ||
		    queued >= d->media.pcm_ring_capacity) {
			d->pcm_backpressure = 1U;
			d->media_flags |= SDK_VIDEO_MEDIA_FLAG_BACKPRESSURE;
			return 0;
		}
		room = d->media.pcm_high_water_bytes - (uint32_t)queued;
		if (d->media.pcm_ring_capacity - (uint32_t)queued < room)
			room = d->media.pcm_ring_capacity - (uint32_t)queued;
		frames = room / frame_bytes;
		if (frames == 0U) {
			d->pcm_backpressure = 1U;
			d->media_flags |= SDK_VIDEO_MEDIA_FLAG_BACKPRESSURE;
			return 0;
		}
		if (frames > (uint32_t)n)
			frames = (uint32_t)n;
		chunk = frames;
		if (chunk > 2048U)
			chunk = 2048U;
		for (i = 0U; i < chunk; i++)
			for (ch = 0U; ch < d->channels; ch++)
				interleaved[i * d->channels + ch] =
					tremor_s16(pcm[ch][i]);
		write_interleaved(d, interleaved, chunk);
		if (!note_audio(d, have_raw ? raw : SDK_MEDIA_TIMELINE_NO_PTS,
				chunk))
			return -1;
		have_raw = 0;
		(void)vorbis_synthesis_read(&d->vd, (int)chunk);
	}
}

static int emit_vorbis(struct sdk_video_webm *d, const uint8_t *data,
		       uint32_t size, int64_t tc)
{
	ogg_packet op;
	uint64_t raw = SDK_MEDIA_TIMELINE_NO_PTS;

	memset(&op, 0, sizeof(op));
	op.packet = (unsigned char *)(uintptr_t)data;
	op.bytes = (long)size;
	op.packetno = (ogg_int64_t)d->audio_frames;
	if (vorbis_synthesis(&d->vb, &op) != 0 ||
	    vorbis_synthesis_blockin(&d->vd, &d->vb) != 0)
		return -1;
	if (!webm_pts90(tc, d->demux.scale_ns, &raw))
		return -1;
	return drain_vorbis(d, raw, 1);
}

static int take_audio(struct sdk_video_webm *d, const struct webm_block *blk)
{
	const uint8_t *data = blk->data;
	uint32_t size = blk->size;
	int64_t tc = blk->timecode;
	int rc;

	if (d->media.audio_codec == SDK_VIDEO_MEDIA_AUDIO_NONE ||
	    blk->codec != (d->media.audio_codec == SDK_VIDEO_MEDIA_AUDIO_OPUS
				   ? WEBM_CODEC_OPUS : WEBM_CODEC_VORBIS))
		return 1;
	if (d->have_held_audio)
		return 0;
	if (d->media.audio_codec == SDK_VIDEO_MEDIA_AUDIO_OPUS)
		rc = emit_opus(d, data, size, tc);
	else
		rc = emit_vorbis(d, data, size, tc);
	if (rc < 0) {
		d->failed = 1U;
		return -1;
	}
	return rc == 0 ? 0 : 1;
}

static int fill_video(struct sdk_video_webm *d, const struct webm_block *blk,
		      struct SDKVideoDecodedFrame *out)
{
	vpx_codec_iter_t iter = 0;
	vpx_image_t *img;
	struct SDKMediaTimelineResult timing;
	uint64_t raw;
	uint32_t profile_start;
	vpx_codec_err_t err;

	if (!webm_pts90(blk->timecode, d->demux.scale_ns, &raw)) {
		d->failed = 1U;
		return -1;
	}
	profile_start = sdk_media_profile_now_us();
	err = vpx_codec_decode(&d->vpx, blk->data, blk->size, 0, 0);
	sdk_media_profile_record(SDK_MEDIA_PROFILE_VIDEO_DECODE, profile_start);
	if (err != VPX_CODEC_OK) {
		d->failed = 1U;
		return -1;
	}
	img = vpx_codec_get_frame(&d->vpx, &iter);
	if (!img) {
		/* A show-existing or invisible frame can decode without a
		 * picture. Keep pulling; the caller treats this as progress. */
		return 0;
	}
	if (img->d_w != d->width || img->d_h != d->height ||
	    img->fmt != VPX_IMG_FMT_I420) {
		d->unsupported = 1U;
		return -1;
	}
	if (!sdk_media_timeline_map(
		    &d->timeline, SDK_MEDIA_TIMELINE_VIDEO, raw, 3000U, 90000U,
		    &timing)) {
		d->failed = 1U;
		return -1;
	}
	memset(out, 0, sizeof(*out));
	out->width = d->width;
	out->height = d->height;
	out->y_pitch = (uint32_t)img->stride[0];
	out->chroma_pitch = (uint32_t)img->stride[1];
	out->y = img->planes[0];
	out->cb = img->planes[1];
	out->cr = img->planes[2];
	out->media_pts = timing.pts;
	out->raw_pts = timing.raw_pts;
	out->time_millis = (uint32_t)(timing.pts / 90U);
	out->picture_flags = SDK_VIDEO_PICTURE_FLAG_PROGRESSIVE;
	out->display_fields = 1U;
	out->media_flags =
		((timing.flags & SDK_MEDIA_TIMELINE_DERIVED)
			 ? SDK_VIDEO_MEDIA_FLAG_DERIVED_TIME : 0U) |
		((timing.flags & SDK_MEDIA_TIMELINE_DISCONTINUITY)
			 ? SDK_VIDEO_MEDIA_FLAG_DISCONTINUITY : 0U) |
		((timing.flags & SDK_MEDIA_TIMELINE_REBASED)
			 ? SDK_VIDEO_MEDIA_FLAG_REBASED : 0U);
	d->last_raw_pts = timing.raw_pts;
	d->last_video_pts = timing.pts;
	d->media_event_flags |= out->media_flags;
	d->video_frames++;
	return 1;
}

static void *webm_create(uint32_t codec)
{
	struct sdk_video_webm *d =
		(struct sdk_video_webm *)WEBM_ALLOC(sizeof(*d));

	if (!d)
		return 0;
	memset(d, 0, sizeof(*d));
	d->expect_codec = codec;
	d->first_audio_pts = SDK_VIDEO_MEDIA_NO_PTS;
	d->current_audio_pts = SDK_VIDEO_MEDIA_NO_PTS;
	d->audio_tail_pts = SDK_VIDEO_MEDIA_NO_PTS;
	d->last_raw_pts = SDK_VIDEO_MEDIA_NO_PTS;
	d->last_video_pts = SDK_VIDEO_MEDIA_NO_PTS;
	sdk_media_timeline_init(&d->timeline);
	d->block = (uint8_t *)WEBM_ALLOC(WEBM_MAX_FRAME);
	webm_window_init(&d->win,
			 (uint8_t *)WEBM_ALLOC(WEBM_MAX_FRAME + WEBM_WINDOW_SLACK),
			 WEBM_MAX_FRAME + WEBM_WINDOW_SLACK);
	if (!d->block || !d->win.data) {
		WEBM_FREE(d->block);
		WEBM_FREE(d->win.data);
		WEBM_FREE(d);
		return 0;
	}
	return d;
}

static void *create_vp8(void)
{
	return webm_create(SDK_VIDEO_CODEC_VP8);
}

static void *create_vp9(void)
{
	return webm_create(SDK_VIDEO_CODEC_VP9);
}

static void webm_destroy(void *opaque)
{
	struct sdk_video_webm *d = (struct sdk_video_webm *)opaque;

	if (!d)
		return;
#ifndef SDK_VIDEO_HOST_TEST
	if (d->heap_inited) {
		sdk_vorbis_heap_select(&d->heap, 0);
		if (d->vpx_ready)
			vpx_codec_destroy(&d->vpx);
		if (d->opus)
			opus_decoder_destroy(d->opus);
		if (d->vorbis_ready) {
			vorbis_block_clear(&d->vb);
			vorbis_dsp_clear(&d->vd);
		}
		vorbis_info_clear(&d->vi);
		vorbis_comment_clear(&d->vc);
		sdk_vorbis_heap_release(&d->heap);
		heap_clear();
	} else
#endif
	{
		if (d->vpx_ready)
			vpx_codec_destroy(&d->vpx);
		if (d->opus)
			opus_decoder_destroy(d->opus);
		if (d->vorbis_ready) {
			vorbis_block_clear(&d->vb);
			vorbis_dsp_clear(&d->vd);
			vorbis_info_clear(&d->vi);
			vorbis_comment_clear(&d->vc);
		}
	}
	webm_close(&d->demux);
	WEBM_FREE(d->block);
	WEBM_FREE(d->win.data);
	WEBM_FREE(d);
}

static int webm_write(void *opaque, const uint8_t *src, uint32_t length,
		      int eof, uint32_t *accepted)
{
	struct sdk_video_webm *d = (struct sdk_video_webm *)opaque;
#ifndef SDK_VIDEO_HOST_TEST
	int jumped;
#endif

	if (!d || !accepted || d->failed)
		return SDK_VIDEO_BACKEND_WRITE_ERROR;
	if (d->unsupported)
		return SDK_VIDEO_BACKEND_WRITE_UNSUPPORTED;
	if (length != 0U && !src)
		return SDK_VIDEO_BACKEND_WRITE_ERROR;
	if (d->opened)
		webm_window_compact(&d->win);
	if (length > webm_window_space(&d->win))
		return SDK_VIDEO_BACKEND_WRITE_BACKPRESSURE;
#ifndef SDK_VIDEO_HOST_TEST
	jumped = 0;
	if (setjmp(d->alloc_fail) != 0)
		jumped = 1;
	if (jumped) {
		d->nomem = 1U;
		d->failed = 1U;
		heap_clear();
		return SDK_VIDEO_BACKEND_WRITE_ERROR;
	}
	heap_select(d);
#endif
	if (length != 0U && !webm_window_append(&d->win, src, length)) {
		heap_clear();
		return SDK_VIDEO_BACKEND_WRITE_BACKPRESSURE;
	}
	if (eof)
		d->win.eof = 1;
	*accepted = length;
	if (!d->opened)
		(void)try_open(d);
	heap_clear();
	if (d->unsupported)
		return SDK_VIDEO_BACKEND_WRITE_UNSUPPORTED;
	if (d->failed)
		return SDK_VIDEO_BACKEND_WRITE_ERROR;
	return SDK_VIDEO_BACKEND_WRITE_OK;
}

static int webm_get_info(void *opaque, struct SDKVideoDecoderInfo *info)
{
	struct sdk_video_webm *d = (struct sdk_video_webm *)opaque;

	if (!d || !info || !d->opened)
		return 0;
	info->width = d->width;
	info->height = d->height;
	info->frame_rate_milli = d->frame_rate_milli;
	return d->width != 0U && d->height != 0U;
}

static int webm_configure_media(void *opaque, const struct SDKVideoMediaConfig *config)
{
	struct sdk_video_webm *d = (struct sdk_video_webm *)opaque;

	if (!d || !config || d->media_configured)
		return 0;
	if (config->audio_codec != SDK_VIDEO_MEDIA_AUDIO_NONE &&
	    config->audio_codec != SDK_VIDEO_MEDIA_AUDIO_OPUS &&
	    config->audio_codec != SDK_VIDEO_MEDIA_AUDIO_VORBIS)
		return 0;
	if (config->audio_codec == SDK_VIDEO_MEDIA_AUDIO_NONE) {
		if (config->pcm_ring || config->pcm_ring_capacity != 0U)
			return 0;
	} else if (!config->pcm_ring || config->pcm_ring_capacity == 0U) {
		return 0;
	}
	d->media = *config;
	d->media_configured = 1U;
	if (config->audio_codec == SDK_VIDEO_MEDIA_AUDIO_NONE) {
		d->audio_done = 1U;
		d->media_flags |= SDK_VIDEO_MEDIA_FLAG_AUDIO_DONE;
	}
	return 1;
}

static uint64_t audio_pts_at(const struct sdk_video_webm *d)
{
	uint32_t i;

	if (d->pcm_acknowledged == d->pcm_produced)
		return d->audio_tail_pts;
	for (i = 0U; i < d->pcm_anchor_count; i++) {
		const struct webm_pcm_anchor *a = &d->pcm_anchor[i];
		uint64_t frames;

		if (d->pcm_acknowledged < a->start ||
		    d->pcm_acknowledged >= a->end)
			continue;
		if (d->sample_rate == 0U || d->channels == 0U)
			return a->pts;
		frames = (d->pcm_acknowledged - a->start) / (d->channels * 2U);
		return a->pts + frames * UINT64_C(90000) / d->sample_rate;
	}
	return SDK_VIDEO_MEDIA_NO_PTS;
}

static int webm_get_media_info(void *opaque, struct SDKVideoMediaInfo *info)
{
	struct sdk_video_webm *d = (struct sdk_video_webm *)opaque;

	if (!d || !info || !d->media_configured)
		return 0;
	memset(info, 0, sizeof(*info));
	info->sample_rate = d->sample_rate;
	info->channels = d->channels;
	if (d->media.audio_codec != SDK_VIDEO_MEDIA_AUDIO_NONE)
		info->sample_format = SDK_VIDEO_MEDIA_SAMPLE_S16BE;
	info->pcm_produced = d->pcm_produced;
	info->pcm_acknowledged = d->pcm_acknowledged;
	info->audio_pts = audio_pts_at(d);
	info->current_audio_pts = d->current_audio_pts;
	info->first_audio_pts = d->first_audio_pts;
	info->pts_origin = d->timeline.have_origin
		? d->timeline.origin : SDK_VIDEO_MEDIA_NO_PTS;
	info->raw_pts = d->last_raw_pts;
	info->audio_frames = d->audio_frames;
	info->backpressure_events = d->backpressure_events;
	info->flags = d->media_flags | d->media_event_flags;
	if (d->audio_done)
		info->flags |= SDK_VIDEO_MEDIA_FLAG_AUDIO_DONE;
	d->media_event_flags = 0U;
	return 1;
}

static void prune_anchors(struct sdk_video_webm *d)
{
	uint32_t remove = 0U;

	while (remove < d->pcm_anchor_count &&
	       d->pcm_anchor[remove].end <= d->pcm_acknowledged)
		remove++;
	if (remove == 0U)
		return;
	memmove(d->pcm_anchor, d->pcm_anchor + remove,
		(d->pcm_anchor_count - remove) * sizeof(d->pcm_anchor[0]));
	d->pcm_anchor_count -= remove;
}

static int webm_ack_media(void *opaque, uint64_t acknowledged)
{
	struct sdk_video_webm *d = (struct sdk_video_webm *)opaque;
	uint32_t stride;

	if (!d || !d->media_configured ||
	    d->media.audio_codec == SDK_VIDEO_MEDIA_AUDIO_NONE)
		return acknowledged == 0U;
	stride = d->channels ? d->channels * 2U : 2U;
	if (acknowledged < d->pcm_acknowledged ||
	    acknowledged > d->pcm_produced ||
	    (stride != 0U && (acknowledged % stride) != 0U))
		return 0;
	d->pcm_acknowledged = acknowledged;
	prune_anchors(d);
	if (d->pcm_produced - d->pcm_acknowledged <=
	    d->media.pcm_low_water_bytes) {
		d->pcm_backpressure = 0U;
		d->media_flags &= ~SDK_VIDEO_MEDIA_FLAG_BACKPRESSURE;
	}
	if (d->pcm_produced == d->pcm_acknowledged)
		d->media_flags &= ~SDK_VIDEO_MEDIA_FLAG_AUDIO_READY;
	return 1;
}

static void webm_set_flags(void *opaque, uint32_t flags)
{
	struct sdk_video_webm *d = (struct sdk_video_webm *)opaque;

	if (d)
		d->decode_flags = flags;
}

static int replenish_audio(struct sdk_video_webm *d)
{
	if (d->audio_done ||
	    d->media.audio_codec == SDK_VIDEO_MEDIA_AUDIO_NONE)
		return 1;
	if (d->vorbis_ready) {
		int drained = drain_vorbis(d, 0U, 0);

		if (drained < 0)
			return -1;
		if (drained == 0)
			return 0;
	}
	while (d->pcm_produced - d->pcm_acknowledged <
	       d->media.pcm_low_water_bytes) {
		struct webm_block blk;
		int rc;

		if (d->have_held_audio) {
			rc = emit_opus(d, d->held_audio, d->held_audio_len,
				       d->held_audio_tc);
			if (rc < 0)
				return -1;
			if (rc == 0)
				return 0;
			d->have_held_audio = 0U;
			continue;
		}
		rc = pull_block(d, &blk);
		if (rc < 0)
			return -1;
		if (rc == 0)
			return 1;
		if (blk.codec == WEBM_CODEC_VP8 || blk.codec == WEBM_CODEC_VP9) {
			/* The block is already out of the demuxer and its
			 * bytes live only until the next pull, so webm_decode
			 * decodes the stashed block before pulling again. */
			d->stashed_video = blk;
			d->have_stashed_video = 1U;
			return 2;
		}
		rc = take_audio(d, &blk);
		if (rc < 0)
			return -1;
		if (rc == 0)
			return 0;
	}
	return 1;
}

static int webm_decode(void *opaque, struct SDKVideoDecodedFrame *out)
{
	struct sdk_video_webm *d = (struct sdk_video_webm *)opaque;
	struct webm_skip skip;
#ifndef SDK_VIDEO_HOST_TEST
	int jumped = 0;
#endif

	if (!d || !out || d->failed)
		return SDK_VIDEO_BACKEND_ERROR;
	if (d->unsupported)
		return SDK_VIDEO_BACKEND_UNSUPPORTED;
#ifndef SDK_VIDEO_HOST_TEST
	if (setjmp(d->alloc_fail) != 0)
		jumped = 1;
	if (jumped) {
		d->failed = 1U;
		d->nomem = 1U;
		heap_clear();
		return SDK_VIDEO_BACKEND_ERROR;
	}
	heap_select(d);
#endif
	if (!d->opened) {
		if (!try_open(d)) {
			heap_clear();
			if (d->unsupported)
				return SDK_VIDEO_BACKEND_UNSUPPORTED;
			if (d->failed)
				return SDK_VIDEO_BACKEND_ERROR;
			return d->win.eof ? SDK_VIDEO_BACKEND_ERROR
					  : SDK_VIDEO_BACKEND_NEED_INPUT;
		}
	}
	webm_skip_start(&skip, (d->decode_flags & 1U) != 0U, WEBM_SKIP_BUDGET);
	for (;;) {
		struct webm_block blk;
		int rc;
		int video;

		rc = replenish_audio(d);
		if (rc < 0) {
			heap_clear();
			return d->unsupported ? SDK_VIDEO_BACKEND_UNSUPPORTED
					      : SDK_VIDEO_BACKEND_ERROR;
		}
		if (d->have_stashed_video) {
			blk = d->stashed_video;
			d->have_stashed_video = 0U;
			video = 1;
		} else if (rc == 0) {
			heap_clear();
			return SDK_VIDEO_BACKEND_BACKPRESSURE;
		} else {
			rc = pull_block(d, &blk);
			if (rc < 0) {
				heap_clear();
				return d->unsupported
					? SDK_VIDEO_BACKEND_UNSUPPORTED
					: SDK_VIDEO_BACKEND_ERROR;
			}
			if (rc == 0) {
				heap_clear();
				if (d->video_frames == 0U && d->win.eof)
					return SDK_VIDEO_BACKEND_ERROR;
				if (d->win.eof && d->video_done && d->audio_done)
					return d->video_frames
						? SDK_VIDEO_BACKEND_DONE
						: SDK_VIDEO_BACKEND_ERROR;
				return SDK_VIDEO_BACKEND_NEED_INPUT;
			}
			video = blk.codec == WEBM_CODEC_VP8 ||
				blk.codec == WEBM_CODEC_VP9;
			if (!video) {
				int audio = take_audio(d, &blk);

				if (audio < 0) {
					heap_clear();
					return SDK_VIDEO_BACKEND_ERROR;
				}
				if (audio == 0) {
					heap_clear();
					return SDK_VIDEO_BACKEND_BACKPRESSURE;
				}
				continue;
			}
		}
		rc = webm_skip_block(&skip, &blk);
		if (rc == WEBM_SKIP_YIELD) {
			heap_clear();
			return SDK_VIDEO_BACKEND_PROGRESS;
		}
		if (rc == WEBM_SKIP_DROP)
			continue;
		rc = fill_video(d, &blk, out);
		/* No picture (invisible or show-existing frame): keep pulling
		 * with the decoder arena still selected. */
		if (rc == 0)
			continue;
		heap_clear();
		if (rc < 0)
			return d->unsupported ? SDK_VIDEO_BACKEND_UNSUPPORTED
					      : SDK_VIDEO_BACKEND_ERROR;
		return SDK_VIDEO_BACKEND_FRAME;
	}
}

static const struct SDKVideoDecoderOps webm_vp8_ops = {
	.codec = SDK_VIDEO_CODEC_VP8,
	.container = SDK_VIDEO_CONTAINER_WEBM,
	.name = "webm vp8",
	.create = create_vp8,
	.destroy = webm_destroy,
	.write = webm_write,
	.get_info = webm_get_info,
	.decode = webm_decode,
	.configure_media = webm_configure_media,
	.get_media_info = webm_get_media_info,
	.ack_media = webm_ack_media,
	.set_decode_flags = webm_set_flags,
	.geometry_ok = webm_size_allowed
};

static const struct SDKVideoDecoderOps webm_vp9_ops = {
	.codec = SDK_VIDEO_CODEC_VP9,
	.container = SDK_VIDEO_CONTAINER_WEBM,
	.name = "webm vp9",
	.create = create_vp9,
	.destroy = webm_destroy,
	.write = webm_write,
	.get_info = webm_get_info,
	.decode = webm_decode,
	.configure_media = webm_configure_media,
	.get_media_info = webm_get_media_info,
	.ack_media = webm_ack_media,
	.set_decode_flags = webm_set_flags,
	.geometry_ok = webm_size_allowed
};

const struct SDKVideoDecoderOps *sdk_video_webm_ops(uint32_t codec)
{
	if (codec == SDK_VIDEO_CODEC_VP8)
		return &webm_vp8_ops;
	if (codec == SDK_VIDEO_CODEC_VP9)
		return &webm_vp9_ops;
	return 0;
}
