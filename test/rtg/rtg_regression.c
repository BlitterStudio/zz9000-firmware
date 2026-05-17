/*
 * Host-side RTG regression and benchmark harness for ZZ9000OS gfx.c.
 *
 * This deliberately keeps the references independent from the production
 * macros in gfx.h so optimized firmware paths can be checked before commit.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gfx.h"
#include "zz_video_modes.h"

enum {
	FB_W = 96,
	FB_H = 72,
	FB_PITCH_WORDS = 128,
	FB_WORDS = FB_PITCH_WORDS * FB_H,
};

static uint32_t actual_fb[FB_WORDS];
static uint32_t expected_fb[FB_WORDS];
static uint32_t rng_state;
static unsigned failures;

static uint32_t rng32(void)
{
	uint32_t x = rng_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	rng_state = x;
	return x;
}

static void seed_frame(uint32_t seed)
{
	uint8_t *p = (uint8_t *)actual_fb;

	rng_state = seed;
	for (size_t i = 0; i < sizeof(actual_fb); i++)
		p[i] = (uint8_t)rng32();

	memcpy(expected_fb, actual_fb, sizeof(actual_fb));
	set_fb(actual_fb, FB_PITCH_WORDS);
}

static int check_frame(const char *name)
{
	const uint8_t *a = (const uint8_t *)actual_fb;
	const uint8_t *e = (const uint8_t *)expected_fb;

	for (size_t i = 0; i < sizeof(actual_fb); i++) {
		if (a[i] != e[i]) {
			size_t row = i / (FB_PITCH_WORDS * sizeof(uint32_t));
			size_t col = i % (FB_PITCH_WORDS * sizeof(uint32_t));
			printf("FAIL %-30s byte=%zu row=%zu byte_col=%zu actual=%02x expected=%02x\n",
				name, i, row, col, a[i], e[i]);
			failures++;
			return 0;
		}
	}

	printf("ok   %s\n", name);
	return 1;
}

static int bpp_for_format(uint32_t color_format)
{
	switch (color_format) {
	case MNTVA_COLOR_8BIT:
		return 1;
	case MNTVA_COLOR_16BIT565:
	case MNTVA_COLOR_15BIT:
		return 2;
	case MNTVA_COLOR_32BIT:
		return 4;
	default:
		return 0;
	}
}

static uint8_t *row8(uint32_t *fb, int pitch_words, int y)
{
	return (uint8_t *)fb + ((size_t)y * pitch_words * sizeof(uint32_t));
}

static uint16_t *row16(uint32_t *fb, int pitch_words, int y)
{
	return (uint16_t *)((uint8_t *)fb + ((size_t)y * pitch_words * sizeof(uint32_t)));
}

static uint32_t *row32(uint32_t *fb, int pitch_words, int y)
{
	return fb + ((size_t)y * pitch_words);
}

static uint32_t get_pixel(uint32_t *fb, int pitch_words, int x, int y, uint32_t color_format)
{
	switch (color_format) {
	case MNTVA_COLOR_8BIT:
		return row8(fb, pitch_words, y)[x];
	case MNTVA_COLOR_16BIT565:
	case MNTVA_COLOR_15BIT:
		return row16(fb, pitch_words, y)[x];
	case MNTVA_COLOR_32BIT:
		return row32(fb, pitch_words, y)[x];
	default:
		return 0;
	}
}

static void put_pixel(uint32_t *fb, int pitch_words, int x, int y,
	uint32_t color_format, uint32_t value)
{
	switch (color_format) {
	case MNTVA_COLOR_8BIT:
		row8(fb, pitch_words, y)[x] = (uint8_t)value;
		break;
	case MNTVA_COLOR_16BIT565:
	case MNTVA_COLOR_15BIT:
		row16(fb, pitch_words, y)[x] = (uint16_t)value;
		break;
	case MNTVA_COLOR_32BIT:
		row32(fb, pitch_words, y)[x] = value;
		break;
	}
}

static uint32_t color_to_pixel(uint32_t color_format, uint32_t color)
{
	switch (color_format) {
	case MNTVA_COLOR_8BIT:
		return color >> 24;
	case MNTVA_COLOR_16BIT565:
	case MNTVA_COLOR_15BIT:
		return color & 0xffff;
	case MNTVA_COLOR_32BIT:
		return color;
	default:
		return 0;
	}
}

static void ref_fill_rect_solid(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
	uint32_t color, uint32_t color_format)
{
	uint32_t pixel = color_to_pixel(color_format, color);

	for (uint16_t yy = y; yy < y + h; yy++) {
		for (uint16_t xx = x; xx < x + w; xx++)
			put_pixel(expected_fb, FB_PITCH_WORDS, xx, yy, color_format, pixel);
	}
}

static void ref_fill_rect_masked_8(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
	uint32_t color, uint8_t mask)
{
	uint8_t fg = (uint8_t)(color >> 24);

	for (uint16_t yy = y; yy < y + h; yy++) {
		uint8_t *row = row8(expected_fb, FB_PITCH_WORDS, yy);
		for (uint16_t xx = x; xx < x + w; xx++)
			row[xx] = (uint8_t)((row[xx] & (uint8_t)~mask) | (fg & mask));
	}
}

static void ref_invert_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
	uint8_t mask, uint32_t color_format)
{
	for (uint16_t yy = y; yy < y + h; yy++) {
		for (uint16_t xx = x; xx < x + w; xx++) {
			uint32_t pixel = get_pixel(expected_fb, FB_PITCH_WORDS, xx, yy, color_format);
			switch (color_format) {
			case MNTVA_COLOR_8BIT:
				pixel ^= mask;
				break;
			case MNTVA_COLOR_16BIT565:
			case MNTVA_COLOR_15BIT:
				pixel ^= 0xffff;
				break;
			case MNTVA_COLOR_32BIT:
				pixel ^= 0xffffffffu;
				break;
			}
			put_pixel(expected_fb, FB_PITCH_WORDS, xx, yy, color_format, pixel);
		}
	}
}

static void ref_copy_rect_src(uint16_t dx, uint16_t dy, uint16_t w, uint16_t h,
	uint16_t sx, uint16_t sy, uint32_t color_format,
	uint32_t *src, uint32_t src_pitch)
{
	int bpp = bpp_for_format(color_format);
	uint8_t *tmp = malloc((size_t)w * h * bpp);

	if (!tmp) {
		printf("FAIL alloc copy temp\n");
		exit(2);
	}

	for (uint16_t yy = 0; yy < h; yy++) {
		for (uint16_t xx = 0; xx < w; xx++) {
			uint32_t pixel = get_pixel(src, src_pitch, sx + xx, sy + yy, color_format);
			memcpy(tmp + (((size_t)yy * w + xx) * bpp), &pixel, (size_t)bpp);
		}
	}
	for (uint16_t yy = 0; yy < h; yy++) {
		for (uint16_t xx = 0; xx < w; xx++) {
			uint32_t pixel = 0;
			memcpy(&pixel, tmp + (((size_t)yy * w + xx) * bpp), (size_t)bpp);
			put_pixel(expected_fb, FB_PITCH_WORDS, dx + xx, dy + yy, color_format, pixel);
		}
	}

	free(tmp);
}

static void ref_copy_rect_masked_8(uint16_t dx, uint16_t dy, uint16_t w, uint16_t h,
	uint16_t sx, uint16_t sy, uint32_t *src, uint32_t src_pitch, uint8_t mask)
{
	uint8_t *tmp = malloc((size_t)w * h);

	if (!tmp) {
		printf("FAIL alloc masked copy temp\n");
		exit(2);
	}

	for (uint16_t yy = 0; yy < h; yy++) {
		const uint8_t *srow = row8(src, src_pitch, sy + yy);
		memcpy(tmp + ((size_t)yy * w), srow + sx, w);
	}
	for (uint16_t yy = 0; yy < h; yy++) {
		uint8_t *drow = row8(expected_fb, FB_PITCH_WORDS, dy + yy);
		for (uint16_t xx = 0; xx < w; xx++)
			drow[dx + xx] = (uint8_t)((drow[dx + xx] & (uint8_t)~mask) |
				(tmp[(size_t)yy * w + xx] & mask));
	}

	free(tmp);
}

static void ref_draw_line_solid(int16_t x1, int16_t y1, int16_t x2_delta, int16_t y2_delta,
	uint16_t len, uint32_t color, uint32_t color_format)
{
	int32_t x = x1;
	int32_t y = y1;
	int32_t x2 = x1 + x2_delta;
	int32_t y2 = y1 + y2_delta;
	int32_t dx = x2 - x1;
	int32_t dy = y2 - y1;
	int32_t step_x = dx < 0 ? -1 : 1;
	int32_t step_y = dy < 0 ? -1 : 1;
	uint32_t dx_abs = dx < 0 ? (uint32_t)-dx : (uint32_t)dx;
	uint32_t dy_abs = dy < 0 ? (uint32_t)-dy : (uint32_t)dy;
	uint32_t draw_len = len ? len : (dx_abs >= dy_abs ? dx_abs : dy_abs);
	uint32_t pixel = color_to_pixel(color_format, color);

	put_pixel(expected_fb, FB_PITCH_WORDS, x, y, color_format, pixel);

	if (dx_abs >= dy_abs) {
		uint32_t err = dx_abs >> 1;
		for (uint32_t i = 0; i < draw_len; i++) {
			err += dy_abs;
			if (err >= dx_abs) {
				err -= dx_abs;
				y += step_y;
			}
			x += step_x;
			put_pixel(expected_fb, FB_PITCH_WORDS, x, y, color_format, pixel);
		}
	} else {
		uint32_t err = dy_abs >> 1;
		for (uint32_t i = 0; i < draw_len; i++) {
			err += dx_abs;
			if (err >= dy_abs) {
				err -= dy_abs;
				x += step_x;
			}
			y += step_y;
			put_pixel(expected_fb, FB_PITCH_WORDS, x, y, color_format, pixel);
		}
	}
}

static uint8_t ref_planar_pixel(const uint8_t *plane_data, uint8_t planes,
	uint8_t layer_mask, uint16_t src_line_pitch, int h, int sx, int sy,
	int x, int y, int invert)
{
	uint8_t pixel = 0;
	uint32_t plane_size = (uint32_t)src_line_pitch * h;
	int bit_pos = sx + x;
	int src_y = (sy + y) % h;
	uint16_t byte = (uint16_t)((bit_pos / 8) % src_line_pitch);
	uint8_t bit = (uint8_t)(0x80 >> (bit_pos & 7));

	for (uint8_t plane = 0; plane < planes && plane < 8; plane++) {
		const uint8_t *p = plane_data + ((size_t)plane_size * plane);
		uint8_t v = p[(size_t)src_y * src_line_pitch + byte];
		if (invert)
			v ^= 0xff;
		if ((layer_mask & (1u << plane)) && (v & bit))
			pixel |= (uint8_t)(1u << plane);
	}

	return pixel;
}

static void ref_p2c_rect(int16_t sx, int16_t sy, int16_t dx, int16_t dy,
	int16_t w, int16_t h, uint8_t draw_mode, uint8_t planes,
	uint8_t layer_mask, uint16_t src_line_pitch, const uint8_t *bmp_data)
{
	int invert = draw_mode & 1;

	for (int16_t yy = 0; yy < h; yy++) {
		uint8_t *drow = row8(expected_fb, FB_PITCH_WORDS, dy + yy);
		for (int16_t xx = 0; xx < w; xx++) {
			drow[dx + xx] = ref_planar_pixel(bmp_data, planes, layer_mask,
				src_line_pitch, h, sx, sy, xx, yy, invert);
		}
	}
}

static uint8_t ref_reverse_lookup(const uint32_t *pal, uint8_t planes, uint32_t color)
{
	uint8_t colors = (uint8_t)(1u << planes);

	for (uint8_t i = 0; i < colors; i++) {
		if (pal[i] == color)
			return i;
	}

	return 0;
}

static uint8_t ref_minterm_index(uint8_t src, uint8_t dst, uint8_t mode)
{
	switch (mode) {
	case MINTERM_AND:
		return src & dst;
	case MINTERM_EOR:
		return src ^ dst;
	case MINTERM_DST:
		return dst;
	case MINTERM_NOTSRC:
	case MINTERM_SRC:
		return src;
	case MINTERM_OR:
		return src | dst;
	default:
		printf("FAIL unsupported p2d reference minterm %u\n", mode);
		exit(2);
	}
}

static void ref_p2d_rect(int16_t sx, int16_t sy, int16_t dx, int16_t dy,
	int16_t w, int16_t h, uint8_t draw_mode, uint8_t planes,
	uint8_t layer_mask, uint16_t src_line_pitch, const uint8_t *bmp_data_src,
	uint32_t color_format)
{
	const uint32_t *pal = (const uint32_t *)bmp_data_src;
	const uint8_t *plane_data = bmp_data_src + (256 * sizeof(uint32_t));
	int invert = draw_mode & 1;

	for (int16_t yy = 0; yy < h; yy++) {
		for (int16_t xx = 0; xx < w; xx++) {
			uint8_t src = ref_planar_pixel(plane_data, planes, layer_mask,
				src_line_pitch, h, sx, sy, xx, yy, invert);
			uint8_t dst = 0;

			if (draw_mode != MINTERM_SRC)
				dst = ref_reverse_lookup(pal, planes,
					get_pixel(expected_fb, FB_PITCH_WORDS, dx + xx, dy + yy, color_format));

			put_pixel(expected_fb, FB_PITCH_WORDS, dx + xx, dy + yy,
				color_format, pal[ref_minterm_index(src, dst, draw_mode)]);
		}
	}
}

static void ref_template_fill_rect(uint32_t color_format, uint16_t x, uint16_t y,
	uint16_t w, uint16_t h, uint8_t draw_mode, uint8_t mask,
	uint32_t fg_color, uint32_t bg_color, uint16_t x_offset,
	const uint8_t *tmpl_data, uint16_t tmpl_pitch)
{
	uint32_t fg = color_to_pixel(color_format, fg_color);
	uint32_t bg = color_to_pixel(color_format, bg_color);
	int invert = draw_mode & INVERSVID;
	uint8_t mode = draw_mode & 0x03;

	for (uint16_t yy = 0; yy < h; yy++) {
		for (uint16_t xx = 0; xx < w; xx++) {
			uint32_t bit_pos = x_offset + xx;
			uint8_t byte = tmpl_data[(size_t)yy * tmpl_pitch + (bit_pos / 8)];
			uint8_t bit = (uint8_t)(0x80 >> (bit_pos & 7));
			int set = ((invert ? (byte ^ 0xff) : byte) & bit) != 0;

			if (mode == JAM1 && !set)
				continue;

			if (mode == JAM1 || mode == JAM2) {
				uint32_t pixel = set ? fg : bg;
				if (color_format == MNTVA_COLOR_8BIT && mask != 0xff) {
					uint8_t *drow = row8(expected_fb, FB_PITCH_WORDS, y + yy);
					drow[x + xx] = (uint8_t)((drow[x + xx] & (uint8_t)~mask) |
						((uint8_t)pixel & mask));
				} else {
					put_pixel(expected_fb, FB_PITCH_WORDS, x + xx, y + yy,
						color_format, pixel);
				}
			}
		}
	}
}

static void make_planar(uint8_t *dst, uint8_t planes, uint16_t pitch, int h, uint32_t seed)
{
	rng_state = seed;
	memset(dst, 0, (size_t)planes * pitch * h);
	for (uint8_t p = 0; p < planes; p++) {
		for (int y = 0; y < h; y++) {
			for (uint16_t x = 0; x < pitch; x++)
				dst[(size_t)p * pitch * h + (size_t)y * pitch + x] = (uint8_t)rng32();
		}
	}
}

static void test_fill_and_invert(void)
{
	seed_frame(0x1001);
	fill_rect_solid(7, 5, 31, 9, 0xa5001234, MNTVA_COLOR_8BIT);
	ref_fill_rect_solid(7, 5, 31, 9, 0xa5001234, MNTVA_COLOR_8BIT);
	check_frame("fill_rect_solid 8");

	seed_frame(0x1002);
	fill_rect_solid(11, 3, 22, 13, 0x00005678, MNTVA_COLOR_16BIT565);
	ref_fill_rect_solid(11, 3, 22, 13, 0x00005678, MNTVA_COLOR_16BIT565);
	check_frame("fill_rect_solid 16");

	seed_frame(0x1003);
	fill_rect_solid(2, 9, 25, 8, 0x89abcdef, MNTVA_COLOR_32BIT);
	ref_fill_rect_solid(2, 9, 25, 8, 0x89abcdef, MNTVA_COLOR_32BIT);
	check_frame("fill_rect_solid 32");

	seed_frame(0x1004);
	fill_rect(6, 7, 37, 11, 0x5a001122, MNTVA_COLOR_8BIT, 0x3c);
	ref_fill_rect_masked_8(6, 7, 37, 11, 0x5a001122, 0x3c);
	check_frame("fill_rect masked 8");

	seed_frame(0x1005);
	invert_rect(5, 2, 39, 12, 0x5a, MNTVA_COLOR_8BIT);
	ref_invert_rect(5, 2, 39, 12, 0x5a, MNTVA_COLOR_8BIT);
	check_frame("invert_rect 8");

	seed_frame(0x1006);
	invert_rect(4, 4, 27, 10, 0xff, MNTVA_COLOR_32BIT);
	ref_invert_rect(4, 4, 27, 10, 0xff, MNTVA_COLOR_32BIT);
	check_frame("invert_rect 32");
}

static void test_copy(void)
{
	seed_frame(0x2001);
	copy_rect_nomask(14, 12, 35, 16, 3, 4, MNTVA_COLOR_8BIT,
		actual_fb, FB_PITCH_WORDS, MINTERM_SRC);
	ref_copy_rect_src(14, 12, 35, 16, 3, 4, MNTVA_COLOR_8BIT,
		expected_fb, FB_PITCH_WORDS);
	check_frame("copy_rect_nomask 8 overlap");

	seed_frame(0x2002);
	copy_rect_nomask(12, 13, 28, 14, 5, 6, MNTVA_COLOR_16BIT565,
		actual_fb, FB_PITCH_WORDS, MINTERM_SRC);
	ref_copy_rect_src(12, 13, 28, 14, 5, 6, MNTVA_COLOR_16BIT565,
		expected_fb, FB_PITCH_WORDS);
	check_frame("copy_rect_nomask 16 overlap");

	seed_frame(0x2003);
	copy_rect_nomask(8, 7, 30, 17, 20, 19, MNTVA_COLOR_32BIT,
		actual_fb, FB_PITCH_WORDS, MINTERM_SRC);
	ref_copy_rect_src(8, 7, 30, 17, 20, 19, MNTVA_COLOR_32BIT,
		expected_fb, FB_PITCH_WORDS);
	check_frame("copy_rect_nomask 32 reverse");

	seed_frame(0x2004);
	copy_rect(18, 11, 33, 15, 7, 3, MNTVA_COLOR_8BIT,
		actual_fb, FB_PITCH_WORDS, 0x5a);
	ref_copy_rect_masked_8(18, 11, 33, 15, 7, 3,
		expected_fb, FB_PITCH_WORDS, 0x5a);
	check_frame("copy_rect masked 8");
}

static void test_lines(void)
{
	seed_frame(0x3001);
	draw_line_solid(4, 8, 42, 0, 0, 0xde000000, MNTVA_COLOR_8BIT);
	ref_draw_line_solid(4, 8, 42, 0, 0, 0xde000000, MNTVA_COLOR_8BIT);
	check_frame("draw_line_solid hline 8");

	seed_frame(0x3002);
	draw_line_solid(18, 3, 0, 31, 0, 0x00001234, MNTVA_COLOR_16BIT565);
	ref_draw_line_solid(18, 3, 0, 31, 0, 0x00001234, MNTVA_COLOR_16BIT565);
	check_frame("draw_line_solid vline 16");

	seed_frame(0x3003);
	draw_line_solid(9, 6, 25, 25, 0, 0xff884422, MNTVA_COLOR_32BIT);
	ref_draw_line_solid(9, 6, 25, 25, 0, 0xff884422, MNTVA_COLOR_32BIT);
	check_frame("draw_line_solid diag 32");
}

static void test_planar(void)
{
	enum {
		P2C_W = 41,
		P2C_H = 13,
		P2C_PITCH = 8,
		P2C_PLANES = 5,
	};
	uint8_t planar[P2C_PLANES * P2C_PITCH * P2C_H];

	make_planar(planar, P2C_PLANES, P2C_PITCH, P2C_H, 0x4001);
	seed_frame(0x4002);
	p2c_rect(3, 0, 10, 7, P2C_W, P2C_H, MINTERM_SRC,
		P2C_PLANES, 0xff, 0x1f, P2C_PITCH, planar);
	ref_p2c_rect(3, 0, 10, 7, P2C_W, P2C_H, MINTERM_SRC,
		P2C_PLANES, 0x1f, P2C_PITCH, planar);
	check_frame("p2c_rect src");

	make_planar(planar, P2C_PLANES, P2C_PITCH, P2C_H, 0x4003);
	seed_frame(0x4004);
	p2c_rect(5, 0, 12, 9, P2C_W, P2C_H, MINTERM_NOTSRC,
		P2C_PLANES, 0xff, 0x1f, P2C_PITCH, planar);
	ref_p2c_rect(5, 0, 12, 9, P2C_W, P2C_H, MINTERM_NOTSRC,
		P2C_PLANES, 0x1f, P2C_PITCH, planar);
	check_frame("p2c_rect notsrc");
}

static void test_p2d(void)
{
	enum {
		P2D_W = 37,
		P2D_H = 12,
		P2D_PITCH = 8,
		P2D_PLANES = 3,
		P2D_COLORS = 1 << P2D_PLANES,
		P2D_BYTES = 256 * 4 + P2D_PLANES * P2D_PITCH * P2D_H,
	};
	uint8_t data[P2D_BYTES];
	uint32_t *pal = (uint32_t *)data;
	uint8_t *planes = data + 256 * 4;

	for (int i = 0; i < 256; i++)
		pal[i] = 0xff000000u | (uint32_t)(i * 0x00010101u);
	make_planar(planes, P2D_PLANES, P2D_PITCH, P2D_H, 0x5001);

	seed_frame(0x5002);
	for (int y = 0; y < FB_H; y++) {
		for (int x = 0; x < FB_W; x++) {
			uint8_t idx = (uint8_t)((x + y) & (P2D_COLORS - 1));
			put_pixel(actual_fb, FB_PITCH_WORDS, x, y, MNTVA_COLOR_32BIT, pal[idx]);
			put_pixel(expected_fb, FB_PITCH_WORDS, x, y, MNTVA_COLOR_32BIT, pal[idx]);
		}
	}
	p2d_rect(2, 0, 11, 6, P2D_W, P2D_H, MINTERM_EOR,
		P2D_PLANES, 0xff, 0x07, 0x00ffffff, P2D_PITCH, data, MNTVA_COLOR_32BIT);
	ref_p2d_rect(2, 0, 11, 6, P2D_W, P2D_H, MINTERM_EOR,
		P2D_PLANES, 0x07, P2D_PITCH, data, MNTVA_COLOR_32BIT);
	check_frame("p2d_rect eor 32");

	for (int i = 0; i < 256; i++)
		pal[i] = (uint32_t)(0x1000 + i);
	make_planar(planes, P2D_PLANES, P2D_PITCH, P2D_H, 0x5003);
	seed_frame(0x5004);
	p2d_rect(1, 0, 9, 8, P2D_W, P2D_H, MINTERM_SRC,
		P2D_PLANES, 0xff, 0x07, 0xffff, P2D_PITCH, data, MNTVA_COLOR_16BIT565);
	ref_p2d_rect(1, 0, 9, 8, P2D_W, P2D_H, MINTERM_SRC,
		P2D_PLANES, 0x07, P2D_PITCH, data, MNTVA_COLOR_16BIT565);
	check_frame("p2d_rect src 16");

	for (int i = 0; i < 256; i++)
		pal[i] = 0xff000000u | (uint32_t)(i * 0x00010101u);
	make_planar(planes, P2D_PLANES, P2D_PITCH, P2D_H, 0x5005);
	seed_frame(0x5006);
	p2d_rect(4, 0, 7, 5, P2D_W, P2D_H, MINTERM_NOTSRC,
		P2D_PLANES, 0xff, 0x07, 0x00ffffff, P2D_PITCH, data, MNTVA_COLOR_32BIT);
	ref_p2d_rect(4, 0, 7, 5, P2D_W, P2D_H, MINTERM_NOTSRC,
		P2D_PLANES, 0x07, P2D_PITCH, data, MNTVA_COLOR_32BIT);
	check_frame("p2d_rect notsrc 32");
}

static void test_template(void)
{
	enum {
		T_W = 35,
		T_H = 11,
		T_PITCH = 8,
	};
	uint8_t tmpl[T_H * T_PITCH];

	rng_state = 0x6001;
	for (size_t i = 0; i < sizeof(tmpl); i++)
		tmpl[i] = (uint8_t)rng32();

	seed_frame(0x6002);
	set_fb(actual_fb, FB_PITCH_WORDS * sizeof(uint32_t));
	template_fill_rect(MNTVA_COLOR_8BIT, 9, 5, T_W, T_H, JAM2, 0x3c,
		0xa5000000, 0x5a000000, 3, 0, tmpl, T_PITCH);
	set_fb(actual_fb, FB_PITCH_WORDS);
	ref_template_fill_rect(MNTVA_COLOR_8BIT, 9, 5, T_W, T_H, JAM2, 0x3c,
		0xa5000000, 0x5a000000, 3, tmpl, T_PITCH);
	check_frame("template_fill_rect jam2 8");

	seed_frame(0x6003);
	set_fb(actual_fb, FB_PITCH_WORDS * sizeof(uint32_t));
	template_fill_rect(MNTVA_COLOR_32BIT, 4, 7, T_W, T_H, JAM1, 0xff,
		0xff112233, 0xff556677, 5, 0, tmpl, T_PITCH);
	set_fb(actual_fb, FB_PITCH_WORDS);
	ref_template_fill_rect(MNTVA_COLOR_32BIT, 4, 7, T_W, T_H, JAM1, 0xff,
		0xff112233, 0xff556677, 5, tmpl, T_PITCH);
	check_frame("template_fill_rect jam1 32");
}

static uint64_t monotime_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t checksum_words(const uint32_t *buf, size_t words)
{
	uint64_t hash = 1469598103934665603ull;

	for (size_t i = 0; i < words; i++) {
		hash ^= buf[i];
		hash *= 1099511628211ull;
	}

	return hash;
}

static void bench_one(const char *name, void (*fn)(void), unsigned iterations)
{
	uint64_t start = monotime_ns();

	for (unsigned i = 0; i < iterations; i++)
		fn();

	uint64_t elapsed = monotime_ns() - start;
	printf("%-28s %10.1f ns/op\n", name, (double)elapsed / iterations);
}

static void bench_fill8(void)
{
	fill_rect_solid(3, 2, 80, 48, 0x7b000000, MNTVA_COLOR_8BIT);
}

static void bench_fill32(void)
{
	fill_rect_solid(3, 2, 80, 48, 0xff336699, MNTVA_COLOR_32BIT);
}

static void bench_copy32(void)
{
	copy_rect_nomask(17, 15, 64, 40, 2, 3, MNTVA_COLOR_32BIT,
		actual_fb, FB_PITCH_WORDS, MINTERM_SRC);
}

static void bench_p2c(void)
{
	enum {
		B_W = 72,
		B_H = 48,
		B_PITCH = 12,
		B_PLANES = 8,
	};
	static uint8_t planar[B_PLANES * B_PITCH * B_H];
	static int initialized;

	if (!initialized) {
		make_planar(planar, B_PLANES, B_PITCH, B_H, 0x7001);
		initialized = 1;
	}

	p2c_rect(3, 0, 8, 8, B_W, B_H, MINTERM_SRC,
		B_PLANES, 0xff, 0xff, B_PITCH, planar);
}

static void bench_p2d(void)
{
	enum {
		B_W = 72,
		B_H = 48,
		B_PITCH = 12,
		B_PLANES = 8,
		B_BYTES = 256 * 4 + B_PLANES * B_PITCH * B_H,
	};
	static uint8_t data[B_BYTES];
	static int initialized;

	if (!initialized) {
		uint32_t *pal = (uint32_t *)data;
		for (int i = 0; i < 256; i++)
			pal[i] = 0xff000000u | (uint32_t)(i * 0x00010101u);
		make_planar(data + 256 * 4, B_PLANES, B_PITCH, B_H, 0x7002);
		initialized = 1;
	}

	p2d_rect(3, 0, 8, 8, B_W, B_H, MINTERM_SRC,
		B_PLANES, 0xff, 0xff, 0x00ffffff, B_PITCH, data, MNTVA_COLOR_32BIT);
}

static void run_benchmarks(void)
{
	seed_frame(0x7000);
	bench_one("fill_rect_solid 8", bench_fill8, 200000);
	bench_one("fill_rect_solid 32", bench_fill32, 120000);
	bench_one("copy_rect_nomask 32", bench_copy32, 120000);
	bench_one("p2c_rect src 8 planes", bench_p2c, 20000);
	bench_one("p2d_rect src 8 planes", bench_p2d, 20000);
	printf("checksum %016llx\n", (unsigned long long)checksum_words(actual_fb, FB_WORDS));
}

static void run_tests(void)
{
	test_fill_and_invert();
	test_copy();
	test_lines();
	test_planar();
	test_p2d();
	test_template();
}

int main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--bench") == 0) {
		run_benchmarks();
		return 0;
	}

	run_tests();
	if (failures) {
		printf("%u RTG regression test(s) failed\n", failures);
		return 1;
	}

	printf("all RTG regression tests passed\n");
	return 0;
}
