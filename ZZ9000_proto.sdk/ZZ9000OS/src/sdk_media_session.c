/*
 * Additive video-first media session layered over sdk_video_stream.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_media_session.h"

#ifndef SDK_MEDIA_HOST_TEST
#include "memorymap.h"
#include <xil_cache.h>
#endif
#include "overlay.h"
#include "overlay_path.h"
#include "sdk_media_profile.h"
#include "sdk_video_yuy2.h"
#include "sdk_video_stream.h"

#include <string.h>

struct SDKMediaSessionState {
	uint32_t session;
	uint32_t state;
	uint32_t width;
	uint32_t height;
	uint32_t frame_rate_num;
	uint32_t frame_rate_den;
	/* Display fields of the last decoded frame: a repeat-first-field
	 * (3:2 pulldown) frame holds the screen for display_fields/2 frame
	 * durations, which is how long the derived PTS must advance for the
	 * frame that follows it. */
	uint32_t last_display_fields;
	uint32_t frame_number;
	uint64_t video_pts;
	uint64_t pts_origin;
	uint64_t pts_remainder;
	uint32_t bytes_accepted;
	uint32_t decoded_frames;
	uint32_t presented_frames;
	uint32_t discarded_frames;
	struct SDKVideoMediaInfo audio;
	uint8_t *pcm_ring;
	uint32_t pcm_ring_capacity;
	uint32_t audio_codec;
	/* Absolute PCM-byte cursors. The AX pump stages ahead of playback, but
	 * decoder backpressure is released only through retired_bytes so pause
	 * can discard and restage the DMA queue without losing media samples. */
	uint64_t audio_staged_bytes;
	uint64_t audio_retired_bytes;
	uint32_t audio_underruns;
	uint32_t oneshot_flags;
	uint32_t just_closed_session;
	uint16_t owner_id;
	uint8_t active;
	uint8_t held;
	uint8_t header_ready;
	uint8_t close_retired;
	uint8_t present_pending;
	uint8_t audio_bound;
	uint8_t audio_paused;
	/* SDK_VIDEO_OUTPUT_SHARED_BUFFER session. Kept after close so
	 * close_retired knows the overlay was never involved. */
	uint8_t shared;
	/* Shared-buffer output. Core 0 owns it; core 1 only sees the job. */
	struct {
		uint32_t slot0_addr;
		uint32_t slot_stride;
		uint32_t palette_addr;
		uint32_t pitch;
		uint32_t picture_bytes;
		uint32_t written;
		uint32_t presented;
		uint32_t last_us;
		uint16_t width;
		uint16_t height;
		uint16_t flags;
		uint8_t format;
		uint8_t depth;
		uint8_t slots;
		uint8_t configured;
		uint8_t in_flight;
		uint8_t complete;
		uint8_t newest_slot;
		uint8_t next_slot;
		uint8_t pending_slot;
	} output;
	struct SDKMediaSessionMainResult last_close;
};

#ifdef SDK_MEDIA_HOST_TEST
static struct SDKMediaSessionState host_media;
static struct SDKMediaSessionState *const media_state = &host_media;
#else
static struct SDKMediaSessionState *const media_state =
	(struct SDKMediaSessionState *)SDK_MEDIA_SESSION_ADDRESS;
typedef char media_session_state_fit_check[
	(sizeof(struct SDKMediaSessionState) <= SDK_MEDIA_SESSION_MAX_BYTES)
		? 1 : -1];
#endif
#define media (*media_state)

static uint32_t gcd_u32(uint32_t a, uint32_t b)
{
	while (b != 0U) {
		uint32_t next = a % b;

		a = b;
		b = next;
	}
	return a;
}

static void rate_from_milli(uint32_t milli, uint32_t *num, uint32_t *den)
{
	uint32_t divisor;

	switch (milli) {
	case 23976U:
		*num = 24000U;
		*den = 1001U;
		return;
	case 29970U:
		*num = 30000U;
		*den = 1001U;
		return;
	case 59940U:
		*num = 60000U;
		*den = 1001U;
		return;
	default:
		break;
	}
	if (milli == 0U) {
		*num = 0U;
		*den = 0U;
		return;
	}
	divisor = gcd_u32(milli, 1000U);
	*num = milli / divisor;
	*den = 1000U / divisor;
}

static uint32_t mapped_flags(uint32_t video_flags)
{
	uint32_t flags = 0U;

	if ((video_flags & SDK_VIDEO_SESSION_RESULT_HEADER_READY) != 0U)
		flags |= SDK_MEDIA_SESSION_RESULT_HEADER_READY;
	if ((video_flags & SDK_VIDEO_SESSION_RESULT_NEED_INPUT) != 0U)
		flags |= SDK_MEDIA_SESSION_RESULT_NEED_INPUT;
	if ((video_flags & SDK_VIDEO_SESSION_RESULT_FRAME_READY) != 0U)
		flags |= SDK_MEDIA_SESSION_RESULT_FRAME_HELD;
	if ((video_flags & SDK_VIDEO_SESSION_RESULT_DONE) != 0U)
		flags |= SDK_MEDIA_SESSION_RESULT_DONE;
	return flags;
}

static uint32_t mapped_media_flags(uint32_t media_flags)
{
	uint32_t flags = 0U;

	if ((media_flags & SDK_VIDEO_MEDIA_FLAG_DERIVED_TIME) != 0U)
		flags |= SDK_MEDIA_SESSION_RESULT_DERIVED_TIME;
	if ((media_flags & SDK_VIDEO_MEDIA_FLAG_DISCONTINUITY) != 0U)
		flags |= SDK_MEDIA_SESSION_RESULT_DISCONTINUITY;
	if ((media_flags & SDK_VIDEO_MEDIA_FLAG_REBASED) != 0U)
		flags |= SDK_MEDIA_SESSION_RESULT_REBASED;
	if ((media_flags & SDK_VIDEO_MEDIA_FLAG_AUDIO_READY) != 0U)
		flags |= SDK_MEDIA_SESSION_RESULT_AUDIO_READY;
	if ((media_flags & SDK_VIDEO_MEDIA_FLAG_BACKPRESSURE) != 0U)
		flags |= SDK_MEDIA_SESSION_RESULT_BACKPRESSURE;
	return flags;
}

static uint32_t mapped_state(uint32_t video_state)
{
	switch (video_state) {
	case SDK_VIDEO_SESSION_STATE_NEED_INPUT:
		return SDK_MEDIA_SESSION_STATE_NEED_INPUT;
	case SDK_VIDEO_SESSION_STATE_READY:
		return SDK_MEDIA_SESSION_STATE_READY;
	case SDK_VIDEO_SESSION_STATE_FRAME_READY:
		return SDK_MEDIA_SESSION_STATE_FRAME_HELD;
	case SDK_VIDEO_SESSION_STATE_DONE:
		return SDK_MEDIA_SESSION_STATE_DONE;
	default:
		return SDK_MEDIA_SESSION_STATE_ERROR;
	}
}

static void update_from_video(const struct SDKVideoStreamResult *video)
{
	media.width = video->width;
	media.height = video->height;
	media.frame_number = video->frame_number;
	media.bytes_accepted = video->bytes_accepted;
	rate_from_milli(video->frame_rate_milli,
	                &media.frame_rate_num, &media.frame_rate_den);
	if ((video->flags & SDK_VIDEO_SESSION_RESULT_HEADER_READY) != 0U)
		media.header_ready = 1U;
	if (video->media_pts != SDK_VIDEO_MEDIA_NO_PTS)
		media.video_pts = video->media_pts;
	if (video->media_flags != 0U)
		media.oneshot_flags |= video->media_flags;
}

static void update_audio(void)
{
	struct SDKVideoMediaInfo info;

	if (!media.active)
		return;
	if (media.audio_bound &&
	    media.audio_retired_bytes > media.audio.pcm_acknowledged) {
		/* Only DMA retirement releases decoder ring space. Staged samples
		 * remain owned by the decoder ring, so pause can rewind the staging
		 * cursor and replay them instead of skipping queued audio. */
		(void)sdk_video_stream_ack_media(
			media.session, media.audio_retired_bytes);
	}
	if (!sdk_video_stream_get_media_info(media.session, &info))
		return;
	media.audio = info;
	media.audio.flags &=
		SDK_VIDEO_MEDIA_FLAG_AUDIO_READY |
		SDK_VIDEO_MEDIA_FLAG_BACKPRESSURE |
		SDK_VIDEO_MEDIA_FLAG_AUDIO_DONE;
	media.pts_origin = info.pts_origin;
	media.oneshot_flags |= info.flags &
		(SDK_VIDEO_MEDIA_FLAG_DERIVED_TIME |
		 SDK_VIDEO_MEDIA_FLAG_DISCONTINUITY |
		 SDK_VIDEO_MEDIA_FLAG_REBASED);
}

static uint32_t audio_frame_bytes(void)
{
	return media.audio.channels * 2U;
}

static void audio_cursor_snapshot(uint64_t *retired_bytes,
	                              uint64_t *staged_bytes)
{
	volatile const struct SDKMediaSessionState *state = media_state;
	uint64_t retired_before;
	uint64_t retired_after;
	uint64_t staged_before;
	uint64_t staged_after;

	/* The audio ISR advances both 64-bit cursors on this 32-bit core. Read
	 * each twice until the pair is stable: otherwise a mailbox STATUS can
	 * combine an old retired value with a new queued calculation (or observe
	 * a torn 64-bit load), producing counters that cannot describe any real
	 * playback state. */
	do {
		retired_before = state->audio_retired_bytes;
		staged_before = state->audio_staged_bytes;
		retired_after = state->audio_retired_bytes;
		staged_after = state->audio_staged_bytes;
	} while (retired_before != retired_after ||
	         staged_before != staged_after);
	*retired_bytes = retired_after;
	*staged_bytes = staged_after;
}

static uint64_t audio_acknowledged(void)
{
	return media.audio_retired_bytes > media.audio.pcm_acknowledged
		? media.audio_retired_bytes : media.audio.pcm_acknowledged;
}

static uint64_t audio_output_pts(void)
{
	uint32_t frame_bytes = audio_frame_bytes();
	uint64_t frames;

	if (media.audio.first_audio_pts == SDK_MEDIA_NO_PTS ||
	    media.audio.sample_rate == 0U || frame_bytes == 0U)
		return SDK_MEDIA_NO_PTS;
	frames = media.audio_retired_bytes / frame_bytes;
	return media.audio.first_audio_pts +
	       (frames / media.audio.sample_rate) * UINT64_C(90000) +
	       ((frames % media.audio.sample_rate) * UINT64_C(90000)) /
	           media.audio.sample_rate;
}

static int audio_output_drained(void)
{
	return media.audio_bound &&
	       (media.audio.flags & SDK_VIDEO_MEDIA_FLAG_AUDIO_DONE) != 0U &&
	       media.audio_retired_bytes >= media.audio.pcm_produced;
}

static uint32_t audio_output_flags(void)
{
	uint32_t flags = mapped_media_flags(media.audio.flags);

	if (media.audio_bound) {
		flags |= SDK_MEDIA_SESSION_RESULT_AUDIO_BOUND;
		if (!media.audio_paused)
			flags |= SDK_MEDIA_SESSION_RESULT_AUDIO_PLAYING;
		if (audio_output_drained())
			flags |= SDK_MEDIA_SESSION_RESULT_AUDIO_DRAINED;
		if (media.audio_underruns != 0U)
			flags |= SDK_MEDIA_SESSION_RESULT_AUDIO_UNDERRUN;
	}
	return flags;
}

static void fill_audio_result(struct SDKMediaSessionAudioResult *result)
{
	memset(result, 0, sizeof(*result));
	result->session = media.session;
	result->state = media.state;
	result->sample_rate = media.audio.sample_rate;
	result->channels = media.audio.channels;
	result->sample_format = media.audio.sample_format;
	result->pcm_produced = media.audio.pcm_produced;
	result->pcm_acknowledged = audio_acknowledged();
	result->audio_pts =
		media.audio_bound ||
		media.audio_retired_bytes > media.audio.pcm_acknowledged
			? audio_output_pts() : media.audio.audio_pts;
	result->flags = audio_output_flags();
}

static uint32_t state_flags(void)
{
	uint32_t flags = 0U;

	if (media.header_ready)
		flags |= SDK_MEDIA_SESSION_RESULT_HEADER_READY;
	if (media.held)
		flags |= SDK_MEDIA_SESSION_RESULT_FRAME_HELD;
	if (media.state == SDK_MEDIA_SESSION_STATE_NEED_INPUT)
		flags |= SDK_MEDIA_SESSION_RESULT_NEED_INPUT;
	if (media.state == SDK_MEDIA_SESSION_STATE_DONE)
		flags |= SDK_MEDIA_SESSION_RESULT_DONE;
	flags |= mapped_media_flags(media.audio.flags);
	return flags;
}

static void fill_main(uint32_t bytes_written, uint32_t extra_flags,
	                  struct SDKMediaSessionMainResult *result)
{
	memset(result, 0, sizeof(*result));
	result->session = media.session;
	result->state = media.state;
	result->width = media.width;
	result->height = media.height;
	result->frame_rate_num = media.frame_rate_num;
	result->frame_rate_den = media.frame_rate_den;
	result->frame_number = media.frame_number;
	result->video_pts = media.video_pts;
	result->bytes_accepted = media.bytes_accepted;
	result->bytes_written = bytes_written;
	result->flags = state_flags() | extra_flags |
		mapped_media_flags(media.oneshot_flags);
	media.oneshot_flags = 0U;
}

static int active_session(uint32_t session)
{
	return media.active && media.session == session &&
	       sdk_video_stream_session_live(session);
}

static void advance_video_pts(const struct SDKVideoStreamResult *video)
{
	uint64_t scaled;

	if (media.decoded_frames == 0U) {
		media.video_pts = 0U;
		media.pts_origin = 0U;
		media.pts_remainder = 0U;
		media.oneshot_flags |= SDK_VIDEO_MEDIA_FLAG_DERIVED_TIME;
		return;
	}
	if (media.frame_rate_num != 0U && media.frame_rate_den != 0U) {
		uint32_t fields = media.last_display_fields;

		/* Backends without field knowledge report 0 or 1 (WebM);
		 * both mean one plain two-field frame duration. */
		if (fields < 2U)
			fields = 2U;
		/* fields/2 frame durations: 90000*den*fields / (2*num). */
		scaled = UINT64_C(45000) * media.frame_rate_den * fields +
		         media.pts_remainder;
		media.video_pts += scaled / media.frame_rate_num;
		media.pts_remainder = scaled % media.frame_rate_num;
		media.oneshot_flags |= SDK_VIDEO_MEDIA_FLAG_DERIVED_TIME;
		return;
	}
	media.video_pts = (uint64_t)video->frame_time_millis * 90U;
	media.oneshot_flags |= SDK_VIDEO_MEDIA_FLAG_DERIVED_TIME;
	if (media.pts_origin == SDK_MEDIA_NO_PTS)
		media.pts_origin = media.video_pts;
}

void sdk_media_session_init(void)
{
	memset(&media, 0, sizeof(media));
	media.video_pts = SDK_MEDIA_NO_PTS;
	media.pts_origin = SDK_MEDIA_NO_PTS;

#ifndef SDK_MEDIA_HOST_TEST
	Xil_DCacheFlushRange((INTPTR)(uintptr_t)&media, sizeof(media));
#endif
}

int sdk_media_session_core1(uint32_t session)
{
	return active_session(session) ? 1 : -1;
}

int sdk_media_session_close_known(uint32_t session)
{
	return active_session(session) ||
	       (!media.active && media.just_closed_session == session);
}

void sdk_media_session_poison_core1(void)
{
	if (!media.active)
		return;
	/* sdk_video_stream_poison_core1_sessions() keeps the faulted slot alive
	 * specifically so CLOSE can release it without touching reclaimed decoder
	 * memory. Keep the wrapper handle for the same error/status/close path. */
	media.state = SDK_MEDIA_SESSION_STATE_ERROR;
	media.held = 0U;
	media.present_pending = 0U;
	/* A faulted core 1 never retires a queued conversion. */
	media.output.in_flight = 0U;
}

void sdk_media_session_present_queued(uint32_t session)
{
	if (!active_session(session) || !media.present_pending)
		return;
	/* The compose now precedes any later decode in the single-producer
	 * core-1 queue, so the decoder-owned planes may be reused safely. */
	media.present_pending = 0U;
	media.held = 0U;
	media.state = SDK_MEDIA_SESSION_STATE_READY;
}

void sdk_media_session_close_retired(uint32_t session)
{
	if (media.just_closed_session == session && !media.close_retired) {
		/* A shared-buffer session never touched the overlay. */
		if (!media.shared)
			overlay_video_session_closed(session);
		media.close_retired = 1U;
	}
}

int sdk_media_session_audio_uses_ring(uint32_t codec)
{
	return codec == SDK_MEDIA_AUDIO_MP2 ||
	       codec == SDK_MEDIA_AUDIO_LPCM ||
	       codec == SDK_MEDIA_AUDIO_AC3 ||
	       codec == SDK_MEDIA_AUDIO_OPUS ||
	       codec == SDK_MEDIA_AUDIO_VORBIS;
}

uint16_t sdk_media_session_begin_owned(
	const struct SDKMediaSessionBegin *begin, uint16_t owner_id,
	struct SDKMediaSessionMainResult *result)
{
	struct SDKVideoStreamBegin video_begin;
	struct SDKVideoStreamResult video_result;
	uint16_t status;

	if (!begin || !result || begin->flags != 0U)
		return SDK_STATUS_BAD_REQUEST;
	if (begin->audio_codec != SDK_MEDIA_AUDIO_NONE &&
	    !sdk_media_session_audio_uses_ring(begin->audio_codec))
		return SDK_STATUS_UNSUPPORTED;
	if (begin->audio_codec == SDK_MEDIA_AUDIO_NONE &&
	    (begin->pcm_ring_handle != 0U || begin->pcm_ring ||
	     begin->pcm_ring_capacity != 0U ||
	     begin->pcm_low_water_bytes != 0U ||
	     begin->pcm_high_water_bytes != 0U))
		return SDK_STATUS_BAD_REQUEST;
	if (sdk_media_session_audio_uses_ring(begin->audio_codec) &&
	    (begin->pcm_ring_handle == 0U || !begin->pcm_ring ||
	    begin->pcm_ring_capacity <
		    1152U * SDK_VIDEO_MEDIA_PCM_FRAME_BYTES ||
	    begin->pcm_ring_capacity > SDK_VIDEO_MEDIA_MAX_PCM_RING ||
	    (begin->pcm_ring_capacity &
	     (SDK_VIDEO_MEDIA_PCM_FRAME_BYTES - 1U)) != 0U ||
	    begin->pcm_low_water_bytes >=
		    begin->pcm_high_water_bytes ||
	    begin->pcm_high_water_bytes <
		    1152U * SDK_VIDEO_MEDIA_PCM_FRAME_BYTES ||
	    begin->pcm_high_water_bytes >
		    begin->pcm_ring_capacity ||
	    (begin->pcm_low_water_bytes &
	     (SDK_VIDEO_MEDIA_PCM_FRAME_BYTES - 1U)) != 0U ||
	    (begin->pcm_high_water_bytes &
	     (SDK_VIDEO_MEDIA_PCM_FRAME_BYTES - 1U)) != 0U))
		return SDK_STATUS_BAD_REQUEST;
	if (media.active)
		return SDK_STATUS_NO_MEMORY;

	memset(&video_begin, 0, sizeof(video_begin));
	video_begin.codec = begin->video_codec;
	video_begin.container = begin->container;
	video_begin.width = begin->width;
	video_begin.height = begin->height;
	video_begin.output_format = begin->output_format;
	video_begin.audio_codec = begin->audio_codec;
	video_begin.pcm_ring = begin->pcm_ring;
	video_begin.pcm_ring_capacity = begin->pcm_ring_capacity;
	video_begin.pcm_low_water_bytes = begin->pcm_low_water_bytes;
	video_begin.pcm_high_water_bytes = begin->pcm_high_water_bytes;
	status = sdk_video_stream_begin(&video_begin, &video_result);
	if (status != SDK_STATUS_OK)
		return status;

	memset(&media, 0, sizeof(media));
	/* Reset here, not in sdk_media_session_init(): that only runs on an
	 * Amiga reset, so the r1 profiling round had every later run
	 * accumulating on top of the first one's totals. */
	sdk_media_profile_reset();
	/* Re-verify the NEON pack against the scalar oracle each run, so a
	 * profiling comparison always states which kernel it measured. */
	sdk_video_yuy2_reset_dispatch();
	media.session = video_result.session;
	media.owner_id = owner_id;
	media.state = mapped_state(video_result.state);
	media.audio_codec = begin->audio_codec;
	media.shared = begin->output_format == SDK_VIDEO_OUTPUT_SHARED_BUFFER;
	media.pcm_ring = begin->pcm_ring;
	media.pcm_ring_capacity = begin->pcm_ring_capacity;
	media.video_pts = SDK_MEDIA_NO_PTS;
	media.pts_origin = SDK_MEDIA_NO_PTS;
	media.audio.audio_pts = SDK_MEDIA_NO_PTS;
	media.audio.current_audio_pts = SDK_MEDIA_NO_PTS;
	media.audio.first_audio_pts = SDK_MEDIA_NO_PTS;
	media.audio.pts_origin = SDK_MEDIA_NO_PTS;
	media.audio.raw_pts = SDK_MEDIA_NO_PTS;
	media.active = 1U;
	update_from_video(&video_result);
	fill_main(video_result.bytes_written,
	          mapped_flags(video_result.flags), result);
	return SDK_STATUS_OK;
}

uint16_t sdk_media_session_begin(
	const struct SDKMediaSessionBegin *begin,
	struct SDKMediaSessionMainResult *result)
{
	return sdk_media_session_begin_owned(begin, 0U, result);
}

uint16_t sdk_media_session_write(
	const struct SDKMediaSessionWrite *write,
	struct SDKMediaSessionMainResult *result)
{
	struct SDKVideoStreamWrite video_write;
	struct SDKVideoStreamResult video_result;
	uint16_t status;

	if (!write || !result || write->session == 0U ||
	    (write->flags & ~SDK_MEDIA_SESSION_WRITE_EOF) != 0U)
		return SDK_STATUS_BAD_REQUEST;
	if (!active_session(write->session))
		return SDK_STATUS_BAD_HANDLE;
	if (media.state == SDK_MEDIA_SESSION_STATE_ERROR)
		return SDK_STATUS_IO_ERROR;
	if (media.audio_bound)
		update_audio();
	memset(&video_write, 0, sizeof(video_write));
	video_write.session = write->session;
	video_write.src = write->src;
	video_write.src_length = write->src_length;
	video_write.flags =
		(write->flags & SDK_MEDIA_SESSION_WRITE_EOF) != 0U
			? SDK_VIDEO_SESSION_WRITE_EOF : 0U;
	status = sdk_video_stream_write(&video_write, &video_result);
	if (status != SDK_STATUS_OK) {
		if (status == SDK_STATUS_BUSY)
			update_audio();
		if (status == SDK_STATUS_IO_ERROR ||
		    status == SDK_STATUS_UNSUPPORTED)
			media.state = SDK_MEDIA_SESSION_STATE_ERROR;
		return status;
	}
	update_from_video(&video_result);
	update_audio();
	if (!media.held)
		media.state = mapped_state(video_result.state);
	fill_main(video_result.bytes_written,
	          mapped_flags(video_result.flags), result);
	return SDK_STATUS_OK;
}

uint16_t sdk_media_session_decode(
	uint32_t session, uint32_t flags,
	struct SDKMediaSessionMainResult *result)
{
	struct SDKVideoStreamDecode decode;
	struct SDKVideoStreamResult video_result;
	uint16_t status;

	if (!result || session == 0U ||
	    (flags & ~(SDK_MEDIA_DECODE_SKIP_TO_KEYFRAME |
	               SDK_MEDIA_DECODE_SKIP_TO_I_PICTURE)) != 0U)
		return SDK_STATUS_BAD_REQUEST;
	if (!active_session(session))
		return SDK_STATUS_BAD_HANDLE;
	if (media.state == SDK_MEDIA_SESSION_STATE_ERROR)
		return SDK_STATUS_IO_ERROR;
	if (media.audio_bound)
		update_audio();
	if (media.held) {
		fill_main(0U, SDK_MEDIA_SESSION_RESULT_BACKPRESSURE, result);
		return SDK_STATUS_BUSY;
	}
	memset(&decode, 0, sizeof(decode));
	decode.session = session;
	decode.flags = flags;
	status = sdk_video_stream_decode(&decode, &video_result);
	if (status != SDK_STATUS_OK) {
		if (status == SDK_STATUS_BUSY)
			update_audio();
		if (status == SDK_STATUS_IO_ERROR ||
		    status == SDK_STATUS_UNSUPPORTED)
			media.state = SDK_MEDIA_SESSION_STATE_ERROR;
		return status;
	}
	update_from_video(&video_result);
	update_audio();
	media.state = mapped_state(video_result.state);
	if ((video_result.flags & SDK_VIDEO_SESSION_RESULT_FRAME_READY) != 0U) {
		if (video_result.media_pts == SDK_VIDEO_MEDIA_NO_PTS)
			advance_video_pts(&video_result);
		media.last_display_fields = video_result.display_fields;
		media.decoded_frames++;
		media.held = 1U;
		media.state = SDK_MEDIA_SESSION_STATE_FRAME_HELD;
	}
	fill_main(video_result.bytes_written,
	          mapped_flags(video_result.flags), result);
	return SDK_STATUS_OK;
}

/* PRESENT of a shared-buffer session: queue the conversion of the held
 * picture on core 1 and report the slot it goes to. The task is queued
 * ahead of any later DECODE, so the decoder-owned planes are read before
 * they can be reused and the frame is released at once. */
static uint16_t present_shared(struct SDKMediaSessionMainResult *result)
{
	struct SDKMediaOutputJob job;
	uint32_t slot;

	if (!media.output.configured)
		return SDK_STATUS_BAD_REQUEST;
	if (media.output.in_flight)
		return SDK_STATUS_BUSY;
	slot = media.output.next_slot;
	memset(&job, 0, sizeof(job));
	job.session = media.session;
	job.dst_addr = media.output.slot0_addr +
		(slot != 0U ? media.output.slot_stride : 0U);
	job.palette_addr = media.output.palette_addr;
	job.pitch = media.output.pitch;
	job.width = media.output.width;
	job.height = media.output.height;
	job.flags = media.output.flags;
	job.format = media.output.format;
	job.depth = media.output.depth;
	job.slot = (uint8_t)slot;
	if (!sdk_mailbox_enqueue_internal(TASKQ_OP_MEDIA_OUTPUT, &job,
	                                  sizeof(job)))
		return SDK_STATUS_BUSY;
	media.output.in_flight = 1U;
	media.output.pending_slot = (uint8_t)slot;
	media.output.next_slot =
		media.output.slots == 2U ? (uint8_t)(slot ^ 1U) : 0U;
	media.output.presented++;
	media.presented_frames++;
	media.held = 0U;
	media.state = SDK_MEDIA_SESSION_STATE_READY;
	fill_main(media.output.picture_bytes,
	          SDK_MEDIA_SESSION_RESULT_PRESENTED |
	          SDK_MEDIA_SESSION_RESULT_OUTPUT_PENDING |
	          (slot != 0U ? SDK_MEDIA_SESSION_RESULT_OUTPUT_SLOT1 : 0U),
	          result);
	return SDK_STATUS_OK;
}

uint16_t sdk_media_session_present(
	uint32_t session, uint32_t flags,
	struct SDKMediaSessionMainResult *result)
{
	if (!result || session == 0U || flags != 0U)
		return SDK_STATUS_BAD_REQUEST;
	if (!active_session(session))
		return SDK_STATUS_BAD_HANDLE;
	if (media.state == SDK_MEDIA_SESSION_STATE_ERROR)
		return SDK_STATUS_IO_ERROR;
	if (!media.held)
		return SDK_STATUS_BAD_REQUEST;
	if (media.present_pending)
		return SDK_STATUS_BUSY;
	if (media.shared)
		return present_shared(result);
	{
		/* Core-0 cost of handing a decoded frame to the overlay. This
		 * excludes the compose itself, which core 1 times separately as
		 * the pack stage. */
		uint32_t profile_start = sdk_media_profile_now_us();
		int ready = overlay_video_frame_ready(session);

		sdk_media_profile_record(
			SDK_MEDIA_PROFILE_PRESENT, profile_start);
		if (!ready)
			return SDK_STATUS_BUSY;
	}
	/* Keep the frame held until overlay_main_poll has actually enqueued its
	 * compose. sdk_mailbox_task may drain PRESENT and DECODE/CLOSE from one
	 * host batch before that poll runs. */
	media.present_pending = 1U;
	media.presented_frames++;
	fill_main(0U, SDK_MEDIA_SESSION_RESULT_PRESENTED, result);
	return SDK_STATUS_OK;
}

uint16_t sdk_media_session_discard(
	uint32_t session, uint32_t flags,
	struct SDKMediaSessionMainResult *result)
{
	if (!result || session == 0U || flags != 0U)
		return SDK_STATUS_BAD_REQUEST;
	if (!active_session(session))
		return SDK_STATUS_BAD_HANDLE;
	if (media.state == SDK_MEDIA_SESSION_STATE_ERROR)
		return SDK_STATUS_IO_ERROR;
	if (!media.held)
		return SDK_STATUS_BAD_REQUEST;
	if (media.present_pending)
		return SDK_STATUS_BUSY;
	media.held = 0U;
	media.discarded_frames++;
	media.state = SDK_MEDIA_SESSION_STATE_READY;
	fill_main(0U, SDK_MEDIA_SESSION_RESULT_DISCARDED, result);
	return SDK_STATUS_OK;
}

uint16_t sdk_media_session_status(
	uint32_t session, uint32_t page, uint32_t flags,
	struct SDKMediaSessionStatusResult *result)
{
	if (!result || session == 0U || flags != 0U ||
	    page > SDK_MEDIA_STATUS_OUTPUT)
		return SDK_STATUS_BAD_REQUEST;
	if (!active_session(session))
		return SDK_STATUS_BAD_HANDLE;
	memset(result, 0, sizeof(*result));
	result->session = session;
	result->state = media.state;
	result->page = page;
	if (page == SDK_MEDIA_STATUS_TIMING) {
		result->flags = mapped_media_flags(media.audio.flags);
		result->value[0] = media.pts_origin;
		result->value[1] = media.video_pts;
		result->value[2] = media.audio.audio_pts;
		result->value[3] = media.audio.raw_pts;
	} else if (page == SDK_MEDIA_STATUS_AUDIO) {
		result->flags = audio_output_flags();
		result->value[0] = media.audio.pcm_produced;
		result->value[1] = audio_acknowledged();
		result->value[2] = media.audio.current_audio_pts;
		result->value[3] = media.audio.first_audio_pts;
	} else if (page == SDK_MEDIA_STATUS_COUNTERS) {
		result->value[0] = media.bytes_accepted;
		result->value[1] = media.decoded_frames;
		result->value[2] = media.presented_frames;
		result->value[3] = media.discarded_frames;
	} else if (page == SDK_MEDIA_STATUS_PROFILE) {
		uint32_t stage;

		result->flags = sdk_video_yuy2_neon_active()
			? SDK_MEDIA_PROFILE_FLAG_NEON_PACK : 0U;

		/* Per-stage microseconds and call counts, packed one stage per
		 * value as (microseconds << 32) | calls. The host divides to get
		 * per-frame averages. */
		for (stage = 0U; stage < SDK_MEDIA_PROFILE_STAGES &&
		                 stage < 4U; stage++) {
			uint32_t microseconds = 0U;
			uint32_t calls = 0U;

			(void)sdk_media_profile_read(stage, &microseconds,
			                             &calls);
			result->value[stage] =
				((uint64_t)microseconds << 32) | calls;
		}
	} else if (page == SDK_MEDIA_STATUS_PRESENTATION) {
		struct overlay_path_info path;

		overlay_path_snapshot(session, &path);
		result->flags =
			(path.configured ? SDK_MEDIA_PRESENT_CONFIGURED : 0U) |
			(path.active ? SDK_MEDIA_PRESENT_ACTIVE : 0U) |
			(path.hw_active ? SDK_MEDIA_PRESENT_NATIVE : 0U) |
			(path.owns_session ? SDK_MEDIA_PRESENT_OWNED : 0U);
		/* Geometry is reported as raw facts in fixed 16-bit halves. The
		 * 1:1 / scaled / clipped classification is arithmetic on these
		 * and is done host-side so it stays testable off-hardware. */
		result->value[0] =
			SDK_MEDIA_PACK_PAIR(path.src_w, path.src_h);
		result->value[1] =
			SDK_MEDIA_PACK_PAIR(path.dst_w, path.dst_h);
		result->value[2] =
			SDK_MEDIA_PACK_PAIR(path.dst_x, path.dst_y);
		result->value[3] =
			SDK_MEDIA_PACK_PAIR(path.screen_w, path.screen_h);
	} else if (page == SDK_MEDIA_STATUS_OUTPUT) {
		/* An overlay session answers with everything zero. */
		if (media.shared) {
			result->flags =
				(media.output.configured
					? SDK_MEDIA_OUTPUT_STATUS_CONFIGURED : 0U) |
				(media.output.complete
					? SDK_MEDIA_OUTPUT_STATUS_COMPLETE : 0U) |
				(media.output.complete && media.output.newest_slot != 0U
					? SDK_MEDIA_OUTPUT_STATUS_SLOT1 : 0U) |
				(media.output.in_flight
					? SDK_MEDIA_OUTPUT_STATUS_PENDING : 0U);
			result->value[0] = media.output.written;
			result->value[1] = media.output.presented;
			result->value[2] = media.output.last_us;
			result->value[3] = SDK_MEDIA_OUTPUT_FLAGS_SUPPORTED;
		}
	} else {
		uint32_t frame_bytes = audio_frame_bytes();
		uint64_t retired_bytes;
		uint64_t staged_bytes;

		result->flags = audio_output_flags();
		audio_cursor_snapshot(&retired_bytes, &staged_bytes);
		if (frame_bytes != 0U) {
			result->value[0] =
				retired_bytes / frame_bytes;
			result->value[1] =
				(staged_bytes - retired_bytes) / frame_bytes;
			result->value[2] =
				staged_bytes / frame_bytes;
		}
		result->value[3] = media.audio_underruns;
	}
	return SDK_STATUS_OK;
}

uint16_t sdk_media_session_audio_read(
	uint32_t session, uint64_t acknowledged, uint32_t flags,
	struct SDKMediaSessionAudioResult *result)
{
	if (!result || session == 0U || flags != 0U)
		return SDK_STATUS_BAD_REQUEST;
	if (!active_session(session))
		return SDK_STATUS_BAD_HANDLE;
	if (media.state == SDK_MEDIA_SESSION_STATE_ERROR)
		return SDK_STATUS_IO_ERROR;
	if (media.audio_bound)
		return SDK_STATUS_BUSY;
	if (!sdk_video_stream_ack_media(session, acknowledged))
		return SDK_STATUS_BAD_REQUEST;
	update_audio();
	fill_audio_result(result);
	return SDK_STATUS_OK;
}

uint16_t sdk_media_session_audio_bind(
	uint32_t session, uint32_t flags,
	struct SDKMediaSessionAudioResult *result)
{
	if (!result || session == 0U ||
	    (flags & ~SDK_MEDIA_AUDIO_BIND_PAUSE) != 0U)
		return SDK_STATUS_BAD_REQUEST;
	if (!active_session(session))
		return SDK_STATUS_BAD_HANDLE;
	if (media.state == SDK_MEDIA_SESSION_STATE_ERROR)
		return SDK_STATUS_IO_ERROR;
	if (!sdk_media_session_audio_uses_ring(media.audio_codec))
		return SDK_STATUS_UNSUPPORTED;
	if (!media.audio_bound) {
		if (flags != 0U)
			return SDK_STATUS_BAD_REQUEST;
		/* MEDIA_SESSION decode/write publish this snapshot from core 1.
		 * BIND runs inline on core 0 because it also owns the AX pump;
		 * never reach through the wrapper into core-1-owned decoder
		 * objects here. Clients prebuffer until format discovery before
		 * binding, so the coherent snapshot is already authoritative. */
		if (media.audio.sample_rate == 0U ||
		    (media.audio.channels != 1U &&
		     media.audio.channels != 2U) ||
		    media.audio.sample_format !=
		        SDK_VIDEO_MEDIA_SAMPLE_S16BE)
			return SDK_STATUS_BAD_REQUEST;
		media.audio_staged_bytes = audio_acknowledged();
		media.audio_retired_bytes = media.audio_staged_bytes;
		media.audio_underruns = 0U;
		media.audio_bound = 1U;
	}
	if ((flags & SDK_MEDIA_AUDIO_BIND_PAUSE) != 0U) {
		/* The pump will wipe its TX queue after this store. Rewind staging
		 * to actual retirement; decoder acknowledgement also follows
		 * retirement, so every unplayed sample remains available. */
		media.audio_paused = 1U;
		media.audio_staged_bytes = media.audio_retired_bytes;
	} else {
		media.audio_paused = 0U;
	}
	fill_audio_result(result);
	return SDK_STATUS_OK;
}

uint16_t sdk_media_session_audio_unbind(
	uint32_t session, uint32_t flags,
	struct SDKMediaSessionAudioResult *result)
{
	if (!result || session == 0U || flags != 0U)
		return SDK_STATUS_BAD_REQUEST;
	if (!active_session(session))
		return SDK_STATUS_BAD_HANDLE;
	if (media.audio_bound)
		media.audio_staged_bytes = media.audio_retired_bytes;
	media.audio_bound = 0U;
	media.audio_paused = 0U;
	fill_audio_result(result);
	return SDK_STATUS_OK;
}

int sdk_media_session_audio_source(
	uint32_t session, struct SDKMediaAudioSource *source)
{
	if (!source || !active_session(session) ||
	    !media.audio_bound || media.audio_paused ||
	    !media.pcm_ring || media.pcm_ring_capacity == 0U)
		return 0;
	memset(source, 0, sizeof(*source));
	source->ring = media.pcm_ring;
	source->capacity = media.pcm_ring_capacity;
	source->produced_bytes = media.audio.pcm_produced;
	source->staged_bytes = media.audio_staged_bytes;
	source->sample_rate = media.audio.sample_rate;
	source->channels = media.audio.channels;
	source->sample_format = media.audio.sample_format;
	source->done =
		(media.audio.flags & SDK_VIDEO_MEDIA_FLAG_AUDIO_DONE) != 0U;
	return 1;
}

int sdk_media_session_audio_stage(uint32_t session, uint32_t bytes)
{
	uint64_t available;
	uint32_t frame_bytes = audio_frame_bytes();

	if (!active_session(session) || !media.audio_bound ||
	    media.audio_paused || frame_bytes == 0U ||
	    (bytes % frame_bytes) != 0U ||
	    media.audio_staged_bytes > media.audio.pcm_produced)
		return 0;
	available = media.audio.pcm_produced - media.audio_staged_bytes;
	if (bytes > available)
		return 0;
	media.audio_staged_bytes += bytes;
	return 1;
}

int sdk_media_session_audio_retire(uint32_t session, uint32_t bytes)
{
	uint32_t frame_bytes = audio_frame_bytes();

	if (!active_session(session) || !media.audio_bound ||
	    frame_bytes == 0U || (bytes % frame_bytes) != 0U ||
	    media.audio_retired_bytes > media.audio_staged_bytes ||
	    bytes > media.audio_staged_bytes - media.audio_retired_bytes)
		return 0;
	media.audio_retired_bytes += bytes;
	return 1;
}

void sdk_media_session_audio_underrun(uint32_t session)
{
	if (active_session(session) && media.audio_bound &&
	    !media.audio_paused && media.audio_underruns != 0xffffffffU)
		media.audio_underruns++;
}

uint16_t sdk_media_session_close(
	uint32_t session, uint32_t flags,
	struct SDKMediaSessionMainResult *result)
{
	struct SDKVideoStreamResult video_result;
	uint16_t status;

	if (!result || session == 0U || flags != 0U)
		return SDK_STATUS_BAD_REQUEST;
	if (!media.active) {
		if (media.just_closed_session != session)
			return SDK_STATUS_BAD_HANDLE;
		*result = media.last_close;
		return SDK_STATUS_OK;
	}
	if (!active_session(session))
		return SDK_STATUS_BAD_HANDLE;
	if (media.audio_bound)
		return SDK_STATUS_BUSY;
	if (media.present_pending || media.output.in_flight)
		return SDK_STATUS_BUSY;
	status = sdk_video_stream_close(session, &video_result);
	if (status != SDK_STATUS_OK)
		return status;
	update_from_video(&video_result);
	media.state = SDK_MEDIA_SESSION_STATE_DONE;
	media.held = 0U;
	media.present_pending = 0U;
	fill_main(video_result.bytes_written,
	          SDK_MEDIA_SESSION_RESULT_DONE, result);
	media.last_close = *result;
	media.just_closed_session = session;
	media.close_retired = 0U;
	media.active = 0U;
	return SDK_STATUS_OK;
}

int sdk_media_session_release_client(uint16_t owner_id, uint32_t *close_session)
{
	if (close_session)
		*close_session = 0U;
	if (!media.active || media.owner_id != owner_id)
		return 1;
	/* A bound session refuses CLOSE until its audio is unbound (the PCM
	 * ring stays pinned meanwhile). CLIENT_RELEASE is the owner going
	 * away, so unbind here: rewind staging to retirement exactly as the
	 * explicit unbind op does, leaving the close free to proceed on
	 * whichever core runs it. The AX pump is stopped by the mailbox
	 * before it calls this, so no ISR reads the view between the rewind
	 * and the close. */
	if (media.audio_bound) {
		media.audio_staged_bytes = media.audio_retired_bytes;
		media.audio_bound = 0U;
		media.audio_paused = 0U;
	}
	if (close_session)
		*close_session = media.session;
	return 0;
}

int sdk_media_session_owned_by(uint32_t session, uint16_t owner_id)
{
	return media.active && media.session == session &&
	       media.owner_id == owner_id;
}

int sdk_media_session_output_shared(uint32_t session)
{
	if (!active_session(session))
		return -1;
	return media.shared ? 1 : 0;
}

uint16_t sdk_media_session_output_config(
	uint32_t session, const struct SDKMediaOutputBinding *binding,
	struct SDKMediaSessionMainResult *result)
{
	struct SDKMediaOutputLayout layout;
	uint16_t status;

	if (!binding || !result || session == 0U)
		return SDK_STATUS_BAD_REQUEST;
	if (!active_session(session))
		return SDK_STATUS_BAD_HANDLE;
	if (!media.shared)
		return SDK_STATUS_UNSUPPORTED;
	if (media.state == SDK_MEDIA_SESSION_STATE_ERROR)
		return SDK_STATUS_IO_ERROR;
	if (media.output.in_flight)
		return SDK_STATUS_BUSY;
	status = sdk_media_output_check(&binding->config, &layout);
	if (status != SDK_STATUS_OK)
		return status;
	if (binding->buffer_addr == 0U ||
	    binding->config.buffer_offset > binding->buffer_length ||
	    layout.span_bytes >
	        binding->buffer_length - binding->config.buffer_offset ||
	    ((binding->config.flags & SDK_MEDIA_OUTPUT_PALETTE_HOST) != 0U) !=
	        (binding->palette_addr != 0U))
		return SDK_STATUS_BAD_REQUEST;

	media.output.slot0_addr =
		binding->buffer_addr + binding->config.buffer_offset;
	media.output.slot_stride = binding->config.slot_stride;
	media.output.palette_addr = binding->palette_addr;
	media.output.pitch = binding->config.pitch;
	media.output.picture_bytes = layout.picture_bytes;
	media.output.width = (uint16_t)binding->config.width;
	media.output.height = (uint16_t)binding->config.height;
	media.output.flags = (uint16_t)binding->config.flags;
	media.output.format = (uint8_t)binding->config.format;
	media.output.depth = (uint8_t)layout.depth;
	media.output.slots = (uint8_t)layout.slots;
	media.output.configured = 1U;
	/* The slots hold pictures of the old layout until a new one lands. */
	media.output.complete = 0U;
	media.output.newest_slot = 0U;
	media.output.next_slot = 0U;
	fill_main(layout.picture_bytes, 0U, result);
	return SDK_STATUS_OK;
}

void sdk_media_session_output_retired(uint32_t session, int ok,
	                                  uint32_t elapsed_us)
{
	if (!media.active || !media.output.in_flight ||
	    (session != 0U && session != media.session))
		return;
	media.output.in_flight = 0U;
	if (!ok) {
		media.state = SDK_MEDIA_SESSION_STATE_ERROR;
		return;
	}
	media.output.complete = 1U;
	media.output.newest_slot = media.output.pending_slot;
	media.output.written++;
	media.output.last_us = elapsed_us;
}

uint16_t sdk_media_session_run_output(const struct SDKMediaOutputJob *job,
	                                  uint32_t *elapsed_us)
{
	struct SDKVideoDecodedFrame frame;
	const uint8_t *palette = 0;
	uint32_t start = sdk_media_profile_now_us();
	uint16_t status;

	*elapsed_us = 0U;
	if (!sdk_video_stream_get_direct_frame(job->session, &frame))
		return SDK_STATUS_BAD_HANDLE;
	if (job->palette_addr != 0U) {
		palette = (const uint8_t *)(uintptr_t)job->palette_addr;
#ifndef SDK_MEDIA_HOST_TEST
		/* The host wrote the palette; core 1 may hold an older copy. */
		Xil_DCacheInvalidateRange((INTPTR)job->palette_addr,
		                          SDK_MEDIA_OUTPUT_PALETTE_BYTES);
#endif
	}
	status = sdk_media_output_convert(job, &frame, palette);
	if (status != SDK_STATUS_OK)
		return status;
#ifndef SDK_MEDIA_HOST_TEST
	/* L1 and L2: the host reads the slot through the Zorro bridge. */
	Xil_DCacheFlushRange((INTPTR)job->dst_addr,
	                     sdk_media_output_job_bytes(job));
#endif
	sdk_media_profile_record(SDK_MEDIA_PROFILE_YUY2_PACK, start);
	*elapsed_us = sdk_media_profile_now_us() - start;
	return SDK_STATUS_OK;
}
