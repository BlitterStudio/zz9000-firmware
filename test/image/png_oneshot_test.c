/*
 * One-shot DECODE_PNG (0x0402) through the production mailbox dispatcher.
 *
 * sdk_mailbox.c is compiled for the host with the real session decoder
 * (sdk_image_stream.c, libpng) behind it. Only the hardware seams are
 * replaced: the surface allocator, the scheduler queue and the CPU id.
 * Every request runs twice -- inline on core 0 (no worker) and deferred
 * (the request is queued and the test plays the core-1 worker, slice by
 * slice, through the same sdk_mailbox_run_offload_task the firmware
 * uses) -- and a successful decode must match, byte for byte, the same
 * file decoded through a session.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <png.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "memorymap.h"
#include "scheduler.h"
#include "sdk_aperture_layout.h"
#include "sdk_image_stream.h"
#include "sdk_mailbox.h"

struct SDKMailboxEntry {
	uint8_t request_id[4];
	uint8_t opcode[2];
	uint8_t status[2];
	uint8_t flags[2];
	uint8_t payload_len[2];
	uint8_t user_cookie[4];
	uint8_t payload[48];
};

extern uint16_t sdk_mailbox_test_handle_request(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp);
extern int sdk_mailbox_task_gen_ok(int slot);
extern void sdk_mailbox_test_set_batch_tail(int batch_tail);
extern void sdk_mailbox_task_forget(int slot);

void host_runtime_reset(void);
size_t host_runtime_allocated_bytes(void);

static int checks;
static int failures;

#define CHECK(expr) do { \
	checks++; \
	if (!(expr)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
	} \
} while (0)

#define CHECK_EQ(actual, expected) do { \
	unsigned long a_ = (unsigned long)(actual); \
	unsigned long e_ = (unsigned long)(expected); \
	checks++; \
	if (a_ != e_) { \
		failures++; \
		printf("FAIL %s:%d: %s == %lu, expected %lu\n", __FILE__, \
		       __LINE__, #actual, a_, e_); \
	} \
} while (0)

/* ---- hardware seams ---- */

#define WINDOW_SIZE 0x10000U
#define MAX_BLOCKS 16U

static uint8_t *g_window;
static int g_core1;       /* scheduler_core1_available() */
static int g_cpu;         /* smp_cpu_id() */

static struct {
	uint8_t *base;
	uint32_t length;
	int live;
} g_block[MAX_BLOCKS];

static taskq_shared_t g_shared;
taskq_shared_t *scheduler_shared(void) { return &g_shared; }
int scheduler_core1_available(void) { return g_core1; }
void scheduler_core1_publish(void) {}
int smp_cpu_id(void) { return g_cpu; }
uint32_t smp_local_irq_save(void) { return 0U; }
void smp_local_irq_restore(uint32_t saved) { (void)saved; }

uint32_t sdk_aperture_runtime_flags(void) { return SDK_APERTURE_FLAG_VALID; }
int sdk_aperture_runtime_is_zorro3(void) { return 0; }
int sdk_aperture_runtime_is_legacy(void) { return 0; }
uint32_t sdk_aperture_host_window_address(void)
{
	return (uint32_t)(uintptr_t)g_window;
}
uint32_t sdk_aperture_host_window_size(void) { return WINDOW_SIZE; }

uint32_t surface_allocator_total_bytes(void) { return 64U * 1024U * 1024U; }

uint32_t surface_allocator_alloc_aligned(uint32_t size, uint32_t alignment)
{
	unsigned i;
	void *p;

	(void)alignment;
	for (i = 0U; i < MAX_BLOCKS; i++) {
		if (g_block[i].live)
			continue;
		p = mmap(NULL, size, PROT_READ | PROT_WRITE,
		         MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
		if (p == MAP_FAILED)
			return 0U;
		g_block[i].base = p;
		g_block[i].length = size;
		g_block[i].live = 1;
		return (uint32_t)(uintptr_t)p;
	}
	return 0U;
}

int surface_allocator_free(uint32_t addr)
{
	unsigned i;

	for (i = 0U; i < MAX_BLOCKS; i++) {
		if (g_block[i].live &&
		    (uint32_t)(uintptr_t)g_block[i].base == addr) {
			munmap(g_block[i].base, g_block[i].length);
			g_block[i].live = 0;
			return 1;
		}
	}
	return 0;
}

int surface_allocator_owns(uint32_t addr, uint32_t length)
{
	unsigned i;

	for (i = 0U; i < MAX_BLOCKS; i++) {
		uint32_t base = (uint32_t)(uintptr_t)g_block[i].base;

		if (g_block[i].live && addr >= base &&
		    length <= g_block[i].length &&
		    addr - base <= g_block[i].length - length)
			return 1;
	}
	return 0;
}

/* ---- request plumbing ---- */

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static struct SDKMailboxEntry g_comp;

static uint16_t call(uint16_t opcode, const uint8_t payload[48],
                     uint16_t payload_len)
{
	struct SDKMailboxEntry req;

	memset(&req, 0, sizeof(req));
	memset(&g_comp, 0, sizeof(g_comp));
	put32(req.request_id, 1U);
	req.opcode[0] = (uint8_t)(opcode >> 8);
	req.opcode[1] = (uint8_t)opcode;
	req.payload_len[0] = (uint8_t)(payload_len >> 8);
	req.payload_len[1] = (uint8_t)payload_len;
	if (payload != NULL)
		memcpy(req.payload, payload, 48U);
	return sdk_mailbox_test_handle_request(&req, &g_comp);
}

struct buffer {
	uint32_t handle;
	uint8_t *ptr;
	uint32_t length;
};

static struct buffer alloc_buffer(uint32_t length)
{
	uint8_t payload[48];
	struct buffer b = { 0U, NULL, 0U };

	memset(payload, 0, sizeof(payload));
	put32(payload, length);
	put32(payload + 4, 64U);
	if (call(SDK_OP_ALLOC_SHARED, payload, 48U) == SDK_STATUS_OK) {
		b.handle = get32(g_comp.payload);
		b.ptr = (uint8_t *)(uintptr_t)get32(g_comp.payload + 4);
		b.length = length;
	}
	return b;
}

static void free_buffer(const struct buffer *b)
{
	uint8_t payload[48];

	memset(payload, 0, sizeof(payload));
	put32(payload, b->handle);
	CHECK_EQ(call(SDK_OP_FREE_SHARED, payload, 48U), SDK_STATUS_OK);
}

struct surface {
	uint32_t handle;
	uint8_t *ptr;
	uint32_t width, height, pitch, format, length;
};

static struct surface alloc_surface(uint32_t width, uint32_t height,
                                    uint32_t format)
{
	uint8_t payload[48];
	struct surface s;

	memset(&s, 0, sizeof(s));
	memset(payload, 0, sizeof(payload));
	put32(payload, width);
	put32(payload + 4, height);
	put32(payload + 8, format);
	if (call(SDK_OP_ALLOC_SURFACE, payload, 48U) == SDK_STATUS_OK) {
		s.handle = get32(g_comp.payload);
		s.ptr = (uint8_t *)(uintptr_t)get32(g_comp.payload + 4);
		s.width = get32(g_comp.payload + 8);
		s.height = get32(g_comp.payload + 12);
		s.pitch = get32(g_comp.payload + 16);
		s.format = get32(g_comp.payload + 20);
		s.length = get32(g_comp.payload + 28);
	}
	return s;
}

static void free_surface(const struct surface *s)
{
	uint8_t payload[48];

	memset(payload, 0, sizeof(payload));
	put32(payload, s->handle);
	CHECK_EQ(call(SDK_OP_FREE_SURFACE, payload, 48U), SDK_STATUS_OK);
}

/* ---- PNG fixtures ---- */

struct membuf {
	uint8_t *data;
	size_t len;
	size_t cap;
};

static void mem_write(png_structp png, png_bytep data, png_size_t length)
{
	struct membuf *mb = png_get_io_ptr(png);

	if (mb->len + length > mb->cap) {
		mb->cap = (mb->len + length) * 2U;
		mb->data = realloc(mb->data, mb->cap);
	}
	memcpy(mb->data + mb->len, data, length);
	mb->len += length;
}

static void mem_flush(png_structp png) { (void)png; }

enum pattern { PATTERN_GRADIENT, PATTERN_NOISE };

static uint8_t pix(enum pattern pattern, uint32_t x, uint32_t y, uint32_t c)
{
	uint32_t h;

	if (pattern == PATTERN_GRADIENT)
		return (uint8_t)((x * (13U + c * 4U) + y * (7U + c * 6U)) ^
		                 (x * y));
	/* an integer hash: incompressible for practical purposes */
	h = x * 374761393U + y * 668265263U + c * 2246822519U + 1013904223U;
	h = (h ^ (h >> 15)) * 2246822519U;
	h = (h ^ (h >> 13)) * 3266489917U;
	return (uint8_t)((h ^ (h >> 16)) >> 8);
}

static size_t make_png(uint8_t **out, uint32_t width, uint32_t height,
                       int color_type, int interlaced,
                       enum pattern pattern)
{
	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL,
	                                          NULL, NULL);
	png_infop info = png_create_info_struct(png);
	struct membuf mb = { NULL, 0U, 0U };
	uint32_t channels = color_type == PNG_COLOR_TYPE_RGB_ALPHA ? 4U : 3U;
	uint8_t *image = malloc((size_t)width * height * channels);
	uint32_t x, y, c;
	int passes, pass;

	if (setjmp(png_jmpbuf(png))) {
		printf("png write failed\n");
		exit(2);
	}
	for (y = 0U; y < height; y++)
		for (x = 0U; x < width; x++)
			for (c = 0U; c < channels; c++)
				image[((size_t)y * width + x) * channels + c] =
					pix(pattern, x, y, c);
	png_set_write_fn(png, &mb, mem_write, mem_flush);
	png_set_IHDR(png, info, width, height, 8, color_type,
	             interlaced ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE,
	             PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png, info);
	passes = interlaced ? png_set_interlace_handling(png) : 1;
	for (pass = 0; pass < passes; pass++)
		for (y = 0U; y < height; y++)
			png_write_row(png, image + (size_t)y * width * channels);
	png_write_end(png, info);
	png_destroy_write_struct(&png, &info);
	free(image);
	*out = mb.data;
	return mb.len;
}

/* ---- the same file through a session ---- */

static int session_decode(const uint8_t *png, size_t length,
                          uint32_t output_format, uint32_t x, uint32_t y,
                          uint32_t bound_w, uint32_t bound_h, uint32_t flags,
                          uint8_t *dst, uint32_t pitch, uint32_t surface_h)
{
	struct SDKImageStreamBegin begin;
	struct SDKImageStreamResult result;
	size_t offset = 0U;

	memset(&begin, 0, sizeof(begin));
	begin.codec = SDK_IMAGE_CODEC_PNG;
	begin.output_mode = SDK_IMAGE_OUTPUT_SURFACE;
	begin.dst_surface = 0x1234U;
	begin.dst_x = x;
	begin.dst_y = y;
	begin.dst_width = bound_w;
	begin.dst_height = bound_h;
	begin.output_format = output_format;
	begin.flags = flags;
	begin.dst_address = (uintptr_t)dst;
	begin.dst_pitch = pitch;
	begin.dst_length = pitch * surface_h;
	if (sdk_image_stream_begin(&begin, &result) != SDK_STATUS_OK)
		return 0;
	for (;;) {
		struct SDKImageStreamFeed feed;
		size_t chunk = length - offset;
		uint16_t status;

		if (chunk > SDK_IMAGE_STREAM_PNG_MAX_FEED_BYTES)
			chunk = SDK_IMAGE_STREAM_PNG_MAX_FEED_BYTES;
		memset(&feed, 0, sizeof(feed));
		feed.session = result.session;
		feed.src_handle = 1U;
		feed.src_length = (uint32_t)chunk;
		feed.flags = offset + chunk == length ?
			SDK_IMAGE_SESSION_FEED_EOF : 0U;
		status = sdk_image_stream_feed(&feed, png + offset, &result);
		offset += chunk;
		if (status != SDK_STATUS_OK) {
			(void)sdk_image_stream_close(result.session);
			return 0;
		}
		if (result.state == SDK_IMAGE_SESSION_STATE_COMPLETE)
			break;
		if (offset == length) {
			(void)sdk_image_stream_close(result.session);
			return 0;
		}
	}
	return sdk_image_stream_close(result.session) == SDK_STATUS_OK;
}

/* ---- running one request ---- */

struct reply {
	uint16_t status;
	uint8_t payload[48];
	unsigned slices;     /* worker slices run (deferred mode) */
	int deferred;
};

static struct reply g_posted;
static int g_posted_count;

static int post(int slot, uint32_t request_id, uint32_t user_cookie,
                uint16_t opcode, uint16_t status, const uint8_t *payload,
                uint16_t payload_len)
{
	/* sdk_mailbox_post_deferred retires the task's client pins */
	sdk_mailbox_task_forget(slot);
	(void)request_id;
	(void)user_cookie;
	(void)opcode;
	g_posted.status = status;
	memset(g_posted.payload, 0, sizeof(g_posted.payload));
	if (payload != NULL && payload_len != 0U)
		memcpy(g_posted.payload, payload, payload_len);
	g_posted_count++;
	return 1;
}

/* The core-1 worker: claim, run one slice, requeue or complete. */
static void run_worker(struct reply *reply)
{
	int slot;
	unsigned guard = 0U;

	g_cpu = 1;
	while ((slot = taskq_claim_any(&g_shared.queue)) >= 0 && guard++ < 4096U) {
		taskq_desc_t *d = &g_shared.queue.descs[slot];
		uint8_t payload[TASKQ_RESULT_PAYLOAD];
		uint32_t len = 0U;
		uint16_t status = sdk_mailbox_run_offload_task(d, payload, &len);

		reply->slices++;
		if (status == SDK_TASK_IN_PROGRESS)
			taskq_requeue(&g_shared.queue, slot);
		else
			taskq_complete(&g_shared.queue, slot, status, payload,
			               (uint16_t)len);
	}
	g_cpu = 0;
	g_posted_count = 0;
	scheduler_harvest_post_with(&g_shared.queue, NULL,
	                            SDK_STATUS_INTERNAL_ERROR,
	                            sdk_mailbox_task_gen_ok, post,
	                            sdk_mailbox_task_forget);
	CHECK_EQ(g_posted_count, 1);
	reply->status = g_posted.status;
	memcpy(reply->payload, g_posted.payload, sizeof(reply->payload));
}

static struct reply decode_request(uint16_t opcode, int deferred,
                                   const uint8_t payload[48])
{
	struct reply reply;
	uint16_t status;

	memset(&reply, 0, sizeof(reply));
	taskq_shared_boot_init(&g_shared);
	g_core1 = deferred;
	sdk_mailbox_test_set_batch_tail(deferred);
	status = call(opcode, payload, 48U);
	if (status == SDK_STATUS_QUEUED) {
		reply.deferred = 1;
		run_worker(&reply);
	} else {
		reply.status = status;
		memcpy(reply.payload, g_comp.payload, sizeof(reply.payload));
	}
	g_core1 = 0;
	return reply;
}

static void decode_payload(uint8_t payload[48], uint32_t src_handle,
                           uint32_t src_offset, uint32_t src_length,
                           uint32_t dst_surface, uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h, uint32_t format,
                           uint32_t flags)
{
	memset(payload, 0, 48U);
	put32(payload, src_handle);
	put32(payload + 4, src_offset);
	put32(payload + 8, src_length);
	put32(payload + 12, dst_surface);
	put32(payload + 16, x);
	put32(payload + 20, y);
	put32(payload + 24, w);
	put32(payload + 28, h);
	put32(payload + 32, format);
	put32(payload + 36, flags);
}

static uint32_t bytes_per_pixel(uint32_t format)
{
	return format == SDK_SURFACE_FORMAT_RGB888 ? 3U : 4U;
}

/*
 * Decode `png` into a fresh surface of surface_w x surface_h through
 * DECODE_PNG. A successful decode is compared with the session path,
 * and optionally with the generator pattern (rgba = 4-channel source).
 */
static struct reply decode_case(const uint8_t *png, size_t length,
                                uint32_t surface_w, uint32_t surface_h,
                                uint32_t format, uint32_t x, uint32_t y,
                                uint32_t req_w, uint32_t req_h,
                                uint32_t flags, int deferred,
                                struct surface *keep)
{
	struct buffer src = alloc_buffer((uint32_t)length + 64U);
	struct surface dst = alloc_surface(surface_w, surface_h, format);
	uint8_t payload[48];
	struct reply reply;
	size_t baseline = host_runtime_allocated_bytes();

	CHECK(src.ptr != NULL);
	CHECK(dst.ptr != NULL);
	memcpy(src.ptr + 16, png, length);
	memset(dst.ptr, 0xa5, dst.length);
	decode_payload(payload, src.handle, 16U, (uint32_t)length, dst.handle,
	               x, y, req_w, req_h, format, flags);
	reply = decode_request(SDK_OP_DECODE_PNG, deferred, payload);
	CHECK_EQ(sdk_image_stream_active_count(), 0U);
	CHECK_EQ(host_runtime_allocated_bytes(), baseline);

	if (reply.status == SDK_STATUS_OK) {
		CHECK(deferred ? reply.deferred : !reply.deferred);
		/* the session path, with the same bounds the one-shot derived */
		uint32_t bound_w = req_w == 0U ? surface_w : x + req_w;
		uint32_t bound_h = req_h == 0U ? surface_h : y + req_h;
		uint8_t *ref = malloc(dst.length);

		memset(ref, 0xa5, dst.length);
		CHECK(session_decode(png, length, format, x, y, bound_w, bound_h,
		                     flags, ref, dst.pitch, dst.height));
		CHECK(memcmp(ref, dst.ptr, dst.length) == 0);
		free(ref);
		CHECK_EQ(get32(reply.payload + 8), format);
		CHECK_EQ(get32(reply.payload + 12), 0U);
		CHECK_EQ(get32(reply.payload + 16),
		         get32(reply.payload) * get32(reply.payload + 4) *
		         bytes_per_pixel(format));
	}
	free_buffer(&src);
	if (keep) {
		*keep = dst;
	} else {
		free_surface(&dst);
	}
	CHECK_EQ(sdk_image_stream_active_count(), 0U);
	return reply;
}

/* ---- tests ---- */

static void check_pixel(const uint8_t *pixel, uint32_t format,
                        enum pattern pattern, uint32_t x, uint32_t y,
                        int rgba)
{
	uint8_t r = pix(pattern, x, y, 0U);
	uint8_t g = pix(pattern, x, y, 1U);
	uint8_t b = pix(pattern, x, y, 2U);
	uint8_t a = rgba ? pix(pattern, x, y, 3U) : 0xffU;

	switch (format) {
	case SDK_SURFACE_FORMAT_BGRA8888:
		CHECK(pixel[0] == b && pixel[1] == g && pixel[2] == r &&
		      pixel[3] == a);
		break;
	case SDK_SURFACE_FORMAT_RGBA8888:
		CHECK(pixel[0] == r && pixel[1] == g && pixel[2] == b &&
		      pixel[3] == a);
		break;
	case SDK_SURFACE_FORMAT_ARGB8888:
		CHECK(pixel[0] == a && pixel[1] == r && pixel[2] == g &&
		      pixel[3] == b);
		break;
	default:
		CHECK(pixel[0] == r && pixel[1] == g && pixel[2] == b);
		break;
	}
}

static void test_formats(int deferred)
{
	static const uint32_t formats[] = {
		SDK_SURFACE_FORMAT_BGRA8888, SDK_SURFACE_FORMAT_ARGB8888,
		SDK_SURFACE_FORMAT_RGBA8888, SDK_SURFACE_FORMAT_RGB888
	};
	uint8_t *png;
	size_t length = make_png(&png, 37U, 23U, PNG_COLOR_TYPE_RGB_ALPHA, 0,
	                         PATTERN_GRADIENT);
	unsigned f;

	for (f = 0U; f < 4U; f++) {
		struct surface dst;
		struct reply r = decode_case(png, length, 37U, 23U, formats[f],
		                             0U, 0U, 0U, 0U, 0U, deferred, &dst);
		uint32_t bpp = bytes_per_pixel(formats[f]);

		CHECK_EQ(r.status, SDK_STATUS_OK);
		CHECK_EQ(get32(r.payload), 37U);
		CHECK_EQ(get32(r.payload + 4), 23U);
		/* the decoded pixels against the generator, not only the session */
		if (r.status == SDK_STATUS_OK) {
			uint32_t px, py;

			for (py = 0U; py < 23U; py += 5U)
				for (px = 0U; px < 37U; px += 3U)
					check_pixel(dst.ptr + py * dst.pitch + px * bpp,
					            formats[f], PATTERN_GRADIENT, px, py,
					            formats[f] != SDK_SURFACE_FORMAT_RGB888);
		}
		free_surface(&dst);
	}
	free(png);
}

static void test_offset_and_bounds(int deferred)
{
	uint8_t *png;
	size_t length = make_png(&png, 20U, 11U, PNG_COLOR_TYPE_RGB_ALPHA, 0,
	                         PATTERN_GRADIENT);
	struct reply r;
	struct surface dst;

	/* placed at (3, 2) inside a larger surface, whole-surface bounds */
	r = decode_case(png, length, 40U, 30U, SDK_SURFACE_FORMAT_BGRA8888,
	                3U, 2U, 0U, 0U, 0U, deferred, &dst);
	CHECK_EQ(r.status, SDK_STATUS_OK);
	if (r.status == SDK_STATUS_OK) {
		CHECK(dst.ptr[0] == 0xa5U);   /* outside the image: untouched */
		check_pixel(dst.ptr + 2U * dst.pitch + 3U * 4U,
		            SDK_SURFACE_FORMAT_BGRA8888, PATTERN_GRADIENT, 0U, 0U,
		            1);
		check_pixel(dst.ptr + 12U * dst.pitch + 22U * 4U,
		            SDK_SURFACE_FORMAT_BGRA8888, PATTERN_GRADIENT, 19U, 10U,
		            1);
		CHECK(dst.ptr[13U * dst.pitch] == 0xa5U);
	}
	free_surface(&dst);

	/* an explicit rect that fits, then one the image does not fit */
	r = decode_case(png, length, 40U, 30U, SDK_SURFACE_FORMAT_BGRA8888,
	                3U, 2U, 20U, 11U, 0U, deferred, NULL);
	CHECK_EQ(r.status, SDK_STATUS_OK);
	r = decode_case(png, length, 40U, 30U, SDK_SURFACE_FORMAT_BGRA8888,
	                3U, 2U, 19U, 11U, 0U, deferred, NULL);
	CHECK_EQ(r.status, SDK_STATUS_BAD_REQUEST);
	/* the surface itself is too small */
	r = decode_case(png, length, 19U, 11U, SDK_SURFACE_FORMAT_BGRA8888,
	                0U, 0U, 0U, 0U, 0U, deferred, NULL);
	CHECK_EQ(r.status, SDK_STATUS_BAD_REQUEST);
	/* an origin beyond the surface */
	r = decode_case(png, length, 40U, 30U, SDK_SURFACE_FORMAT_BGRA8888,
	                41U, 0U, 0U, 0U, 0U, deferred, NULL);
	CHECK_EQ(r.status, SDK_STATUS_BAD_REQUEST);
	free(png);
}

static void test_interlaced(int deferred)
{
	uint8_t *png;
	size_t length = make_png(&png, 41U, 29U, PNG_COLOR_TYPE_RGB, 1,
	                         PATTERN_NOISE);
	struct surface dst;
	struct reply r = decode_case(png, length, 41U, 29U,
	                             SDK_SURFACE_FORMAT_BGRA8888, 0U, 0U, 0U,
	                             0U, 0U, deferred, &dst);

	CHECK_EQ(r.status, SDK_STATUS_OK);
	if (r.status == SDK_STATUS_OK) {
		uint32_t px, py;

		for (py = 0U; py < 29U; py++)
			for (px = 0U; px < 41U; px++)
				check_pixel(dst.ptr + py * dst.pitch + px * 4U,
				            SDK_SURFACE_FORMAT_BGRA8888, PATTERN_NOISE,
				            px, py, 0);
	}
	free_surface(&dst);
	r = decode_case(png, length, 41U, 29U, SDK_SURFACE_FORMAT_RGB888, 0U,
	                0U, 0U, 0U, 0U, deferred, NULL);
	CHECK_EQ(r.status, SDK_STATUS_OK);
	free(png);
}

/* Incompressible, larger than one input slice: the worker needs several. */
static void test_multi_slice(int deferred)
{
	uint8_t *png;
	size_t length = make_png(&png, 200U, 180U, PNG_COLOR_TYPE_RGB, 0,
	                         PATTERN_NOISE);
	struct surface dst;
	struct reply r;

	CHECK(length > SDK_IMAGE_STREAM_PNG_ONESHOT_SLICE_BYTES);
	r = decode_case(png, length, 200U, 180U, SDK_SURFACE_FORMAT_BGRA8888,
	                0U, 0U, 0U, 0U, 0U, deferred, &dst);
	CHECK_EQ(r.status, SDK_STATUS_OK);
	if (deferred) {
		unsigned expected = (unsigned)((length +
			SDK_IMAGE_STREAM_PNG_ONESHOT_SLICE_BYTES - 1U) /
			SDK_IMAGE_STREAM_PNG_ONESHOT_SLICE_BYTES);

		CHECK(expected >= 2U);
		CHECK_EQ(r.slices, expected);
	}
	if (r.status == SDK_STATUS_OK) {
		check_pixel(dst.ptr + 179U * dst.pitch + 199U * 4U,
		            SDK_SURFACE_FORMAT_BGRA8888, PATTERN_NOISE, 199U, 179U,
		            0);
		check_pixel(dst.ptr + 90U * dst.pitch + 7U * 4U,
		            SDK_SURFACE_FORMAT_BGRA8888, PATTERN_NOISE, 7U, 90U, 0);
	}
	free_surface(&dst);
	free(png);
}

static void test_rejections(int deferred)
{
	uint8_t *png;
	size_t length = make_png(&png, 16U, 16U, PNG_COLOR_TYPE_RGB_ALPHA, 0,
	                         PATTERN_GRADIENT);
	uint8_t garbage[64];
	uint8_t *copy;
	struct reply r;
	uint8_t payload[48];
	struct buffer src;
	struct surface dst;

	/* FIT scaling is not available for PNG, exactly as for a session */
	r = decode_case(png, length, 16U, 16U, SDK_SURFACE_FORMAT_BGRA8888,
	                0U, 0U, 0U, 0U, SDK_IMAGE_DECODE_FLAG_FIT, deferred,
	                NULL);
	CHECK_EQ(r.status, SDK_STATUS_UNSUPPORTED);
	r = decode_case(png, length, 16U, 16U, SDK_SURFACE_FORMAT_BGRA8888,
	                0U, 0U, 0U, 0U,
	                SDK_IMAGE_DECODE_FLAG_FIT |
	                SDK_IMAGE_DECODE_FLAG_PRESERVE_ASPECT, deferred, NULL);
	CHECK_EQ(r.status, SDK_STATUS_UNSUPPORTED);
	/* unknown flag bits */
	r = decode_case(png, length, 16U, 16U, SDK_SURFACE_FORMAT_BGRA8888,
	                0U, 0U, 0U, 0U, 1U << 9, deferred, NULL);
	CHECK_EQ(r.status, SDK_STATUS_BAD_REQUEST);
	/* PRESERVE_ASPECT without FIT changes nothing, as in a session */
	r = decode_case(png, length, 16U, 16U, SDK_SURFACE_FORMAT_BGRA8888,
	                0U, 0U, 0U, 0U, SDK_IMAGE_DECODE_FLAG_PRESERVE_ASPECT,
	                deferred, NULL);
	CHECK_EQ(r.status, SDK_STATUS_OK);

	/* a PNG without its end: the same IO error a session reports */
	r = decode_case(png, length - 12U, 16U, 16U,
	                SDK_SURFACE_FORMAT_BGRA8888, 0U, 0U, 0U, 0U, 0U,
	                deferred, NULL);
	CHECK_EQ(r.status, SDK_STATUS_IO_ERROR);
	r = decode_case(png, length / 2U, 16U, 16U,
	                SDK_SURFACE_FORMAT_BGRA8888, 0U, 0U, 0U, 0U, 0U,
	                deferred, NULL);
	CHECK_EQ(r.status, SDK_STATUS_IO_ERROR);
	/* not a PNG at all */
	memset(garbage, 0x5a, sizeof(garbage));
	r = decode_case(garbage, sizeof(garbage), 16U, 16U,
	                SDK_SURFACE_FORMAT_BGRA8888, 0U, 0U, 0U, 0U, 0U,
	                deferred, NULL);
	CHECK_EQ(r.status, SDK_STATUS_IO_ERROR);
	/* a corrupted IDAT */
	copy = malloc(length);
	memcpy(copy, png, length);
	copy[length / 2U] ^= 0xffU;
	copy[length / 2U + 1U] ^= 0xffU;
	copy[length / 2U + 2U] ^= 0xffU;
	r = decode_case(copy, length, 16U, 16U, SDK_SURFACE_FORMAT_BGRA8888,
	                0U, 0U, 0U, 0U, 0U, deferred, NULL);
	CHECK_EQ(r.status, SDK_STATUS_IO_ERROR);
	free(copy);

	/* request validation shared with JPEG */
	src = alloc_buffer(256U);
	dst = alloc_surface(16U, 16U, SDK_SURFACE_FORMAT_BGRA8888);
	memcpy(src.ptr, png, 128U);
	decode_payload(payload, src.handle, 0U, 0U, dst.handle, 0U, 0U, 0U,
	               0U, SDK_SURFACE_FORMAT_BGRA8888, 0U);
	r = decode_request(SDK_OP_DECODE_PNG, deferred, payload);
	CHECK_EQ(r.status, SDK_STATUS_BAD_REQUEST);           /* empty source */
	decode_payload(payload, src.handle, 200U, 100U, dst.handle, 0U, 0U,
	               0U, 0U, SDK_SURFACE_FORMAT_BGRA8888, 0U);
	r = decode_request(SDK_OP_DECODE_PNG, deferred, payload);
	CHECK_EQ(r.status, SDK_STATUS_BAD_REQUEST);           /* range */
	decode_payload(payload, src.handle, 0U, 128U, dst.handle, 0U, 0U, 0U,
	               0U, SDK_SURFACE_FORMAT_RGBA8888, 0U);
	r = decode_request(SDK_OP_DECODE_PNG, deferred, payload);
	CHECK_EQ(r.status, SDK_STATUS_UNSUPPORTED);        /* format != surface */
	decode_payload(payload, 0x7777U, 0U, 128U, dst.handle, 0U, 0U, 0U, 0U,
	               SDK_SURFACE_FORMAT_BGRA8888, 0U);
	r = decode_request(SDK_OP_DECODE_PNG, deferred, payload);
	CHECK_EQ(r.status, SDK_STATUS_BAD_HANDLE);
	decode_payload(payload, src.handle, 0U, 128U, 0x7777U, 0U, 0U, 0U, 0U,
	               SDK_SURFACE_FORMAT_BGRA8888, 0U);
	r = decode_request(SDK_OP_DECODE_PNG, deferred, payload);
	CHECK_EQ(r.status, SDK_STATUS_BAD_HANDLE);
	CHECK_EQ(sdk_image_stream_active_count(), 0U);
	free_surface(&dst);
	free_buffer(&src);
	free(png);
}

/* GIF has no decoder: it stays UNSUPPORTED whether or not core 1 is up. */
static void test_gif_unsupported(int deferred)
{
	uint8_t *png;
	size_t length = make_png(&png, 8U, 8U, PNG_COLOR_TYPE_RGB, 0,
	                         PATTERN_GRADIENT);
	struct buffer src = alloc_buffer((uint32_t)length);
	struct surface dst = alloc_surface(8U, 8U, SDK_SURFACE_FORMAT_BGRA8888);
	uint8_t payload[48];
	struct reply r;

	memcpy(src.ptr, png, length);
	decode_payload(payload, src.handle, 0U, (uint32_t)length, dst.handle,
	               0U, 0U, 0U, 0U, SDK_SURFACE_FORMAT_BGRA8888, 0U);
	r = decode_request(SDK_OP_DECODE_GIF, 0, payload);
	CHECK_EQ(r.status, SDK_STATUS_UNSUPPORTED);
	(void)deferred;
	free_surface(&dst);
	free_buffer(&src);
	free(png);
}

/* A core-1 fault poisons the worker's sessions: a one-shot decode in
 * flight leaves no slot behind, while a client session is kept failed
 * for its owner to close. */
static void test_fault_poison(void)
{
	uint8_t *png;
	size_t length = make_png(&png, 200U, 180U, PNG_COLOR_TYPE_RGB, 0,
	                         PATTERN_NOISE);
	struct buffer src = alloc_buffer((uint32_t)length);
	struct surface dst = alloc_surface(200U, 180U,
	                                   SDK_SURFACE_FORMAT_BGRA8888);
	struct SDKImageStreamBegin begin;
	struct SDKImageStreamResult result;
	uint8_t payload[48];
	uint8_t out[TASKQ_RESULT_PAYLOAD];
	uint32_t out_len = 0U;
	uint16_t status;
	int slot;
	size_t baseline = host_runtime_allocated_bytes();

	memcpy(src.ptr, png, length);
	taskq_shared_boot_init(&g_shared);
	g_core1 = 1;
	sdk_mailbox_test_set_batch_tail(1);
	decode_payload(payload, src.handle, 0U, (uint32_t)length, dst.handle,
	               0U, 0U, 0U, 0U, SDK_SURFACE_FORMAT_BGRA8888, 0U);
	CHECK_EQ(call(SDK_OP_DECODE_PNG, payload, 48U), SDK_STATUS_QUEUED);

	/* a client session on core 1 as well */
	memset(&begin, 0, sizeof(begin));
	begin.codec = SDK_IMAGE_CODEC_PNG;
	begin.output_mode = SDK_IMAGE_OUTPUT_SURFACE;
	begin.dst_surface = dst.handle;
	begin.dst_width = 200U;
	begin.dst_height = 180U;
	begin.output_format = SDK_SURFACE_FORMAT_BGRA8888;
	begin.dst_address = (uintptr_t)dst.ptr;
	begin.dst_pitch = dst.pitch;
	begin.dst_length = dst.length;
	begin.core1_affine = 1U;
	CHECK_EQ(sdk_image_stream_begin(&begin, &result), SDK_STATUS_OK);

	g_cpu = 1;
	slot = taskq_claim_any(&g_shared.queue);
	CHECK(slot >= 0);
	status = sdk_mailbox_run_offload_task(&g_shared.queue.descs[slot], out,
	                                      &out_len);
	CHECK_EQ(status, SDK_TASK_IN_PROGRESS);
	CHECK_EQ(sdk_image_stream_active_count(), 2U);   /* one-shot + client */
	taskq_requeue(&g_shared.queue, slot);

	sdk_image_stream_poison_core1_sessions();
	CHECK_EQ(sdk_image_stream_active_count(), 1U);   /* the client's */
	CHECK_EQ(sdk_image_stream_release_poisoned(result.session),
	         SDK_STATUS_OK);
	CHECK_EQ(sdk_image_stream_active_count(), 0U);

	/* the task's next slice finds its session gone and fails */
	slot = taskq_claim_any(&g_shared.queue);
	CHECK(slot >= 0);
	status = sdk_mailbox_run_offload_task(&g_shared.queue.descs[slot], out,
	                                      &out_len);
	CHECK(status != SDK_STATUS_OK && status != SDK_TASK_IN_PROGRESS);
	taskq_complete(&g_shared.queue, slot, status, out, 0U);
	g_cpu = 0;
	scheduler_harvest_post_with(&g_shared.queue, NULL,
	                            SDK_STATUS_INTERNAL_ERROR,
	                            sdk_mailbox_task_gen_ok, post,
	                            sdk_mailbox_task_forget);
	g_core1 = 0;
	CHECK_EQ(sdk_image_stream_active_count(), 0U);
	CHECK_EQ(host_runtime_allocated_bytes(), baseline);
	free_surface(&dst);
	free_buffer(&src);
	free(png);
}

static void test_discovery_flag(void)
{
	uint8_t payload[48];

	memset(payload, 0, sizeof(payload));
	put32(payload, SDK_SERVICE_IMAGE);
	CHECK_EQ(call(SDK_OP_QUERY_SERVICE, payload, 4U), SDK_STATUS_OK);
#ifdef SDK_SERVICE_FLAG_IMAGE_PNG_ONE_SHOT
	CHECK((get32(g_comp.payload + 12) &
	       SDK_SERVICE_FLAG_IMAGE_PNG_ONE_SHOT) != 0U);
#else
	CHECK(0 && "image service does not announce one-shot PNG");
#endif
	CHECK_EQ(get32(g_comp.payload + 20), 12U);
}

int main(void)
{
	void *region = mmap((void *)SDK_IMAGE_SESSIONS_ADDRESS,
	                    SDK_IMAGE_SESSIONS_MAX_BYTES,
	                    PROT_READ | PROT_WRITE,
	                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	int deferred;

	if (region == MAP_FAILED) {
		perror("session region");
		return 2;
	}
	g_window = mmap(NULL, WINDOW_SIZE, PROT_READ | PROT_WRITE,
	                MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
	host_runtime_reset();
	sdk_image_stream_init();

	test_discovery_flag();
	for (deferred = 0; deferred <= 1; deferred++) {
		printf("%s\n", deferred ? "deferred to core 1" : "inline on core 0");
		test_formats(deferred);
		test_offset_and_bounds(deferred);
		test_interlaced(deferred);
		test_multi_slice(deferred);
		test_rejections(deferred);
		test_gif_unsupported(deferred);
	}
	test_fault_poison();

	printf("%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
