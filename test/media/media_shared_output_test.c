/*
 * Shared-buffer output of a media session: configuration, the double-buffer
 * slot state machine, OUTPUT_PENDING between PRESENT and retirement, the BUSY
 * rules and status page 6. The core-1 conversion is covered by
 * media_output_test.c; here the task queue is a mock that records the job.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "overlay_path.h"
#include "sdk_mailbox.h"
#include "sdk_media_profile.h"
#include "sdk_media_session.h"
#include "sdk_video_stream.h"

static int checks;
static int failures;

#define CHECK(expr) do { \
	checks++; \
	if (!(expr)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
	} \
} while (0)

#define SESSION 9U

/* ---- mocks ----------------------------------------------------------- */

static uint8_t stream_active;
static uint32_t stream_frame_number;
static uint32_t stream_begin_output;
static uint32_t overlay_ready_calls;
static uint32_t overlay_closed_calls;
static uint32_t enqueue_calls;
static int enqueue_accept;
static uint32_t enqueue_opcode;
static struct SDKMediaOutputJob enqueue_job;
static struct SDKVideoDecodedFrame direct_frame;
static int direct_frame_valid;

static void fill_video_result(struct SDKVideoStreamResult *result,
	                          uint32_t state, uint32_t flags)
{
	memset(result, 0, sizeof(*result));
	result->session = SESSION;
	result->state = state;
	result->width = 16U;
	result->height = 16U;
	result->frame_rate_milli = 25000U;
	result->frame_number = stream_frame_number;
	result->flags = flags;
	result->media_pts = SDK_VIDEO_MEDIA_NO_PTS;
	result->raw_pts = SDK_VIDEO_MEDIA_NO_PTS;
	result->display_fields = 2U;
}

uint16_t sdk_video_stream_begin(const struct SDKVideoStreamBegin *begin,
	                            struct SDKVideoStreamResult *result)
{
	if (stream_active)
		return SDK_STATUS_NO_MEMORY;
	stream_begin_output = begin->output_format;
	stream_active = 1U;
	stream_frame_number = 0U;
	fill_video_result(result, SDK_VIDEO_SESSION_STATE_NEED_INPUT,
	                  SDK_VIDEO_SESSION_RESULT_NEED_INPUT);
	return SDK_STATUS_OK;
}

uint16_t sdk_video_stream_write(const struct SDKVideoStreamWrite *write,
	                            struct SDKVideoStreamResult *result)
{
	(void)write;
	fill_video_result(result, SDK_VIDEO_SESSION_STATE_READY,
	                  SDK_VIDEO_SESSION_RESULT_HEADER_READY);
	return SDK_STATUS_OK;
}

uint16_t sdk_video_stream_decode(const struct SDKVideoStreamDecode *decode,
	                             struct SDKVideoStreamResult *result)
{
	(void)decode;
	stream_frame_number++;
	fill_video_result(result, SDK_VIDEO_SESSION_STATE_FRAME_READY,
	                  SDK_VIDEO_SESSION_RESULT_HEADER_READY |
	                  SDK_VIDEO_SESSION_RESULT_FRAME_READY);
	return SDK_STATUS_OK;
}

uint16_t sdk_video_stream_close(uint32_t session,
	                            struct SDKVideoStreamResult *result)
{
	if (session != SESSION)
		return SDK_STATUS_BAD_HANDLE;
	stream_active = 0U;
	fill_video_result(result, SDK_VIDEO_SESSION_STATE_DONE,
	                  SDK_VIDEO_SESSION_RESULT_DONE);
	return SDK_STATUS_OK;
}

int sdk_video_stream_session_live(uint32_t session)
{
	return stream_active && session == SESSION;
}

int sdk_video_stream_get_media_info(uint32_t session,
	                                struct SDKVideoMediaInfo *info)
{
	(void)session;
	(void)info;
	return 0;
}

int sdk_video_stream_ack_media(uint32_t session, uint64_t acknowledged)
{
	(void)session;
	return acknowledged == 0U;
}

int sdk_video_stream_get_direct_frame(uint32_t session,
	                                  struct SDKVideoDecodedFrame *frame)
{
	if (!direct_frame_valid || session != SESSION)
		return 0;
	*frame = direct_frame;
	return 1;
}

int overlay_video_frame_ready(uint32_t session)
{
	(void)session;
	overlay_ready_calls++;
	return 1;
}

void overlay_video_session_closed(uint32_t session)
{
	(void)session;
	overlay_closed_calls++;
}

void overlay_path_snapshot(uint32_t session, struct overlay_path_info *out)
{
	memset(out, 0, sizeof(*out));
	(void)session;
}

int sdk_mailbox_enqueue_internal(uint32_t opcode, const void *params,
	                             uint32_t params_len)
{
	enqueue_calls++;
	enqueue_opcode = opcode;
	if (!enqueue_accept)
		return 0;
	if (params_len != sizeof(enqueue_job) || params_len > TASKQ_OP_PARAM_BYTES)
		return 0;
	memcpy(&enqueue_job, params, params_len);
	return 1;
}

/* ---- helpers --------------------------------------------------------- */

static void reset(void)
{
	stream_active = 0U;
	stream_frame_number = 0U;
	overlay_ready_calls = 0U;
	overlay_closed_calls = 0U;
	enqueue_calls = 0U;
	enqueue_accept = 1;
	direct_frame_valid = 0;
	memset(&enqueue_job, 0, sizeof(enqueue_job));
	sdk_media_session_init();
}

static void begin_session(uint32_t output)
{
	struct SDKMediaSessionBegin begin;
	struct SDKMediaSessionMainResult result;
	struct SDKMediaSessionWrite write;
	uint8_t bytes[16] = { 0 };

	memset(&begin, 0, sizeof(begin));
	begin.video_codec = SDK_VIDEO_CODEC_MPEG1;
	begin.container = SDK_VIDEO_CONTAINER_MPEG_PS;
	begin.width = 16U;
	begin.height = 16U;
	begin.output_format = output;
	CHECK(sdk_media_session_begin(&begin, &result) == SDK_STATUS_OK);
	memset(&write, 0, sizeof(write));
	write.session = SESSION;
	write.src = bytes;
	write.src_length = sizeof(bytes);
	CHECK(sdk_media_session_write(&write, &result) == SDK_STATUS_OK);
}

static struct SDKMediaOutputBinding binding(uint32_t flags, uint32_t stride)
{
	struct SDKMediaOutputBinding b;

	memset(&b, 0, sizeof(b));
	b.config.width = 64U;
	b.config.height = 32U;
	b.config.format = SDK_SURFACE_FORMAT_RGB565;
	b.config.pitch = 128U;
	b.config.flags = flags;
	b.config.slot_stride = stride;
	b.config.buffer_offset = 16U;
	b.buffer_addr = 0x08400000U;
	b.buffer_length = 16U + 4096U * 2U + 64U;
	return b;
}

static struct SDKMediaSessionMainResult decode_held(void)
{
	struct SDKMediaSessionMainResult result;

	CHECK(sdk_media_session_decode(SESSION, 0U, &result) == SDK_STATUS_OK);
	CHECK(result.state == SDK_MEDIA_SESSION_STATE_FRAME_HELD);
	return result;
}

static struct SDKMediaSessionStatusResult output_page(void)
{
	struct SDKMediaSessionStatusResult s;

	CHECK(sdk_media_session_status(SESSION, SDK_MEDIA_STATUS_OUTPUT, 0U,
	                               &s) == SDK_STATUS_OK);
	CHECK(s.page == SDK_MEDIA_STATUS_OUTPUT);
	return s;
}

/* ---- tests ----------------------------------------------------------- */

static void test_abi(void)
{
	CHECK(SDK_VIDEO_OUTPUT_SHARED_BUFFER == 2U);
	CHECK(SDK_OP_MEDIA_SESSION_OUTPUT_CONFIG == 0x0b0eU);
	CHECK(SDK_OP_MEDIA_SESSION_OUTPUT_CONFIG ==
	      SDK_SERVICE_VIDEO + 0x0eU);
	CHECK(SDK_VIDEO_OPCODE_COUNT_SHARED_OUTPUT == 15U);
	CHECK(SDK_MEDIA_SESSION_RESULT_OUTPUT_SLOT1 == (1U << 15));
	CHECK(SDK_MEDIA_SESSION_RESULT_OUTPUT_PENDING == (1U << 16));
	CHECK(SDK_MEDIA_STATUS_OUTPUT == 6U);
	CHECK(SDK_MEDIA_OUTPUT_STATUS_CONFIGURED == (1U << 0));
	CHECK(SDK_MEDIA_OUTPUT_STATUS_COMPLETE == (1U << 1));
	CHECK(SDK_MEDIA_OUTPUT_STATUS_SLOT1 == (1U << 2));
	CHECK(SDK_MEDIA_OUTPUT_STATUS_PENDING == (1U << 3));
	CHECK(SDK_MEDIA_OUTPUT_DEPTH_MASK == 0x0fU);
	CHECK(SDK_MEDIA_OUTPUT_PLANAR_INTERLEAVED == (1U << 4));
	CHECK(SDK_MEDIA_OUTPUT_DITHER_ORDERED == (1U << 5));
	CHECK(SDK_MEDIA_OUTPUT_DITHER_DIFFUSION == (1U << 6));
	CHECK(SDK_MEDIA_OUTPUT_PALETTE_HOST == (1U << 7));
	CHECK(SDK_MEDIA_OUTPUT_SCALE_NEAREST == (1U << 8));
	CHECK(sizeof(struct SDKMediaSessionOutputPayload) == 48U);
	CHECK(sizeof(struct SDKMediaOutputJob) <= TASKQ_OP_PARAM_BYTES);
	CHECK(TASKQ_OP_MEDIA_OUTPUT != TASKQ_OP_VIDEO_COMPOSE);
}

static void test_overlay_session_is_unchanged(void)
{
	struct SDKMediaSessionMainResult result;
	struct SDKMediaOutputBinding b = binding(0U, 0U);
	struct SDKMediaSessionStatusResult s;

	reset();
	begin_session(SDK_VIDEO_OUTPUT_DIRECT_OVERLAY);
	CHECK(stream_begin_output == SDK_VIDEO_OUTPUT_DIRECT_OVERLAY);
	CHECK(sdk_media_session_output_shared(SESSION) == 0);
	CHECK(sdk_media_session_output_shared(SESSION + 1U) == -1);
	CHECK(sdk_media_session_output_config(SESSION, &b, &result) ==
	      SDK_STATUS_UNSUPPORTED);
	/* the page is answered, with everything zero */
	s = output_page();
	CHECK(s.flags == 0U && s.value[0] == 0U && s.value[1] == 0U &&
	      s.value[2] == 0U && s.value[3] == 0U);
	/* PRESENT still goes to the overlay and queues no conversion */
	(void)decode_held();
	CHECK(sdk_media_session_present(SESSION, 0U, &result) == SDK_STATUS_OK);
	CHECK(overlay_ready_calls == 1U && enqueue_calls == 0U);
	CHECK((result.flags & (SDK_MEDIA_SESSION_RESULT_OUTPUT_PENDING |
	                       SDK_MEDIA_SESSION_RESULT_OUTPUT_SLOT1)) == 0U);
	sdk_media_session_present_queued(SESSION);
	CHECK(sdk_media_session_close(SESSION, 0U, &result) == SDK_STATUS_OK);
	sdk_media_session_close_retired(SESSION);
	CHECK(overlay_closed_calls == 1U);
}

static void test_configure(void)
{
	struct SDKMediaSessionMainResult result;
	struct SDKMediaOutputBinding b = binding(SDK_MEDIA_OUTPUT_DITHER_ORDERED,
	                                         4096U);
	struct SDKMediaSessionStatusResult s;

	reset();
	begin_session(SDK_VIDEO_OUTPUT_SHARED_BUFFER);
	CHECK(stream_begin_output == SDK_VIDEO_OUTPUT_SHARED_BUFFER);
	CHECK(sdk_media_session_output_shared(SESSION) == 1);

	/* before any configuration: nothing configured, the capability mask is
	 * already readable */
	s = output_page();
	CHECK(s.flags == 0U);
	CHECK(s.value[3] == SDK_MEDIA_OUTPUT_FLAGS_SUPPORTED);
	CHECK((s.value[3] & SDK_MEDIA_OUTPUT_DITHER_DIFFUSION) == 0U);

	/* unknown session and bad configurations are refused untouched */
	CHECK(sdk_media_session_output_config(SESSION + 1U, &b, &result) ==
	      SDK_STATUS_BAD_HANDLE);
	b.config.flags = SDK_MEDIA_OUTPUT_DITHER_DIFFUSION;
	CHECK(sdk_media_session_output_config(SESSION, &b, &result) ==
	      SDK_STATUS_UNSUPPORTED);
	b = binding(0U, 4096U);
	b.config.pitch = 100U; /* below width * 2 */
	CHECK(sdk_media_session_output_config(SESSION, &b, &result) ==
	      SDK_STATUS_BAD_REQUEST);
	b = binding(0U, 4096U);
	b.buffer_length = 16U + 4096U + 4095U; /* slot 1 would end beyond it */
	CHECK(sdk_media_session_output_config(SESSION, &b, &result) ==
	      SDK_STATUS_BAD_REQUEST);
	b = binding(0U, 4096U);
	b.palette_addr = 0x08500000U; /* a palette without PALETTE_HOST */
	CHECK(sdk_media_session_output_config(SESSION, &b, &result) ==
	      SDK_STATUS_BAD_REQUEST);
	b = binding(SDK_MEDIA_OUTPUT_PALETTE_HOST, 0U);
	b.config.format = SDK_SURFACE_FORMAT_INDEX8;
	b.config.pitch = 64U;
	CHECK(sdk_media_session_output_config(SESSION, &b, &result) ==
	      SDK_STATUS_BAD_REQUEST); /* PALETTE_HOST with no palette */
	s = output_page();
	CHECK((s.flags & SDK_MEDIA_OUTPUT_STATUS_CONFIGURED) == 0U);

	b = binding(0U, 4096U);
	CHECK(sdk_media_session_output_config(SESSION, &b, &result) ==
	      SDK_STATUS_OK);
	CHECK(result.session == SESSION);
	CHECK(result.bytes_written == 128U * 32U); /* one picture */
	s = output_page();
	CHECK(s.flags == SDK_MEDIA_OUTPUT_STATUS_CONFIGURED);
	CHECK(s.value[0] == 0U && s.value[1] == 0U);
}

static void test_present_requires_configuration(void)
{
	struct SDKMediaSessionMainResult result;

	reset();
	begin_session(SDK_VIDEO_OUTPUT_SHARED_BUFFER);
	(void)decode_held();
	CHECK(sdk_media_session_present(SESSION, 0U, &result) ==
	      SDK_STATUS_BAD_REQUEST);
	CHECK(enqueue_calls == 0U && overlay_ready_calls == 0U);
	/* the picture is still held: DISCARD works */
	CHECK(sdk_media_session_discard(SESSION, 0U, &result) == SDK_STATUS_OK);
}

static void test_slots_alternate_and_pending(void)
{
	struct SDKMediaSessionMainResult result;
	struct SDKMediaOutputBinding b = binding(SDK_MEDIA_OUTPUT_DITHER_ORDERED,
	                                         4096U);
	struct SDKMediaSessionStatusResult s;
	const uint32_t slot0 = 0x08400000U + 16U;

	reset();
	begin_session(SDK_VIDEO_OUTPUT_SHARED_BUFFER);
	CHECK(sdk_media_session_output_config(SESSION, &b, &result) ==
	      SDK_STATUS_OK);

	/* picture 1 goes to slot 0 */
	(void)decode_held();
	CHECK(sdk_media_session_present(SESSION, 0U, &result) == SDK_STATUS_OK);
	CHECK((result.flags & SDK_MEDIA_SESSION_RESULT_PRESENTED) != 0U);
	CHECK((result.flags & SDK_MEDIA_SESSION_RESULT_OUTPUT_PENDING) != 0U);
	CHECK((result.flags & SDK_MEDIA_SESSION_RESULT_OUTPUT_SLOT1) == 0U);
	CHECK((result.flags & SDK_MEDIA_SESSION_RESULT_FRAME_HELD) == 0U);
	CHECK(result.state == SDK_MEDIA_SESSION_STATE_READY);
	CHECK(result.bytes_written == 128U * 32U);
	CHECK(overlay_ready_calls == 0U); /* the overlay is never involved */
	CHECK(enqueue_calls == 1U && enqueue_opcode == TASKQ_OP_MEDIA_OUTPUT);
	CHECK(enqueue_job.session == SESSION);
	CHECK(enqueue_job.dst_addr == slot0 && enqueue_job.slot == 0U);
	CHECK(enqueue_job.width == 64U && enqueue_job.height == 32U);
	CHECK(enqueue_job.pitch == 128U);
	CHECK(enqueue_job.format == SDK_SURFACE_FORMAT_RGB565);
	CHECK(enqueue_job.flags == SDK_MEDIA_OUTPUT_DITHER_ORDERED);
	CHECK(enqueue_job.palette_addr == 0U);

	/* between PRESENT and retirement: pending, nothing complete yet */
	s = output_page();
	CHECK(s.flags == (SDK_MEDIA_OUTPUT_STATUS_CONFIGURED |
	                  SDK_MEDIA_OUTPUT_STATUS_PENDING));
	CHECK(s.value[0] == 0U && s.value[1] == 1U);
	/* the decoder frame is released at once, the next picture can decode
	 * (it queues behind the conversion on core 1) but not be presented */
	(void)decode_held();
	CHECK(sdk_media_session_present(SESSION, 0U, &result) == SDK_STATUS_BUSY);
	CHECK(enqueue_calls == 1U);
	/* reconfiguring and closing answer BUSY while a picture is written */
	CHECK(sdk_media_session_output_config(SESSION, &b, &result) ==
	      SDK_STATUS_BUSY);
	CHECK(sdk_media_session_close(SESSION, 0U, &result) == SDK_STATUS_BUSY);

	/* retirement publishes slot 0 */
	sdk_media_session_output_retired(SESSION, 1, 4321U);
	s = output_page();
	CHECK(s.flags == (SDK_MEDIA_OUTPUT_STATUS_CONFIGURED |
	                  SDK_MEDIA_OUTPUT_STATUS_COMPLETE));
	CHECK(s.value[0] == 1U && s.value[1] == 1U && s.value[2] == 4321U);

	/* picture 2 (the held one) goes to slot 1 */
	CHECK(sdk_media_session_present(SESSION, 0U, &result) == SDK_STATUS_OK);
	CHECK((result.flags & SDK_MEDIA_SESSION_RESULT_OUTPUT_SLOT1) != 0U);
	CHECK((result.flags & SDK_MEDIA_SESSION_RESULT_OUTPUT_PENDING) != 0U);
	CHECK(enqueue_job.dst_addr == slot0 + 4096U && enqueue_job.slot == 1U);
	/* while slot 1 is written, slot 0 is still the newest complete one */
	s = output_page();
	CHECK((s.flags & SDK_MEDIA_OUTPUT_STATUS_COMPLETE) != 0U);
	CHECK((s.flags & SDK_MEDIA_OUTPUT_STATUS_SLOT1) == 0U);
	CHECK((s.flags & SDK_MEDIA_OUTPUT_STATUS_PENDING) != 0U);
	sdk_media_session_output_retired(SESSION, 1, 1000U);
	s = output_page();
	CHECK((s.flags & SDK_MEDIA_OUTPUT_STATUS_SLOT1) != 0U);
	CHECK((s.flags & SDK_MEDIA_OUTPUT_STATUS_PENDING) == 0U);
	CHECK(s.value[0] == 2U && s.value[1] == 2U && s.value[2] == 1000U);

	/* picture 3 is back in slot 0 */
	(void)decode_held();
	CHECK(sdk_media_session_present(SESSION, 0U, &result) == SDK_STATUS_OK);
	CHECK((result.flags & SDK_MEDIA_SESSION_RESULT_OUTPUT_SLOT1) == 0U);
	CHECK(enqueue_job.dst_addr == slot0);
	sdk_media_session_output_retired(SESSION, 1, 1U);
	s = output_page();
	CHECK((s.flags & SDK_MEDIA_OUTPUT_STATUS_SLOT1) == 0U);

	/* a retirement nobody queued changes nothing */
	sdk_media_session_output_retired(SESSION, 1, 99U);
	s = output_page();
	CHECK(s.value[0] == 3U && s.value[2] == 1U);
	CHECK(sdk_media_session_close(SESSION, 0U, &result) == SDK_STATUS_OK);
}

static void test_reconfigure(void)
{
	struct SDKMediaSessionMainResult result;
	struct SDKMediaOutputBinding b = binding(0U, 4096U);
	struct SDKMediaSessionStatusResult s;

	reset();
	begin_session(SDK_VIDEO_OUTPUT_SHARED_BUFFER);
	CHECK(sdk_media_session_output_config(SESSION, &b, &result) ==
	      SDK_STATUS_OK);
	(void)decode_held();
	CHECK(sdk_media_session_present(SESSION, 0U, &result) == SDK_STATUS_OK);
	sdk_media_session_output_retired(SESSION, 1, 10U);
	(void)decode_held();
	CHECK(sdk_media_session_present(SESSION, 0U, &result) == SDK_STATUS_OK);
	CHECK((result.flags & SDK_MEDIA_SESSION_RESULT_OUTPUT_SLOT1) != 0U);
	sdk_media_session_output_retired(SESSION, 1, 10U);

	/* between pictures a reconfigure is accepted: the pictures in the slots
	 * are of the old layout, so none counts as complete, and the next picture
	 * goes to slot 0 whatever slot the last one used */
	b = binding(SDK_MEDIA_OUTPUT_SCALE_NEAREST, 4096U);
	b.config.width = 32U;
	b.config.height = 16U;
	b.config.pitch = 64U;
	CHECK(sdk_media_session_output_config(SESSION, &b, &result) ==
	      SDK_STATUS_OK);
	CHECK(result.bytes_written == 64U * 16U);
	s = output_page();
	CHECK(s.flags == SDK_MEDIA_OUTPUT_STATUS_CONFIGURED);
	CHECK(s.value[0] == 2U && s.value[1] == 2U); /* counters keep counting */
	(void)decode_held();
	CHECK(sdk_media_session_present(SESSION, 0U, &result) == SDK_STATUS_OK);
	CHECK((result.flags & SDK_MEDIA_SESSION_RESULT_OUTPUT_SLOT1) == 0U);
	CHECK(enqueue_job.slot == 0U && enqueue_job.width == 32U &&
	      enqueue_job.flags == SDK_MEDIA_OUTPUT_SCALE_NEAREST);
}

static void test_single_slot(void)
{
	struct SDKMediaSessionMainResult result;
	struct SDKMediaOutputBinding b = binding(0U, 0U);
	unsigned i;

	reset();
	begin_session(SDK_VIDEO_OUTPUT_SHARED_BUFFER);
	CHECK(sdk_media_session_output_config(SESSION, &b, &result) ==
	      SDK_STATUS_OK);
	for (i = 0; i < 3; i++) {
		(void)decode_held();
		CHECK(sdk_media_session_present(SESSION, 0U, &result) ==
		      SDK_STATUS_OK);
		CHECK((result.flags & SDK_MEDIA_SESSION_RESULT_OUTPUT_SLOT1) == 0U);
		CHECK(enqueue_job.slot == 0U &&
		      enqueue_job.dst_addr == 0x08400000U + 16U);
		sdk_media_session_output_retired(SESSION, 1, 1U);
	}
}

static void test_queue_full_and_failures(void)
{
	struct SDKMediaSessionMainResult result;
	struct SDKMediaOutputBinding b = binding(0U, 4096U);
	struct SDKMediaSessionStatusResult s;

	reset();
	begin_session(SDK_VIDEO_OUTPUT_SHARED_BUFFER);
	CHECK(sdk_media_session_output_config(SESSION, &b, &result) ==
	      SDK_STATUS_OK);
	(void)decode_held();

	/* a full task queue leaves the picture held and nothing pending */
	enqueue_accept = 0;
	CHECK(sdk_media_session_present(SESSION, 0U, &result) == SDK_STATUS_BUSY);
	s = output_page();
	CHECK((s.flags & SDK_MEDIA_OUTPUT_STATUS_PENDING) == 0U &&
	      s.value[1] == 0U);
	enqueue_accept = 1;
	CHECK(sdk_media_session_present(SESSION, 0U, &result) == SDK_STATUS_OK);
	CHECK(enqueue_job.slot == 0U); /* the failed try used no slot */

	/* a failed conversion publishes nothing and ends the session's
	 * output with an error, but frees the in-flight marker so CLOSE works */
	sdk_media_session_output_retired(0U, 0, 0U);
	s = output_page();
	CHECK((s.flags & SDK_MEDIA_OUTPUT_STATUS_COMPLETE) == 0U);
	CHECK((s.flags & SDK_MEDIA_OUTPUT_STATUS_PENDING) == 0U);
	CHECK(s.state == SDK_MEDIA_SESSION_STATE_ERROR);
	CHECK(sdk_media_session_present(SESSION, 0U, &result) ==
	      SDK_STATUS_IO_ERROR);
	CHECK(sdk_media_session_close(SESSION, 0U, &result) == SDK_STATUS_OK);
	sdk_media_session_close_retired(SESSION);
	CHECK(overlay_closed_calls == 0U); /* shared sessions skip the overlay */
}

static void test_core1_fault_clears_pending(void)
{
	struct SDKMediaSessionMainResult result;
	struct SDKMediaOutputBinding b = binding(0U, 4096U);

	reset();
	begin_session(SDK_VIDEO_OUTPUT_SHARED_BUFFER);
	CHECK(sdk_media_session_output_config(SESSION, &b, &result) ==
	      SDK_STATUS_OK);
	(void)decode_held();
	CHECK(sdk_media_session_present(SESSION, 0U, &result) == SDK_STATUS_OK);
	CHECK(sdk_media_session_close(SESSION, 0U, &result) == SDK_STATUS_BUSY);
	sdk_media_session_poison_core1();
	/* the faulted worker never retires the task; CLOSE must not wait */
	CHECK(sdk_media_session_close(SESSION, 0U, &result) == SDK_STATUS_OK);
}

static void test_run_output(void)
{
	struct SDKMediaSessionMainResult result;
	struct SDKMediaOutputBinding b;
	uint8_t *picture;
	static uint8_t y[16 * 16], cb[8 * 8], cr[8 * 8];
	struct SDKMediaOutputJob job;
	uint32_t elapsed = 99U;

	/* a job names its slot with a 32-bit address, as on the card */
	picture = mmap(0, 8192, PROT_READ | PROT_WRITE,
	               MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
	CHECK(picture != MAP_FAILED);
	if (picture == MAP_FAILED)
		return;
	reset();
	memset(y, 235, sizeof(y));
	memset(cb, 128, sizeof(cb));
	memset(cr, 128, sizeof(cr));
	memset(&direct_frame, 0, sizeof(direct_frame));
	direct_frame.width = 16;
	direct_frame.height = 16;
	direct_frame.y_pitch = 16;
	direct_frame.chroma_pitch = 8;
	direct_frame.y = y;
	direct_frame.cb = cb;
	direct_frame.cr = cr;
	direct_frame_valid = 1;
	begin_session(SDK_VIDEO_OUTPUT_SHARED_BUFFER);
	(void)result;
	(void)b;

	memset(&job, 0, sizeof(job));
	job.session = SESSION;
	job.dst_addr = (uint32_t)(uintptr_t)picture;
	job.pitch = 128U;
	job.width = 64U;
	job.height = 32U;
	job.format = SDK_SURFACE_FORMAT_RGB565;
	memset(picture, 0, 8192);
	CHECK(sdk_media_session_run_output(&job, &elapsed) == SDK_STATUS_OK);
	CHECK(picture[0] == 0xff && picture[1] == 0xff);
	CHECK(picture[2 * 64 * 32 - 1] == 0xff);
	CHECK(picture[2 * 64 * 32] == 0U); /* nothing written past the picture */
	/* no held decoder frame: BAD_HANDLE, and the elapsed time stays zero */
	direct_frame_valid = 0;
	CHECK(sdk_media_session_run_output(&job, &elapsed) ==
	      SDK_STATUS_BAD_HANDLE && elapsed == 0U);
	munmap(picture, 8192);
}

int main(void)
{
	test_abi();
	test_overlay_session_is_unchanged();
	test_configure();
	test_present_requires_configuration();
	test_slots_alternate_and_pending();
	test_reconfigure();
	test_single_slot();
	test_queue_full_and_failures();
	test_core1_fault_clears_pending();
	test_run_output();
	if (failures != 0) {
		printf("media_shared_output_test: %d failure(s) of %d checks\n",
		       failures, checks);
		return 1;
	}
	printf("media_shared_output_test: %d checks passed\n", checks);
	return 0;
}
