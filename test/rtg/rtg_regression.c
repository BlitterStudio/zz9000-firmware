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

/* The guard zone sits directly after the fb inside one object so clamp-test
 * overruns land somewhere deterministic; check_guard verifies it is never
 * written. */
static struct {
	uint32_t fb[FB_WORDS];
	uint32_t guard[FB_PITCH_WORDS * 16];
} actual_buf;
#define actual_fb (actual_buf.fb)
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

static void ref_blit_bytes(uint16_t dx, uint16_t dy, uint16_t w, uint16_t h,
	const uint8_t *src, uint16_t src_pitch, uint16_t dest_pitch)
{
	uint8_t *tmp = malloc((size_t)w * h);
	uint8_t *dst_base = (uint8_t *)expected_fb;

	if (!tmp) {
		printf("FAIL alloc blit temp\n");
		exit(2);
	}
	for (uint16_t y = 0; y < h; y++)
		memcpy(tmp + (size_t)y * w, src + (size_t)y * src_pitch, w);
	for (uint16_t y = 0; y < h; y++)
		memcpy(dst_base + dx + ((size_t)(dy + y) * dest_pitch),
			tmp + (size_t)y * w, w);
	free(tmp);
}

static void ref_blit_bytes_masked(uint16_t dx, uint16_t dy, uint16_t w, uint16_t h,
	const uint8_t *src, uint16_t src_pitch, uint16_t dest_pitch, uint8_t mask_color)
{
	uint8_t *dst_base = (uint8_t *)expected_fb;

	for (uint16_t y = 0; y < h; y++) {
		const uint8_t *srow = src + (size_t)y * src_pitch;
		uint8_t *drow = dst_base + dx + ((size_t)(dy + y) * dest_pitch);
		for (uint16_t x = 0; x < w; x++) {
			if (srow[x] != mask_color)
				drow[x] = srow[x];
		}
	}
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

/* Independent oracle for the P96 line contract (no shared code with the
 * firmware recurrence): pixel i (i = 0..L) of a line from (ox,oy) with deltas
 * (dx,dy) sits at minor offset floor((2*i*S + L) / (2*L)) == round(i*S/L) with
 * ties going up. DrawLineDefault uses round-half-up: verified on ZZ9000
 * hardware (the RoundCheck test drew DrawLineDefault beside both candidate
 * staircases; round-half-up matched pixel-for-pixel, truncation was 1px off).
 * The wiki's sExtend formula reduces to floor(i*S/L), but that describes the
 * minor EXTENT used for clip rectangles, not the per-pixel stepping. */
static void ref_draw_line_oracle(int ox, int oy, int dx, int dy,
	uint32_t color, uint32_t color_format)
{
	int dxa = dx < 0 ? -dx : dx;
	int dya = dy < 0 ? -dy : dy;
	int horiz = dxa >= dya;
	int L = horiz ? dxa : dya;
	int S = horiz ? dya : dxa;
	int sx = dx < 0 ? -1 : 1;
	int sy = dy < 0 ? -1 : 1;
	uint32_t pixel = color_to_pixel(color_format, color);
	int Ldiv = L > 0 ? L : 1;

	for (int i = 0; i <= L; i++) {
		int minor = (2 * i * S + L) / (2 * Ldiv);
		int x = horiz ? ox + sx * i : ox + sx * minor;
		int y = horiz ? oy + sy * minor : oy + sy * i;
		put_pixel(expected_fb, FB_PITCH_WORDS, x, y, color_format, pixel);
	}
}

/* As ref_draw_line_oracle, but skips pixels outside the framebuffer so a line
 * whose origin is off-screen (a negative Xorigin/Yorigin from top/left clipping)
 * marks only its on-screen part. put_pixel itself does not bounds-check, so the
 * clipping lives here rather than in the shared helper. */
static void ref_draw_line_oracle_clipped(int ox, int oy, int dx, int dy,
	uint32_t color, uint32_t color_format)
{
	int dxa = dx < 0 ? -dx : dx;
	int dya = dy < 0 ? -dy : dy;
	int horiz = dxa >= dya;
	int L = horiz ? dxa : dya;
	int S = horiz ? dya : dxa;
	int sx = dx < 0 ? -1 : 1;
	int sy = dy < 0 ? -1 : 1;
	uint32_t pixel = color_to_pixel(color_format, color);
	int Ldiv = L > 0 ? L : 1;

	for (int i = 0; i <= L; i++) {
		int minor = (2 * i * S + L) / (2 * Ldiv);
		int x = horiz ? ox + sx * i : ox + sx * minor;
		int y = horiz ? oy + sy * minor : oy + sy * i;
		if (x < 0 || x >= FB_W || y < 0 || y >= FB_H)
			continue;
		put_pixel(expected_fb, FB_PITCH_WORDS, x, y, color_format, pixel);
	}
}

/* Oracle pixel at major index i (0..L) of the line from (ox,oy), deltas (dx,dy). */
static void oracle_point(int ox, int oy, int dx, int dy, int i, int *px, int *py)
{
	int dxa = dx < 0 ? -dx : dx;
	int dya = dy < 0 ? -dy : dy;
	int horiz = dxa >= dya;
	int L = horiz ? dxa : dya;
	int S = horiz ? dya : dxa;
	int sx = dx < 0 ? -1 : 1;
	int sy = dy < 0 ? -1 : 1;
	int Ldiv = L > 0 ? L : 1;
	int minor = (2 * i * S + L) / (2 * Ldiv);

	*px = horiz ? ox + sx * i : ox + sx * minor;
	*py = horiz ? oy + sy * minor : oy + sy * i;
}

/* Compare actual vs expected without the per-case "ok" chatter; returns 1 on
 * mismatch. */
static int frames_differ(void)
{
	return memcmp(actual_fb, expected_fb, sizeof(actual_fb)) != 0;
}

/* Work-variable seed the driver computes for a line segment: the round-half-up
 * Bresenham accumulator at the segment start, derived from geometry in
 * magnitudes so it is octant-independent (the signed twoSDminusLD is not used).
 * For a segment starting k major / m minor units from the origin, the
 * accumulator is e = S*k - L*m + L/2 == (S*k + L/2) mod L, in [0, L); L/2 for
 * an unclipped line. The L/2 bias is what makes the staircase round-half-up
 * (matching DrawLineDefault). The firmware does e += S; if (e >= L) { e -= L;
 * minor-step; }. */
static int32_t p96_err_seed(int dx, int dy, int seg_x, int seg_y, int ox, int oy)
{
	int dxa = dx < 0 ? -dx : dx;
	int dya = dy < 0 ? -dy : dy;
	int horiz = dxa >= dya;
	int L = horiz ? dxa : dya;
	int S = horiz ? dya : dxa;
	int dmajor = horiz ? seg_x - ox : seg_y - oy;
	int dminor = horiz ? seg_y - oy : seg_x - ox;
	int k = dmajor < 0 ? -dmajor : dmajor;
	int m = dminor < 0 ? -dminor : dminor;
	return S * k - L * m + L / 2;
}

/* Oracle for a patterned (JAM2) line: pixel i uses pattern bit
 * 0x8000 >> ((pattern_shift + i) & 15); foreground where set, background where
 * clear. The geometry is the same independent Bresenham oracle as above. */
static void ref_draw_line_pattern_oracle(int ox, int oy, int dx, int dy,
	uint16_t pattern, int pattern_shift, uint32_t fg, uint32_t bg,
	uint32_t color_format)
{
	int dxa = dx < 0 ? -dx : dx;
	int dya = dy < 0 ? -dy : dy;
	int horiz = dxa >= dya;
	int L = horiz ? dxa : dya;
	int S = horiz ? dya : dxa;
	int sx = dx < 0 ? -1 : 1;
	int sy = dy < 0 ? -1 : 1;
	int Ldiv = L > 0 ? L : 1;
	uint32_t fgp = color_to_pixel(color_format, fg);
	uint32_t bgp = color_to_pixel(color_format, bg);

	for (int i = 0; i <= L; i++) {
		int minor = (2 * i * S + L) / (2 * Ldiv);
		int x = horiz ? ox + sx * i : ox + sx * minor;
		int y = horiz ? oy + sy * minor : oy + sy * i;
		int on = (pattern & (0x8000 >> ((pattern_shift + i) & 15))) != 0;
		put_pixel(expected_fb, FB_PITCH_WORDS, x, y, color_format, on ? fgp : bgp);
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

/* Independent oracle for BlitPattern (pattern_fill_rect): a 16-wide,
 * loop_rows-tall monochrome pattern (2 bytes/row, MSB-first) tiled across the
 * rect, phase (x_offset,y_offset) measured from the rect's top-left. JAM1 sets
 * fg on set bits (others untouched), JAM2 sets fg/bg, COMPLEMENT XOR-inverts
 * the dst on set bits; INVERSVID flips the pattern bits. Deliberately a naive
 * per-pixel implementation that shares no code with the firmware's byte-wrap,
 * 8-pixel fast path, or JAM2 block-copy "cheat" path. */
static void ref_pattern_fill_rect(uint32_t color_format, uint16_t x, uint16_t y,
	uint16_t w, uint16_t h, uint8_t draw_mode, uint8_t mask,
	uint32_t fg_color, uint32_t bg_color, uint16_t x_offset, uint16_t y_offset,
	const uint8_t *tmpl_data, uint16_t loop_rows)
{
	uint32_t fg = color_to_pixel(color_format, fg_color);
	uint32_t bg = color_to_pixel(color_format, bg_color);
	int invert = draw_mode & INVERSVID;
	uint8_t mode = draw_mode & 0x03;

	for (uint16_t yy = 0; yy < h; yy++) {
		for (uint16_t xx = 0; xx < w; xx++) {
			uint32_t pcol = (uint32_t)(x_offset + xx) % 16;
			uint32_t prow = (uint32_t)(y_offset + yy) % loop_rows;
			uint8_t byte = tmpl_data[prow * 2 + (pcol / 8)];
			uint8_t bit = (uint8_t)(0x80 >> (pcol & 7));
			int set = ((invert ? (byte ^ 0xff) : byte) & bit) != 0;

			if (mode == COMPLEMENT) {
				if (!set)
					continue;
				if (color_format == MNTVA_COLOR_8BIT)
					row8(expected_fb, FB_PITCH_WORDS, y + yy)[x + xx] ^= mask;
				else if (color_format == MNTVA_COLOR_32BIT)
					row32(expected_fb, FB_PITCH_WORDS, y + yy)[x + xx] ^= 0xFFFFFFFFu;
				else
					row16(expected_fb, FB_PITCH_WORDS, y + yy)[x + xx] ^= 0xFFFFu;
				continue;
			}

			if (mode == JAM1 && !set)
				continue;

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
	draw_line_solid(4, 8, 42, 0, 42, p96_err_seed(42, 0, 4, 8, 4, 8), 0xde000000, MNTVA_COLOR_8BIT);
	ref_draw_line_solid(4, 8, 42, 0, 42, 0xde000000, MNTVA_COLOR_8BIT);
	check_frame("draw_line_solid hline 8");

	seed_frame(0x3002);
	draw_line_solid(18, 3, 0, 31, 31, p96_err_seed(0, 31, 18, 3, 18, 3), 0x00001234, MNTVA_COLOR_16BIT565);
	ref_draw_line_solid(18, 3, 0, 31, 31, 0x00001234, MNTVA_COLOR_16BIT565);
	check_frame("draw_line_solid vline 16");

	seed_frame(0x3003);
	draw_line_solid(9, 6, 25, 25, 25, p96_err_seed(25, 25, 9, 6, 9, 6), 0xff884422, MNTVA_COLOR_32BIT);
	ref_draw_line_solid(9, 6, 25, 25, 25, 0xff884422, MNTVA_COLOR_32BIT);
	check_frame("draw_line_solid diag 32");

	/* Steep (major-Y, dy_abs > dx_abs) solid lines exercise the otherwise
	 * untested major-Y branch of draw_line_solid - the 45-degree "diag" case
	 * above takes the major-X branch. Slope 2:1 makes the round-half-up staircase
	 * unambiguous; both step directions are covered. */
	seed_frame(0x3004);
	draw_line_solid(20, 4, 14, 28, 28, p96_err_seed(14, 28, 20, 4, 20, 4), 0x000089ab, MNTVA_COLOR_16BIT565);
	ref_draw_line_oracle(20, 4, 14, 28, 0x000089ab, MNTVA_COLOR_16BIT565);
	check_frame("draw_line_solid steep 2:1 major-Y down");

	seed_frame(0x3005);
	draw_line_solid(60, 40, 14, -28, 28, p96_err_seed(14, -28, 60, 40, 60, 40), 0xaa113355, MNTVA_COLOR_32BIT);
	ref_draw_line_oracle(60, 40, 14, -28, 0xaa113355, MNTVA_COLOR_32BIT);
	check_frame("draw_line_solid steep 2:1 major-Y up");
}

static void test_lines_clipped(void)
{
	/* Full odd-major line must match the independent round-half-up oracle. An
	 * odd L is where round-half-up and truncation diverge, so this pins the
	 * firmware to DrawLineDefault's exact staircase (unclipped seed L/2) and
	 * guards against a regression back to truncation (seed 0). */
	{
		int ox = 6, oy = 9, dx = 31, dy = 13;	/* L=31 (odd), S=13 */
		seed_frame(0x3101);
		draw_line_solid(ox, oy, dx, dy, 31,
			p96_err_seed(dx, dy, ox, oy, ox, oy),
			0xc4000000, MNTVA_COLOR_8BIT);
		ref_draw_line_oracle(ox, oy, dx, dy, 0xc4000000, MNTVA_COLOR_8BIT);
		check_frame("draw_line_solid full odd-L == oracle");
	}

	/* The P96 clipping contract: a line split into two segments at ANY interior
	 * point must reassemble into exactly the same pixels as the whole line.
	 * Segment B starts mid-line, so it only renders correctly if the firmware
	 * resumes the staircase from err_seed rather than restarting at (X,Y). A
	 * single clip point can accidentally land on an aligned phase, so sweep
	 * every clip point of several octants. */
	{
		static const int dxs[] = { 31, 17, 40, -33, 25, 9, -40 };
		static const int dys[] = { 13, 30, 9, 23, -25, -30, -7 };
		int ox = 48, oy = 36;	/* center, so negative octants stay in-bounds */
		unsigned bad = 0, total = 0;

		for (size_t t = 0; t < sizeof(dxs) / sizeof(dxs[0]); t++) {
			int dx = dxs[t], dy = dys[t];
			int dxa = dx < 0 ? -dx : dx, dya = dy < 0 ? -dy : dy;
			int L = dxa >= dya ? dxa : dya;

			for (int k = 1; k < L; k++) {
				int bx, by;
				oracle_point(ox, oy, dx, dy, k, &bx, &by);
				seed_frame(0x3200u + (uint32_t)(t * 64 + k));
				draw_line_solid(ox, oy, dx, dy, k,
					p96_err_seed(dx, dy, ox, oy, ox, oy),
					0xc4000000, MNTVA_COLOR_8BIT);
				draw_line_solid(bx, by, dx, dy, L - k,
					p96_err_seed(dx, dy, bx, by, ox, oy),
					0xc4000000, MNTVA_COLOR_8BIT);
				ref_draw_line_oracle(ox, oy, dx, dy, 0xc4000000, MNTVA_COLOR_8BIT);
				total++;
				if (frames_differ())
					bad++;
			}
		}

		if (bad) {
			printf("FAIL %-30s %u/%u clip points mismatch the whole line\n",
				"draw_line split == whole", bad, total);
			failures++;
		} else {
			printf("ok   draw_line split == whole (%u clip points)\n", total);
		}
	}
}

static void test_lines_negative_origin(void)
{
	/* A line clipped at the top/left has its true start above/left of the
	 * RenderInfo, so P96 hands DrawLine a NEGATIVE Xorigin/Yorigin. err_seed is
	 * computed from (segment_start - origin), so the origin must be treated as
	 * signed (the driver's field is UWORD and has to be cast through WORD, or
	 * -8 reads as 65528 and the seed is wrong). P96 clips segment A away and
	 * only asks for the on-screen segment B, which must still match the
	 * on-screen part of the whole line drawn from the negative origin. */
	int ox = 20, oy = -10, dx = 14, dy = 28, L = 28;
	int k = -oy;			/* major steps before the line reaches y = 0 */
	int bx, by;

	oracle_point(ox, oy, dx, dy, k, &bx, &by);

	seed_frame(0x3500);
	draw_line_solid(bx, by, dx, dy, L - k,
		p96_err_seed(dx, dy, bx, by, ox, oy),
		0xc4000000, MNTVA_COLOR_8BIT);
	ref_draw_line_oracle_clipped(ox, oy, dx, dy, 0xc4000000, MNTVA_COLOR_8BIT);
	check_frame("draw_line negative-origin clip resumes");

	/* Guard the signedness directly: promoting the origin unsigned (the bug)
	 * must change the seed, otherwise the WORD cast in the driver is moot. */
	if (p96_err_seed(dx, dy, bx, by, ox, oy) ==
	    p96_err_seed(dx, dy, bx, by, ox, (int)(uint16_t)oy)) {
		printf("FAIL %-30s unsigned origin leaves the seed unchanged\n",
			"negative-origin sign");
		failures++;
	} else {
		printf("ok   negative-origin seed is sign-sensitive\n");
	}
}

static void test_lines_pattern(void)
{
	/* A dotted (JAM2) line split at an interior point must reassemble exactly.
	 * Beyond the staircase seed this also requires the firmware to resume the
	 * dot pattern mid-line via cur_bit = 0x8000 >> PatternShift; segment B's
	 * PatternShift advances by the major-step count of segment A. */
	int ox = 20, oy = 30, dx = 37, dy = 15, L = 37, shift0 = 3;
	uint16_t pat = 0xCCCC;
	unsigned bad = 0, total = 0;

	for (int k = 1; k < L; k++) {
		int bx, by;
		oracle_point(ox, oy, dx, dy, k, &bx, &by);
		seed_frame(0x3400u + (uint32_t)k);
		draw_line(ox, oy, dx, dy, k,
			p96_err_seed(dx, dy, ox, oy, ox, oy),
			pat, shift0, 0xaa000000, 0x55000000,
			MNTVA_COLOR_8BIT, 0xFF, JAM2);
		draw_line(bx, by, dx, dy, L - k,
			p96_err_seed(dx, dy, bx, by, ox, oy),
			pat, (shift0 + k) & 15, 0xaa000000, 0x55000000,
			MNTVA_COLOR_8BIT, 0xFF, JAM2);
		ref_draw_line_pattern_oracle(ox, oy, dx, dy, pat, shift0,
			0xaa000000, 0x55000000, MNTVA_COLOR_8BIT);
		total++;
		if (frames_differ())
			bad++;
	}

	if (bad) {
		printf("FAIL %-30s %u/%u clip points mismatch the dotted line\n",
			"draw_line pattern split", bad, total);
		failures++;
	} else {
		printf("ok   draw_line pattern split == whole (%u clip points)\n", total);
	}
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

static void test_pattern(void)
{
	/* BlitPattern (OP_RECT_PATTERN) had no host coverage. A 16x8 monochrome
	 * pattern with a varied bit layout, tiled across rects wider than 16 and
	 * taller than loop_rows, at non-byte-aligned phases, exercises the byte
	 * wrap, the 8-pixel fast path, the vertical wrap, and the JAM2 block-copy
	 * "cheat" path (triggered by JAM2 + mask 0xFF + loop_rows <= 64). */
	static const uint8_t pat[16] = {
		0x80, 0x01,  0xC0, 0x03,  0xE0, 0x07,  0xF0, 0x0F,
		0x12, 0x48,  0x24, 0x90,  0xA5, 0x5A,  0xFF, 0x00,
	};
	static const struct {
		const char *name;
		uint32_t fmt;
		uint16_t x, y, w, h, xoff, yoff;
		uint8_t mode, mask;
		uint32_t fg, bg;
	} cases[] = {
		{ "pattern jam1 8 tiled",     MNTVA_COLOR_8BIT,     5, 4, 22, 19,  0, 0, JAM1,           0xFF, 0x11000000, 0x22000000 },
		{ "pattern jam2 8 cheat",     MNTVA_COLOR_8BIT,     6, 3, 24, 23,  3, 2, JAM2,           0xFF, 0xaa000000, 0x55000000 },
		{ "pattern jam2 8 masked",    MNTVA_COLOR_8BIT,     4, 5, 20, 14,  5, 1, JAM2,           0x0F, 0xaa000000, 0x55000000 },
		{ "pattern jam2 16 cheat",    MNTVA_COLOR_16BIT565, 8, 6, 18, 17, 11, 5, JAM2,           0xFF, 0x00001234, 0x0000fedc },
		{ "pattern jam1 32 tiled",    MNTVA_COLOR_32BIT,    3, 2, 21, 20,  7, 3, JAM1,           0xFF, 0xff884422, 0x11335577 },
		{ "pattern jam2 inversvid 8", MNTVA_COLOR_8BIT,     9, 7, 19, 12,  2, 4, JAM2|INVERSVID, 0xFF, 0x77000000, 0x33000000 },
		{ "pattern complement 16",    MNTVA_COLOR_16BIT565, 5, 8, 20, 15,  4, 6, COMPLEMENT,     0xFF, 0,          0 },
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		seed_frame(0x3600u + (uint32_t)i);
		/* pattern_fill_rect takes its pitch in BYTES (it computes fb_pitch/4
		 * words/row), like template_fill_rect and unlike fill_rect/draw_line
		 * which take words. The driver feeds it BytesPerRow accordingly; seed_frame
		 * set the word pitch, so override with the byte pitch here (then restore). */
		set_fb(actual_fb, FB_PITCH_WORDS * sizeof(uint32_t));
		pattern_fill_rect(cases[i].fmt, cases[i].x, cases[i].y, cases[i].w,
			cases[i].h, cases[i].mode, cases[i].mask, cases[i].fg, cases[i].bg,
			cases[i].xoff, cases[i].yoff, (uint8_t *)pat, 16, 8);
		set_fb(actual_fb, FB_PITCH_WORDS);
		ref_pattern_fill_rect(cases[i].fmt, cases[i].x, cases[i].y, cases[i].w,
			cases[i].h, cases[i].mode, cases[i].mask, cases[i].fg, cases[i].bg,
			cases[i].xoff, cases[i].yoff, pat, 8);
		check_frame(cases[i].name);
	}
}

static void test_acc_blit(void)
{
	const uint16_t row_bytes = FB_PITCH_WORDS * sizeof(uint32_t);

	seed_frame(0xa001);
	acc_blit_rect((uintptr_t)((uint8_t *)actual_fb + 5 * row_bytes + 3),
		(uintptr_t)actual_fb, 40, 30, 24, 9, row_bytes, row_bytes, 0, 0);
	ref_blit_bytes(40, 30, 24, 9,
		(uint8_t *)expected_fb + 5 * row_bytes + 3, row_bytes, row_bytes);
	check_frame("acc_blit_rect forward");

	/* Overlapping downward move: dest rect starts 4 rows below the source
	 * rect, so a correct implementation must copy bottom-up (mode 1). */
	seed_frame(0xa002);
	acc_blit_rect((uintptr_t)((uint8_t *)actual_fb + 10 * row_bytes + 8),
		(uintptr_t)actual_fb, 8, 14, 32, 12, row_bytes, row_bytes, 1, 0);
	ref_blit_bytes(8, 14, 32, 12,
		(uint8_t *)expected_fb + 10 * row_bytes + 8, row_bytes, row_bytes);
	check_frame("acc_blit_rect reverse overlap");

	seed_frame(0xa003);
	acc_blit_rect((uintptr_t)((uint8_t *)actual_fb + 2 * row_bytes + 1),
		(uintptr_t)actual_fb, 60, 40, 20, 7, row_bytes, row_bytes, 2, 0x42);
	ref_blit_bytes_masked(60, 40, 20, 7,
		(uint8_t *)expected_fb + 2 * row_bytes + 1, row_bytes, row_bytes, 0x42);
	check_frame("acc_blit_rect masked");
}

static void ref_clear_rows(uint16_t w, uint16_t h, uint16_t pitch_px,
	uint32_t color, int bpp)
{
	for (uint16_t y = 0; y < h; y++) {
		uint8_t *row = (uint8_t *)expected_fb + (size_t)y * pitch_px * bpp;
		for (uint16_t x = 0; x < w; x++) {
			if (bpp == 1)
				row[x] = (uint8_t)(color >> 24);
			else if (bpp == 2)
				((uint16_t *)row)[x] = (uint16_t)color;
			else
				((uint32_t *)row)[x] = color;
		}
	}
}

static void test_acc_clear(void)
{
	seed_frame(0x8001);
	acc_clear_buffer((uintptr_t)actual_fb, 60, 40, 256, 0x00003456, 2);
	ref_clear_rows(60, 40, 256, 0x00003456, 2);
	check_frame("acc_clear_buffer 16");

	seed_frame(0x8002);
	acc_clear_buffer((uintptr_t)actual_fb, 50, 30, 128, 0xdeadbeef, 4);
	ref_clear_rows(50, 30, 128, 0xdeadbeef, 4);
	check_frame("acc_clear_buffer 32");

	seed_frame(0x8003);
	acc_clear_buffer((uintptr_t)actual_fb, 70, 50, 512, 0xa5000000, 1);
	ref_clear_rows(70, 50, 512, 0xa5000000, 1);
	check_frame("acc_clear_buffer 8");
}

static void check_guard(const char *name)
{
	const uint8_t *g = (const uint8_t *)actual_buf.guard;

	for (size_t i = 0; i < sizeof(actual_buf.guard); i++) {
		if (g[i] != 0x5c) {
			printf("FAIL %-30s guard byte=%zu actual=%02x expected=5c\n",
				name, i, g[i]);
			failures++;
			return;
		}
	}
	printf("ok   %s\n", name);
}

/* Independent YUV 4:2:2 reference: the layouts are expressed as
 * byte-position -> component, the opposite direction from the production
 * table, and pixels are composed byte-by-byte in surface memory order.
 * Orders are the P96 V3.6.3-corrected ones (the old Picasso96.h comments
 * have U and V interchanged): CGX = YUY2, 422PC = UYVY. */
enum { RC_Y0, RC_Y1, RC_U, RC_V };

static const uint8_t ref_yuv_byte_comp[YUV422_VARIANT_NUM][4] = {
	{ RC_Y0, RC_U, RC_Y1, RC_V },  /* CGX:  Y0-U-Y1-V (YUY2) */
	{ RC_Y1, RC_V, RC_Y0, RC_U },  /* STD:  Y1-V-Y0-U */
	{ RC_U, RC_Y0, RC_V, RC_Y1 },  /* PC:   U-Y0-V-Y1 (UYVY) */
	{ RC_Y0, RC_Y1, RC_U, RC_V },  /* PA:   Y0-Y1-U-V */
	{ RC_V, RC_U, RC_Y1, RC_Y0 },  /* PAPC: V-U-Y1-Y0 */
};

static int32_t ref_yuv_clamp(int32_t v)
{
	return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static void ref_yuv422_rect(int16_t phase, int16_t dx, int16_t dy, int16_t w,
	int16_t h, uint8_t variant, uint8_t color_format, uint16_t src_pitch,
	const uint8_t *src)
{
	for (int16_t yy = 0; yy < h; yy++) {
		const uint8_t *srow = src + (size_t)yy * src_pitch;
		uint8_t *drow = row8(expected_fb, FB_PITCH_WORDS, dy + yy);

		for (int16_t xx = 0; xx < w; xx++) {
			int16_t sx = phase + xx;
			const uint8_t *mp = srow + (sx / 2) * 4;
			int want_y = (sx & 1) ? RC_Y1 : RC_Y0;
			int32_t y = 0, u = 0, v = 0;

			for (int b = 0; b < 4; b++) {
				uint8_t comp = ref_yuv_byte_comp[variant][b];
				if (comp == want_y)
					y = mp[b];
				else if (comp == RC_U)
					u = mp[b];
				else if (comp == RC_V)
					v = mp[b];
			}

			int32_t c = y - 16, d = u - 128, e = v - 128;
			int32_t r = ref_yuv_clamp((298 * c + 409 * e + 128) >> 8);
			int32_t g = ref_yuv_clamp((298 * c - 100 * d - 208 * e + 128) >> 8);
			int32_t bl = ref_yuv_clamp((298 * c + 516 * d + 128) >> 8);

			switch (color_format) {
			case MNTVA_COLOR_32BIT: {
				uint8_t *px = drow + (size_t)(dx + xx) * 4;
				px[0] = (uint8_t)bl;
				px[1] = (uint8_t)g;
				px[2] = (uint8_t)r;
				px[3] = 0xFF;
				break;
			}
			case MNTVA_COLOR_16BIT565: {
				uint8_t *px = drow + (size_t)(dx + xx) * 2;
				uint16_t val = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3));
				px[0] = (uint8_t)(val >> 8);   /* pixels sit big-endian in surface memory */
				px[1] = (uint8_t)val;
				break;
			}
			case MNTVA_COLOR_15BIT: {
				uint8_t *px = drow + (size_t)(dx + xx) * 2;
				uint16_t val = (uint16_t)(((r >> 3) << 10) | ((g >> 3) << 5) | (bl >> 3));
				px[0] = (uint8_t)(val >> 8);
				px[1] = (uint8_t)val;
				break;
			}
			}
		}
	}
}

static uint8_t yuv_src[192 * 64];

static void fill_yuv_src(uint32_t seed)
{
	uint32_t saved = rng_state;

	rng_state = seed;
	for (size_t i = 0; i < sizeof(yuv_src); i++)
		yuv_src[i] = (uint8_t)rng32();
	rng_state = saved;
}

static void test_yuv(void)
{
	static const uint8_t colormodes[3] = {
		MNTVA_COLOR_32BIT, MNTVA_COLOR_16BIT565, MNTVA_COLOR_15BIT,
	};
	static const char *cm_names[3] = { "32", "565", "15" };
	char name[64];

	for (uint8_t variant = 0; variant < YUV422_VARIANT_NUM; variant++) {
		for (int cm = 0; cm < 3; cm++) {
			seed_frame(0xA000 + variant * 8 + cm);
			fill_yuv_src(0xC0DE0000u + variant * 8 + cm);
			yuv422_to_rgb_rect(0, 5, 3, 24, 16, variant, colormodes[cm], 96, yuv_src);
			ref_yuv422_rect(0, 5, 3, 24, 16, variant, colormodes[cm], 96, yuv_src);
			snprintf(name, sizeof(name), "yuv422 v%u -> %s", variant, cm_names[cm]);
			check_frame(name);
		}
	}

	/* geometry edge cases */
	seed_frame(0xA100);
	fill_yuv_src(0xC0DE0100);
	yuv422_to_rgb_rect(0, 7, 2, 33, 9, YUV422_VARIANT_CGX, MNTVA_COLOR_16BIT565, 96, yuv_src);
	ref_yuv422_rect(0, 7, 2, 33, 9, YUV422_VARIANT_CGX, MNTVA_COLOR_16BIT565, 96, yuv_src);
	check_frame("yuv422 odd width");

	seed_frame(0xA101);
	fill_yuv_src(0xC0DE0101);
	yuv422_to_rgb_rect(1, 4, 1, 21, 7, YUV422_VARIANT_STD, MNTVA_COLOR_32BIT, 96, yuv_src);
	ref_yuv422_rect(1, 4, 1, 21, 7, YUV422_VARIANT_STD, MNTVA_COLOR_32BIT, 96, yuv_src);
	check_frame("yuv422 phase 1 odd width");

	seed_frame(0xA102);
	fill_yuv_src(0xC0DE0102);
	yuv422_to_rgb_rect(0, 13, 5, 1, 6, YUV422_VARIANT_PC, MNTVA_COLOR_16BIT565, 96, yuv_src);
	ref_yuv422_rect(0, 13, 5, 1, 6, YUV422_VARIANT_PC, MNTVA_COLOR_16BIT565, 96, yuv_src);
	check_frame("yuv422 w=1 odd dx");

	seed_frame(0xA103);
	fill_yuv_src(0xC0DE0103);
	yuv422_to_rgb_rect(1, 2, 4, 1, 3, YUV422_VARIANT_PA, MNTVA_COLOR_15BIT, 96, yuv_src);
	ref_yuv422_rect(1, 2, 4, 1, 3, YUV422_VARIANT_PA, MNTVA_COLOR_15BIT, 96, yuv_src);
	check_frame("yuv422 w=1 phase 1");

	seed_frame(0xA104);
	fill_yuv_src(0xC0DE0104);
	yuv422_to_rgb_rect(0, 0, 0, 40, 1, YUV422_VARIANT_PAPC, MNTVA_COLOR_32BIT, 96, yuv_src);
	ref_yuv422_rect(0, 0, 0, 40, 1, YUV422_VARIANT_PAPC, MNTVA_COLOR_32BIT, 96, yuv_src);
	check_frame("yuv422 h=1 origin");

	/* rejected inputs must leave the frame untouched */
	seed_frame(0xA105);
	yuv422_to_rgb_rect(0, 5, 3, 24, 16, YUV422_VARIANT_NUM, MNTVA_COLOR_32BIT, 96, yuv_src);
	yuv422_to_rgb_rect(0, 5, 3, 24, 16, YUV422_VARIANT_CGX, MNTVA_COLOR_8BIT, 96, yuv_src);
	yuv422_to_rgb_rect(0, 5, 3, 0, 16, YUV422_VARIANT_CGX, MNTVA_COLOR_32BIT, 96, yuv_src);
	yuv422_to_rgb_rect(0, 5, 3, 24, 0, YUV422_VARIANT_CGX, MNTVA_COLOR_32BIT, 96, yuv_src);
	yuv422_to_rgb_rect(0, -1, 3, 24, 16, YUV422_VARIANT_CGX, MNTVA_COLOR_32BIT, 96, yuv_src);
	check_frame("yuv422 rejected inputs");
}

static void test_fb_limit_clamp(void)
{
	seed_frame(0xb001);
	memset(actual_buf.guard, 0x5c, sizeof(actual_buf.guard));
	set_fb_limit((uint8_t *)actual_fb + sizeof(actual_fb));

	/* Rect of 12 rows starting 4 rows above the buffer end: only the 4 rows
	 * that fully fit below the limit may be written; the rest would land in
	 * the guard zone. */
	fill_rect_solid(10, FB_H - 4, 20, 12, 0x77000000, MNTVA_COLOR_8BIT);
	ref_fill_rect_solid(10, FB_H - 4, 20, 4, 0x77000000, MNTVA_COLOR_8BIT);
	check_frame("fill_rect_solid clamped");
	check_guard("fill_rect_solid guard");

	seed_frame(0xb002);
	memset(actual_buf.guard, 0x5c, sizeof(actual_buf.guard));
	invert_rect(5, FB_H - 2, 30, 10, 0xff, MNTVA_COLOR_8BIT);
	ref_invert_rect(5, FB_H - 2, 30, 2, 0xff, MNTVA_COLOR_8BIT);
	check_frame("invert_rect clamped");
	check_guard("invert_rect guard");

	seed_frame(0xb003);
	memset(actual_buf.guard, 0x5c, sizeof(actual_buf.guard));
	fill_yuv_src(0xC0DE0200);
	yuv422_to_rgb_rect(0, 4, FB_H - 3, 20, 8, YUV422_VARIANT_CGX,
		MNTVA_COLOR_32BIT, 96, yuv_src);
	ref_yuv422_rect(0, 4, FB_H - 3, 20, 3, YUV422_VARIANT_CGX,
		MNTVA_COLOR_32BIT, 96, yuv_src);
	check_frame("yuv422 clamped");
	check_guard("yuv422 guard");

	set_fb_limit(0);
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

static void bench_clear32(void)
{
	acc_clear_buffer((uintptr_t)actual_fb, 80, 48, FB_PITCH_WORDS, 0xff336699, 4);
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

static void bench_yuv565(void)
{
	yuv422_to_rgb_rect(0, 3, 2, 80, 48, YUV422_VARIANT_CGX,
		MNTVA_COLOR_16BIT565, 160, yuv_src);
}

static void bench_yuv32(void)
{
	yuv422_to_rgb_rect(0, 3, 2, 80, 48, YUV422_VARIANT_CGX,
		MNTVA_COLOR_32BIT, 160, yuv_src);
}

static void run_benchmarks(void)
{
	seed_frame(0x7000);
	fill_yuv_src(0x7001);
	bench_one("fill_rect_solid 8", bench_fill8, 200000);
	bench_one("fill_rect_solid 32", bench_fill32, 120000);
	bench_one("acc_clear_buffer 32", bench_clear32, 120000);
	bench_one("copy_rect_nomask 32", bench_copy32, 120000);
	bench_one("p2c_rect src 8 planes", bench_p2c, 20000);
	bench_one("p2d_rect src 8 planes", bench_p2d, 20000);
	bench_one("yuv422_to_rgb 565", bench_yuv565, 20000);
	bench_one("yuv422_to_rgb 32", bench_yuv32, 20000);
	printf("checksum %016llx\n", (unsigned long long)checksum_words(actual_fb, FB_WORDS));
}

static void run_tests(void)
{
	test_fill_and_invert();
	test_copy();
	test_lines();
	test_lines_clipped();
	test_lines_negative_origin();
	test_lines_pattern();
	test_planar();
	test_p2d();
	test_template();
	test_pattern();
	test_acc_blit();
	test_acc_clear();
	test_yuv();
	test_fb_limit_clamp();
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
