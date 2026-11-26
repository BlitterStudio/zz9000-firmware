/*
 * Shared-buffer output of a media session. See sdk_media_output.h.
 *
 * The conversion is specified in sdk/docs/zz9k-media-output.md: the scaler
 * works per axis on the three planes (vertical pass into a row, horizontal
 * pass into the output width), the colour matrix is BT.601 limited range with
 * the constants the overlay compositor uses, and the quantiser adds a 4x4
 * Bayer threshold per channel. Tables are cached across pictures keyed by the
 * geometry and format, so a picture costs the per-pixel work only.
 *
 * Core 1 is the only caller at run time, so the scratch below is static and
 * unlocked. The host tests call it from one thread.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_media_output.h"

#include <string.h>

#define MAX_W SDK_MEDIA_OUTPUT_MAX_WIDTH
#define MAX_CHROMA_W ((SDK_MEDIA_OUTPUT_MAX_SOURCE_WIDTH + 1U) / 2U)

static const uint8_t bayer4[4][4] = {
	{ 0U, 8U, 2U, 10U },
	{ 12U, 4U, 14U, 6U },
	{ 3U, 11U, 1U, 9U },
	{ 15U, 7U, 13U, 5U },
};

/* Cube geometry of the fixed palettes. Colour i of a cube palette is
 * (r * lg + g) * lb + b with r < lr, g < lg, b < lb; a gray palette has
 * 1 << depth levels. */
struct fixed_dims {
	uint8_t lr;
	uint8_t lg;
	uint8_t lb;
	uint8_t gray;
};

static const struct fixed_dims fixed_dims[9] = {
	{ 0U, 0U, 0U, 0U },
	{ 2U, 2U, 2U, 1U },
	{ 4U, 4U, 4U, 1U },
	{ 2U, 2U, 2U, 0U },
	{ 2U, 4U, 2U, 0U },
	{ 4U, 4U, 2U, 0U },
	{ 4U, 4U, 4U, 0U },
	{ 5U, 5U, 5U, 0U },
	{ 6U, 7U, 6U, 0U },
};

/* Bayer amplitude of a host palette, one cube step of an equivalent cube. */
static const uint8_t host_step[9] = {
	0U, 255U, 255U, 255U, 170U, 128U, 85U, 64U, 48U,
};

static uint32_t level_value(uint32_t i, uint32_t levels)
{
	if (levels <= 1U)
		return 0U;
	return (i * 255U + (levels - 1U) / 2U) / (levels - 1U);
}

uint32_t sdk_media_output_fixed_palette(uint32_t depth, uint32_t out[256])
{
	const struct fixed_dims *d;
	uint32_t i;
	uint32_t used;

	memset(out, 0, 256U * sizeof(out[0]));
	if (depth < 1U || depth > 8U)
		return 0U;
	d = &fixed_dims[depth];
	if (d->gray) {
		used = 1U << depth;
		for (i = 0U; i < used; i++) {
			uint32_t v = level_value(i, used);

			out[i] = (v << 16) | (v << 8) | v;
		}
		return used;
	}
	used = (uint32_t)d->lr * d->lg * d->lb;
	for (i = 0U; i < used; i++) {
		uint32_t b = i % d->lb;
		uint32_t g = (i / d->lb) % d->lg;
		uint32_t r = i / ((uint32_t)d->lb * d->lg);

		out[i] = (level_value(r, d->lr) << 16) |
		         (level_value(g, d->lg) << 8) | level_value(b, d->lb);
	}
	return used;
}

static uint32_t row_bytes_for(uint32_t format, uint32_t width)
{
	if (format == SDK_SURFACE_FORMAT_INDEX8)
		return width;
	if (format == SDK_SURFACE_FORMAT_PLANAR)
		return (width + 7U) / 8U;
	return width * 2U;
}

uint16_t sdk_media_output_check(const struct SDKMediaOutputConfig *cfg,
	                            struct SDKMediaOutputLayout *layout)
{
	struct SDKMediaOutputLayout out;
	uint32_t flags;
	uint64_t picture;
	uint64_t span;

	if (!cfg || !layout)
		return SDK_STATUS_BAD_REQUEST;
	flags = cfg->flags;
	if ((flags & ~SDK_MEDIA_OUTPUT_FLAGS_KNOWN) != 0U ||
	    cfg->width == 0U || cfg->width > SDK_MEDIA_OUTPUT_MAX_WIDTH ||
	    cfg->height == 0U || cfg->height > SDK_MEDIA_OUTPUT_MAX_HEIGHT ||
	    (cfg->buffer_offset & 3U) != 0U || (cfg->slot_stride & 3U) != 0U)
		return SDK_STATUS_BAD_REQUEST;
	if ((flags & SDK_MEDIA_OUTPUT_DITHER_ORDERED) != 0U &&
	    (flags & SDK_MEDIA_OUTPUT_DITHER_DIFFUSION) != 0U)
		return SDK_STATUS_BAD_REQUEST;

	memset(&out, 0, sizeof(out));
	switch (cfg->format) {
	case SDK_SURFACE_FORMAT_INDEX8:
	case SDK_SURFACE_FORMAT_PLANAR:
		out.depth = flags & SDK_MEDIA_OUTPUT_DEPTH_MASK;
		if (out.depth == 0U)
			out.depth = 8U;
		if (out.depth > 8U)
			return SDK_STATUS_BAD_REQUEST;
		if (cfg->format == SDK_SURFACE_FORMAT_INDEX8 &&
		    (flags & SDK_MEDIA_OUTPUT_PLANAR_INTERLEAVED) != 0U)
			return SDK_STATUS_BAD_REQUEST;
		break;
	case SDK_SURFACE_FORMAT_RGB565:
	case SDK_SURFACE_FORMAT_RGB555:
		if ((flags & (SDK_MEDIA_OUTPUT_DEPTH_MASK |
		              SDK_MEDIA_OUTPUT_PLANAR_INTERLEAVED |
		              SDK_MEDIA_OUTPUT_PALETTE_HOST)) != 0U)
			return SDK_STATUS_BAD_REQUEST;
		break;
	default:
		return SDK_STATUS_UNSUPPORTED;
	}
	if ((flags & SDK_MEDIA_OUTPUT_DITHER_DIFFUSION) != 0U)
		return SDK_STATUS_UNSUPPORTED;

	out.row_bytes = row_bytes_for(cfg->format, cfg->width);
	if (cfg->pitch < out.row_bytes)
		return SDK_STATUS_BAD_REQUEST;
	picture = (uint64_t)cfg->pitch * cfg->height;
	if (cfg->format == SDK_SURFACE_FORMAT_PLANAR)
		picture *= out.depth;
	out.slots = cfg->slot_stride != 0U ? 2U : 1U;
	span = picture;
	if (out.slots == 2U) {
		if (cfg->slot_stride < picture)
			return SDK_STATUS_BAD_REQUEST;
		span += cfg->slot_stride;
	}
	if (span > 0xffffffffU)
		return SDK_STATUS_BAD_REQUEST;
	out.picture_bytes = (uint32_t)picture;
	out.span_bytes = (uint32_t)span;
	*layout = out;
	return SDK_STATUS_OK;
}

uint32_t sdk_media_output_job_bytes(const struct SDKMediaOutputJob *job)
{
	uint32_t bytes = job->pitch * job->height;

	if (job->format == SDK_SURFACE_FORMAT_PLANAR)
		bytes *= job->depth;
	return bytes;
}

/* ---- scratch and caches --------------------------------------------- */

struct tap2 {
	uint16_t i0;
	uint16_t i1;
	uint16_t w;
	uint16_t pad;
};

/* One horizontal pass. Either two taps per output pixel (nearest and
 * bilinear: out = lerp(src[i0], src[i1], w / 256)) or an area average over
 * count[x] source pixels starting at first[x]. */
struct hplan {
	union {
		struct tap2 tap[MAX_W];
		struct {
			uint16_t first[MAX_W];
			uint16_t count[MAX_W];
			uint16_t weight[2U * MAX_W];
		} area;
	} u;
	uint32_t r24;
	uint8_t area;
	uint8_t identity;
};

static struct {
	uint32_t sw, sh, dw, dh;
	uint8_t nearest;
	uint8_t valid;
	struct hplan luma;
	struct hplan chroma;
} plan;

static struct {
	uint32_t format, depth;
	uint8_t dither, host;
	uint8_t valid;
	uint16_t table[3][17][256];
} quant;

static struct {
	uint8_t pal[SDK_MEDIA_OUTPUT_PALETTE_BYTES];
	uint32_t depth;
	uint8_t valid;
	uint8_t lut[32768];
} inverse;

static struct {
	uint8_t vy[MAX_W];
	uint8_t vcb[MAX_CHROMA_W];
	uint8_t vcr[MAX_CHROMA_W];
	uint8_t hy[MAX_W];
	uint8_t hcb[MAX_W];
	uint8_t hcr[MAX_W];
	uint8_t index[MAX_W + 8U];
	uint32_t acc[MAX_W];
} row;

/* ---- scaler ---------------------------------------------------------- */

static uint32_t plane_size(uint32_t luma, int chroma)
{
	return chroma ? (luma + 1U) / 2U : luma;
}

static int use_area(uint32_t luma_src, uint32_t dst, int nearest)
{
	return !nearest && luma_src >= 2U * dst;
}

/* Two-tap position of output index i on an axis whose luma size is `sl`,
 * scaled to `d` output pixels. `chroma` selects the half-size chroma plane,
 * whose samples sit between pairs of luma pixels. */
static void tap_at(struct tap2 *t, uint32_t i, uint32_t sl, uint32_t d,
	               int chroma, int nearest)
{
	uint32_t size = plane_size(sl, chroma);
	int64_t pos;

	if (nearest) {
		uint32_t at = ((2U * i + 1U) * sl) / (2U * d);

		if (chroma)
			at >>= 1;
		if (at >= size)
			at = size - 1U;
		t->i0 = (uint16_t)at;
		t->i1 = (uint16_t)at;
		t->w = 0U;
		return;
	}
	pos = (int64_t)((((uint64_t)(2U * i + 1U) * sl) << (chroma ? 14 : 15)) /
	                d) - 32768;
	if (pos < 0 || size == 1U) {
		t->i0 = 0U;
		t->i1 = 0U;
		t->w = 0U;
		return;
	}
	if ((uint64_t)(pos >> 16) >= size - 1U) {
		t->i0 = (uint16_t)(size - 1U);
		t->i1 = (uint16_t)(size - 1U);
		t->w = 0U;
		return;
	}
	t->i0 = (uint16_t)(pos >> 16);
	t->i1 = (uint16_t)(t->i0 + 1U);
	t->w = (uint16_t)((pos >> 8) & 255);
}

static uint32_t reciprocal24(uint32_t size)
{
	return (uint32_t)(((1ULL << 24) + size / 2U) / size);
}

static uint8_t area_round(uint32_t sum, uint32_t r24)
{
	return (uint8_t)(((uint64_t)sum * r24 + (1ULL << 23)) >> 24);
}

static void build_hplan(struct hplan *p, uint32_t sl, uint32_t d, int chroma,
	                    int nearest)
{
	uint32_t size = plane_size(sl, chroma);
	uint32_t x;

	p->area = (uint8_t)use_area(sl, d, nearest);
	p->identity = 0U;
	if (p->area) {
		uint32_t total = 0U;

		p->r24 = reciprocal24(size);
		for (x = 0U; x < d; x++) {
			uint32_t s0 = x * size;
			uint32_t s1 = s0 + size;
			uint32_t j0 = s0 / d;
			uint32_t j1 = (s1 + d - 1U) / d;
			uint32_t j;

			if (j1 > size)
				j1 = size;
			p->u.area.first[x] = (uint16_t)j0;
			p->u.area.count[x] = (uint16_t)(j1 - j0);
			for (j = j0; j < j1; j++) {
				uint32_t lo = j * d > s0 ? j * d : s0;
				uint32_t hi = (j + 1U) * d < s1 ? (j + 1U) * d : s1;

				p->u.area.weight[total++] = (uint16_t)(hi - lo);
			}
		}
		return;
	}
	for (x = 0U; x < d; x++)
		tap_at(&p->u.tap[x], x, sl, d, chroma, nearest);
	if (!chroma) {
		p->identity = 1U;
		for (x = 0U; x < d; x++) {
			if (p->u.tap[x].i0 != x || p->u.tap[x].w != 0U) {
				p->identity = 0U;
				break;
			}
		}
	}
}

static void ensure_plan(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh,
	                    int nearest)
{
	if (plan.valid && plan.sw == sw && plan.sh == sh && plan.dw == dw &&
	    plan.dh == dh && plan.nearest == (uint8_t)nearest)
		return;
	build_hplan(&plan.luma, sw, dw, 0, nearest);
	build_hplan(&plan.chroma, sw, dw, 1, nearest);
	plan.sw = sw;
	plan.sh = sh;
	plan.dw = dw;
	plan.dh = dh;
	plan.nearest = (uint8_t)nearest;
	plan.valid = 1U;
}

/* The row of a plane that output row y reads: a source row when it needs no
 * blending, else `tmp` holding the vertical pass. */
static const uint8_t *vertical_row(const uint8_t *src, uint32_t pitch,
	                               uint32_t width, uint32_t sl_height,
	                               uint32_t dh, uint32_t y, int chroma,
	                               int nearest, uint8_t *tmp)
{
	uint32_t ph = plane_size(sl_height, chroma);
	uint32_t x;

	if (use_area(sl_height, dh, nearest)) {
		uint32_t s0 = y * ph;
		uint32_t s1 = s0 + ph;
		uint32_t j0 = s0 / dh;
		uint32_t j1 = (s1 + dh - 1U) / dh;
		uint32_t r24 = reciprocal24(ph);
		uint32_t j;

		if (j1 > ph)
			j1 = ph;
		memset(row.acc, 0, width * sizeof(row.acc[0]));
		for (j = j0; j < j1; j++) {
			uint32_t lo = j * dh > s0 ? j * dh : s0;
			uint32_t hi = (j + 1U) * dh < s1 ? (j + 1U) * dh : s1;
			uint32_t w = hi - lo;
			const uint8_t *r = src + (size_t)j * pitch;

			for (x = 0U; x < width; x++)
				row.acc[x] += w * r[x];
		}
		for (x = 0U; x < width; x++)
			tmp[x] = area_round(row.acc[x], r24);
		return tmp;
	} else {
		struct tap2 t;
		const uint8_t *a;
		const uint8_t *b;

		tap_at(&t, y, sl_height, dh, chroma, nearest);
		a = src + (size_t)t.i0 * pitch;
		if (t.w == 0U)
			return a;
		b = src + (size_t)t.i1 * pitch;
		for (x = 0U; x < width; x++)
			tmp[x] = (uint8_t)((a[x] * (256U - t.w) + b[x] * t.w + 128U) >> 8);
		return tmp;
	}
}

static void horizontal_row(const struct hplan *p, const uint8_t *src,
	                       uint8_t *dst, uint32_t dw)
{
	uint32_t x;

	if (p->area) {
		const uint16_t *weight = p->u.area.weight;

		for (x = 0U; x < dw; x++) {
			const uint8_t *s = src + p->u.area.first[x];
			uint32_t n = p->u.area.count[x];
			uint32_t sum = 0U;
			uint32_t k;

			for (k = 0U; k < n; k++)
				sum += (uint32_t)weight[k] * s[k];
			weight += n;
			dst[x] = area_round(sum, p->r24);
		}
		return;
	}
	for (x = 0U; x < dw; x++) {
		const struct tap2 *t = &p->u.tap[x];
		uint32_t a = src[t->i0];
		uint32_t b = src[t->i1];

		dst[x] = (uint8_t)((a * (256U - t->w) + b * t->w + 128U) >> 8);
	}
}

/* ---- quantiser ------------------------------------------------------- */

/* Level of value v on a channel of `levels` levels with Bayer threshold
 * (2 * t + 1) / 32: floor(v * (levels - 1) / 255 + (2t + 1) / 32), capped.
 * Row 16 is the plain rounding threshold of one half. */
static uint32_t level_for(uint32_t v, uint32_t levels, uint32_t row)
{
	uint32_t thr = row < 16U ? 2U * row + 1U : 16U;
	uint32_t q = (v * (levels - 1U) * 32U + thr * 255U) / (255U * 32U);

	return q > levels - 1U ? levels - 1U : q;
}

static void build_quant(uint32_t format, uint32_t depth, int dither, int host)
{
	static const uint32_t shift565[3] = { 11U, 5U, 0U };
	static const uint32_t shift555[3] = { 10U, 5U, 0U };
	static const uint32_t shift_cell[3] = { 10U, 5U, 0U };
	uint32_t ch, r, v;

	if (quant.valid && quant.format == format && quant.depth == depth &&
	    quant.dither == (uint8_t)dither && quant.host == (uint8_t)host)
		return;
	for (ch = 0U; ch < 3U; ch++) {
		for (r = 0U; r < 17U; r++) {
			for (v = 0U; v < 256U; v++) {
				uint32_t entry;

				if (format == SDK_SURFACE_FORMAT_RGB565) {
					entry = level_for(v, ch == 1U ? 64U : 32U, r)
					        << shift565[ch];
				} else if (format == SDK_SURFACE_FORMAT_RGB555) {
					entry = level_for(v, 32U, r) << shift555[ch];
				} else if (host) {
					int32_t off = r < 16U
						? ((2 * (int32_t)r - 15) *
						   (int32_t)host_step[depth]) / 32 : 0;
					int32_t w = (int32_t)v + off;
					uint32_t cell;

					w = w < 0 ? 0 : w > 255 ? 255 : w;
					cell = ((uint32_t)w * 31U + 127U) / 255U;
					entry = cell << shift_cell[ch];
				} else {
					const struct fixed_dims *d = &fixed_dims[depth];

					if (d->gray) {
						entry = ch == 1U
							? level_for(v, 1U << depth, r) : 0U;
					} else {
						uint32_t levels = ch == 0U ? d->lr
							: ch == 1U ? d->lg : d->lb;
						uint32_t mult = ch == 0U
							? (uint32_t)d->lg * d->lb
							: ch == 1U ? d->lb : 1U;

						entry = level_for(v, levels, r) * mult;
					}
				}
				quant.table[ch][r][v] = (uint16_t)entry;
			}
		}
	}
	quant.format = format;
	quant.depth = depth;
	quant.dither = (uint8_t)dither;
	quant.host = (uint8_t)host;
	quant.valid = 1U;
}

/* Nearest palette entry (squared RGB distance, lowest index on a tie) for
 * every 5-5-5 cell, measured at the cell's replicated centre colour. */
static void build_inverse(const uint8_t *palette, uint32_t depth)
{
	uint8_t cc[32];
	uint32_t entries = depth >= 8U ? 256U : 1U << depth;
	uint32_t rg, i;

	if (inverse.valid && inverse.depth == depth &&
	    memcmp(inverse.pal, palette, entries * 4U) == 0)
		return;
	for (i = 0U; i < 32U; i++)
		cc[i] = (uint8_t)((i << 3) | (i >> 2));
	for (rg = 0U; rg < 1024U; rg++) {
		uint32_t best_d[32];
		uint8_t best[32];
		uint32_t p, b;

		for (b = 0U; b < 32U; b++) {
			best_d[b] = 0xffffffffU;
			best[b] = 0U;
		}
		for (p = 0U; p < entries; p++) {
			int32_t dr = (int32_t)cc[rg >> 5] - palette[p * 4U + 1U];
			int32_t dg = (int32_t)cc[rg & 31U] - palette[p * 4U + 2U];
			int32_t pb = palette[p * 4U + 3U];
			uint32_t base = (uint32_t)(dr * dr + dg * dg);

			for (b = 0U; b < 32U; b++) {
				int32_t db = (int32_t)cc[b] - pb;
				uint32_t d = base + (uint32_t)(db * db);

				if (d < best_d[b]) {
					best_d[b] = d;
					best[b] = (uint8_t)p;
				}
			}
		}
		memcpy(&inverse.lut[rg * 32U], best, 32U);
	}
	memcpy(inverse.pal, palette, entries * 4U);
	inverse.depth = depth;
	inverse.valid = 1U;
}

/* ---- per-row conversion ---------------------------------------------- */

static int32_t clamp8(int32_t v)
{
	return v < 0 ? 0 : v > 255 ? 255 : v;
}

#define YUV_TO_RGB(Y, CB, CR, R, G, B) \
	do { \
		int32_t c_ = 298 * ((int32_t)(Y) - 16); \
		int32_t d_ = (int32_t)(CB) - 128; \
		int32_t e_ = (int32_t)(CR) - 128; \
		(R) = clamp8((c_ + 409 * e_ + 128) >> 8); \
		(G) = clamp8((c_ - 100 * d_ - 208 * e_ + 128) >> 8); \
		(B) = clamp8((c_ + 516 * d_ + 128) >> 8); \
	} while (0)

#define ROW_LOOP(STORE) \
	for (x = 0U; x < w; x++) { \
		uint32_t k_ = x & 3U; \
		int32_t r_, g_, b_; \
		YUV_TO_RGB(yl[x], cbl[x], crl[x], r_, g_, b_); \
		STORE; \
	}

enum row_kind {
	ROW_CUBE,
	ROW_GRAY,
	ROW_HOST,
	ROW_RGB,
};

static void convert_row(enum row_kind kind, int dither, uint32_t y,
	                    uint32_t w, const uint8_t *yl, const uint8_t *cbl,
	                    const uint8_t *crl, uint8_t *out)
{
	const uint16_t *tr[4];
	const uint16_t *tg[4];
	const uint16_t *tb[4];
	uint32_t k, x;

	for (k = 0U; k < 4U; k++) {
		uint32_t t = dither ? bayer4[y & 3U][k] : 16U;

		tr[k] = quant.table[0][t];
		tg[k] = quant.table[1][t];
		tb[k] = quant.table[2][t];
	}
	switch (kind) {
	case ROW_CUBE:
		ROW_LOOP(out[x] = (uint8_t)(tr[k_][r_] + tg[k_][g_] + tb[k_][b_]));
		break;
	case ROW_GRAY:
		ROW_LOOP(out[x] = (uint8_t)tg[k_][(77 * r_ + 150 * g_ + 29 * b_ + 128) >> 8]);
		break;
	case ROW_HOST:
		ROW_LOOP(out[x] = inverse.lut[tr[k_][r_] + tg[k_][g_] + tb[k_][b_]]);
		break;
	default:
		ROW_LOOP({
			uint32_t v_ = (uint32_t)(tr[k_][r_] + tg[k_][g_] + tb[k_][b_]);

			out[2U * x] = (uint8_t)(v_ >> 8);
			out[2U * x + 1U] = (uint8_t)v_;
		});
		break;
	}
}

/* Splits `w` indices (already padded with zeros to a multiple of 8) into
 * `depth` plane bytes per eight pixels, leftmost pixel in the top bit. */
static void pack_planes(const uint8_t *index, uint32_t w, uint32_t depth,
	                    uint8_t *const plane[8])
{
	uint32_t groups = (w + 7U) / 8U;
	uint32_t g, p;

	for (g = 0U; g < groups; g++) {
		const uint8_t *s = index + 8U * g;
		uint32_t x = ((uint32_t)s[0] << 24) | ((uint32_t)s[1] << 16) |
		             ((uint32_t)s[2] << 8) | s[3];
		uint32_t y = ((uint32_t)s[4] << 24) | ((uint32_t)s[5] << 16) |
		             ((uint32_t)s[6] << 8) | s[7];
		uint32_t t;

		/* 8x8 bit-matrix transpose (Hacker's Delight 7-3): afterwards
		 * byte 7 - p of the pair holds bit p of every index. */
		t = (x ^ (x >> 7)) & 0x00aa00aaU;
		x = x ^ t ^ (t << 7);
		t = (y ^ (y >> 7)) & 0x00aa00aaU;
		y = y ^ t ^ (t << 7);
		t = (x ^ (x >> 14)) & 0x0000ccccU;
		x = x ^ t ^ (t << 14);
		t = (y ^ (y >> 14)) & 0x0000ccccU;
		y = y ^ t ^ (t << 14);
		t = (x & 0xf0f0f0f0U) | ((y >> 4) & 0x0f0f0f0fU);
		y = ((x << 4) & 0xf0f0f0f0U) | (y & 0x0f0f0f0fU);
		x = t;
		for (p = 0U; p < depth; p++) {
			uint32_t b = 7U - p;
			uint32_t word = b < 4U ? x : y;

			plane[p][g] = (uint8_t)(word >> (8U * (3U - (b & 3U))));
		}
	}
}

uint16_t sdk_media_output_convert(const struct SDKMediaOutputJob *job,
	                              const struct SDKVideoDecodedFrame *frame,
	                              const uint8_t *palette)
{
	uint32_t sw, sh, dw, dh, cw, depth, y;
	uint8_t *dst;
	int nearest, dither, host, planar, interleaved;
	enum row_kind kind;

	if (!job || !frame || job->dst_addr == 0U)
		return SDK_STATUS_BAD_REQUEST;
	sw = frame->width;
	sh = frame->height;
	dw = job->width;
	dh = job->height;
	cw = (sw + 1U) / 2U;
	if (sw == 0U || sh == 0U || sw > SDK_MEDIA_OUTPUT_MAX_SOURCE_WIDTH ||
	    dw == 0U || dh == 0U || dw > SDK_MEDIA_OUTPUT_MAX_WIDTH ||
	    dh > SDK_MEDIA_OUTPUT_MAX_HEIGHT || !frame->y || !frame->cb ||
	    !frame->cr || frame->y_pitch < sw || frame->chroma_pitch < cw)
		return SDK_STATUS_BAD_REQUEST;

	nearest = (job->flags & SDK_MEDIA_OUTPUT_SCALE_NEAREST) != 0U;
	dither = (job->flags & SDK_MEDIA_OUTPUT_DITHER_ORDERED) != 0U;
	host = (job->flags & SDK_MEDIA_OUTPUT_PALETTE_HOST) != 0U;
	depth = job->depth;
	planar = job->format == SDK_SURFACE_FORMAT_PLANAR;
	interleaved = (job->flags & SDK_MEDIA_OUTPUT_PLANAR_INTERLEAVED) != 0U;
	if (job->format == SDK_SURFACE_FORMAT_INDEX8 || planar) {
		if (depth < 1U || depth > 8U || (host && !palette))
			return SDK_STATUS_BAD_REQUEST;
		kind = host ? ROW_HOST
		            : fixed_dims[depth].gray ? ROW_GRAY : ROW_CUBE;
	} else if (job->format == SDK_SURFACE_FORMAT_RGB565 ||
	           job->format == SDK_SURFACE_FORMAT_RGB555) {
		host = 0;
		depth = 0U;
		kind = ROW_RGB;
	} else {
		return SDK_STATUS_BAD_REQUEST;
	}
	dst = (uint8_t *)(uintptr_t)job->dst_addr;

	ensure_plan(sw, sh, dw, dh, nearest);
	build_quant(job->format, depth, dither, host);
	if (host)
		build_inverse(palette, depth);

	memset(&row.index[dw], 0, 8U);
	for (y = 0U; y < dh; y++) {
		const uint8_t *vy = vertical_row(frame->y, frame->y_pitch, sw, sh,
			dh, y, 0, nearest, row.vy);
		const uint8_t *vcb = vertical_row(frame->cb, frame->chroma_pitch,
			cw, sh, dh, y, 1, nearest, row.vcb);
		const uint8_t *vcr = vertical_row(frame->cr, frame->chroma_pitch,
			cw, sh, dh, y, 1, nearest, row.vcr);
		const uint8_t *yl = vy;
		uint8_t *out;

		if (!plan.luma.identity) {
			horizontal_row(&plan.luma, vy, row.hy, dw);
			yl = row.hy;
		}
		horizontal_row(&plan.chroma, vcb, row.hcb, dw);
		horizontal_row(&plan.chroma, vcr, row.hcr, dw);
		if (planar) {
			uint8_t *plane[8];
			uint32_t p;

			convert_row(kind, dither, y, dw, yl, row.hcb, row.hcr,
			            row.index);
			for (p = 0U; p < depth; p++)
				plane[p] = dst + ((size_t)(interleaved
					? (size_t)y * depth + p
					: (size_t)p * job->height + y) * job->pitch);
			pack_planes(row.index, dw, depth, plane);
		} else {
			out = dst + (size_t)y * job->pitch;
			convert_row(kind, dither, y, dw, yl, row.hcb, row.hcr, out);
		}
	}
	return SDK_STATUS_OK;
}
