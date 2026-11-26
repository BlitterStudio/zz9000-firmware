/*
 * Shared-buffer media output: validation, fixed palettes and the scaling and
 * conversion pipeline against a straightforward scalar reference.
 *
 * The reference follows docs/media-shared-output.md literally, one pixel at a
 * time with no tables or caches, so it shares the specification with
 * sdk_media_output.c and nothing else.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "sdk_media_output.h"

static int checks;
static int failures;

#define CHECK(expr) do { \
	checks++; \
	if (!(expr)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
	} \
} while (0)

/* ---- reference ------------------------------------------------------- */

static const uint8_t ref_bayer[16] = {
	0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5,
};

struct ref_dims {
	unsigned lr, lg, lb, gray;
};

static const struct ref_dims ref_dims[9] = {
	{ 0, 0, 0, 0 }, { 2, 2, 2, 1 }, { 4, 4, 4, 1 }, { 2, 2, 2, 0 },
	{ 2, 4, 2, 0 }, { 4, 4, 2, 0 }, { 4, 4, 4, 0 }, { 5, 5, 5, 0 },
	{ 6, 7, 6, 0 },
};

static const unsigned ref_host_step[9] = { 0, 255, 255, 255, 170, 128, 85, 64, 48 };

static unsigned ref_level(unsigned v, unsigned levels, int threshold_16)
{
	/* floor(v * (levels - 1) / 255 + threshold / 32), threshold in 1..31 */
	unsigned q = (v * (levels - 1) * 32 + (unsigned)threshold_16 * 255) /
	             (255 * 32);

	return q > levels - 1 ? levels - 1 : q;
}

static uint8_t ref_lerp(unsigned a, unsigned b, unsigned w)
{
	return (uint8_t)((a * (256 - w) + b * w + 128) >> 8);
}

/* (i0, i1, w) of output index i on an axis of luma size sl, to d pixels. */
static void ref_tap(unsigned i, unsigned sl, unsigned d, int chroma,
	                int nearest, unsigned *i0, unsigned *i1, unsigned *w)
{
	unsigned size = chroma ? (sl + 1) / 2 : sl;

	if (nearest) {
		unsigned at = ((2 * i + 1) * sl) / (2 * d);

		if (chroma)
			at /= 2;
		if (at > size - 1)
			at = size - 1;
		*i0 = *i1 = at;
		*w = 0;
		return;
	}
	{
		/* centre-aligned: (i + 1/2) * sl / d - 1/2, in 1/65536ths; chroma
		 * samples sit at the middle of luma pairs */
		long long num = (long long)(2 * i + 1) * sl;
		long long pos = (chroma ? num * 16384 : num * 32768) / d - 32768;

		if (pos < 0 || size == 1) {
			*i0 = *i1 = 0;
			*w = 0;
		} else if ((pos >> 16) >= (long long)size - 1) {
			*i0 = *i1 = size - 1;
			*w = 0;
		} else {
			*i0 = (unsigned)(pos >> 16);
			*i1 = *i0 + 1;
			*w = (unsigned)((pos >> 8) & 255);
		}
	}
}

/* Exact box filter of source pixels [0, n) onto d pixels: weights are the
 * overlaps in units of 1/d source pixel, the sum is divided by n with a
 * 24-bit reciprocal. */
static uint8_t ref_area(const uint8_t *src, unsigned stride, unsigned n,
	                    unsigned d, unsigned i)
{
	unsigned long long sum = 0;
	unsigned long long r24 = ((1ULL << 24) + n / 2) / n;
	unsigned s0 = i * n, s1 = (i + 1) * n;
	unsigned j;

	for (j = 0; j < n; j++) {
		unsigned lo = j * d > s0 ? j * d : s0;
		unsigned hi = (j + 1) * d < s1 ? (j + 1) * d : s1;

		if (hi > lo)
			sum += (unsigned long long)(hi - lo) * src[(size_t)j * stride];
	}
	return (uint8_t)((sum * r24 + (1ULL << 23)) >> 24);
}

/* Scales one plane to dw x dh: vertical pass to 8 bits, then horizontal. */
static void ref_scale(const uint8_t *src, unsigned pitch, unsigned pw,
	                  unsigned ph, unsigned lw, unsigned lh, unsigned dw,
	                  unsigned dh, int chroma, int nearest, uint8_t *out)
{
	uint8_t *mid = malloc((size_t)dh * pw);
	unsigned x, y;

	for (y = 0; y < dh; y++) {
		for (x = 0; x < pw; x++) {
			if (!nearest && lh >= 2 * dh) {
				mid[y * pw + x] = ref_area(src + x, pitch, ph, dh, y);
			} else {
				unsigned i0, i1, w;

				ref_tap(y, lh, dh, chroma, nearest, &i0, &i1, &w);
				mid[y * pw + x] = ref_lerp(src[i0 * pitch + x],
				                           src[i1 * pitch + x], w);
			}
		}
	}
	for (y = 0; y < dh; y++) {
		for (x = 0; x < dw; x++) {
			if (!nearest && lw >= 2 * dw) {
				out[y * dw + x] = ref_area(mid + y * pw, 1, pw, dw, x);
			} else {
				unsigned i0, i1, w;

				ref_tap(x, lw, dw, chroma, nearest, &i0, &i1, &w);
				out[y * dw + x] = ref_lerp(mid[y * pw + i0],
				                           mid[y * pw + i1], w);
			}
		}
	}
	(void)ph;
	free(mid);
}

static int clamp8(int v)
{
	return v < 0 ? 0 : v > 255 ? 255 : v;
}

static void ref_rgb(int y, int cb, int cr, int *r, int *g, int *b)
{
	int c = 298 * (y - 16), d = cb - 128, e = cr - 128;

	*r = clamp8((c + 409 * e + 128) >> 8);
	*g = clamp8((c - 100 * d - 208 * e + 128) >> 8);
	*b = clamp8((c + 516 * d + 128) >> 8);
}

static unsigned ref_nearest_entry(const uint8_t *pal, unsigned entries,
	                              unsigned r5, unsigned g5, unsigned b5)
{
	int cr = (int)((r5 << 3) | (r5 >> 2));
	int cg = (int)((g5 << 3) | (g5 >> 2));
	int cb = (int)((b5 << 3) | (b5 >> 2));
	unsigned best = 0, p;
	long best_d = -1;

	for (p = 0; p < entries; p++) {
		long dr = cr - pal[p * 4 + 1], dg = cg - pal[p * 4 + 2];
		long db = cb - pal[p * 4 + 3];
		long d = dr * dr + dg * dg + db * db;

		if (best_d < 0 || d < best_d) {
			best_d = d;
			best = p;
		}
	}
	return best;
}

/* The expected picture in a buffer laid out like the real one. */
static void ref_convert(const struct SDKMediaOutputJob *job,
	                    const struct SDKVideoDecodedFrame *f,
	                    const uint8_t *palette, uint8_t *dst)
{
	unsigned dw = job->width, dh = job->height;
	int nearest = (job->flags & SDK_MEDIA_OUTPUT_SCALE_NEAREST) != 0;
	int dither = (job->flags & SDK_MEDIA_OUTPUT_DITHER_ORDERED) != 0;
	int host = (job->flags & SDK_MEDIA_OUTPUT_PALETTE_HOST) != 0;
	int interleaved = (job->flags & SDK_MEDIA_OUTPUT_PLANAR_INTERLEAVED) != 0;
	unsigned cw = (f->width + 1) / 2, ch = (f->height + 1) / 2;
	uint8_t *py = malloc((size_t)dw * dh), *pcb = malloc((size_t)dw * dh);
	uint8_t *pcr = malloc((size_t)dw * dh);
	uint8_t *index = malloc(dw);
	unsigned depth = job->depth, x, y, p;

	ref_scale(f->y, f->y_pitch, f->width, f->height, f->width, f->height,
	          dw, dh, 0, nearest, py);
	ref_scale(f->cb, f->chroma_pitch, cw, ch, f->width, f->height, dw, dh, 1,
	          nearest, pcb);
	ref_scale(f->cr, f->chroma_pitch, cw, ch, f->width, f->height, dw, dh, 1,
	          nearest, pcr);
	for (y = 0; y < dh; y++) {
		for (x = 0; x < dw; x++) {
			int rgb[3];
			unsigned t = dither ? ref_bayer[(y & 3) * 4 + (x & 3)] : 16;
			int thr = t < 16 ? (int)(2 * t + 1) : 16;
			unsigned v;

			ref_rgb(py[y * dw + x], pcb[y * dw + x], pcr[y * dw + x],
			        &rgb[0], &rgb[1], &rgb[2]);
			if (job->format == SDK_SURFACE_FORMAT_RGB565 ||
			    job->format == SDK_SURFACE_FORMAT_RGB555) {
				int c565 = job->format == SDK_SURFACE_FORMAT_RGB565;
				unsigned r = ref_level(rgb[0], 32, thr);
				unsigned g = ref_level(rgb[1], c565 ? 64 : 32, thr);
				unsigned b = ref_level(rgb[2], 32, thr);

				v = c565 ? (r << 11) | (g << 5) | b
				         : (r << 10) | (g << 5) | b;
				dst[(size_t)y * job->pitch + 2 * x] = (uint8_t)(v >> 8);
				dst[(size_t)y * job->pitch + 2 * x + 1] = (uint8_t)v;
				continue;
			}
			if (host) {
				unsigned cell[3];
				int c;
				unsigned entries = depth >= 8 ? 256 : 1u << depth;

				for (c = 0; c < 3; c++) {
					int off = t < 16
						? ((2 * (int)t - 15) * (int)ref_host_step[depth]) / 32
						: 0;
					int w = clamp8(rgb[c] + off);

					cell[c] = ((unsigned)w * 31 + 127) / 255;
				}
				v = ref_nearest_entry(palette, entries, cell[0], cell[1],
				                      cell[2]);
			} else if (ref_dims[depth].gray) {
				unsigned lum = (unsigned)((77 * rgb[0] + 150 * rgb[1] +
				                           29 * rgb[2] + 128) >> 8);

				v = ref_level(lum, 1u << depth, thr);
			} else {
				const struct ref_dims *d = &ref_dims[depth];

				v = (ref_level(rgb[0], d->lr, thr) * d->lg +
				     ref_level(rgb[1], d->lg, thr)) * d->lb +
				    ref_level(rgb[2], d->lb, thr);
			}
			index[x] = (uint8_t)v;
		}
		if (job->format == SDK_SURFACE_FORMAT_INDEX8) {
			memcpy(dst + (size_t)y * job->pitch, index, dw);
		} else if (job->format == SDK_SURFACE_FORMAT_PLANAR) {
			for (p = 0; p < depth; p++) {
				uint8_t *row = dst + (size_t)(interleaved
					? y * depth + p : p * dh + y) * job->pitch;
				unsigned b, k;

				for (b = 0; b < (dw + 7) / 8; b++) {
					unsigned byte = 0;

					for (k = 0; k < 8; k++)
						if (8 * b + k < dw)
							byte |= ((index[8 * b + k] >> p) & 1u)
							        << (7 - k);
					row[b] = (uint8_t)byte;
				}
			}
		}
	}
	free(py);
	free(pcb);
	free(pcr);
	free(index);
}

/* ---- fixtures -------------------------------------------------------- */

/* A job names its destination with a 32-bit address, as on the card; the
 * host build is 64-bit, so pictures live in an arena mapped below 4 GiB.
 * free() ignores arena pointers; the arena is reset between tests. */
#define ARENA_BYTES (64U * 1024U * 1024U)
static uint8_t *arena;
static size_t arena_used;

static uint8_t *low_alloc(size_t bytes)
{
	uint8_t *p;

	if (!arena) {
		arena = mmap(0, ARENA_BYTES, PROT_READ | PROT_WRITE,
		             MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
		if (arena == MAP_FAILED) {
			printf("cannot map a low arena\n");
			exit(2);
		}
	}
	bytes = (bytes + 63U) & ~(size_t)63U;
	if (arena_used + bytes > ARENA_BYTES) {
		printf("arena exhausted\n");
		exit(2);
	}
	p = arena + arena_used;
	arena_used += bytes;
	return p;
}

static void test_free(void *p)
{
	if (arena && (uint8_t *)p >= arena && (uint8_t *)p < arena + ARENA_BYTES)
		return;
	(free)(p);
}
#define free(p) test_free(p)

static uint32_t rng_state = 12345U;

static uint8_t rnd(void)
{
	rng_state = rng_state * 1664525U + 1013904223U;
	return (uint8_t)(rng_state >> 24);
}

struct frame_buf {
	struct SDKVideoDecodedFrame f;
	uint8_t *y, *cb, *cr;
};

static void make_frame(struct frame_buf *fb, unsigned w, unsigned h,
	                   int smooth)
{
	unsigned cw = (w + 1) / 2, ch = (h + 1) / 2, x, y;
	unsigned ypitch = w + 5, cpitch = cw + 3;

	memset(fb, 0, sizeof(*fb));
	fb->y = malloc((size_t)ypitch * h);
	fb->cb = malloc((size_t)cpitch * ch);
	fb->cr = malloc((size_t)cpitch * ch);
	for (y = 0; y < h; y++)
		for (x = 0; x < ypitch; x++)
			fb->y[y * ypitch + x] = smooth
				? (uint8_t)(16 + (x * 219) / (w > 1 ? w - 1 : 1) / 2 +
				            (y * 219) / (h > 1 ? h - 1 : 1) / 2)
				: rnd();
	for (y = 0; y < ch; y++) {
		for (x = 0; x < cpitch; x++) {
			fb->cb[y * cpitch + x] = smooth ? (uint8_t)(64 + x * 8 % 128) : rnd();
			fb->cr[y * cpitch + x] = smooth ? (uint8_t)(192 - y * 8 % 128) : rnd();
		}
	}
	fb->f.width = w;
	fb->f.height = h;
	fb->f.y_pitch = ypitch;
	fb->f.chroma_pitch = cpitch;
	fb->f.y = fb->y;
	fb->f.cb = fb->cb;
	fb->f.cr = fb->cr;
}

static void free_frame(struct frame_buf *fb)
{
	free(fb->y);
	free(fb->cb);
	free(fb->cr);
}

static void make_job(struct SDKMediaOutputJob *job, unsigned w, unsigned h,
	                 uint32_t format, uint32_t flags, uint8_t *dst,
	                 unsigned *bytes)
{
	struct SDKMediaOutputConfig cfg;
	struct SDKMediaOutputLayout layout;
	unsigned row;

	memset(job, 0, sizeof(*job));
	if (format == SDK_SURFACE_FORMAT_INDEX8)
		row = w;
	else if (format == SDK_SURFACE_FORMAT_PLANAR)
		row = (w + 7) / 8;
	else
		row = 2 * w;
	memset(&cfg, 0, sizeof(cfg));
	cfg.width = w;
	cfg.height = h;
	cfg.format = format;
	cfg.pitch = row + 6; /* padding that must stay untouched */
	cfg.flags = flags;
	if (sdk_media_output_check(&cfg, &layout) != SDK_STATUS_OK) {
		printf("make_job: config rejected (%u x %u fmt %u flags %x)\n", w, h,
		       (unsigned)format, (unsigned)flags);
		exit(2);
	}
	job->dst_addr = (uint32_t)(uintptr_t)dst;
	job->pitch = cfg.pitch;
	job->width = (uint16_t)w;
	job->height = (uint16_t)h;
	job->flags = (uint16_t)flags;
	job->format = (uint8_t)format;
	job->depth = (uint8_t)layout.depth;
	*bytes = layout.picture_bytes;
}

/* dst is 32-bit addressed in the job; the host build is 64-bit, so these
 * tests keep pictures in a low static arena reachable by a 32-bit value. */

/* ---- tests ----------------------------------------------------------- */

static struct SDKMediaOutputConfig base_cfg(uint32_t format)
{
	struct SDKMediaOutputConfig cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.width = 320;
	cfg.height = 240;
	cfg.format = format;
	cfg.pitch = format == SDK_SURFACE_FORMAT_INDEX8 ? 320
	          : format == SDK_SURFACE_FORMAT_PLANAR ? 40 : 640;
	return cfg;
}

static void test_check(void)
{
	struct SDKMediaOutputConfig cfg;
	struct SDKMediaOutputLayout l;

	cfg = base_cfg(SDK_SURFACE_FORMAT_PLANAR);
	cfg.flags = 8;
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_OK);
	CHECK(l.depth == 8 && l.row_bytes == 40 && l.picture_bytes == 76800 &&
	      l.slots == 1 && l.span_bytes == 76800);
	cfg.flags = 0; /* 0 means 8 */
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_OK && l.depth == 8);
	cfg.flags = 5;
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_OK &&
	      l.picture_bytes == 48000);
	cfg.flags = 9;
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_BAD_REQUEST);
	cfg.flags = 15;
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_BAD_REQUEST);
	cfg.flags = 5 | SDK_MEDIA_OUTPUT_PLANAR_INTERLEAVED;
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_OK);
	cfg.slot_stride = 48000;
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_OK &&
	      l.slots == 2 && l.span_bytes == 96000);
	cfg.slot_stride = 47996; /* overlaps slot 0 */
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_BAD_REQUEST);
	cfg.slot_stride = 48002; /* not a multiple of 4 */
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_BAD_REQUEST);
	cfg.slot_stride = 0;
	cfg.buffer_offset = 2;
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_BAD_REQUEST);
	cfg.buffer_offset = 8;
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_OK);

	/* INDEX8 has one byte per pixel and no interleave */
	cfg = base_cfg(SDK_SURFACE_FORMAT_INDEX8);
	cfg.flags = 6;
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_OK &&
	      l.depth == 6 && l.picture_bytes == 76800);
	cfg.flags = SDK_MEDIA_OUTPUT_PLANAR_INTERLEAVED;
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_BAD_REQUEST);
	cfg.flags = 0;
	cfg.pitch = 319;
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_BAD_REQUEST);

	/* RGB takes no depth, interleave or host palette */
	cfg = base_cfg(SDK_SURFACE_FORMAT_RGB565);
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_OK &&
	      l.depth == 0 && l.picture_bytes == 153600);
	cfg.flags = SDK_MEDIA_OUTPUT_DITHER_ORDERED | SDK_MEDIA_OUTPUT_SCALE_NEAREST;
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_OK);
	cfg.flags = 4;
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_BAD_REQUEST);
	cfg.flags = SDK_MEDIA_OUTPUT_PALETTE_HOST;
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_BAD_REQUEST);
	cfg.flags = SDK_MEDIA_OUTPUT_PLANAR_INTERLEAVED;
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_BAD_REQUEST);
	cfg.flags = 0;
	cfg.pitch = 639;
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_BAD_REQUEST);
	cfg = base_cfg(SDK_SURFACE_FORMAT_RGB555);
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_OK);

	/* diffusion is not implemented, and never with ordered */
	cfg = base_cfg(SDK_SURFACE_FORMAT_INDEX8);
	cfg.flags = SDK_MEDIA_OUTPUT_DITHER_DIFFUSION;
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_UNSUPPORTED);
	cfg.flags |= SDK_MEDIA_OUTPUT_DITHER_ORDERED;
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_BAD_REQUEST);
	CHECK((SDK_MEDIA_OUTPUT_FLAGS_SUPPORTED &
	       SDK_MEDIA_OUTPUT_DITHER_DIFFUSION) == 0);
	CHECK((SDK_MEDIA_OUTPUT_FLAGS_SUPPORTED &
	       SDK_MEDIA_OUTPUT_DITHER_ORDERED) != 0);

	/* unknown flag bits, other formats, bad sizes */
	cfg = base_cfg(SDK_SURFACE_FORMAT_INDEX8);
	cfg.flags = 1U << 9;
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_BAD_REQUEST);
	cfg = base_cfg(SDK_SURFACE_FORMAT_ARGB8888);
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_UNSUPPORTED);
	cfg = base_cfg(SDK_SURFACE_FORMAT_UNKNOWN);
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_UNSUPPORTED);
	cfg = base_cfg(SDK_SURFACE_FORMAT_INDEX8);
	cfg.width = 0;
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_BAD_REQUEST);
	cfg.width = 1921;
	cfg.pitch = 4000;
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_BAD_REQUEST);
	cfg.width = 1920;
	cfg.height = 1088;
	cfg.pitch = 1920;
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_OK &&
	      l.picture_bytes == 1920U * 1088U);
	cfg.height = 1089;
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_BAD_REQUEST);
	cfg.height = 0;
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_BAD_REQUEST);
	/* a slot stride that pushes the span past 32 bits */
	cfg.height = 1088;
	cfg.slot_stride = 0xfffffffcU;
	CHECK(sdk_media_output_check(&cfg, &l) == SDK_STATUS_BAD_REQUEST);
}

static void test_fixed_palettes(void)
{
	uint32_t pal[256];
	static const uint32_t colours[9] = { 0, 2, 4, 8, 16, 32, 64, 125, 252 };
	unsigned d;

	for (d = 1; d <= 8; d++) {
		uint32_t used = sdk_media_output_fixed_palette(d, pal);
		unsigned i;

		CHECK(used == colours[d]);
		for (i = used; i < 256; i++)
			if (pal[i] != 0)
				break;
		CHECK(i == 256); /* nothing beyond the colours in use */
		CHECK(used <= (1u << d));
	}
	/* depth 8: r in 0..5, g in 0..6, b in 0..5, index (r * 7 + g) * 6 + b */
	sdk_media_output_fixed_palette(8, pal);
	CHECK(pal[0] == 0x000000);
	CHECK(pal[251] == 0xffffff);
	CHECK(pal[(1 * 7 + 0) * 6 + 0] == 0x330000);
	CHECK(pal[(0 * 7 + 1) * 6 + 0] == 0x002b00); /* 255 / 6 rounded: 42.5 -> 43 */
	CHECK(pal[1] == 0x000033);
	/* depth 1 and 2 are gray ramps */
	sdk_media_output_fixed_palette(1, pal);
	CHECK(pal[0] == 0x000000 && pal[1] == 0xffffff);
	sdk_media_output_fixed_palette(2, pal);
	CHECK(pal[1] == 0x555555 && pal[2] == 0xaaaaaa && pal[3] == 0xffffff);
	/* depth 3 is the eight corners of the colour cube */
	sdk_media_output_fixed_palette(3, pal);
	CHECK(pal[0] == 0x000000 && pal[1] == 0x0000ff && pal[2] == 0x00ff00 &&
	      pal[4] == 0xff0000 && pal[7] == 0xffffff);
	CHECK(sdk_media_output_fixed_palette(0, pal) == 0);
	CHECK(sdk_media_output_fixed_palette(9, pal) == 0);
}


static int compare_picture(const struct SDKMediaOutputJob *job,
	                       const struct SDKVideoDecodedFrame *f,
	                       const uint8_t *palette, const char *what)
{
	unsigned bytes;
	size_t total = (size_t)job->pitch * job->height *
	               (job->format == SDK_SURFACE_FORMAT_PLANAR ? job->depth : 1) +
	               64;
	uint8_t *want = malloc(total);
	uint8_t *got = (uint8_t *)(uintptr_t)job->dst_addr;
	size_t i;
	int ok = 1;

	(void)bytes;
	memset(want, 0xa5, total);
	memset(got, 0xa5, total);
	ref_convert(job, f, palette, want);
	if (sdk_media_output_convert(job, f, palette) != SDK_STATUS_OK) {
		printf("convert failed: %s\n", what);
		free(want);
		return 0;
	}
	for (i = 0; i < total; i++) {
		if (want[i] != got[i]) {
			printf("MISMATCH %s at byte %zu: want %02x got %02x\n", what, i,
			       want[i], got[i]);
			ok = 0;
			break;
		}
	}
	free(want);
	return ok;
}

struct geom {
	unsigned sw, sh, dw, dh;
};

static const struct geom geoms[] = {
	{ 16, 16, 16, 16 },   /* 1:1 */
	{ 17, 13, 17, 13 },   /* 1:1, odd */
	{ 16, 12, 33, 25 },   /* up, odd result */
	{ 33, 20, 40, 31 },   /* up, odd source */
	{ 32, 24, 20, 15 },   /* down < 2x */
	{ 32, 24, 16, 12 },   /* exactly 2x: area */
	{ 33, 25, 7, 5 },     /* well beyond 2x, odd source */
	{ 40, 24, 9, 24 },    /* area horizontally only */
	{ 40, 24, 40, 5 },    /* area vertically only */
	{ 2, 2, 5, 3 },       /* tiny source */
	{ 1, 1, 4, 4 },       /* single pixel source */
	{ 9, 9, 1, 1 },       /* single pixel result */
	{ 24, 16, 31, 8 },    /* up horizontally, area vertically */
};

static void test_geometries(void)
{
	static const struct {
		uint32_t format;
		uint32_t flags;
	} variants[] = {
		{ SDK_SURFACE_FORMAT_RGB565, 0 },
		{ SDK_SURFACE_FORMAT_RGB565, SDK_MEDIA_OUTPUT_DITHER_ORDERED },
		{ SDK_SURFACE_FORMAT_RGB555, SDK_MEDIA_OUTPUT_DITHER_ORDERED },
		{ SDK_SURFACE_FORMAT_RGB555, SDK_MEDIA_OUTPUT_SCALE_NEAREST },
		{ SDK_SURFACE_FORMAT_INDEX8, 0 },
		{ SDK_SURFACE_FORMAT_INDEX8, SDK_MEDIA_OUTPUT_DITHER_ORDERED },
		{ SDK_SURFACE_FORMAT_INDEX8, SDK_MEDIA_OUTPUT_DITHER_ORDERED |
		                             SDK_MEDIA_OUTPUT_SCALE_NEAREST | 5 },
		{ SDK_SURFACE_FORMAT_PLANAR, 8 | SDK_MEDIA_OUTPUT_DITHER_ORDERED },
		{ SDK_SURFACE_FORMAT_PLANAR, 4 | SDK_MEDIA_OUTPUT_PLANAR_INTERLEAVED },
	};
	unsigned g, v;

	for (g = 0; g < sizeof(geoms) / sizeof(geoms[0]); g++) {
		for (v = 0; v < sizeof(variants) / sizeof(variants[0]); v++) {
			struct frame_buf fb;
			struct SDKMediaOutputJob job;
			unsigned bytes;
			uint8_t *dst;
			char what[96];
			/* output = source scaled by the geometry's ratio */
			unsigned ow = geoms[g].dw, oh = geoms[g].dh;

			make_frame(&fb, geoms[g].sw, geoms[g].sh, 0);
			dst = low_alloc((size_t)(ow + 8) * (oh + 1) * 8 * 2 + 256);
			make_job(&job, ow, oh, variants[v].format, variants[v].flags,
			         dst, &bytes);
			snprintf(what, sizeof(what), "%ux%u -> %ux%u variant %u",
			         geoms[g].sw, geoms[g].sh, ow, oh, v);
			CHECK(compare_picture(&job, &fb.f, 0, what));
			free(dst);
			free_frame(&fb);
		}
	}
}

static void test_all_depths_and_layouts(void)
{
	unsigned depth, layout;

	for (depth = 1; depth <= 8; depth++) {
		for (layout = 0; layout < 3; layout++) {
			struct frame_buf fb;
			struct SDKMediaOutputJob job;
			unsigned bytes;
			uint8_t *dst;
			uint32_t format = layout == 0 ? SDK_SURFACE_FORMAT_INDEX8
			                              : SDK_SURFACE_FORMAT_PLANAR;
			uint32_t flags = depth | SDK_MEDIA_OUTPUT_DITHER_ORDERED |
			    (layout == 2 ? SDK_MEDIA_OUTPUT_PLANAR_INTERLEAVED : 0);
			char what[64];

			make_frame(&fb, 26, 18, 0);
			dst = low_alloc(64 * 1024);
			/* 27 wide: not a multiple of 8 */
			make_job(&job, 27, 19, format, flags, dst, &bytes);
			snprintf(what, sizeof(what), "depth %u layout %u", depth, layout);
			CHECK(compare_picture(&job, &fb.f, 0, what));
			free(dst);
			free_frame(&fb);
		}
	}
}

static void test_depth_zero_means_eight(void)
{
	struct frame_buf fb;
	struct SDKMediaOutputJob job8, job0;
	unsigned bytes;
	uint8_t *a = low_alloc(4096), *b = low_alloc(4096);

	make_frame(&fb, 16, 16, 0);
	make_job(&job8, 16, 16, SDK_SURFACE_FORMAT_INDEX8, 8, a, &bytes);
	make_job(&job0, 16, 16, SDK_SURFACE_FORMAT_INDEX8, 0, b, &bytes);
	memset(a, 0, 4096);
	memset(b, 0, 4096);
	CHECK(sdk_media_output_convert(&job8, &fb.f, 0) == SDK_STATUS_OK);
	CHECK(sdk_media_output_convert(&job0, &fb.f, 0) == SDK_STATUS_OK);
	CHECK(memcmp(a, b, 4096) == 0);
	free(a);
	free(b);
	free_frame(&fb);
}

static void test_padding_untouched(void)
{
	struct frame_buf fb;
	struct SDKMediaOutputJob job;
	unsigned bytes, y, x;
	uint8_t *dst = low_alloc(8192);

	make_frame(&fb, 20, 10, 0);
	make_job(&job, 20, 10, SDK_SURFACE_FORMAT_RGB565, 0, dst, &bytes);
	memset(dst, 0x5a, 8192);
	CHECK(sdk_media_output_convert(&job, &fb.f, 0) == SDK_STATUS_OK);
	for (y = 0; y < 10; y++)
		for (x = 40; x < job.pitch; x++)
			CHECK(dst[y * job.pitch + x] == 0x5a);
	CHECK(dst[10 * job.pitch] == 0x5a); /* nothing past the last row */
	free(dst);
	free_frame(&fb);
}

static void test_flat_colour_stays_flat(void)
{
	/* a flat picture must stay flat through every scaler, so the area
	 * average's reciprocal cannot lose a level (white must not become 254) */
	static const struct geom g[] = {
		{ 64, 48, 64, 48 }, { 64, 48, 30, 20 }, { 64, 48, 7, 5 },
		{ 64, 48, 100, 90 },
	};
	unsigned i, n;

	for (i = 0; i < sizeof(g) / sizeof(g[0]); i++) {
		struct frame_buf fb;
		struct SDKMediaOutputJob job;
		unsigned bytes;
		uint8_t *dst = low_alloc(64 * 1024);
		uint8_t first[2];

		make_frame(&fb, g[i].sw, g[i].sh, 0);
		memset(fb.y, 235, (size_t)fb.f.y_pitch * fb.f.height);
		memset(fb.cb, 128, (size_t)fb.f.chroma_pitch * ((fb.f.height + 1) / 2));
		memset(fb.cr, 128, (size_t)fb.f.chroma_pitch * ((fb.f.height + 1) / 2));
		make_job(&job, g[i].dw, g[i].dh, SDK_SURFACE_FORMAT_RGB565, 0, dst,
		         &bytes);
		CHECK(sdk_media_output_convert(&job, &fb.f, 0) == SDK_STATUS_OK);
		first[0] = dst[0];
		first[1] = dst[1];
		CHECK(first[0] == 0xff && first[1] == 0xff); /* white */
		for (n = 0; n < g[i].dh; n++) {
			unsigned x;

			for (x = 0; x < g[i].dw; x++)
				CHECK(dst[n * job.pitch + 2 * x] == first[0] &&
				      dst[n * job.pitch + 2 * x + 1] == first[1]);
		}
		free(dst);
		free_frame(&fb);
	}
}

static void test_known_pixels(void)
{
	/* hand-checked: Y=16 black, Y=235 white, mid grey 126 -> 128ish */
	struct frame_buf fb;
	struct SDKMediaOutputJob job;
	unsigned bytes;
	uint8_t *dst = low_alloc(4096);

	make_frame(&fb, 2, 2, 0);
	memset(fb.y, 16, (size_t)fb.f.y_pitch * 2);
	memset(fb.cb, 128, (size_t)fb.f.chroma_pitch);
	memset(fb.cr, 128, (size_t)fb.f.chroma_pitch);
	make_job(&job, 2, 2, SDK_SURFACE_FORMAT_RGB555, 0, dst, &bytes);
	CHECK(sdk_media_output_convert(&job, &fb.f, 0) == SDK_STATUS_OK);
	CHECK(dst[0] == 0 && dst[1] == 0);
	memset(fb.y, 235, (size_t)fb.f.y_pitch * 2);
	CHECK(sdk_media_output_convert(&job, &fb.f, 0) == SDK_STATUS_OK);
	CHECK(dst[0] == 0x7f && dst[1] == 0xff); /* 0x7fff, big-endian */
	/* red: Y=81 Cb=90 Cr=240 (BT.601 limited) */
	memset(fb.y, 81, (size_t)fb.f.y_pitch * 2);
	memset(fb.cb, 90, (size_t)fb.f.chroma_pitch);
	memset(fb.cr, 240, (size_t)fb.f.chroma_pitch);
	make_job(&job, 2, 2, SDK_SURFACE_FORMAT_RGB565, 0, dst, &bytes);
	CHECK(sdk_media_output_convert(&job, &fb.f, 0) == SDK_STATUS_OK);
	CHECK(dst[0] == 0xf8 && dst[1] == 0x00); /* 0xf800 */
	free(dst);
	free_frame(&fb);
}

static void test_dither_is_stable_and_alternates(void)
{
	struct frame_buf fb;
	struct SDKMediaOutputJob job;
	unsigned bytes, x, ones = 0;
	uint8_t *a = low_alloc(8192), *b = low_alloc(8192);

	memset(a, 0, 8192);
	memset(b, 0, 8192);
	make_frame(&fb, 64, 4, 0);
	memset(fb.y, 126, (size_t)fb.f.y_pitch * 4); /* mid gray */
	memset(fb.cb, 128, (size_t)fb.f.chroma_pitch * 2);
	memset(fb.cr, 128, (size_t)fb.f.chroma_pitch * 2);
	make_job(&job, 64, 4, SDK_SURFACE_FORMAT_INDEX8,
	         1 | SDK_MEDIA_OUTPUT_DITHER_ORDERED, a, &bytes);
	CHECK(sdk_media_output_convert(&job, &fb.f, 0) == SDK_STATUS_OK);
	job.dst_addr = (uint32_t)(uintptr_t)b;
	CHECK(sdk_media_output_convert(&job, &fb.f, 0) == SDK_STATUS_OK);
	CHECK(memcmp(a, b, (size_t)job.pitch * 4) == 0); /* frame to frame */
	for (x = 0; x < 64; x++)
		ones += a[x];
	/* mid gray on a 1-bit gray palette lights about half the pixels */
	CHECK(ones > 20 && ones < 44);
	free(a);
	free(b);
	free_frame(&fb);
}

static void test_host_palette(void)
{
	struct frame_buf fb;
	struct SDKMediaOutputJob job;
	unsigned bytes, depth, i;
	uint8_t pal[SDK_MEDIA_OUTPUT_PALETTE_BYTES];
	uint8_t *dst = low_alloc(16384);
	char what[64];

	make_frame(&fb, 24, 16, 0);
	for (depth = 1; depth <= 8; depth += 7) {
		/* a scrambled palette, so index order carries no hint */
		memset(pal, 0, sizeof(pal));
		for (i = 0; i < 256; i++) {
			pal[i * 4 + 1] = rnd();
			pal[i * 4 + 2] = rnd();
			pal[i * 4 + 3] = rnd();
		}
		make_job(&job, 24, 16, SDK_SURFACE_FORMAT_INDEX8,
		         depth | SDK_MEDIA_OUTPUT_PALETTE_HOST |
		         SDK_MEDIA_OUTPUT_DITHER_ORDERED, dst, &bytes);
		snprintf(what, sizeof(what), "host palette depth %u", depth);
		CHECK(compare_picture(&job, &fb.f, pal, what));
		/* the card re-reads the palette: a changed table changes the
		 * result without a new configuration */
		for (i = 0; i < 256; i++) {
			pal[i * 4 + 1] = rnd();
			pal[i * 4 + 2] = rnd();
			pal[i * 4 + 3] = rnd();
		}
		snprintf(what, sizeof(what), "host palette changed depth %u", depth);
		CHECK(compare_picture(&job, &fb.f, pal, what));
	}
	/* planar through a host palette, with no dither */
	make_job(&job, 24, 16, SDK_SURFACE_FORMAT_PLANAR,
	         6 | SDK_MEDIA_OUTPUT_PALETTE_HOST, dst, &bytes);
	CHECK(compare_picture(&job, &fb.f, pal, "host palette planar"));
	/* a host palette that is the fixed palette maps flat colours onto the
	 * same entries the fixed path picks */
	{
		uint32_t fixed[256];
		struct SDKMediaOutputJob fjob;
		uint8_t *other = low_alloc(16384);

		sdk_media_output_fixed_palette(8, fixed);
		for (i = 0; i < 256; i++) {
			pal[i * 4] = 0;
			pal[i * 4 + 1] = (uint8_t)(fixed[i] >> 16);
			pal[i * 4 + 2] = (uint8_t)(fixed[i] >> 8);
			pal[i * 4 + 3] = (uint8_t)fixed[i];
		}
		memset(fb.y, 150, (size_t)fb.f.y_pitch * fb.f.height);
		memset(fb.cb, 128, (size_t)fb.f.chroma_pitch * 8);
		memset(fb.cr, 128, (size_t)fb.f.chroma_pitch * 8);
		make_job(&job, 24, 16, SDK_SURFACE_FORMAT_INDEX8,
		         8 | SDK_MEDIA_OUTPUT_PALETTE_HOST, dst, &bytes);
		make_job(&fjob, 24, 16, SDK_SURFACE_FORMAT_INDEX8, 8, other, &bytes);
		CHECK(sdk_media_output_convert(&job, &fb.f, pal) == SDK_STATUS_OK);
		CHECK(sdk_media_output_convert(&fjob, &fb.f, 0) == SDK_STATUS_OK);
		{
			uint32_t host_rgb = fixed[dst[0]];
			uint32_t fixed_rgb = fixed[other[0]];
			int dr = (int)(host_rgb >> 16 & 255) - (int)(fixed_rgb >> 16 & 255);
			int dg = (int)(host_rgb >> 8 & 255) - (int)(fixed_rgb >> 8 & 255);
			int db = (int)(host_rgb & 255) - (int)(fixed_rgb & 255);

			/* both land on the palette entry nearest mid gray */
			CHECK(dr * dr + dg * dg + db * db < 60 * 60);
		}
		free(other);
	}
	free(dst);
	free_frame(&fb);
}

static void test_rejects(void)
{
	struct frame_buf fb;
	struct SDKMediaOutputJob job;
	unsigned bytes;
	uint8_t pal[SDK_MEDIA_OUTPUT_PALETTE_BYTES] = { 0 };
	uint8_t *dst = low_alloc(8192);
	struct SDKVideoDecodedFrame bad;

	make_frame(&fb, 16, 16, 0);
	make_job(&job, 16, 16, SDK_SURFACE_FORMAT_INDEX8, 8, dst, &bytes);
	bad = fb.f;
	bad.y = 0;
	CHECK(sdk_media_output_convert(&job, &bad, 0) == SDK_STATUS_BAD_REQUEST);
	bad = fb.f;
	bad.chroma_pitch = 7;
	CHECK(sdk_media_output_convert(&job, &bad, 0) == SDK_STATUS_BAD_REQUEST);
	bad = fb.f;
	bad.width = 4000;
	CHECK(sdk_media_output_convert(&job, &bad, 0) == SDK_STATUS_BAD_REQUEST);
	CHECK(sdk_media_output_convert(0, &fb.f, 0) == SDK_STATUS_BAD_REQUEST);
	job.flags |= SDK_MEDIA_OUTPUT_PALETTE_HOST;
	CHECK(sdk_media_output_convert(&job, &fb.f, 0) == SDK_STATUS_BAD_REQUEST);
	CHECK(sdk_media_output_convert(&job, &fb.f, pal) == SDK_STATUS_OK);
	free(dst);
	free_frame(&fb);
}

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* Cost per picture on this host, for the figures in the documentation. */
static void bench(void)
{
	static const struct {
		const char *name;
		unsigned sw, sh, dw, dh;
		uint32_t format, flags;
	} cases[] = {
		{ "320x240 -> 320x240 RGB565 bilinear", 320, 240, 320, 240,
		  SDK_SURFACE_FORMAT_RGB565, 0 },
		{ "320x240 -> 320x240 RGB565 dither", 320, 240, 320, 240,
		  SDK_SURFACE_FORMAT_RGB565, SDK_MEDIA_OUTPUT_DITHER_ORDERED },
		{ "320x240 -> 320x240 INDEX8 6x7x6 dither", 320, 240, 320, 240,
		  SDK_SURFACE_FORMAT_INDEX8, SDK_MEDIA_OUTPUT_DITHER_ORDERED },
		{ "320x240 -> 320x240 planar 8 interleaved dither", 320, 240, 320,
		  240, SDK_SURFACE_FORMAT_PLANAR,
		  8 | SDK_MEDIA_OUTPUT_PLANAR_INTERLEAVED |
		  SDK_MEDIA_OUTPUT_DITHER_ORDERED },
		{ "320x240 -> 320x240 planar 5 dither", 320, 240, 320, 240,
		  SDK_SURFACE_FORMAT_PLANAR, 5 | SDK_MEDIA_OUTPUT_DITHER_ORDERED },
		{ "352x288 -> 320x240 planar 8 dither (bilinear)", 352, 288, 320,
		  240, SDK_SURFACE_FORMAT_PLANAR, 8 | SDK_MEDIA_OUTPUT_DITHER_ORDERED },
		{ "320x240 -> 640x480 RGB565 bilinear", 320, 240, 640, 480,
		  SDK_SURFACE_FORMAT_RGB565, 0 },
		{ "640x480 -> 640x480 RGB565 dither", 640, 480, 640, 480,
		  SDK_SURFACE_FORMAT_RGB565, SDK_MEDIA_OUTPUT_DITHER_ORDERED },
		{ "640x480 -> 640x480 planar 8 interleaved dither", 640, 480, 640,
		  480, SDK_SURFACE_FORMAT_PLANAR,
		  8 | SDK_MEDIA_OUTPUT_PLANAR_INTERLEAVED |
		  SDK_MEDIA_OUTPUT_DITHER_ORDERED },
		{ "640x480 -> 320x240 planar 8 dither (area)", 640, 480, 320, 240,
		  SDK_SURFACE_FORMAT_PLANAR, 8 | SDK_MEDIA_OUTPUT_DITHER_ORDERED },
		{ "640x480 -> 320x240 INDEX8 dither nearest", 640, 480, 320, 240,
		  SDK_SURFACE_FORMAT_INDEX8,
		  SDK_MEDIA_OUTPUT_DITHER_ORDERED | SDK_MEDIA_OUTPUT_SCALE_NEAREST },
	};
	unsigned c;

	printf("converter cost on this host (best of 20, one picture):\n");
	for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		struct frame_buf fb;
		struct SDKMediaOutputJob job;
		unsigned bytes, i;
		uint8_t *dst = low_alloc(2 * 1024 * 1024);
		double best = 1e9;

		make_frame(&fb, cases[c].sw, cases[c].sh, 1);
		make_job(&job, cases[c].dw, cases[c].dh, cases[c].format,
		         cases[c].flags, dst, &bytes);
		(void)sdk_media_output_convert(&job, &fb.f, 0);
		for (i = 0; i < 20; i++) {
			double t0 = now_ms(), t;

			(void)sdk_media_output_convert(&job, &fb.f, 0);
			t = now_ms() - t0;
			if (t < best)
				best = t;
		}
		printf("  %-52s %7.3f ms  (%6.1f ns/pixel)\n", cases[c].name, best,
		       best * 1e6 / ((double)cases[c].dw * cases[c].dh));
		free(dst);
		free_frame(&fb);
	}
	{
		struct frame_buf fb;
		struct SDKMediaOutputJob job;
		unsigned bytes, i;
		uint8_t *dst = low_alloc(64 * 1024);
		uint8_t pal[SDK_MEDIA_OUTPUT_PALETTE_BYTES];
		double t0;

		make_frame(&fb, 64, 48, 1);
		for (i = 0; i < 256; i++) {
			pal[i * 4] = 0;
			pal[i * 4 + 1] = (uint8_t)(i * 7);
			pal[i * 4 + 2] = (uint8_t)(i * 13);
			pal[i * 4 + 3] = (uint8_t)(i * 29);
		}
		make_job(&job, 64, 48, SDK_SURFACE_FORMAT_INDEX8,
		         8 | SDK_MEDIA_OUTPUT_PALETTE_HOST, dst, &bytes);
		t0 = now_ms();
		(void)sdk_media_output_convert(&job, &fb.f, pal);
		printf("  one-off inverse colour map for a 256-colour host palette: "
		       "%.2f ms\n", now_ms() - t0);
		free(dst);
		free_frame(&fb);
	}
}

int main(int argc, char **argv)
{
	test_check();
	test_fixed_palettes();
	arena_used = 0;
	test_geometries();
	arena_used = 0;
	test_all_depths_and_layouts();
	arena_used = 0;
	test_depth_zero_means_eight();
	arena_used = 0;
	test_padding_untouched();
	arena_used = 0;
	test_flat_colour_stays_flat();
	arena_used = 0;
	test_known_pixels();
	arena_used = 0;
	test_dither_is_stable_and_alternates();
	arena_used = 0;
	test_host_palette();
	arena_used = 0;
	test_rejects();
	if (argc > 1 && strcmp(argv[1], "--bench") == 0) {
		arena_used = 0;
		bench();
	}
	if (failures != 0) {
		printf("media_output_test: %d failure(s) of %d checks\n", failures,
		       checks);
		return 1;
	}
	printf("media_output_test: %d checks passed\n", checks);
	return 0;
}
