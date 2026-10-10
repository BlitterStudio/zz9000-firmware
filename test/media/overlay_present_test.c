/*
 * Host checks for overlay.c's hand-off of media-session frames.
 *
 * A media PRESENT stays pending until the overlay queues its compose; the
 * session cannot decode, discard or close until then. These checks pin that
 * the overlay releases such a frame when it is closed or hidden before the
 * compose is queued, instead of leaving the session busy for good.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "overlay.h"
#include "memorymap.h"
#include "sdk_video_stream.h"

uint8_t stride_div = 1U;

static uint32_t released[8];
static unsigned released_count;
static unsigned enqueued;
static uint32_t next_surface = 0x10000000U;

void sdk_media_session_present_queued(uint32_t session)
{
	if (released_count < sizeof(released) / sizeof(released[0]))
		released[released_count] = session;
	released_count++;
}

int sdk_mailbox_enqueue_internal(uint32_t op, const void *payload,
                                 uint32_t length)
{
	(void)op;
	(void)payload;
	(void)length;
	enqueued++;
	return 1;
}

uint32_t surface_allocator_alloc(uint32_t size)
{
	uint32_t addr = next_surface;

	next_surface += (size + 0xfffU) & ~0xfffU;
	return addr;
}

void surface_allocator_free(uint32_t addr)
{
	(void)addr;
}

int scheduler_core1_available(void) { return 1; }
int overlay_hw_supported(void) { return 0; }
void overlay_hw_stop(void) {}
void overlay_hw_set_buffer(uint32_t addr) { (void)addr; }

int overlay_hw_start_scaled(uint32_t src, uint16_t pitch, uint16_t src_w,
                            uint16_t src_h, int16_t dst_x, int16_t dst_y,
                            uint16_t dst_w, uint16_t dst_h, uint8_t variant,
                            uint32_t key_rgb, int key_enabled,
                            uint32_t generation)
{
	(void)src; (void)pitch; (void)src_w; (void)src_h; (void)dst_x;
	(void)dst_y; (void)dst_w; (void)dst_h; (void)variant; (void)key_rgb;
	(void)key_enabled; (void)generation;
	return 0;
}

uint32_t sdk_media_profile_now_us(void) { return 0U; }
void sdk_media_profile_record(uint32_t stage, uint32_t start)
{
	(void)stage;
	(void)start;
}

int sdk_image_stream_presented_canvas(uint32_t session, uint32_t *address,
                                      uint32_t *pitch)
{
	(void)session;
	(void)address;
	(void)pitch;
	return 0;
}

int sdk_video_stream_get_direct_frame(uint32_t session,
                                      struct SDKVideoDecodedFrame *frame)
{
	(void)session;
	(void)frame;
	return 0;
}

int sdk_video_yuv420_to_yuy2(uint8_t *dst, uint32_t dst_pitch, uint32_t width,
                             uint32_t height, const uint8_t *y,
                             uint32_t y_pitch, const uint8_t *cb,
                             const uint8_t *cr, uint32_t chroma_pitch,
                             uint32_t *bytes_written)
{
	(void)dst; (void)dst_pitch; (void)width; (void)height; (void)y;
	(void)y_pitch; (void)cb; (void)cr; (void)chroma_pitch;
	(void)bytes_written;
	return 0;
}

/* Compose runs on core 1; the tests only check what gets queued. */
void overlay_composite_frame(uint8_t *dst, uint32_t dst_pitch,
                             const uint8_t *screen, uint32_t screen_pitch,
                             uint16_t scr_w, uint16_t scr_h,
                             uint8_t color_format, const uint8_t *src,
                             uint16_t src_pitch, uint16_t src_w,
                             uint16_t src_h, uint8_t variant, int16_t dst_x,
                             int16_t dst_y, int16_t dst_w, int16_t dst_h,
                             uint32_t key_native, uint8_t key_enabled)
{
	(void)dst; (void)dst_pitch; (void)screen; (void)screen_pitch;
	(void)scr_w; (void)scr_h; (void)color_format; (void)src;
	(void)src_pitch; (void)src_w; (void)src_h; (void)variant; (void)dst_x;
	(void)dst_y; (void)dst_w; (void)dst_h; (void)key_native;
	(void)key_enabled;
}

void overlay_composite_planar420_frame(uint8_t *dst, uint32_t dst_pitch,
	const uint8_t *screen, uint32_t screen_pitch,
	uint16_t scr_w, uint16_t scr_h, uint8_t color_format,
	const uint8_t *y, uint32_t y_pitch, const uint8_t *cb, const uint8_t *cr,
	uint32_t chroma_pitch, uint16_t src_w, uint16_t src_h,
	int16_t dst_x, int16_t dst_y, int16_t dst_w, int16_t dst_h,
	uint32_t key_native, uint8_t key_enabled)
{
	(void)dst; (void)dst_pitch; (void)screen; (void)screen_pitch;
	(void)scr_w; (void)scr_h; (void)color_format; (void)y; (void)y_pitch;
	(void)cb; (void)cr; (void)chroma_pitch; (void)src_w; (void)src_h;
	(void)dst_x; (void)dst_y; (void)dst_w; (void)dst_h; (void)key_native;
	(void)key_enabled;
}

static int failures;

static void check(int cond, const char *what)
{
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", what);
		failures++;
	}
}

static struct ZZ_VIDEO_STATE vs;

static void set_overlay(int active)
{
	struct GFXData data;

	memset(&data, 0, sizeof(data));
	data.offset[1] = 0x00100000U;
	data.pitch[1] = 640U;
	data.x[0] = 16U;
	data.y[0] = 16U;
	data.x[1] = 320U;
	data.y[1] = 240U;
	data.x[2] = 320U;
	data.y[2] = 240U;
	data.user[0] = active ? 2U : 0U;
	data.u8_user[GFXDATA_U8_YUV_VARIANT] = YUV422_VARIANT_CGX;
	overlay_handle_op(&vs, &data);
	check(data.u32_user[0] == 0U, "overlay SET accepted");
}

static void stop_overlay(void)
{
	struct GFXData data;

	memset(&data, 0, sizeof(data));
	data.u8_user[GFXDATA_U8_OVERLAY_SUBCMD] = OVERLAY_SUBCMD_OFF;
	overlay_handle_op(&vs, &data);
}

static void reset(void)
{
	memset(&vs, 0, sizeof(vs));
	vs.framebuffer = (uint32_t *)(uintptr_t)FRAMEBUFFER_ADDRESS;
	vs.colormode = MNTVA_COLOR_32BIT;
	vs.vmode_hsize = 640U;
	vs.vmode_hdiv = 1U;
	vs.vmode_vdma_rows = 480U;
	overlay_amiga_reset(&vs);
	released_count = 0U;
	enqueued = 0U;
}

int main(void)
{
	/* The normal path: the poll queues the compose, then releases. */
	reset();
	set_overlay(1);
	check(overlay_video_frame_ready(7U) == 1, "frame accepted");
	check(released_count == 0U, "held until the compose is queued");
	overlay_main_poll(&vs);
	check(enqueued == 1U && released_count == 1U && released[0] == 7U,
	      "compose queued, then frame released");

	/* The window closes after PRESENT but before the poll. */
	reset();
	set_overlay(1);
	check(overlay_video_frame_ready(7U) == 1, "frame accepted");
	stop_overlay();
	check(enqueued == 0U && released_count == 1U && released[0] == 7U,
	      "closing the overlay releases the pending frame");

	/* The window is hidden (fullscreen toggle) before the poll. */
	reset();
	set_overlay(1);
	check(overlay_video_frame_ready(9U) == 1, "frame accepted");
	set_overlay(0);
	check(enqueued == 0U && released_count == 1U && released[0] == 9U,
	      "hiding the overlay releases the pending frame");
	overlay_main_poll(&vs);
	check(enqueued == 0U, "a hidden overlay composes nothing");
	check(overlay_video_frame_ready(9U) == 0,
	      "a hidden overlay refuses new frames");

	/* A geometry change that keeps the overlay visible keeps the frame. */
	reset();
	set_overlay(1);
	check(overlay_video_frame_ready(5U) == 1, "frame accepted");
	set_overlay(1);
	check(released_count == 0U, "a visible re-SET keeps the frame pending");
	overlay_main_poll(&vs);
	check(enqueued == 1U && released_count == 1U && released[0] == 5U,
	      "the re-SET overlay composes and releases");

	if (failures) {
		fprintf(stderr, "overlay_present_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("overlay_present_test: all checks passed\n");
	return 0;
}
