/*
 * MNT ZZ9000 Amiga Graphics and Coprocessor Card Operating System (ZZ9000OS)
 *
 * Copyright (C) 2019-2026, Lucie L. Hartmann <lucie@mntre.com>
 *                          MNT Research GmbH, Berlin
 *                          https://mntre.com
 * Copyright (C) 2026,      Dimitris Panokostas <midwan@gmail.com>
 *
 * More Info: https://mntre.com/zz9000
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * GNU General Public License v3.0 or later
 *
 * https://spdx.org/licenses/GPL-3.0-or-later.html
 *
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef __ARM_NEON__
#include <arm_neon.h>
#endif
#include "gfx.h"
#include "zz_video_modes.h"

uint32_t* fb=0;
uint32_t fb_pitch=0;

static inline void memset16(uint16_t *dst, uint16_t val, uint32_t count) {
#ifdef __ARM_NEON__
	uint16x8_t v = vdupq_n_u16(val);
	while (count >= 8) {
		vst1q_u16(dst, v);
		dst += 8; count -= 8;
	}
#else
	while (count >= 4) {
		dst[0] = val; dst[1] = val; dst[2] = val; dst[3] = val;
		dst += 4; count -= 4;
	}
#endif
	while (count--) *dst++ = val;
}

static inline void memset32(uint32_t *dst, uint32_t val, uint32_t count) {
#ifdef __ARM_NEON__
	uint32x4_t v = vdupq_n_u32(val);
	while (count >= 4) {
		vst1q_u32(dst, v);
		dst += 4; count -= 4;
	}
#else
	while (count >= 4) {
		dst[0] = val; dst[1] = val; dst[2] = val; dst[3] = val;
		dst += 4; count -= 4;
	}
#endif
	while (count--) *dst++ = val;
}

void set_fb(uint32_t* fb_, uint32_t pitch) {
	fb=fb_;
	fb_pitch=pitch;
}

static uint8_t* fb_limit = 0;

void set_fb_limit(void* limit) {
	fb_limit = (uint8_t*)limit;
}

/* Returns how many of the requested h rows starting at row y1 fit entirely
 * below fb_limit. Returns h unchanged when no limit is configured. Row size
 * is fb_pitch words; for the template/pattern ops fb_pitch is bytes, which
 * makes this strictly more conservative there (never less safe). */
static uint16_t clamp_rows_to_fb_limit(uint32_t y1, uint16_t h)
{
	uint8_t *row0;
	size_t row_bytes, rows_fit;

	if (!fb_limit || !fb)
		return h;
	row_bytes = (size_t)fb_pitch * sizeof(uint32_t);
	if (row_bytes == 0)
		return h;
	row0 = (uint8_t *)fb + (size_t)y1 * row_bytes;
	if (row0 >= fb_limit)
		return 0;
	rows_fit = (size_t)(fb_limit - row0) / row_bytes;
	if (h > rows_fit)
		h = (uint16_t)rows_fit;
	return h;
}

static inline void draw_vertical_line_solid(uint32_t *dp, int32_t x, int32_t line_step,
	uint32_t count, uint32_t fg_color, uint8_t u8_fg, uint32_t color_format)
{
	switch (color_format) {
	case MNTVA_COLOR_8BIT: {
		uint8_t *p = (uint8_t *)dp + x;
		int32_t step = line_step * (int32_t)sizeof(uint32_t);
		while (count >= 4) {
			p[0] = u8_fg;
			p[step] = u8_fg;
			p[step * 2] = u8_fg;
			p[step * 3] = u8_fg;
			p += step * 4;
			count -= 4;
		}
		while (count--) {
			*p = u8_fg;
			p += step;
		}
		break;
	}
	case MNTVA_COLOR_16BIT565:
	case MNTVA_COLOR_15BIT: {
		uint16_t *p = (uint16_t *)dp + x;
		int32_t step = line_step * 2;
		while (count >= 4) {
			p[0] = fg_color;
			p[step] = fg_color;
			p[step * 2] = fg_color;
			p[step * 3] = fg_color;
			p += step * 4;
			count -= 4;
		}
		while (count--) {
			*p = fg_color;
			p += step;
		}
		break;
	}
	case MNTVA_COLOR_32BIT: {
		uint32_t *p = dp + x;
		while (count >= 4) {
			p[0] = fg_color;
			p[line_step] = fg_color;
			p[line_step * 2] = fg_color;
			p[line_step * 3] = fg_color;
			p += line_step * 4;
			count -= 4;
		}
		while (count--) {
			*p = fg_color;
			p += line_step;
		}
		break;
	}
	default:
		break;
	}
}

static inline void draw_horizontal_line_solid(uint32_t *dp, int32_t x,
	uint32_t count, uint32_t fg_color, uint8_t u8_fg, uint32_t color_format)
{
	switch (color_format) {
	case MNTVA_COLOR_8BIT:
		memset((uint8_t *)dp + x, u8_fg, count);
		break;
	case MNTVA_COLOR_16BIT565:
	case MNTVA_COLOR_15BIT:
		memset16((uint16_t *)dp + x, fg_color, count);
		break;
	case MNTVA_COLOR_32BIT:
		memset32(dp + x, fg_color, count);
		break;
	default:
		break;
	}
}

extern int sprite_request_update_data;

uint8_t color_map_16_to_8[65536];

void *get_color_conversion_table(int index)
{
	switch (index) {
		case 0:
			return (void *)color_map_16_to_8;
		default:
			return 0;
	}
}

void fill_rect(uint16_t rect_x1, uint16_t rect_y1, uint16_t w, uint16_t h, uint32_t fg_color, uint32_t color_format, uint8_t mask)
{
	h = clamp_rows_to_fb_limit(rect_y1, h);
	if (!h)
		return;

	uint32_t* dp = fb + (rect_y1 * fb_pitch);
	uint8_t u8_fg = fg_color >> 24;
	uint16_t rect_y2 = rect_y1 + h, rect_x2 = rect_x1 + w;
	uint16_t x;

	for (uint16_t cur_y = rect_y1; cur_y < rect_y2; cur_y++) {
		x = rect_x1;
		switch(color_format) {
			case MNTVA_COLOR_8BIT:
#ifdef __ARM_NEON__
				{
					uint8_t *p8 = (uint8_t *)dp + x;
					uint16_t remaining = rect_x2 - x;
					uint8x16_t vfg = vdupq_n_u8(u8_fg & mask);
					uint8x16_t vmask = vdupq_n_u8(mask);
					while (remaining >= 16) {
						vst1q_u8(p8, vbslq_u8(vmask, vfg, vld1q_u8(p8)));
						p8 += 16; remaining -= 16;
					}
					x = rect_x2 - remaining;
				}
#endif
				while(x < rect_x2) {
					SET_FG_PIXEL8_MASK(0);
					x++;
				}
				break;
		case MNTVA_COLOR_16BIT565:
		case MNTVA_COLOR_15BIT:
			memset16((uint16_t *)dp + x, fg_color, rect_x2 - x);
			break;
		case MNTVA_COLOR_32BIT:
			memset32(dp + x, fg_color, rect_x2 - x);
			break;
		default:
			break;
		}
		dp += fb_pitch;
	}
}

void fill_rect_solid(uint16_t rect_x1, uint16_t rect_y1, uint16_t w, uint16_t h, uint32_t rect_rgb, uint32_t color_format)
{
	h = clamp_rows_to_fb_limit(rect_y1, h);
	if (!h)
		return;

	uint32_t* p = fb + (rect_y1 * fb_pitch);
	uint16_t rect_y2 = rect_y1 + h;

	if (w == 1) {
		draw_vertical_line_solid(p, rect_x1, fb_pitch, h, rect_rgb,
				rect_rgb >> 24, color_format);
		return;
	}
	if (h == 1) {
		draw_horizontal_line_solid(p, rect_x1, w, rect_rgb,
				rect_rgb >> 24, color_format);
		return;
	}

	for (uint16_t cur_y = rect_y1; cur_y < rect_y2; cur_y++) {
		switch(color_format) {
			case MNTVA_COLOR_8BIT:
				memset((uint8_t *)p + rect_x1, (uint8_t)(rect_rgb >> 24), w);
				break;
		case MNTVA_COLOR_16BIT565:
		case MNTVA_COLOR_15BIT:
			memset16((uint16_t *)p + rect_x1, rect_rgb, w);
			break;
		case MNTVA_COLOR_32BIT:
			memset32(p + rect_x1, rect_rgb, w);
			break;
		default:
			break;
		}
		p += fb_pitch;
	}
}

void invert_rect(uint16_t rect_x1, uint16_t rect_y1, uint16_t w, uint16_t h, uint8_t mask, uint32_t color_format)
{
	h = clamp_rows_to_fb_limit(rect_y1, h);
	if (!h)
		return;

	uint32_t* dp = fb + (rect_y1 * fb_pitch);
	uint16_t x;

	uint16_t rect_y2 = rect_y1 + h, rect_x2 = rect_x1 + w;

	for (uint16_t cur_y = rect_y1; cur_y < rect_y2; cur_y++) {
		x = rect_x1;
		switch (color_format) {
		case MNTVA_COLOR_8BIT:
#ifdef __ARM_NEON__
			{
				uint8_t *p8 = (uint8_t *)dp + x;
				uint16_t remaining = rect_x2 - x;
				uint8x16_t vmask = vdupq_n_u8(mask);
				while (remaining >= 16) {
					vst1q_u8(p8, veorq_u8(vld1q_u8(p8), vmask));
					p8 += 16; remaining -= 16;
				}
				x = rect_x2 - remaining;
			}
#else
			while (x + 4 <= rect_x2) {
				((uint8_t *)dp)[x] ^= mask;
				((uint8_t *)dp)[x+1] ^= mask;
				((uint8_t *)dp)[x+2] ^= mask;
				((uint8_t *)dp)[x+3] ^= mask;
				x += 4;
			}
#endif
			while (x < rect_x2) {
				((uint8_t *)dp)[x] ^= mask;
				x++;
			}
			break;
		case MNTVA_COLOR_16BIT565:
		case MNTVA_COLOR_15BIT:
#ifdef __ARM_NEON__
			{
				uint16_t *p16 = (uint16_t *)dp + x;
				uint16_t remaining = rect_x2 - x;
				uint16x8_t ones = vdupq_n_u16(0xFFFF);
				while (remaining >= 8) {
					vst1q_u16(p16, veorq_u16(vld1q_u16(p16), ones));
					p16 += 8; remaining -= 8;
				}
				x = rect_x2 - remaining;
			}
#else
			while (x + 4 <= rect_x2) {
				((uint16_t *)dp)[x] ^= 0xFFFF;
				((uint16_t *)dp)[x+1] ^= 0xFFFF;
				((uint16_t *)dp)[x+2] ^= 0xFFFF;
				((uint16_t *)dp)[x+3] ^= 0xFFFF;
				x += 4;
			}
#endif
			while (x < rect_x2) {
				((uint16_t *)dp)[x] ^= 0xFFFF;
				x++;
			}
			break;
		case MNTVA_COLOR_32BIT:
#ifdef __ARM_NEON__
			{
				uint32_t *p32 = dp + x;
				uint16_t remaining = rect_x2 - x;
				uint32x4_t ones = vdupq_n_u32(0xFFFFFFFF);
				while (remaining >= 4) {
					vst1q_u32(p32, veorq_u32(vld1q_u32(p32), ones));
					p32 += 4; remaining -= 4;
				}
				x = rect_x2 - remaining;
			}
#else
			while (x + 4 <= rect_x2) {
				dp[x] ^= 0xFFFFFFFF;
				dp[x+1] ^= 0xFFFFFFFF;
				dp[x+2] ^= 0xFFFFFFFF;
				dp[x+3] ^= 0xFFFFFFFF;
				x += 4;
			}
#endif
			while (x < rect_x2) {
				dp[x] ^= 0xFFFFFFFF;
				x++;
			}
			break;
		}
		dp += fb_pitch;
	}
}

void copy_rect_nomask(uint16_t rect_x1, uint16_t rect_y1, uint16_t w, uint16_t h, uint16_t rect_sx, uint16_t rect_sy, uint32_t color_format, uint32_t* sp_src, uint32_t src_pitch, uint8_t draw_mode)
{
	if (w == 0 || h == 0)
		return;

	h = clamp_rows_to_fb_limit(rect_y1, h);
	if (!h)
		return;

	uint32_t* dp = fb + (rect_y1 * fb_pitch);
	uint32_t* sp = sp_src + (rect_sy * src_pitch);
	uint16_t rect_y2 = rect_y1 + h - 1;
	uint8_t mask = 0xFF; // Perform mask handling, just in case we get a FillRectComplete at some point.
	uint32_t color_mask = 0x00FFFFFF;

	uint8_t u8_fg = 0;
	uint32_t fg_color = 0;

	int32_t line_step_d = fb_pitch, line_step_s = src_pitch;
	int8_t x_reverse = 0;

	if (rect_sy < rect_y1) {
		line_step_d = -fb_pitch;
		dp = fb + (rect_y2 * fb_pitch);
		line_step_s = -src_pitch;
		sp = sp_src + ((rect_sy + h - 1) * src_pitch);
	}

	if (rect_sx < rect_x1) {
		x_reverse = 1;
	}

	if (draw_mode == MINTERM_SRC) {
		for (uint16_t y_line = 0; y_line < h; y_line++) {
			switch(color_format) {
				case MNTVA_COLOR_8BIT:
					if (!x_reverse)
						memcpy((uint8_t *)dp + rect_x1, (uint8_t *)sp + rect_sx, w);
					else
						memmove((uint8_t *)dp + rect_x1, (uint8_t *)sp + rect_sx, w);
					break;
				case MNTVA_COLOR_16BIT565:
				case MNTVA_COLOR_15BIT:
					if (!x_reverse)
						memcpy((uint16_t *)dp + rect_x1, (uint16_t *)sp + rect_sx, w * 2);
					else
						memmove((uint16_t *)dp + rect_x1, (uint16_t *)sp + rect_sx, w * 2);
					break;
				case MNTVA_COLOR_32BIT:
					if (!x_reverse)
						memcpy(dp + rect_x1, sp + rect_sx, w * 4);
					else
						memmove(dp + rect_x1, sp + rect_sx, w * 4);
					break;
			}
			dp += line_step_d;
			sp += line_step_s;
		}
	}
	else {
		for (uint16_t y_line = 0; y_line < h; y_line++) {
			if (x_reverse) {
				for (int32_t x = (int32_t)w - 1; x >= 0; x--) {
					if (color_format == MNTVA_COLOR_8BIT) {
						u8_fg = ((uint8_t *)sp)[rect_sx + x];
						HANDLE_MINTERM_PIXEL_8(u8_fg, ((uint8_t *)dp)[rect_x1 + x]);
					}
					else if (color_format == MNTVA_COLOR_16BIT565 || color_format == MNTVA_COLOR_15BIT) {
						fg_color = ((uint16_t *)sp)[rect_sx + x];
						uint16_t* dpx1 = (uint16_t*)dp + rect_x1;
						HANDLE_MINTERM_PIXEL_16_32(fg_color, dpx1);
					}
					else {
						fg_color = sp[rect_sx + x];
						uint32_t* dpx1 = dp + rect_x1;
						HANDLE_MINTERM_PIXEL_16_32(fg_color, dpx1);
					}
				}
			}
			else {
				for (int32_t x = 0; x < w; x++) {
					if (color_format == MNTVA_COLOR_8BIT) {
						u8_fg = ((uint8_t *)sp)[rect_sx + x];
						HANDLE_MINTERM_PIXEL_8(u8_fg, ((uint8_t *)dp)[rect_x1 + x]);
					}
					else if (color_format == MNTVA_COLOR_16BIT565 || color_format == MNTVA_COLOR_15BIT) {
						fg_color = ((uint16_t *)sp)[rect_sx + x];
						uint16_t* dpx1 = (uint16_t*)dp + rect_x1;
						HANDLE_MINTERM_PIXEL_16_32(fg_color, dpx1);
					}
					else {
						fg_color = sp[rect_sx + x];
						uint32_t* dpx1 = dp + rect_x1;
						HANDLE_MINTERM_PIXEL_16_32(fg_color, dpx1);
					}
				}
			}
			dp += line_step_d;
			sp += line_step_s;
		}
	}
}

void copy_rect(uint16_t rect_x1, uint16_t rect_y1, uint16_t w, uint16_t h, uint16_t rect_sx, uint16_t rect_sy, uint32_t color_format, uint32_t* sp_src, uint32_t src_pitch, uint8_t mask)
{
	if (w == 0 || h == 0)
		return;

	h = clamp_rows_to_fb_limit(rect_y1, h);
	if (!h)
		return;

	uint32_t* dp = fb + (rect_y1 * fb_pitch);
	uint32_t* sp = sp_src + (rect_sy * src_pitch);
	uint16_t rect_y2 = rect_y1 + h - 1;//, rect_x2 = rect_x1 + h - 1;

	int32_t line_step_d = fb_pitch, line_step_s = src_pitch;
	int8_t x_reverse = 0;

	if (rect_sy < rect_y1) {
		line_step_d = -fb_pitch;
		dp = fb + (rect_y2 * fb_pitch);
		line_step_s = -src_pitch;
		sp = sp_src + ((rect_sy + h - 1) * src_pitch);
	}

	if (rect_sx < rect_x1) {
		x_reverse = 1;
	}

	for (uint16_t y_line = 0; y_line < h; y_line++) {
		if (x_reverse) {
			for (int32_t x = (int32_t)w - 1; x >= 0; x--) {
				((uint8_t *)dp)[rect_x1 + x] = (((uint8_t *)dp)[rect_x1 + x] & (mask ^ 0xFF)) | (((uint8_t *)sp)[rect_sx + x] & mask);
			}
		}
		else {
			int32_t x = 0;
#ifdef __ARM_NEON__
			{
				uint8_t *d8 = (uint8_t *)dp + rect_x1;
				uint8_t *s8 = (uint8_t *)sp + rect_sx;
				uint8x16_t vmask = vdupq_n_u8(mask);
				int32_t remaining = w;
				while (remaining >= 16) {
					vst1q_u8(d8, vbslq_u8(vmask, vld1q_u8(s8), vld1q_u8(d8)));
					d8 += 16; s8 += 16; remaining -= 16;
				}
				x = w - remaining;
			}
#endif
			for (; x < w; x++) {
				((uint8_t *)dp)[rect_x1 + x] = (((uint8_t *)dp)[rect_x1 + x] & (mask ^ 0xFF)) | (((uint8_t *)sp)[rect_sx + x] & mask);
			}
		}
		dp += line_step_d;
		sp += line_step_s;
	}
}

#define DRAW_LINE_PIXEL \
	if (draw_mode == JAM1) { \
		if(pattern & cur_bit) { \
			if (!inversion) { \
				if (mask == 0xFF || color_format == MNTVA_COLOR_16BIT565 || color_format == MNTVA_COLOR_15BIT || color_format == MNTVA_COLOR_32BIT) { SET_FG_PIXEL; } \
				else { SET_FG_PIXEL8_MASK(0) } \
			} \
			else { INVERT_PIXEL; } \
		} \
	} \
	else { \
		if(pattern & cur_bit) { \
			if (!inversion) { \
				if (mask == 0xFF || color_format == MNTVA_COLOR_16BIT565 || color_format == MNTVA_COLOR_15BIT || color_format == MNTVA_COLOR_32BIT) { SET_FG_PIXEL; } \
				else { SET_FG_PIXEL8_MASK(0); } \
			} \
			else { INVERT_PIXEL; } /* JAM2 and complement is kind of useless, as it ends up being the same visual result as JAM1 and a pattern of 0xFFFF */ \
		} \
		else { \
			if (!inversion) { \
				if (mask == 0xFF || color_format == MNTVA_COLOR_16BIT565 || color_format == MNTVA_COLOR_15BIT || color_format == MNTVA_COLOR_32BIT) { SET_BG_PIXEL; } \
				else { SET_BG_PIXEL8_MASK(0); } \
			} \
			else { INVERT_PIXEL; } \
		} \
	} \
	if ((cur_bit >>= 1) == 0) \
		cur_bit = 0x8000; \

// Sneakily adapted version of the good old Bresenham algorithm
void draw_line(int16_t rect_x1, int16_t rect_y1, int16_t rect_x2, int16_t rect_y2, uint16_t len,
	int16_t err_seed,
	uint16_t pattern, uint16_t pattern_offset,
	uint32_t fg_color, uint32_t bg_color, uint32_t color_format,
	uint8_t mask, uint8_t draw_mode)
{
	int32_t x1 = rect_x1, y1 = rect_y1;
	int32_t x2 = x1 + (int32_t)rect_x2, y2 = y1 + (int32_t)rect_y2;

	uint8_t u8_fg = fg_color >> 24;
	uint8_t u8_bg = bg_color >> 24;

	uint32_t* dp = fb + (y1 * fb_pitch);
	int32_t line_step = fb_pitch;
	int8_t x_reverse = 0, inversion = 0;

	/* PatternShift selects which pattern bit maps to the segment start. */
	uint16_t cur_bit = (uint16_t)(0x8000u >> (pattern_offset & 15));

	int32_t dx, dy, x = x1;
	uint32_t dx_abs, dy_abs, draw_len = len;

	if (x2 < x1)
		x_reverse = 1;
	if (y2 < y1)
		line_step = -fb_pitch;

	if (draw_mode & INVERSVID)
		pattern ^= 0xFFFF;
	if (draw_mode & COMPLEMENT) {
		inversion = 1;
		fg_color = 0xFFFF0000;
	}
	draw_mode &= 0x01;

	dx = x2 - x1;
	dy = y2 - y1;
	dx_abs = (dx < 0) ? (uint32_t)-dx : (uint32_t)dx;
	dy_abs = (dy < 0) ? (uint32_t)-dy : (uint32_t)dy;

	DRAW_LINE_PIXEL;

	/* P96 round-half-up Bresenham, seeded with the accumulator at the segment
	 * start (err_seed = S*k - L*m + L/2 == (S*k + L/2) mod L, in [0, L); L/2 for
	 * an unclipped line). Each major step adds S to the accumulator; once it
	 * reaches L it wraps (subtract L) and a minor step is taken. The L/2 seed
	 * bias makes the staircase round-half-up (minor offset = floor((2*i*S+L) /
	 * (2*L)), ties up), which matches DrawLineDefault pixel-for-pixel on ZZ9000
	 * hardware (plain truncation, seed 0, was consistently 1px off). Seeding
	 * from err_seed resumes a clipped line exactly where P96 left off. */
	if (dx_abs >= dy_abs) {
		int32_t s = (int32_t)dy_abs;
		int32_t l = (int32_t)dx_abs;
		int32_t e = err_seed;
		for (uint32_t i = 0; i < draw_len; i++) {
			e += s;
			int minor = (e >= l);
			if (minor)
				e -= l;
			x += (x_reverse) ? -1 : 1;
			if (minor)
				dp += line_step;

			DRAW_LINE_PIXEL;
		}
	}
	else {
		int32_t s = (int32_t)dx_abs;
		int32_t l = (int32_t)dy_abs;
		int32_t e = err_seed;
		for (uint32_t i = 0; i < draw_len; i++) {
			e += s;
			int minor = (e >= l);
			if (minor)
				e -= l;
			dp += line_step;
			if (minor)
				x += (x_reverse) ? -1 : 1;

			DRAW_LINE_PIXEL;
		}
	}
}

void draw_line_solid(int16_t rect_x1, int16_t rect_y1, int16_t rect_x2, int16_t rect_y2, uint16_t len,
	int16_t err_seed, uint32_t fg_color, uint32_t color_format)
{
	int32_t x1 = rect_x1, y1 = rect_y1;
	int32_t x2 = x1 + (int32_t)rect_x2, y2 = y1 + (int32_t)rect_y2;

	uint8_t u8_fg = fg_color >> 24;

	uint32_t* dp = fb + (y1 * fb_pitch);
	int32_t line_step = fb_pitch;
	int8_t x_reverse = 0;

	int32_t dx, dy, x = x1;
	uint32_t dx_abs, dy_abs, draw_len = len;

	if (x2 < x1)
		x_reverse = 1;
	if (y2 < y1)
		line_step = -fb_pitch;

	dx = x2 - x1;
	dy = y2 - y1;
	dx_abs = (dx < 0) ? (uint32_t)-dx : (uint32_t)dx;
	dy_abs = (dy < 0) ? (uint32_t)-dy : (uint32_t)dy;

	if (dx_abs == 0) {
		draw_vertical_line_solid(dp, x, line_step, draw_len + 1,
			fg_color, u8_fg, color_format);
		return;
	}
	if (dy_abs == 0) {
		draw_horizontal_line_solid(dp, x_reverse ? x - (int32_t)draw_len : x,
			draw_len + 1, fg_color, u8_fg, color_format);
		return;
	}

	/* Same seeded P96 round-half-up Bresenham as draw_line (see there): each
	 * major step adds S; when the accumulator reaches L it wraps and a minor
	 * step is taken; seeded from err_seed (which carries the L/2 round-half-up
	 * bias) so clipped segments resume exactly. */
	int32_t s, l;
	if (dx_abs >= dy_abs) {
		s = (int32_t)dy_abs;
		l = (int32_t)dx_abs;
	} else {
		s = (int32_t)dx_abs;
		l = (int32_t)dy_abs;
	}

	switch (color_format) {
	case MNTVA_COLOR_8BIT:
		((uint8_t *)dp)[x] = u8_fg;
		if (dx_abs >= dy_abs) {
			int32_t e = err_seed;
			for (uint32_t i = 0; i < draw_len; i++) {
				e += s;
				if (e >= l) { e -= l; dp += line_step; }
				x += (x_reverse) ? -1 : 1;
				((uint8_t *)dp)[x] = u8_fg;
			}
		} else {
			int32_t e = err_seed;
			for (uint32_t i = 0; i < draw_len; i++) {
				e += s;
				if (e >= l) { e -= l; x += (x_reverse) ? -1 : 1; }
				dp += line_step;
				((uint8_t *)dp)[x] = u8_fg;
			}
		}
		break;
	case MNTVA_COLOR_16BIT565:
	case MNTVA_COLOR_15BIT:
		((uint16_t *)dp)[x] = fg_color;
		if (dx_abs >= dy_abs) {
			int32_t e = err_seed;
			for (uint32_t i = 0; i < draw_len; i++) {
				e += s;
				if (e >= l) { e -= l; dp += line_step; }
				x += (x_reverse) ? -1 : 1;
				((uint16_t *)dp)[x] = fg_color;
			}
		} else {
			int32_t e = err_seed;
			for (uint32_t i = 0; i < draw_len; i++) {
				e += s;
				if (e >= l) { e -= l; x += (x_reverse) ? -1 : 1; }
				dp += line_step;
				((uint16_t *)dp)[x] = fg_color;
			}
		}
		break;
	case MNTVA_COLOR_32BIT:
		dp[x] = fg_color;
		if (dx_abs >= dy_abs) {
			int32_t e = err_seed;
			for (uint32_t i = 0; i < draw_len; i++) {
				e += s;
				if (e >= l) { e -= l; dp += line_step; }
				x += (x_reverse) ? -1 : 1;
				dp[x] = fg_color;
			}
		} else {
			int32_t e = err_seed;
			for (uint32_t i = 0; i < draw_len; i++) {
				e += s;
				if (e >= l) { e -= l; x += (x_reverse) ? -1 : 1; }
				dp += line_step;
				dp[x] = fg_color;
			}
		}
		break;
	}
}

#define DECODE_PLANAR_PIXEL(a) \
	switch (planes) { \
		case 8: if (layer_mask & 0x80 && pp[7][cur_byte] & cur_bit) a |= 0x80; \
		case 7: if (layer_mask & 0x40 && pp[6][cur_byte] & cur_bit) a |= 0x40; \
		case 6: if (layer_mask & 0x20 && pp[5][cur_byte] & cur_bit) a |= 0x20; \
		case 5: if (layer_mask & 0x10 && pp[4][cur_byte] & cur_bit) a |= 0x10; \
		case 4: if (layer_mask & 0x08 && pp[3][cur_byte] & cur_bit) a |= 0x08; \
		case 3: if (layer_mask & 0x04 && pp[2][cur_byte] & cur_bit) a |= 0x04; \
		case 2: if (layer_mask & 0x02 && pp[1][cur_byte] & cur_bit) a |= 0x02; \
		case 1: if (layer_mask & 0x01 && pp[0][cur_byte] & cur_bit) a |= 0x01; \
			break; \
	}

#define DECODE_INVERTED_PLANAR_PIXEL(a) \
	switch (planes) { \
		case 8: if (layer_mask & 0x80 && (pp[7][cur_byte] ^ 0xFF) & cur_bit) a |= 0x80; \
		case 7: if (layer_mask & 0x40 && (pp[6][cur_byte] ^ 0xFF) & cur_bit) a |= 0x40; \
		case 6: if (layer_mask & 0x20 && (pp[5][cur_byte] ^ 0xFF) & cur_bit) a |= 0x20; \
		case 5: if (layer_mask & 0x10 && (pp[4][cur_byte] ^ 0xFF) & cur_bit) a |= 0x10; \
		case 4: if (layer_mask & 0x08 && (pp[3][cur_byte] ^ 0xFF) & cur_bit) a |= 0x08; \
		case 3: if (layer_mask & 0x04 && (pp[2][cur_byte] ^ 0xFF) & cur_bit) a |= 0x04; \
		case 2: if (layer_mask & 0x02 && (pp[1][cur_byte] ^ 0xFF) & cur_bit) a |= 0x02; \
		case 1: if (layer_mask & 0x01 && (pp[0][cur_byte] ^ 0xFF) & cur_bit) a |= 0x01; \
			break; \
	}

static uint64_t p2c_byte_lut[256];
static uint8_t p2c_byte_lut_initialized;

static void p2c_byte_lut_init(void)
{
	if (p2c_byte_lut_initialized)
		return;

	for (uint16_t byte = 0; byte < 256; byte++) {
		uint64_t out = 0;
		for (uint8_t pixel = 0; pixel < 8; pixel++) {
			if (byte & (0x80 >> pixel))
				out |= (uint64_t)1 << (pixel * 8);
		}
		p2c_byte_lut[byte] = out;
	}

	p2c_byte_lut_initialized = 1;
}

static inline uint64_t p2c_decode_8pixels(uint8_t **pp, uint8_t planes,
	uint8_t layer_mask, uint16_t cur_byte, uint8_t invert)
{
	uint64_t out = 0;

	if (planes == 8 && layer_mask == 0xff) {
		if (invert) {
			return p2c_byte_lut[pp[0][cur_byte] ^ 0xff] |
				(p2c_byte_lut[pp[1][cur_byte] ^ 0xff] << 1) |
				(p2c_byte_lut[pp[2][cur_byte] ^ 0xff] << 2) |
				(p2c_byte_lut[pp[3][cur_byte] ^ 0xff] << 3) |
				(p2c_byte_lut[pp[4][cur_byte] ^ 0xff] << 4) |
				(p2c_byte_lut[pp[5][cur_byte] ^ 0xff] << 5) |
				(p2c_byte_lut[pp[6][cur_byte] ^ 0xff] << 6) |
				(p2c_byte_lut[pp[7][cur_byte] ^ 0xff] << 7);
		}

		return p2c_byte_lut[pp[0][cur_byte]] |
			(p2c_byte_lut[pp[1][cur_byte]] << 1) |
			(p2c_byte_lut[pp[2][cur_byte]] << 2) |
			(p2c_byte_lut[pp[3][cur_byte]] << 3) |
			(p2c_byte_lut[pp[4][cur_byte]] << 4) |
			(p2c_byte_lut[pp[5][cur_byte]] << 5) |
			(p2c_byte_lut[pp[6][cur_byte]] << 6) |
			(p2c_byte_lut[pp[7][cur_byte]] << 7);
	}

	for (uint8_t plane = 0; plane < planes && plane < 8; plane++) {
		uint8_t plane_bit = (uint8_t)(1u << plane);
		if (layer_mask & plane_bit) {
			uint8_t byte = pp[plane][cur_byte];
			out |= p2c_byte_lut[invert ? (byte ^ 0xff) : byte] << plane;
		}
	}

	return out;
}

static inline void p2d_store_8pixels(uint32_t *dp, int16_t x, uint64_t pixels,
	uint32_t *bmp_pal, uint32_t color_format)
{
	switch (color_format) {
	case MNTVA_COLOR_16BIT565:
	case MNTVA_COLOR_15BIT: {
		uint16_t *d16 = (uint16_t *)dp + x;
		for (uint8_t i = 0; i < 8; i++)
			d16[i] = bmp_pal[(pixels >> (i * 8)) & 0xff];
		break;
	}
	case MNTVA_COLOR_32BIT:
		for (uint8_t i = 0; i < 8; i++)
			dp[x + i] = bmp_pal[(pixels >> (i * 8)) & 0xff];
		break;
	}
}

void p2c_rect(int16_t sx, int16_t sy, int16_t dx, int16_t dy, int16_t w, int16_t h, uint8_t draw_mode, uint8_t planes, uint8_t mask, uint8_t layer_mask, uint16_t src_line_pitch, uint8_t *bmp_data_src)
{
	/* h also sets the source plane stride and wrap below, so only the row
	 * loop bound is clamped (h_draw), never h itself. */
	if (dy < 0 || h <= 0)
		return;
	int16_t h_draw = (int16_t)clamp_rows_to_fb_limit((uint16_t)dy, (uint16_t)h);
	if (!h_draw)
		return;

	uint32_t *dp = fb + (dy * fb_pitch);

	uint8_t cur_bit, base_bit, base_byte;
	uint16_t cur_byte = 0, u8_fg = 0;
	uint8_t direct_copy = (mask == 0xFF && (draw_mode == MINTERM_SRC || draw_mode == MINTERM_NOTSRC));
	uint8_t invert_planar = draw_mode & 0x01;

	uint32_t plane_size = src_line_pitch * h;
	uint8_t *bmp_data = bmp_data_src;

	uint8_t *pp[8];
	for (int i = 0; i < planes && i < 8; i++)
		pp[i] = bmp_data + (plane_size * i);

	cur_bit = base_bit = (0x80 >> (sx % 8));
	cur_byte = base_byte = ((sx / 8) % src_line_pitch);

	if (direct_copy)
		p2c_byte_lut_init();

	for (int16_t line_y = 0; line_y < h_draw; line_y++) {
		int16_t x = dx;
		int16_t rect_x2 = dx + w;

		if (direct_copy) {
			uint8_t *dst = (uint8_t *)dp;
			while (x < rect_x2) {
				if (cur_bit == 0x80 && x + 8 <= rect_x2) {
					uint64_t pixels = p2c_decode_8pixels(pp, planes, layer_mask,
						cur_byte, invert_planar);
					memcpy(dst + x, &pixels, sizeof(pixels));
					x += 8;
					cur_byte++;
					if (cur_byte >= src_line_pitch) cur_byte = 0;
					continue;
				}

				u8_fg = 0;
				if (invert_planar)
					DECODE_INVERTED_PLANAR_PIXEL(u8_fg)
				else
					DECODE_PLANAR_PIXEL(u8_fg)

				dst[x] = u8_fg;
				x++;
				if ((cur_bit >>= 1) == 0) {
					cur_bit = 0x80;
					cur_byte++;
					if (cur_byte >= src_line_pitch) cur_byte = 0;
				}
			}
		}
		else {
			for (; x < rect_x2; x++) {
				u8_fg = 0;
				/* Odd minterms use the inverted planar source in the
				 * legacy handlers, except NEOR: its handler performs
				 * the source inversion itself. */
				if ((draw_mode & 0x01) && draw_mode != MINTERM_NEOR)
					DECODE_INVERTED_PLANAR_PIXEL(u8_fg)
				else
					DECODE_PLANAR_PIXEL(u8_fg)

				HANDLE_MINTERM_PIXEL_8(u8_fg, ((uint8_t *)dp)[x]);

				if ((cur_bit >>= 1) == 0) {
					cur_bit = 0x80;
					cur_byte++;
					if (cur_byte >= src_line_pitch) cur_byte = 0;
				}
			}
		}
		dp += fb_pitch;
		if ((line_y + sy + 1) % h) {
			bmp_data += src_line_pitch;
			for (int i = 0; i < planes && i < 8; i++)
				pp[i] += src_line_pitch;
		}
		else {
			bmp_data = bmp_data_src;
			for (int i = 0; i < planes && i < 8; i++)
				pp[i] = bmp_data + (plane_size * i);
		}
		cur_bit = base_bit;
		cur_byte = base_byte;
	}
}

/* Packed 4:2:2 macropixel byte positions of Y0/Y1/U/V, indexed by
 * enum yuv422_variant. The Picasso96.h comments describe these orders
 * with U and V interchanged - a documentation defect fixed in P96
 * V3.6.3 ("the documentation on the 422 modes was incorrect and U and
 * V were interchanged", wiki.icomp.de/wiki/P96). The corrected orders
 * used here are also the industry-standard ones (CGX = YUY2,
 * 422PC = UYVY). If PIP colors ever come out red/blue-swapped against
 * WriteYUVRectDefault on hardware, swapping the u/v columns is the
 * knob. */
static const struct {
	uint8_t y0, y1, u, v;
} yuv422_layout[YUV422_VARIANT_NUM] = {
	{ 0, 2, 1, 3 }, /* CGX:  Y0 U  Y1 V  (YUY2) */
	{ 2, 0, 3, 1 }, /* STD:  Y1 V  Y0 U  */
	{ 1, 3, 0, 2 }, /* PC:   U  Y0 V  Y1 (UYVY) */
	{ 0, 1, 2, 3 }, /* PA:   Y0 Y1 U  V  */
	{ 3, 2, 1, 0 }, /* PAPC: V  U  Y1 Y0 */
};

static inline uint8_t yuv_clamp8(int32_t v)
{
	if (v < 0) return 0;
	if (v > 255) return 255;
	return (uint8_t)v;
}

/* Store one RGB pixel in the Amiga surface byte layout: 32-bit BGRA is
 * byte0=B (ARM value A<<24|R<<16|G<<8|B); 16/15-bit pixels sit
 * big-endian in memory (byte0=rrrrrggg), so the ARM uint16 store is
 * byte-swapped relative to the natural R<<11|G<<5|B form. */
static inline void yuv_store_pixel(uint32_t *dp, int32_t x, uint8_t color_format,
	int32_t cy, int32_t rc, int32_t gc, int32_t bc)
{
	uint8_t r = yuv_clamp8((cy + rc) >> 8);
	uint8_t g = yuv_clamp8((cy + gc) >> 8);
	uint8_t b = yuv_clamp8((cy + bc) >> 8);
	uint16_t v16;

	switch (color_format) {
	case MNTVA_COLOR_32BIT:
		dp[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
		break;
	case MNTVA_COLOR_16BIT565:
		v16 = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
		((uint16_t *)dp)[x] = (uint16_t)((v16 >> 8) | (v16 << 8));
		break;
	case MNTVA_COLOR_15BIT:
		v16 = (uint16_t)(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
		((uint16_t *)dp)[x] = (uint16_t)((v16 >> 8) | (v16 << 8));
		break;
	default:
		break;
	}
}

/* Convert a packed 4:2:2 YUV rect (CCIR601 studio range) to RGB at
 * (dx,dy). `src` points at macropixel-aligned rows; `phase`=1 means the
 * first output pixel is the second (Y1) pixel of the first macropixel
 * (i.e. the original source x was odd). Chroma is held per macropixel
 * (no interpolation). */
void yuv422_to_rgb_rect(int16_t phase, int16_t dx, int16_t dy, int16_t w, int16_t h,
	uint8_t variant, uint8_t color_format, uint16_t src_pitch, uint8_t *src)
{
	if (dx < 0 || dy < 0 || w <= 0 || h <= 0 || variant >= YUV422_VARIANT_NUM)
		return;
	if (color_format != MNTVA_COLOR_32BIT &&
	    color_format != MNTVA_COLOR_16BIT565 &&
	    color_format != MNTVA_COLOR_15BIT)
		return;
	h = (int16_t)clamp_rows_to_fb_limit((uint16_t)dy, (uint16_t)h);
	if (!h)
		return;

	uint8_t y0_off = yuv422_layout[variant].y0;
	uint8_t y1_off = yuv422_layout[variant].y1;
	uint8_t u_off = yuv422_layout[variant].u;
	uint8_t v_off = yuv422_layout[variant].v;

	for (int16_t row = 0; row < h; row++) {
		uint8_t *sp = src + (uint32_t)row * src_pitch;
		uint32_t *dp = fb + ((uint32_t)(dy + row) * fb_pitch);
		int32_t x = dx;
		int32_t remaining = w;
		int16_t skip_first = phase & 1;

		while (remaining > 0) {
			int32_t d = (int32_t)sp[u_off] - 128;
			int32_t e = (int32_t)sp[v_off] - 128;
			int32_t rc = 409 * e + 128;
			int32_t gc = -100 * d - 208 * e + 128;
			int32_t bc = 516 * d + 128;

			if (!skip_first) {
				yuv_store_pixel(dp, x, color_format,
					298 * ((int32_t)sp[y0_off] - 16), rc, gc, bc);
				x++;
				remaining--;
			}
			skip_first = 0;

			if (remaining > 0) {
				yuv_store_pixel(dp, x, color_format,
					298 * ((int32_t)sp[y1_off] - 16), rc, gc, bc);
				x++;
				remaining--;
			}
			sp += 4;
		}
	}
}

/* Composite one full frame for the P96 video window (PIP): copy the
 * live screen into the shadow buffer and draw the scaled YUV overlay
 * over the destination rect, color-keyed against the screen content
 * (the pixel just copied into the shadow IS the screen value, so the
 * key test reads the shadow row in place). Scaling is 8.8 fixed-point
 * nearest sampling, WinUAE overlay semantics. Pure function with
 * explicit pointers: runs on core 1 and in the host test harness.
 * If the overlay parameters are unusable the screen is still copied,
 * so the shadow always presents a valid frame. */
void overlay_composite_frame(uint8_t *dst, uint32_t dst_pitch,
	const uint8_t *screen, uint32_t screen_pitch,
	uint16_t scr_w, uint16_t scr_h, uint8_t color_format,
	const uint8_t *src, uint16_t src_pitch,
	uint16_t src_w, uint16_t src_h, uint8_t variant,
	int16_t dst_x, int16_t dst_y, int16_t dst_w, int16_t dst_h,
	uint32_t key_native, uint8_t key_enabled)
{
	uint32_t bpp;

	if (!dst || !screen || !scr_w || !scr_h)
		return;

	switch (color_format) {
	case MNTVA_COLOR_32BIT:
		bpp = 4;
		break;
	case MNTVA_COLOR_16BIT565:
	case MNTVA_COLOR_15BIT:
		bpp = 2;
		break;
	default:
		return;
	}

	uint32_t line_bytes = (uint32_t)scr_w * bpp;

	/* clip the dest rect to the screen */
	int32_t x0 = dst_x < 0 ? 0 : dst_x;
	int32_t y0 = dst_y < 0 ? 0 : dst_y;
	int32_t x1 = (int32_t)dst_x + dst_w;
	int32_t y1 = (int32_t)dst_y + dst_h;
	if (x1 > scr_w) x1 = scr_w;
	if (y1 > scr_h) y1 = scr_h;

	int overlay_ok = src && src_w && src_h && dst_w > 0 && dst_h > 0 &&
		variant < YUV422_VARIANT_NUM && x0 < x1 && y0 < y1;

	if (!overlay_ok) {
		for (int32_t row = 0; row < scr_h; row++)
			memcpy(dst + (uint32_t)row * dst_pitch,
				screen + (uint32_t)row * screen_pitch, line_bytes);
		return;
	}

	uint8_t byte_y0 = yuv422_layout[variant].y0;
	uint8_t byte_y1 = yuv422_layout[variant].y1;
	uint8_t byte_u = yuv422_layout[variant].u;
	uint8_t byte_v = yuv422_layout[variant].v;

	/* 8.8 fixed-point source steps over the UNCLIPPED dest rect */
	uint32_t mx = ((uint32_t)src_w << 8) / (uint32_t)dst_w;
	uint32_t my = ((uint32_t)src_h << 8) / (uint32_t)dst_h;
	uint32_t sy_fix = (uint32_t)(y0 - dst_y) * my;
	uint32_t sx0_fix = (uint32_t)(x0 - dst_x) * mx;
	uint16_t key16 = (uint16_t)key_native;
	uint32_t key24 = key_native & 0x00FFFFFFu;

	for (int32_t row = 0; row < scr_h; row++) {
		uint8_t *drow = dst + (uint32_t)row * dst_pitch;

		memcpy(drow, screen + (uint32_t)row * screen_pitch, line_bytes);

		if (row < y0 || row >= y1)
			continue;

		uint32_t sy = sy_fix >> 8;
		if (sy >= src_h)
			sy = src_h - 1;
		sy_fix += my;

		const uint8_t *srow = src + sy * src_pitch;
		uint32_t *dp = (uint32_t *)drow;
		uint32_t sx_fix = sx0_fix;
		int32_t cached_mp = -1;
		int32_t cy0 = 0, cy1 = 0, rc = 0, gc = 0, bc = 0;

		for (int32_t x = x0; x < x1; x++) {
			uint32_t sx = sx_fix >> 8;
			if (sx >= src_w)
				sx = src_w - 1;
			sx_fix += mx;

			if (key_enabled) {
				if (bpp == 4) {
					if ((dp[x] & 0x00FFFFFFu) != key24)
						continue;
				} else {
					if (((uint16_t *)dp)[x] != key16)
						continue;
				}
			}

			int32_t mp = (int32_t)(sx >> 1);
			if (mp != cached_mp) {
				const uint8_t *s = srow + (uint32_t)mp * 4;
				int32_t d = (int32_t)s[byte_u] - 128;
				int32_t e = (int32_t)s[byte_v] - 128;
				cy0 = 298 * ((int32_t)s[byte_y0] - 16);
				cy1 = 298 * ((int32_t)s[byte_y1] - 16);
				rc = 409 * e + 128;
				gc = -100 * d - 208 * e + 128;
				bc = 516 * d + 128;
				cached_mp = mp;
			}

			yuv_store_pixel(dp, x, color_format,
				(sx & 1) ? cy1 : cy0, rc, gc, bc);
		}
	}
}

/* Scaled/clipped decoder-frame variant of overlay_composite_frame(). It
 * consumes planar 4:2:0 planes in place and writes the software fallback's
 * RGB shadow. Fully visible 1:1 windows use the native PL YUV plane instead. */
void overlay_composite_planar420_frame(uint8_t *dst, uint32_t dst_pitch,
	const uint8_t *screen, uint32_t screen_pitch,
	uint16_t scr_w, uint16_t scr_h, uint8_t color_format,
	const uint8_t *y, uint32_t y_pitch, const uint8_t *cb, const uint8_t *cr,
	uint32_t chroma_pitch, uint16_t src_w, uint16_t src_h,
	int16_t dst_x, int16_t dst_y, int16_t dst_w, int16_t dst_h,
	uint32_t key_native, uint8_t key_enabled)
{
	uint32_t bpp;
	uint32_t line_bytes;
	int32_t x0, y0, x1, y1;
	uint32_t mx, my, sy_fix, sx0_fix;
	uint16_t key16 = (uint16_t)key_native;
	uint32_t key24 = key_native & 0x00ffffffU;

	if (!dst || !screen || !scr_w || !scr_h)
		return;
	if (color_format == MNTVA_COLOR_32BIT)
		bpp = 4U;
	else if (color_format == MNTVA_COLOR_16BIT565 ||
	         color_format == MNTVA_COLOR_15BIT)
		bpp = 2U;
	else
		return;
	line_bytes = (uint32_t)scr_w * bpp;
	x0 = dst_x < 0 ? 0 : dst_x;
	y0 = dst_y < 0 ? 0 : dst_y;
	x1 = (int32_t)dst_x + dst_w;
	y1 = (int32_t)dst_y + dst_h;
	if (x1 > scr_w) x1 = scr_w;
	if (y1 > scr_h) y1 = scr_h;

	if (!y || !cb || !cr || !src_w || !src_h ||
	    y_pitch < src_w || chroma_pitch < ((src_w + 1U) >> 1) ||
	    dst_w <= 0 || dst_h <= 0 || x0 >= x1 || y0 >= y1) {
		for (int32_t row = 0; row < scr_h; row++)
			memcpy(dst + (uint32_t)row * dst_pitch,
			       screen + (uint32_t)row * screen_pitch, line_bytes);
		return;
	}

	mx = ((uint32_t)src_w << 8) / (uint32_t)dst_w;
	my = ((uint32_t)src_h << 8) / (uint32_t)dst_h;
	sy_fix = (uint32_t)(y0 - dst_y) * my;
	sx0_fix = (uint32_t)(x0 - dst_x) * mx;

	for (int32_t row = 0; row < scr_h; row++) {
		uint8_t *drow = dst + (uint32_t)row * dst_pitch;
		uint32_t *dp = (uint32_t *)drow;
		uint32_t sx_fix;
		uint32_t sy;
		int32_t cached_mp = -1;
		int32_t rc = 0, gc = 0, bc = 0;

		memcpy(drow, screen + (uint32_t)row * screen_pitch, line_bytes);
		if (row < y0 || row >= y1)
			continue;
		sy = sy_fix >> 8;
		if (sy >= src_h) sy = src_h - 1U;
		sy_fix += my;
		sx_fix = sx0_fix;

		for (int32_t x = x0; x < x1; x++) {
			uint32_t sx = sx_fix >> 8;
			int32_t cy;
			int32_t mp;

			if (sx >= src_w) sx = src_w - 1U;
			sx_fix += mx;
			if (key_enabled) {
				if (bpp == 4U) {
					if ((dp[x] & 0x00ffffffU) != key24) continue;
				} else if (((uint16_t *)dp)[x] != key16) {
					continue;
				}
			}
			mp = (int32_t)(sx >> 1);
			if (mp != cached_mp) {
				int32_t d = (int32_t)cb[(sy >> 1) * chroma_pitch +
				                                (uint32_t)mp] - 128;
				int32_t e = (int32_t)cr[(sy >> 1) * chroma_pitch +
				                                (uint32_t)mp] - 128;
				rc = 409 * e + 128;
				gc = -100 * d - 208 * e + 128;
				bc = 516 * d + 128;
				cached_mp = mp;
			}
			cy = 298 * ((int32_t)y[sy * y_pitch + sx] - 16);
			yuv_store_pixel(dp, x, color_format, cy, rc, gc, bc);
		}
	}
}

#define RL_CACHE_SIZE 256
#define RL_CACHE_MASK (RL_CACHE_SIZE - 1)
static uint32_t rl_cache_keys[RL_CACHE_SIZE];
static uint8_t rl_cache_vals[RL_CACHE_SIZE];
static uint8_t rl_cache_valid[RL_CACHE_SIZE];

static inline void rl_cache_invalidate(void) {
	memset(rl_cache_valid, 0, sizeof(rl_cache_valid));
}

static inline uint8_t reverse_lookup(uint32_t *bmp_pal, uint8_t planes, uint32_t fg_color) {
	uint8_t slot = (fg_color ^ (fg_color >> 8)) & RL_CACHE_MASK;
	if (rl_cache_valid[slot] && rl_cache_keys[slot] == fg_color)
		return rl_cache_vals[slot];

	uint16_t num_colors = planes >= 8 ? 256 : (uint16_t)(1u << planes);
	for (uint16_t i = 0; i < num_colors; i++) {
		if (bmp_pal[i] == fg_color) {
			rl_cache_keys[slot] = fg_color;
			rl_cache_vals[slot] = (uint8_t)i;
			rl_cache_valid[slot] = 1;
			return (uint8_t)i;
		}
	}
	return 0;
}

void p2d_rect(int16_t sx, int16_t sy, int16_t dx, int16_t dy, int16_t w, int16_t h, uint8_t draw_mode, uint8_t planes, uint8_t mask, uint8_t layer_mask, uint32_t color_mask, uint16_t src_line_pitch, uint8_t *bmp_data_src, uint32_t color_format) {
	/* h also sets the source plane stride and wrap below, so only the row
	 * loop bound is clamped (h_draw), never h itself. */
	if (dy < 0 || h <= 0)
		return;
	int16_t h_draw = (int16_t)clamp_rows_to_fb_limit((uint16_t)dy, (uint16_t)h);
	if (!h_draw)
		return;

	uint32_t *dp = fb + (dy * fb_pitch);

	/* Picasso96 defines direct-color INVERT on the native destination
	 * value, and DST as leaving it untouched. Neither operation needs
	 * planar decoding or an inverse color-map lookup. */
	if (draw_mode == MINTERM_DST)
		return;
	if (draw_mode == MINTERM_INVERT) {
		for (int16_t line_y = 0; line_y < h_draw; line_y++) {
			switch (color_format) {
				case MNTVA_COLOR_16BIT565:
				case MNTVA_COLOR_15BIT:
					for (int16_t x = dx; x < dx + w; x++)
						((uint16_t *)dp)[x] ^= (uint16_t)color_mask;
					break;
				case MNTVA_COLOR_32BIT:
					for (int16_t x = dx; x < dx + w; x++)
						dp[x] ^= color_mask;
					break;
			}
			dp += fb_pitch;
		}
		return;
	}

	uint8_t cur_bit, base_bit, base_byte;
	uint16_t cur_byte = 0;
	uint8_t direct_copy = (draw_mode == MINTERM_SRC || draw_mode == MINTERM_NOTSRC);
	uint8_t invert_planar = draw_mode & 0x01;

	uint32_t plane_size = src_line_pitch * h;
	uint32_t *bmp_pal = (uint32_t *)bmp_data_src;
	uint8_t *bmp_data = bmp_data_src + (256 * 4);

	uint8_t *pp[8];
	for (int i = 0; i < planes && i < 8; i++)
		pp[i] = bmp_data + (plane_size * i);

	cur_bit = base_bit = (0x80 >> (sx % 8));
	cur_byte = base_byte = ((sx / 8) % src_line_pitch);

	if (direct_copy)
		p2c_byte_lut_init();
	else
		rl_cache_invalidate();

	for (int16_t line_y = 0; line_y < h_draw; line_y++) {
		int16_t x = dx;
		int16_t rect_x2 = dx + w;

		if (direct_copy) {
			while (x < rect_x2) {
				if (cur_bit == 0x80 && x + 8 <= rect_x2) {
					uint64_t pixels = p2c_decode_8pixels(pp, planes, layer_mask,
						cur_byte, invert_planar);
					p2d_store_8pixels(dp, x, pixels, bmp_pal, color_format);
					x += 8;
					cur_byte++;
					if (cur_byte >= src_line_pitch) cur_byte = 0;
					continue;
				}

				uint8_t d = 0;
				if (invert_planar)
					DECODE_INVERTED_PLANAR_PIXEL(d)
				else
					DECODE_PLANAR_PIXEL(d)

				switch (color_format) {
					case MNTVA_COLOR_16BIT565:
					case MNTVA_COLOR_15BIT:
						((uint16_t *)dp)[x] = bmp_pal[d];
						break;
					case MNTVA_COLOR_32BIT:
						dp[x] = bmp_pal[d];
						break;
				}

				x++;
				if ((cur_bit >>= 1) == 0) {
					cur_bit = 0x80;
					cur_byte++;
					if (cur_byte >= src_line_pitch) cur_byte = 0;
				}
			}
		}
		else {
			for (; x < rect_x2; x++) {
				uint8_t b=0,nb=0,c,d=0;
				switch(draw_mode) {
					case MINTERM_FALSE:
						d = 0;
					break;
					case MINTERM_NOR:
						DECODE_PLANAR_PIXEL(b);
						c = reverse_lookup(bmp_pal, planes, dp[x]);
						d = ~(c | b);
					break;
					case MINTERM_ONLYDST:
						DECODE_INVERTED_PLANAR_PIXEL(nb);
						c = reverse_lookup(bmp_pal, planes, dp[x]);
						d = c & nb;
					break;
					case MINTERM_ONLYSRC:
						DECODE_PLANAR_PIXEL(b);
						c = reverse_lookup(bmp_pal, planes, dp[x]);
						d = (~c) & b;
					break;
					case MINTERM_INVERT:
						c = reverse_lookup(bmp_pal, planes, dp[x]);
						d = ~c;
					break;
					case MINTERM_EOR:
						DECODE_PLANAR_PIXEL(b);
						c = reverse_lookup(bmp_pal, planes, dp[x]);
						d = c ^ b;
					break;
					case MINTERM_NAND:
						DECODE_PLANAR_PIXEL(b);
						c = reverse_lookup(bmp_pal, planes, dp[x]);
						d = ~(c & b);
					break;
					case MINTERM_AND:
						DECODE_PLANAR_PIXEL(b);
						c = reverse_lookup(bmp_pal, planes, dp[x]);
						d = c & b;
					break;
					case MINTERM_NEOR:
						DECODE_PLANAR_PIXEL(b);
						c = reverse_lookup(bmp_pal, planes, dp[x]);
						d = ~(c ^ b);
					break;
					case MINTERM_DST:
						c = reverse_lookup(bmp_pal, planes, dp[x]);
						d = c;
					break;
					case MINTERM_NOTONLYSRC:
						DECODE_INVERTED_PLANAR_PIXEL(nb);
						c = reverse_lookup(bmp_pal, planes, dp[x]);
						d = c | nb;
					break;
					case MINTERM_NOTONLYDST:
						DECODE_PLANAR_PIXEL(b);
						c = reverse_lookup(bmp_pal, planes, dp[x]);
						d = (~c) | b;
					break;
					case MINTERM_OR:
						DECODE_PLANAR_PIXEL(b);
						c = reverse_lookup(bmp_pal, planes, dp[x]);
						d = c | b;
					break;
					case MINTERM_TRUE:
						d = (1<<planes) - 1;
					break;
				}

				switch (color_format) {
					case MNTVA_COLOR_16BIT565:
					case MNTVA_COLOR_15BIT:
						((uint16_t *)dp)[x] = bmp_pal[d];
						break;
					case MNTVA_COLOR_32BIT:
						dp[x] = bmp_pal[d];
						break;
				}

				if ((cur_bit >>= 1) == 0) {
					cur_bit = 0x80;
					cur_byte++;
					if (cur_byte >= src_line_pitch) cur_byte = 0;
				}
			}
		}
		dp += fb_pitch;
		if ((line_y + sy + 1) % h) {
			bmp_data += src_line_pitch;
			for (int i = 0; i < planes && i < 8; i++)
				pp[i] += src_line_pitch;
		}
		else {
			bmp_data = bmp_data_src + (256 * 4);
			for (int i = 0; i < planes && i < 8; i++)
				pp[i] = bmp_data + (plane_size * i);
		}
		cur_bit = base_bit;
		cur_byte = base_byte;
	}
}

void orig_p2d_rect(int16_t sx, int16_t sy, int16_t dx, int16_t dy, int16_t w, int16_t h, uint8_t draw_mode, uint8_t planes, uint8_t mask, uint8_t layer_mask, uint32_t color_mask, uint16_t src_line_pitch, uint8_t *bmp_data_src, uint32_t color_format) {
	uint32_t *dp = fb + (dy * fb_pitch);

	uint8_t cur_bit, base_bit, base_byte;
	uint16_t cur_byte = 0, cur_pixel = 0;
	uint32_t fg_color = 0;

	uint32_t plane_size = src_line_pitch * h;
	uint32_t *bmp_pal = (uint32_t *)bmp_data_src;
	uint8_t *bmp_data = bmp_data_src + (256 * 4);

	uint8_t *pp[8];
	for (int i = 0; i < planes && i < 8; i++)
		pp[i] = bmp_data + (plane_size * i);

	cur_bit = base_bit = (0x80 >> (sx % 8));
	cur_byte = base_byte = ((sx / 8) % src_line_pitch);

	for (int16_t line_y = 0; line_y < h; line_y++) {
		for (int16_t x = dx; x < dx + w; x++) {
			cur_pixel = 0;
			if (draw_mode & 0x01)
				DECODE_INVERTED_PLANAR_PIXEL(cur_pixel)
			else
				DECODE_PLANAR_PIXEL(cur_pixel)
			fg_color = bmp_pal[cur_pixel];

			if (mask == 0xFF && (draw_mode == 0x0C || draw_mode == 0x03)) {
				switch (color_format) {
					case MNTVA_COLOR_16BIT565:
					case MNTVA_COLOR_15BIT:
						((uint16_t *)dp)[x] = fg_color;
						break;
					case MNTVA_COLOR_32BIT:
						dp[x] = fg_color;
						break;
				}
				goto skip;
			}

			HANDLE_MINTERM_PIXEL_16_32(fg_color, dp);

			skip:;
			if ((cur_bit >>= 1) == 0) {
				cur_bit = 0x80;
				cur_byte++;
				if (cur_byte >= src_line_pitch) cur_byte = 0;
			}

		}
		dp += fb_pitch;
		if ((line_y + sy + 1) % h) {
			bmp_data += src_line_pitch;
			for (int i = 0; i < planes && i < 8; i++)
				pp[i] += src_line_pitch;
		}
		else {
			bmp_data = bmp_data_src + (256 * 4);
			for (int i = 0; i < planes && i < 8; i++)
				pp[i] = bmp_data + (plane_size * i);
		}
		cur_bit = base_bit;
		cur_byte = base_byte;
	}
}

#define PATTERN_FILLRECT_LOOPX \
	tmpl_x ^= 0x01; \
	cur_byte = (inversion) ? tmpl_data[tmpl_x] ^ 0xFF : tmpl_data[tmpl_x];

#define PATTERN_FILLRECT_LOOPY \
	tmpl_data += 2 ; \
	if ((y_line + y_offset + 1) % loop_rows == 0) \
		tmpl_data = tmpl_base; \
	tmpl_x = tmpl_x_base; \
	cur_bit = base_bit; \
	dp += fb_pitch / 4;

void pattern_fill_rect(uint32_t color_format, uint16_t rect_x1, uint16_t rect_y1, uint16_t w, uint16_t h,
	uint8_t draw_mode, uint8_t mask, uint32_t fg_color, uint32_t bg_color,
	uint16_t x_offset, uint16_t y_offset,
	uint8_t *tmpl_data, uint16_t tmpl_pitch, uint16_t loop_rows)
{
	h = clamp_rows_to_fb_limit(rect_y1, h);
	if (!h)
		return;

	uint32_t rect_x2 = rect_x1 + w;
	uint32_t *dp = fb + (rect_y1 * (fb_pitch / 4));
	uint8_t* tmpl_base = tmpl_data;

	uint16_t tmpl_x, tmpl_x_base;

	uint8_t cur_bit, base_bit, inversion = 0;
	uint8_t u8_fg = fg_color >> 24;
	uint8_t u8_bg = bg_color >> 24;
	uint8_t cur_byte = 0;

	uint8_t cur_line = 0;
	uint16_t cheat_y = 0;

	tmpl_x = (x_offset / 8) % 2;
	tmpl_data += (y_offset % loop_rows) * 2;
	tmpl_x_base = tmpl_x;

	cur_bit = base_bit = (0x80 >> (x_offset % 8));

	if (draw_mode & INVERSVID) inversion = 1;
	draw_mode &= 0x03;

	if (draw_mode == JAM1) {
		for (uint16_t y_line = 0; y_line < h; y_line++) {
			uint16_t x = rect_x1;

			cur_byte = (inversion) ? tmpl_data[tmpl_x] ^ 0xFF : tmpl_data[tmpl_x];

			while (x < rect_x2) {
				if (w >= 8 && cur_bit == 0x80 && x < rect_x2 - 8) {
					if (mask == 0xFF) {
						SET_FG_PIXELS;
					}
					else {
						SET_FG_PIXELS_MASK;
					}
					x += 8;
				}
				else {
					while (cur_bit > 0 && x < rect_x2) {
						if (cur_byte & cur_bit) {
							SET_FG_PIXEL_MASK;
						}
						x++;
						cur_bit >>= 1;
					}
					cur_bit = 0x80;
				}
				PATTERN_FILLRECT_LOOPX;
			}
			PATTERN_FILLRECT_LOOPY;
		}

		return;
	}
	else if (draw_mode == JAM2) {
		for (uint16_t y_line = 0; y_line < h; y_line++) {
			uint16_t x = rect_x1;

			cur_byte = (inversion) ? tmpl_data[tmpl_x] ^ 0xFF : tmpl_data[tmpl_x];

			while (x < rect_x2) {
				if (w >= 8 && cur_bit == 0x80 && x < rect_x2 - 8) {
					if (mask == 0xFF) {
						SET_FG_OR_BG_PIXELS;
					}
					else {
						SET_FG_OR_BG_PIXELS_MASK;
					}
					x += 8;
				}
				else {
					while (cur_bit > 0 && x < rect_x2) {
						if (cur_byte & cur_bit) {
							SET_FG_PIXEL_MASK;
						}
						else {
							SET_BG_PIXEL_MASK;
						}
						x++;
						cur_bit >>= 1;
					}
					cur_bit = 0x80;
				}
				PATTERN_FILLRECT_LOOPX;
			}
			if (mask == 0xFF && loop_rows <= 64) {
				cur_line++;
				if (cur_line == loop_rows) {
					cheat_y = y_line + 1;
					goto engage_cheat_codes;
				}
			}
			PATTERN_FILLRECT_LOOPY;
		}

		return;

engage_cheat_codes:;
		dp += (fb_pitch / 4);
		uint32_t *sp = dp - (cur_line * (fb_pitch / 4));
		for (uint16_t y_line = cheat_y; y_line < h; y_line++) {
			switch (color_format) {
				case MNTVA_COLOR_8BIT:
					memcpy(&((uint8_t *)dp)[rect_x1], &((uint8_t *)sp)[rect_x1], w);
					break;
				case MNTVA_COLOR_16BIT565:
				case MNTVA_COLOR_15BIT:
					memcpy(&((uint16_t *)dp)[rect_x1], &((uint16_t *)sp)[rect_x1], w * 2);
					break;
				case MNTVA_COLOR_32BIT:
					memcpy(&dp[rect_x1], &sp[rect_x1], w * 4);
					break;
			}
			dp += fb_pitch / 4;
			sp += fb_pitch / 4;
		}
		return;
	}
	else { // COMPLEMENT
		for (uint16_t y_line = 0; y_line < h; y_line++) {
			uint16_t x = rect_x1;

			cur_byte = (inversion) ? tmpl_data[tmpl_x] ^ 0xFF : tmpl_data[tmpl_x];

			while (x < rect_x2) {
				if (w >= 8 && cur_bit == 0x80 && x < rect_x2 - 8) {
					INVERT_PIXELS;
					x += 8;
				}
				else {
					while (cur_bit > 0 && x < rect_x2) {
						if (cur_byte & cur_bit) {
							INVERT_PIXEL;
						}
						x++;
						cur_bit >>= 1;
					}
					cur_bit = 0x80;
				}
				PATTERN_FILLRECT_LOOPX;
			}
			PATTERN_FILLRECT_LOOPY;	
		}
	}
}

#define TEMPLATE_FILLRECT_LOOPX \
	tmpl_x++; \
	cur_byte = (inversion) ? tmpl_data[tmpl_x] ^ 0xFF : tmpl_data[tmpl_x];

#define TEMPLATE_FILLRECT_LOOPY \
	tmpl_data += tmpl_pitch; \
	tmpl_x = tmpl_x_base; \
	cur_bit = base_bit; \
	dp += fb_pitch / 4;

void template_fill_rect(uint32_t color_format, uint16_t rect_x1, uint16_t rect_y1, uint16_t w, uint16_t h,
	uint8_t draw_mode, uint8_t mask, uint32_t fg_color, uint32_t bg_color,
	uint16_t x_offset, uint16_t y_offset,
	uint8_t *tmpl_data, uint16_t tmpl_pitch)
{
	h = clamp_rows_to_fb_limit(rect_y1, h);
	if (!h)
		return;

	uint32_t rect_x2 = rect_x1 + w;
	uint32_t *dp = fb + (rect_y1 * (fb_pitch / 4));

	uint16_t tmpl_x, tmpl_x_base;

	uint8_t cur_bit, base_bit, inversion = 0;
	uint8_t u8_fg = fg_color >> 24;
	uint8_t u8_bg = bg_color >> 24;
	uint8_t cur_byte = 0;

	tmpl_x = x_offset / 8;
	tmpl_x_base = tmpl_x;

	cur_bit = base_bit = (0x80 >> (x_offset % 8));

	if (draw_mode & INVERSVID) inversion = 1;
	draw_mode &= 0x03;

	if (draw_mode == JAM1) {
		for (uint16_t y_line = 0; y_line < h; y_line++) {
			uint16_t x = rect_x1;

			cur_byte = (inversion) ? tmpl_data[tmpl_x] ^ 0xFF : tmpl_data[tmpl_x];

			while (x < rect_x2) {
				if (w >= 8 && cur_bit == 0x80 && x < rect_x2 - 8) {
					if (mask == 0xFF) {
						SET_FG_PIXELS;
					}
					else {
						SET_FG_PIXELS_MASK;
					}
					x += 8;
				}
				else {
					while (cur_bit > 0 && x < rect_x2) {
						if (cur_byte & cur_bit) {
							SET_FG_PIXEL_MASK;
						}
						x++;
						cur_bit >>= 1;
					}
					cur_bit = 0x80;
				}
				TEMPLATE_FILLRECT_LOOPX;
			}
			TEMPLATE_FILLRECT_LOOPY;
		}

		return;
	}
	else if (draw_mode == JAM2) {
		for (uint16_t y_line = 0; y_line < h; y_line++) {
			uint16_t x = rect_x1;

			cur_byte = (inversion) ? tmpl_data[tmpl_x] ^ 0xFF : tmpl_data[tmpl_x];

			while (x < rect_x2) {
				if (w >= 8 && cur_bit == 0x80 && x < rect_x2 - 8) {
					if (mask == 0xFF) {
						SET_FG_OR_BG_PIXELS;
					}
					else {
						SET_FG_OR_BG_PIXELS_MASK;
					}
					x += 8;
				}
				else {
					while (cur_bit > 0 && x < rect_x2) {
						if (cur_byte & cur_bit) {
							SET_FG_PIXEL_MASK;
						}
						else {
							SET_BG_PIXEL_MASK;
						}
						x++;
						cur_bit >>= 1;
					}
					cur_bit = 0x80;
				}
				TEMPLATE_FILLRECT_LOOPX;
			}
			TEMPLATE_FILLRECT_LOOPY;
		}

		return;
	}
	else { // COMPLEMENT
		for (uint16_t y_line = 0; y_line < h; y_line++) {
			uint16_t x = rect_x1;

			cur_byte = (inversion) ? tmpl_data[tmpl_x] ^ 0xFF : tmpl_data[tmpl_x];

			while (w >= 8 && x < rect_x2) {
				if (cur_bit == 0x80 && x < rect_x2 - 8) {
					INVERT_PIXELS;
					x += 8;
				}
				else {
					while (cur_bit > 0 && x < rect_x2) {
						if (cur_byte & cur_bit) {
							INVERT_PIXEL;
						}
						x++;
						cur_bit >>= 1;
					}
					cur_bit = 0x80;
				}
				TEMPLATE_FILLRECT_LOOPX;
			}
			TEMPLATE_FILLRECT_LOOPY;	
		}
	}
}

#define MNTVA_FROM_BPP(d, s) \
	if (s == 2) { \
		d = MNTVA_COLOR_16BIT565; \
	} else if (s == 4) { \
		d = MNTVA_COLOR_32BIT; \
	}

// Generic graphics acceleration functionality
void acc_clear_buffer(uintptr_t addr, uint16_t w, uint16_t h, uint16_t pitch_, uint32_t fg_color, uint32_t color_format_)
{
	if (!w || !h || !addr)
		return;

	uint16_t pitch = pitch_ * color_format_;
	uint8_t* dp = (uint8_t*)addr;
	uint8_t u8_fg = fg_color >> 24;

	uint8_t color_format = MNTVA_COLOR_8BIT;
	MNTVA_FROM_BPP(color_format, color_format_)

	switch(color_format) {
		case MNTVA_COLOR_8BIT:
			for (int y = 0; y < h; y++) {
				memset(dp, u8_fg, w);
				dp += pitch;
			}
			break;
		case MNTVA_COLOR_16BIT565:
		case MNTVA_COLOR_15BIT:
			for (int y = 0; y < h; y++) {
				memset16((uint16_t *)dp, (uint16_t)fg_color, w);
				dp += pitch;
			}
			break;
		case MNTVA_COLOR_32BIT:
			for (int y = 0; y < h; y++) {
				memset32((uint32_t *)dp, fg_color, w);
				dp += pitch;
			}
			break;
		default:
			// Unknown/unhandled color format.
			break;
	}
}

void acc_flip_to_fb(uint32_t src, uint32_t dest, uint16_t w, uint16_t h, uint16_t pitch_, uint32_t color_format)
{
	// This function assumes a flip of a surface with the same dimensions as the frame buffer.
	if (!w || !h || !src || !dest)
		return;

	uint16_t pitch = pitch_ * color_format;
	uint8_t* sp = (uint8_t*)((uint32_t)src);
	uint8_t* dp = (uint8_t *)((uint32_t)dest);

	memcpy (dp, sp, h * pitch);
}

void acc_blit_rect(uintptr_t src, uintptr_t dest, uint16_t dx, uint16_t dy, uint16_t w, uint16_t h, uint16_t src_pitch, uint16_t dest_pitch, uint8_t draw_mode, uint8_t mask_color)
{
	if (!w || !h || !src || !dest)
		return;

	uint8_t* sp = (uint8_t*)src;
	uint8_t* dp = (uint8_t*)dest;
	dp += (dx + (dy * dest_pitch));

	switch (draw_mode) {
		case 1: // Reverse direction (bottom-up, for overlapping downward moves)
			sp = (uint8_t*)src + (uint32_t)(h - 1) * src_pitch;
			dp = (uint8_t*)dest + dx + ((uint32_t)(dy + h - 1) * dest_pitch);

			for (int y = 0; y < h; y++) {
				memmove(dp, sp, w);
				dp -= dest_pitch;
				sp -= src_pitch;
			}
			return;
			break;
		case 2: // Masked blit
			for (int y = 0; y < h; y++) {
				for (int x = 0; x < w; x++) {
					if (sp[x] != mask_color)
						dp[x] = sp[x];
				}
				dp += dest_pitch;
				sp += src_pitch;
			}
			return;
			break;
		default:
			break;
	}

	for (int i = 0; i < h; i++) {
		memcpy(dp, sp, w);
		dp += dest_pitch;
		sp += src_pitch;
	}
}

void acc_blit_rect_16to8(uint32_t src, uint32_t dest, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t src_pitch, uint16_t dest_pitch)
{
	if (!w || !h || !src || !dest)
		return;

	uint16_t* sp = (uint16_t*)((uint32_t)src);
	uint8_t* dp = (uint8_t *)((uint32_t)dest);
	dp += (x + (y * dest_pitch));

	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			dp[x] = color_map_16_to_8[SWAP16(sp[x])];
		}
		dp += dest_pitch;
		sp += src_pitch;
	}
}

#define ACC_DRAW_LINE_PIXELS \
	for (int y = 0; y < pen_width; y++) { \
		for (int x2 = 0; x2 < pen_width; x2++) { \
			switch(color_format) { \
				case MNTVA_COLOR_8BIT: \
					dp[x + x2 + (y * pitch)] = u8_fg; break; \
				case MNTVA_COLOR_16BIT565: \
				case MNTVA_COLOR_15BIT: \
					((uint16_t *)dp)[x + x2 + (y * pitch)] = fg_color; break; \
				case MNTVA_COLOR_32BIT: \
					((uint32_t *)dp)[x + x2 + (y * pitch)] = fg_color; break; \
			} \
		} \
	}

void acc_draw_line(uint32_t dest, uint16_t pitch, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint32_t fg_color, uint8_t bpp, uint8_t pen_width, uint8_t pen_height)
{
	uint8_t color_format = MNTVA_COLOR_8BIT;
	MNTVA_FROM_BPP(color_format, bpp)
	uint8_t u8_fg = fg_color >> 24;

	uint8_t* dp = (uint8_t *)((uint32_t)dest + (y1 * pitch));
	int32_t line_step = pitch;
	int8_t x_reverse = 0;

	int16_t dx, dy, dx_abs, dy_abs, ix, iy, x = x1, len;

	if (x2 < x1)
		x_reverse = 1;
	if (y2 < y1)
		line_step = -pitch;

	dx = x2 - x1;
	dy = y2 - y1;
	dx_abs = abs(dx);
	dy_abs = abs(dy);
	ix = dy_abs >> 1;
	iy = dx_abs >> 1;

	ACC_DRAW_LINE_PIXELS;

	if (dx_abs >= dy_abs) {
		len = dx_abs;
		for (uint16_t i = 0; i < len; i++) {
			iy += dy_abs;
			if (iy >= dx_abs) {
				iy -= dx_abs;
				dp += line_step;
			}
			x += (x_reverse) ? -1 : 1;

			ACC_DRAW_LINE_PIXELS;
		}
	}
	else {
		len = dy_abs;
		for (uint16_t i = 0; i < len; i++) {
			ix += dx_abs;
			if (ix >= dy_abs) {
				ix -= dy_abs;
				x += (x_reverse) ? -1 : 1;
			}
			dp += line_step;

			ACC_DRAW_LINE_PIXELS;
		}
	}
}

void acc_fill_rect(uint32_t dest, uint16_t pitch, int16_t x, int16_t y, int16_t w, int16_t h, uint32_t fg_color, uint8_t bpp)
{
	uint8_t color_format = MNTVA_COLOR_8BIT;
	MNTVA_FROM_BPP(color_format, bpp)
	uint8_t u8_fg = fg_color >> 24;

	uint8_t* dp = (uint8_t *)((uint32_t)dest + (x * bpp) + (y * pitch));
	for (int i = 0; i < h; i++) {
		for (int j = 0; j < w; j++) {
			switch(color_format) {
				case MNTVA_COLOR_8BIT:
					memset(dp, u8_fg, w);
					j = w;
					break;
				case MNTVA_COLOR_16BIT565:
				case MNTVA_COLOR_15BIT:
					((uint16_t *)dp)[x] = fg_color;
					break;
				case MNTVA_COLOR_32BIT:
					((uint32_t *)dp)[x] = fg_color;
					break;
				default:
					break;
			}
		}
		dp += pitch;
	}
}

#define CHKBLOT(a, b) \
	if (a >= 0 && b >= 0 && a < w && b < h)

//	DrawPixel(surface, x + x1, y + y1, colour);
//	DrawPixel(surface, x - x1, y + y1, colour);
//	DrawPixel(surface, x + x1, y - y1, colour);
//	DrawPixel(surface, x - x1, y - y1, colour);
//	DrawPixel(surface, x + y1, y + x1, colour);
//	DrawPixel(surface, x - y1, y + x1, colour);
//	DrawPixel(surface, x + y1, y - x1, colour);
//	DrawPixel(surface, x - y1, y - x1, colour);

#define BLOTCIRCLE(a, b) \
	CHKBLOT((x + x1),(y + y1)) a[(x + x1) + ((y + y1) * pitch)] = b; \
	CHKBLOT((x - x1),(y + y1)) a[(x - x1) + ((y + y1) * pitch)] = b; \
	CHKBLOT((x + x1),(y - y1)) a[(x + x1) + ((y - y1) * pitch)] = b; \
	CHKBLOT((x - x1),(y - y1)) a[(x - x1) + ((y - y1) * pitch)] = b; \
	CHKBLOT((x + y1),(y + x1)) a[(x + y1) + ((y + x1) * pitch)] = b; \
	CHKBLOT((x - y1),(y + x1)) a[(x - y1) + ((y + x1) * pitch)] = b; \
	CHKBLOT((x + y1),(y - x1)) a[(x + y1) + ((y - x1) * pitch)] = b; \
	CHKBLOT((x - y1),(y - x1)) a[(x - y1) + ((y - x1) * pitch)] = b;

void acc_draw_circle(uint32_t dest, uint16_t pitch, int16_t x, int16_t y, int16_t r, int16_t w, int16_t h, uint32_t fg_color, uint8_t bpp)
{
	uint8_t color_format = MNTVA_COLOR_8BIT;
	MNTVA_FROM_BPP(color_format, bpp)
	uint8_t u8_fg = fg_color >> 24;

	int x1 = r;
	int y1 = 0;
	int d = 3 - 2 * r;
	
	uint8_t* dp = (uint8_t *)(uint32_t)dest;

	while (x1 >= y1) {
		y1++;

		if (d > 0) {
			x1--;
			d = d + 4 * (y1 - x1) + 10;
		}
		else {
			d = d + 4 * y1 + 6;
		}

		switch(color_format) {
			case MNTVA_COLOR_8BIT:
				BLOTCIRCLE(dp, u8_fg);
				break;
			case MNTVA_COLOR_16BIT565:
			case MNTVA_COLOR_15BIT:
				BLOTCIRCLE(((uint16_t *)dp), fg_color);
				break;
			case MNTVA_COLOR_32BIT:
				BLOTCIRCLE(((uint32_t *)dp), fg_color);
				break;
			default:
				break;
		}
	}
}

void acc_fill_circle(uint32_t dest, uint16_t pitch, int16_t x0, int16_t y0, int16_t r, int16_t w, int16_t h, uint32_t fg_color, uint8_t bpp)
{
	uint8_t color_format = MNTVA_COLOR_8BIT;
	MNTVA_FROM_BPP(color_format, bpp)
	uint8_t u8_fg = fg_color >> 24;

	uint8_t* dp = (uint8_t *)(uint32_t)dest;
	float radius_sqr = r * r;

	for (int x = -r; x < r ; x++)
	{
		int hh = (int)sqrt(radius_sqr - x * x);
		int rx = x0 + x;
		int ph = y0 + hh;

		for (int y = y0 - hh; y < ph; y++) {
			switch(color_format) {
				case MNTVA_COLOR_8BIT:
					CHKBLOT(rx, y)
						dp[rx + (y * pitch)] = u8_fg;
					break;
				case MNTVA_COLOR_16BIT565:
				case MNTVA_COLOR_15BIT:
					CHKBLOT(rx, y)
						((uint16_t *)dp)[rx + (y * pitch)] = fg_color;
					break;
				case MNTVA_COLOR_32BIT:
					CHKBLOT(rx, y)
						((uint32_t *)dp)[rx + (y * pitch)] = fg_color;
					break;
				default:
					break;
			}
		}
	}
}

uint8_t *tri_array = 0;

void TriTexLine(int32_t x1, int32_t x2, int32_t y, int32_t tx1, int32_t tx2, int32_t ty1, int32_t ty2, uint16_t w, uint16_t h, uint32_t fg_color)
{
	// Round to ensure that problems caused by rounding errors don't occur (jumping lines)
	x2 &= 0xFFFF0000;
	x1 &= 0xFFFF0000;
	uint8_t u8_fg = fg_color >> 24;

	// Sort values to make drawing from left to right possible
	if (x2 < x1) {
		int32_t temp = x2;
		x2 = x1;
		x1 = temp;
		temp = tx2;
		tx2 = tx1;
		tx1 = temp;
		temp = ty2;
		ty2 = ty1;
		ty1 = temp;
	}

	int32_t xdelta = (x2 - x1) >>16;
	if (xdelta <1)
		return;

	int xd = xdelta;

	//Calculate start Tex-X and Tex-X-Increment
	int txi = tx1; //fixed point
	int32_t txd = (tx2 - tx1) / xdelta;
	int txdi = txd; //same here

	//Same for Tex-Y and Tex-Y-Increment
	int tyi = ty1; //fixed point
	int32_t tyd = (ty2 - ty1) / xdelta;
	int tydi = tyd; //same here


	//Clipping begin
	//If line isn't inside screen -> outta here
	x1 >>= 16;
	x2 >>= 16;

	if (x1 > ((w - 1)) || (x2 < 0))
		return;

	/*If the line is clipped at the left screen border (where we start), the left out
	gouraud and texture steps have to be calculated; x is set to 0 */
	if (x1 < 0) {
		//int xm=-x1;
		x1 = 0;
	}
	/* x is simply clipped at the right border. That's where the loop is going to end
	then */
	if (x2 > (w - 1))
		x2= (w - 1);
	//End of clipping and calculation of screen start address
		int arrayptr = (y * w) + (x1);

	//Recalculate X-Delta because of clipping
	xdelta = (x2 - x1);
	if (xdelta <= 0)
		return;
	xd = (int)(xdelta);

	for (int x = 0; x <= xd; x++) {
		//Fetch a pixel from the texture (256*256)
		*(tri_array + (arrayptr++)) = u8_fg;
		//*(tri_array + (arrayptr++)) = array_tex1[(txi >> 16) + ((tyi >> 8) & 0xff00)];
		
		//Increase Texture- and Gouraud-counter
		txi += txdi;
		tyi += tydi;
	}
}

// filled tri code viciously stolen from mntmn...!
void acc_fill_flat_tri(uint32_t dest, TriangleDef *d, uint16_t w, uint16_t h, uint32_t fg_color, uint8_t bpp)
{
	uint8_t u8_fg = fg_color >> 24;
	int32_t *dataa, *datab, *datac;

	int32_t xs1, xs2, xs3, txs1, txs2, txs3, tys1, tys2, tys3;
	int32_t *tempdata;

	dataa = d->a;
	datab = d->b;
	datac = d->c;

	tri_array = (uint8_t *)dest;

	// Very simple sorting of the three y coordinates
	if (dataa[1] > datab[1]) {
		tempdata = dataa;
		dataa = datab;
		datab = tempdata;
	}
	if (datab[1] > datac[1]) {
		tempdata = datab;
		datab = datac;
		datac = tempdata;
	}
	if (dataa[1] > datab[1]) {
		tempdata = dataa;
		dataa = datab;
		datab = tempdata;
	}

	// Calculate some deltas 
	int32_t xd1 = datab[0] - dataa[0];
	int32_t xd2 = datac[0] - dataa[0];
	int32_t xd3 = datac[0] - datab[0];
	int32_t yd1 = datab[1] - dataa[1];
	int32_t yd2 = datac[1] - dataa[1];
	int32_t yd3 = datac[1] - datab[1];
	int32_t txd1 = datab[2] - dataa[2];
	int32_t txd2 = datac[2] - dataa[2];
	int32_t txd3 = datac[2] - datab[2];
	int32_t tyd1 = datab[3] - dataa[3];
	int32_t tyd2 = datac[3] - dataa[3];
	int32_t tyd3 = datac[3] - datab[3];

	// Calculate steps per line while taking care of division by 0
	if(yd1 != 0) {
		xs1 = xd1 / yd1;
		txs1 = txd1 / yd1;
		tys1 = tyd1 / yd1;
	}
	else {
		xs1 = xd1;
		txs1 = txd1;
		tys1 = tyd1;
	}
	if(yd2 != 0) {
		xs2 = xd2 / yd2;
		txs2 = txd2 / yd2;
		tys2 = tyd2 / yd2;
	}
	else {
		xs2 = xd2;
		txs2 = txd2;
		tys2 = tyd2;
	}
	if(yd3 != 0) {
		xs3 = xd3 / yd3;
		txs3 = txd3 / yd3;
		tys3 = tyd3 / yd3;
	}
	else  {
		xs3 = xd3;
		txs3 = txd3;
		tys3 = tyd3;
	}
	
	/*
	 Variable meanings:
	
	 xs? xstep=delta x
	 txs? delta tx
	 tys? delta ty
	 xd? xdelta
	 yd? dunno
	 txd?  "
	 tyd?  "
	 xw? current x-value used in loop
	 txw? for tx
	 tyw? for ty
	*/
	/*
	 Start values for the first part (up to y of point 2)
	 xw1 and xw2 are x-values for the current line. The triangle is drawn from
	 top to bottom line after line...
	 txw, tyw and gw are values for texture and brightness
	 always for start- and ending-point of the current line
	 A line is also called "Span".
	*/

	int32_t xw1 = dataa[0]; //pax
	int32_t xw2 = dataa[0];
	int32_t txw1 = dataa[2]; //tax
	int32_t txw2 = dataa[2];
	int32_t tyw1 = dataa[3]; //tay
	int32_t tyw2 = dataa[3];

	if (yd1) {
		for (int sz = dataa[1]; sz <= datab[1]; sz++) {
			// draw if y is inside the screen (clipping)   
			if (sz >=h )
				break;
			if (sz >= 0 && sz < h) {
				uint32_t xed = (xw1 < xw2) ? xw1 : xw2;
				uint32_t xed2 = (xw1 < xw2) ? xw2 : xw1;
				xed = ((xed & 0xFFFF0000) >> 16);
				xed2 = ((xed2 & 0xFFFF0000) >> 16);
				if ((xed < 0 && xed2 < 0) || (xed >= w && xed2 >= w) || (xed == xed2))
					goto skip_span;
				if (xed < 0) xed = 0;
				if (xed2 >= w) xed2 = w - 1;

				int clear_w = (xed == xed2) ? 1 : xed2 - xed;
				memset((uint8_t *)(uint32_t)(dest + xed + (sz * w)), u8_fg, clear_w);
				skip_span:;
			}
			xw1 += xs1;
			xw2 += xs2;
			txw1 += txs1;
			txw2 += txs2;
			tyw1 += tys1;
			tyw2 += tys2;
		}
	}
	
	/*
	 New start values for the second part of the triangle
	*/
	xw1 = datab[0] + xs3;
	txw1 = datab[2] + txs3;
	tyw1 = datab[3] + tys3;

	if (yd3) { //If Span-Height 1 or higher
		for (int sz=datab[1] + 1; sz < datac[1]; sz++)
		{
			if (sz >=h )
				break;

			if (sz >= 0 && sz < (h - 1)) {
				int xed = (xw1 < xw2) ? xw1 : xw2;
				int xed2 = (xw1 < xw2) ? xw2 : xw1;
				xed = ((xed & 0xFFFF0000) >> 16);
				xed2 = ((xed2 & 0xFFFF0000) >> 16);
				if ((xed < 0 && xed2 < 0) || (xed >= w && xed2 >= w) || (xed == xed2))
					goto skip_span2;
				if (xed < 0) xed = 0;
				if (xed2 >= w) xed2 = w - 1;

				int clear_w = (xed == xed2) ? 1 : xed2 - xed;
				memset((uint8_t *)(uint32_t)(dest + xed + (sz * w)), u8_fg, clear_w);
				skip_span2:;
			}
			xw1 += xs3;
			xw2 += xs2;
			txw1 += txs3;
			txw2 += txs2;
			tyw1 += tys3;
			tyw2 += tys2;
		}
	}
};
