/* Frame encoder for the remote framebuffer.  Wire format is in the header.
 *
 * A changed tile is read from the source EXACTLY ONCE, into rawbuf, and that
 * copy feeds the XOR, the shadow write and the wire.  That is what lets the
 * caller point this at live bitplanes with no drawing lock held.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <rfb/rfb_encode.h>

/* rfb_u32 must be exactly 32 bits: the word loop below and the wire format
 * both depend on it. */
typedef char rfb_u32_is_four_bytes[(sizeof(rfb_u32) == 4) ? 1 : -1];

#define RFB_PB_FAIL     ((rfb_u32)0xFFFFFFFFu)
#define RFB_PB_BOUND(n) ((n) + ((n) + 127u) / 128u + 1u)

#define RFB_PROBE_MAX_SAMPLES 256
#define RFB_PROBE_MAX_BLK     32

/* Lower bound on a probe column, or a narrow row is divided into
 * RFB_PROBE_MAX_BLK columns of a few bytes each. */
#define RFB_PROBE_MIN_BLK     16

/* ------------------------------------------------------------- PackBits --- */
/*
 * PackBits over UNITS of one or two bytes.  The unit is what a run repeats and
 * what a count counts.  On RFB_FMT_RGB565 a pixel is two bytes, and a byte-wise
 * run exists only where a pixel's two bytes happen to be equal: packed by the
 * byte, every RGB565 capture in the corpus came out RAW (rfbbench ratio 0.985,
 * c_pbraw=0) and a 1280x720 screen on a real A3000 cost 13 s of PackBits to
 * be sent raw anyway.  Packed by the pixel a flat area is a run.  The far end
 * picks the unit from the format, so nothing on the wire names it.
 *
 * Both packers give up with RFB_PB_FAIL the moment the output would pass `cap`
 * -- and ALSO when the first quarter of the input held no run at all.  A tile
 * that starts as noise is noise (a photograph, a dither), and the literal scan
 * costs a 25 MHz 68030 more than sending the tile raw: without the bail a
 * screen of noise costs two full passes a plane on its way to going out RAW.
 * The bail is a guess about the rest of the tile, and a wrong guess costs
 * compression on that tile, never correctness.  Inputs under RFB_PB_BAIL_MIN
 * are too short to guess about.
 *
 * The loops are written for what gcc -Os makes of them on the 68030: pointers
 * and precomputed ends, no per-byte bounds arithmetic.  The three-run literal
 * scan looks at bytes q[1] and q[2] first, because when those differ neither
 * q nor q+1 can start a run and the scan moves by two.
 */

#define RFB_PB_BAIL_MIN 32u

/* Where a literal that started at p has to stop: at the run that begins at
 * the answer, at p + 128, or at end.  The unit at p itself cannot start a
 * run; the caller's run scan said so. */
static const rfb_u8 *rfb_pb8_literal_end(const rfb_u8 *p, const rfb_u8 *end,
                                         int min_run)
{
    const rfb_u8 *lim = (end - p > 128) ? p + 128 : end;
    const rfb_u8 *q = p + 1;

    if (min_run == 3) {
        /* The last unit that can start a three-run is end - 3, so the scan
         * stops short of end - 2; below that q[2] is always in bounds. */
        const rfb_u8 *se = (end - p >= 2) ? end - 2 : p;
        if (se > lim)
            se = lim;
        while (q < se) {
            if (q[1] != q[2])
                q += 2;
            else if (q[0] == q[1])
                return q;
            else
                q++;
        }
        return lim;
    }

    {
        const rfb_u8 *se = (end - p >= 1) ? end - 1 : p;
        if (se > lim)
            se = lim;
        while (q < se) {
            if (q[0] == q[1])
                return q;
            q++;
        }
        return lim;
    }
}

rfb_u32 rfb_packbits(const rfb_u8 *in, rfb_u32 n, rfb_u8 *out, rfb_u32 cap,
                     int min_run)
{
    const rfb_u8 *p = in;
    const rfb_u8 *const end = in + n;
    const rfb_u8 *const bail = (n >= RFB_PB_BAIL_MIN) ? in + (n >> 2) : end;
    rfb_u8 *o = out;
    rfb_u8 *const ocap = out + cap;
    int seen_run = 0;

    if (min_run < 2)
        min_run = 2;

    while (p < end) {
        const rfb_u8 b = *p;
        const rfb_u8 *const rend = (end - p > 128) ? p + 128 : end;
        const rfb_u8 *q = p + 1;

        while (q < rend && *q == b)
            q++;

        if (q - p >= min_run) {
            if (o + 2 > ocap)
                return RFB_PB_FAIL;
            *o++ = (rfb_u8)(257u - (rfb_u32)(q - p));
            *o++ = b;
            p = q;
            seen_run = 1;
            continue;
        }

        q = rfb_pb8_literal_end(p, end, min_run);
        if (!seen_run && q >= bail)
            return RFB_PB_FAIL;
        if (o + 1 + (q - p) > ocap)
            return RFB_PB_FAIL;
        *o++ = (rfb_u8)((q - p) - 1);
        memcpy(o, p, (size_t)(q - p));
        o += q - p;
        p = q;
    }
    return (rfb_u32)(o - out);
}

long rfb_unpackbits(const rfb_u8 *in, rfb_u32 n, rfb_u8 *out, rfb_u32 out_n)
{
    rfb_u32 i = 0, o = 0;

    while (i < n) {
        rfb_u8 c = in[i++];
        if (c < 128u) {
            rfb_u32 lit = (rfb_u32)c + 1u;
            if (i + lit > n || o + lit > out_n)
                return RFB_E_OVERFLOW;
            memcpy(out + o, in + i, (size_t)lit);
            i += lit;
            o += lit;
        } else if (c > 128u) {
            rfb_u32 run = 257u - (rfb_u32)c;
            if (i >= n || o + run > out_n)
                return RFB_E_OVERFLOW;
            memset(out + o, in[i++], (size_t)run);
            o += run;
        }
    }
    return (long)o;
}

/* The two-byte-unit pair.  `in` is read as sixteen-bit words, so it has to be
 * even (the encoder's buffers are) and n is in UNITS; the wire bytes of a unit
 * are its two bytes in memory order, which is what makes this the same stream
 * on either endianness.  cap and the answer are bytes, as for the byte pair. */
static const rfb_u16 *rfb_pb16_literal_end(const rfb_u16 *p,
                                           const rfb_u16 *end, int min_run)
{
    const rfb_u16 *lim = (end - p > 128) ? p + 128 : end;
    const rfb_u16 *q = p + 1;

    if (min_run == 3) {
        const rfb_u16 *se = (end - p >= 2) ? end - 2 : p;
        if (se > lim)
            se = lim;
        while (q < se) {
            if (q[1] != q[2])
                q += 2;
            else if (q[0] == q[1])
                return q;
            else
                q++;
        }
        return lim;
    }

    {
        const rfb_u16 *se = (end - p >= 1) ? end - 1 : p;
        if (se > lim)
            se = lim;
        while (q < se) {
            if (q[0] == q[1])
                return q;
            q++;
        }
        return lim;
    }
}

rfb_u32 rfb_packbits16(const rfb_u16 *in, rfb_u32 n, rfb_u8 *out, rfb_u32 cap,
                       int min_run)
{
    const rfb_u16 *p = in;
    const rfb_u16 *const end = in + n;
    const rfb_u16 *const bail = (n >= RFB_PB_BAIL_MIN) ? in + (n >> 2) : end;
    rfb_u8 *o = out;
    rfb_u8 *const ocap = out + cap;
    int seen_run = 0;

    if (min_run < 2)
        min_run = 2;

    while (p < end) {
        const rfb_u16 b = *p;
        const rfb_u16 *const rend = (end - p > 128) ? p + 128 : end;
        const rfb_u16 *q = p + 1;

        while (q < rend && *q == b)
            q++;

        if (q - p >= min_run) {
            if (o + 3 > ocap)
                return RFB_PB_FAIL;
            *o++ = (rfb_u8)(257u - (rfb_u32)(q - p));
            memcpy(o, p, 2);
            o += 2;
            p = q;
            seen_run = 1;
            continue;
        }

        q = rfb_pb16_literal_end(p, end, min_run);
        if (!seen_run && q >= bail)
            return RFB_PB_FAIL;
        if (o + 1 + 2 * (q - p) > ocap)
            return RFB_PB_FAIL;
        *o++ = (rfb_u8)((q - p) - 1);
        memcpy(o, p, (size_t)(2 * (q - p)));
        o += 2 * (q - p);
        p = q;
    }
    return (rfb_u32)(o - out);
}

long rfb_unpackbits16(const rfb_u8 *in, rfb_u32 n, rfb_u8 *out, rfb_u32 out_n)
{
    rfb_u32 i = 0, o = 0;

    while (i < n) {
        rfb_u8 c = in[i++];
        if (c < 128u) {
            rfb_u32 lit = 2u * ((rfb_u32)c + 1u);
            if (i + lit > n || o + lit > out_n)
                return RFB_E_OVERFLOW;
            memcpy(out + o, in + i, (size_t)lit);
            i += lit;
            o += lit;
        } else if (c > 128u) {
            rfb_u32 run = 257u - (rfb_u32)c;
            rfb_u8 hi, lo;
            if (i + 2u > n || o + 2u * run > out_n)
                return RFB_E_OVERFLOW;
            hi = in[i++];
            lo = in[i++];
            while (run--) {
                out[o++] = hi;
                out[o++] = lo;
            }
        }
    }
    return (long)o;
}

/* ----------------------------------------------------------- geometry ---- */

static int rfb_geom_ok(const rfb_geom *g)
{
    if (!g)
        return 0;
    if (g->height == 0 || g->bytes_per_row == 0)
        return 0;
    if (g->tile_w < 1 || g->tile_w > RFB_MAX_TILE_W)
        return 0;
    if (g->tile_h < 1 || g->tile_h > RFB_MAX_TILE_H)
        return 0;

    /* Depth means a different thing per format, so each is checked
     * separately and a wrong one is refused at init. */
    switch (g->format) {
    case RFB_FMT_PLANAR:
        /* Planes, one bit each. */
        if (g->depth < 1 || g->depth > RFB_MAX_DEPTH)
            return 0;
        break;
    case RFB_FMT_CLUT8:
        if (g->depth != 8)
            return 0;
        break;
    case RFB_FMT_RGB565:
        if (g->depth != RFB_RGB565_DEPTH)
            return 0;
        /* Two bytes a pixel, so an odd row would put the second byte of a
         * pixel in the next row.  The caller rounds bytes_per_row up and this
         * is what says so.  An odd tile would split a pixel between two
         * tiles, and PackBits on this format counts pixels. */
        if ((g->bytes_per_row & 1u) != 0u || (g->tile_w & 1u) != 0u)
            return 0;
        break;
    case RFB_FMT_HAM6:
    case RFB_FMT_EHB:
        /* HAM6 and EHB are six planes exactly; at any other depth the flag
         * is a lie and the control bits would be drawn as picture. */
        if (g->depth != 6)
            return 0;
        break;
    case RFB_FMT_HAM8:
        if (g->depth != 8)
            return 0;
        break;
    default:
        return 0;
    }
    return 1;
}

rfb_u8 rfb_planes(const rfb_geom *g)
{
    if (!g)
        return 0;
    return (rfb_u8)(RFB_FMT_IS_CHUNKY(g->format) ? 1u : (rfb_u32)g->depth);
}

rfb_u32 rfb_pal_colours(const rfb_geom *g)
{
    if (!g)
        return 0;

    switch (g->format) {
    case RFB_FMT_PLANAR:
        return 1u << g->depth;
    case RFB_FMT_CLUT8:
        return 256u;
    case RFB_FMT_RGB565:
        /* The colour is in the pixel.  No `pal` word is sent. */
        return 0u;
    case RFB_FMT_HAM6:
        return 16u;
    case RFB_FMT_HAM8:
        return 64u;
    case RFB_FMT_EHB:
        /* EHB: the hardware holds 32 registers and the receiver derives
         * 32..63 as 0..31 at half brightness. */
        return 32u;
    default:
        return 0u;
    }
}

rfb_u32 rfb_shadow_size(const rfb_geom *g)
{
    if (!rfb_geom_ok(g))
        return 0;
    return (rfb_u32)g->bytes_per_row * g->height * rfb_planes(g);
}

static rfb_u32 rfb_probe_step(const rfb_geom *g, const rfb_scroll_cfg *cfg)
{
    rfb_u32 step = cfg->probe_step ? cfg->probe_step : 1u;
    while ((rfb_u32)g->height / step > RFB_PROBE_MAX_SAMPLES)
        step++;
    return step;
}

rfb_u32 rfb_scratch_size(const rfb_geom *g, rfb_u32 flags,
                         const rfb_scroll_cfg *cfg)
{
    rfb_scroll_cfg def;
    rfb_u32 tb, need;

    if (!rfb_geom_ok(g))
        return 0;
    if (!cfg) {
        rfb_scroll_defaults(&def);
        cfg = &def;
    }
    tb = (rfb_u32)g->tile_w * g->tile_h;
    need = tb * rfb_planes(g)          /* xorbuf, one tile per plane */
         + tb * rfb_planes(g)          /* rawbuf, likewise */
         + 2u * RFB_PB_BOUND(tb)       /* two candidate PackBits outputs */
         + 4u * 4u;                    /* alignment slack */

    if (flags & RFB_F_COPYRECT) {
        rfb_u32 step = rfb_probe_step(g, cfg);
        rfb_u32 samples = (rfb_u32)g->height / step + 1u;
        rfb_u32 ncand = cfg->n_cand ? cfg->n_cand : 1u;
        need += g->bytes_per_row + samples * (ncand + 1u) * 4u + 8u;
    }
    return need;
}

rfb_u32 rfb_worst_case_frame(const rfb_geom *g)
{
    rfb_u32 tx, ty, tiles, tb, head;

    if (!rfb_geom_ok(g))
        return 0;
    tx = ((rfb_u32)g->bytes_per_row + g->tile_w - 1u) / g->tile_w;
    ty = ((rfb_u32)g->height + g->tile_h - 1u) / g->tile_h;
    tiles = tx * ty;
    tb = (rfb_u32)g->tile_w * g->tile_h;

    head = RFB_FMT_IS_CHUNKY(g->format) ? 3u : 4u;

    return 4u + 11u
         + tiles * (head + (rfb_u32)rfb_planes(g) * (3u + RFB_PB_BOUND(tb)))
         + 1u;
}

void rfb_scroll_defaults(rfb_scroll_cfg *cfg)
{
    static const rfb_s16 def[] = { 8, 16, 24, 32, 4, 2, 1, -8, -16 };
    unsigned i;

    if (!cfg)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->probe_step = 4;
    cfg->match_pct  = 75;
    cfg->min_band   = 32;
    cfg->busy_tiles = 8;
    cfg->max_backoff = 16;
    cfg->n_cand     = (rfb_u8)(sizeof(def) / sizeof(def[0]));
    for (i = 0; i < sizeof(def) / sizeof(def[0]); i++)
        cfg->cand[i] = def[i];
}

/* ---------------------------------------------------------------- init --- */

static rfb_u8 *rfb_align4(rfb_u8 *p)
{
    while (((unsigned long)p & 3ul) != 0ul)
        p++;
    return p;
}

long rfb_encoder_init(rfb_encoder *e, const rfb_geom *g, rfb_u32 flags,
                      const rfb_scroll_cfg *cfg,
                      rfb_u8 *shadow, rfb_u32 shadow_len,
                      rfb_u8 *scratch, rfb_u32 scratch_len)
{
    rfb_u32 tb;
    rfb_u8 *p;

    if (!e || !rfb_geom_ok(g) || !shadow || !scratch)
        return RFB_E_GEOM;

    /* BMF_INTERLEAVED is meaningless for a one-plane chunky source and must
     * be refused: it would compute a row stride of bytes_per_row * depth. */
    if (RFB_FMT_IS_CHUNKY(g->format) && (flags & RFB_F_INTERLEAVED))
        return RFB_E_GEOM;

    memset(e, 0, sizeof(*e));
    e->g = *g;
    e->flags = flags;
    e->nplanes = rfb_planes(g);
    if (cfg)
        e->scroll = *cfg;
    else
        rfb_scroll_defaults(&e->scroll);
    if (e->scroll.n_cand > RFB_MAX_CAND)
        e->scroll.n_cand = RFB_MAX_CAND;

    if (shadow_len < rfb_shadow_size(g))
        return RFB_E_BUFFER;
    if (scratch_len < rfb_scratch_size(g, flags, &e->scroll))
        return RFB_E_BUFFER;

    e->tiles_x = (rfb_u16)(((rfb_u32)g->bytes_per_row + g->tile_w - 1u) / g->tile_w);
    e->tiles_y = (rfb_u16)(((rfb_u32)g->height + g->tile_h - 1u) / g->tile_h);
    e->plane_bytes = (rfb_u32)g->bytes_per_row * g->height;
    e->frame_bytes = e->plane_bytes * e->nplanes;
    if (flags & RFB_F_INTERLEAVED) {
        e->row_stride = (rfb_u32)g->bytes_per_row * g->depth;
        e->plane_stride = g->bytes_per_row;
    } else {
        e->row_stride = g->bytes_per_row;
        e->plane_stride = e->plane_bytes;
    }
    e->shadow = shadow;
    e->scratch = scratch;
    e->scratch_len = scratch_len;
    /* memset() left this at "one frame ago"; nothing has scrolled yet. */
    e->since_copy = 255u;

    tb = (rfb_u32)g->tile_w * g->tile_h;
    p = rfb_align4(scratch);
    e->xorbuf = p; p = rfb_align4(p + tb * e->nplanes);
    e->rawbuf = p; p = rfb_align4(p + tb * e->nplanes);
    e->pb_a   = p; p = rfb_align4(p + RFB_PB_BOUND(tb));
    e->pb_b   = p; p = rfb_align4(p + RFB_PB_BOUND(tb));

    if (flags & RFB_F_COPYRECT) {
        e->probe_step = (rfb_u16)rfb_probe_step(g, &e->scroll);
        e->n_samples  = (rfb_u16)((rfb_u32)g->height / e->probe_step + 1u);
        /* Rounded up to a multiple of four so rfb_blk_match() can compare
         * whole longwords. */
        e->blk_bytes  = (rfb_u16)((((rfb_u32)g->bytes_per_row
                                    + RFB_PROBE_MAX_BLK - 1u)
                                   / RFB_PROBE_MAX_BLK + 3u) & ~3u);
        if (e->blk_bytes < RFB_PROBE_MIN_BLK)
            e->blk_bytes = RFB_PROBE_MIN_BLK;
        e->n_blk = (rfb_u16)(((rfb_u32)g->bytes_per_row + e->blk_bytes - 1u)
                             / e->blk_bytes);
        e->probe_row = p; p = rfb_align4(p + g->bytes_per_row);
        e->probe_mask = (rfb_u32 *)(void *)p;
    }
    return 0;
}

/* ------------------------------------------------------------- output ---- */

typedef struct {
    rfb_u8 *buf;
    rfb_u32 cap;
    rfb_u32 len;
    int     over;
} rfb_out;

static void rfb_put8(rfb_out *o, rfb_u8 v)
{
    if (o->len + 1u > o->cap) { o->over = 1; return; }
    o->buf[o->len++] = v;
}

static void rfb_put16(rfb_out *o, rfb_u32 v)
{
    if (o->len + 2u > o->cap) { o->over = 1; return; }
    o->buf[o->len++] = (rfb_u8)(v >> 8);
    o->buf[o->len++] = (rfb_u8)(v & 0xFFu);
}

static void rfb_putblk(rfb_out *o, const rfb_u8 *src, rfb_u32 n)
{
    if (o->len + n > o->cap) { o->over = 1; return; }
    memcpy(o->buf + o->len, src, (size_t)n);
    o->len += n;
}

/* ------------------------------------------------------- the tile pass --- */

/* Does this tile-plane differ from the shadow.  Writes nothing; stops at the
 * first word that disagrees.  `word` is the caller's frame-constant alignment
 * verdict (rfb_words_ok()) and must not be rederived here. */
static int rfb_cmp_plane(const rfb_u8 *src, const rfb_u8 *sh,
                         rfb_u32 bpr, rfb_u32 tw, rfb_u32 th, int word)
{
    rfb_u32 r = th;

    if (word) {
        const rfb_u32 words = tw >> 2;
        const rfb_u32 tail = tw & 3u;

        /* Walk rows by adding the stride: `src + r * bpr` is a __mulsi3 call
         * on the -m68000 build. */
        do {
            rfb_u32 n = words;
            const rfb_u32 *sw = (const rfb_u32 *)(const void *)src;
            const rfb_u32 *dw = (const rfb_u32 *)(const void *)sh;

            /* One longword at a time is the fast form here.  Do not unroll
             * this without a measurement: unrolling has lost twice. */
            while (n--)
                if (*sw++ != *dw++)
                    return 1;

            if (tail) {
                const rfb_u8 *s = (const rfb_u8 *)(const void *)sw;
                const rfb_u8 *d = (const rfb_u8 *)(const void *)dw;
                rfb_u32 c = tail;
                while (c--)
                    if (*s++ != *d++)
                        return 1;
            }
            src += bpr; sh += bpr;
        } while (--r);
        return 0;
    }

    do {
        rfb_u32 c = tw;
        const rfb_u8 *s = src, *d = sh;
        while (c--)
            if (*s++ != *d++)
                return 1;
        src += bpr; sh += bpr;
    } while (--r);
    return 0;
}

static int rfb_run_differs(const rfb_u8 *src, const rfb_u8 *sh, rfb_u32 n,
                           int word)
{
    if (word) {
        const rfb_u32 *sw = (const rfb_u32 *)(const void *)src;
        const rfb_u32 *dw = (const rfb_u32 *)(const void *)sh;
        rfb_u32 w = n >> 2;
        rfb_u32 tail = n & 3u;

        while (w--)
            if (*sw++ != *dw++)
                return 1;

        src = (const rfb_u8 *)(const void *)sw;
        sh  = (const rfb_u8 *)(const void *)dw;
        n = tail;
    }

    while (n--)
        if (*src++ != *sh++)
            return 1;

    return 0;
}

/*
 * Is any byte of this band different from the shadow.  Must read EXACTLY the
 * bytes the tile walk would read, so that a band called clean here is one the
 * walk would also have found clean.
 */
static int rfb_band_clean(const rfb_encoder *e, const rfb_u8 *const *planes,
                          rfb_u32 ty0, rfb_u32 ty1, int word)
{
    rfb_u32 y0 = ty0 * e->g.tile_h;
    rfb_u32 y1 = ty1 * e->g.tile_h;
    rfb_u32 bpr = e->g.bytes_per_row;
    rfb_u32 rows, p;

    if (y1 > e->g.height)
        y1 = e->g.height;
    if (y1 <= y0)
        return 1;
    rows = y1 - y0;

    for (p = 0; p < e->nplanes; p++) {
        const rfb_u8 *src = planes[p] + y0 * e->row_stride;
        const rfb_u8 *sh  = e->shadow + p * e->plane_stride
                                      + y0 * e->row_stride;

        /* Contiguous rows collapse to one run.  A source with padding between
         * rows must be walked a row at a time: the padding is bytes the tile
         * walk never looks at and must not decide this. */
        if (e->row_stride == bpr) {
            if (rfb_run_differs(src, sh, rows * bpr, word))
                return 0;
        } else {
            rfb_u32 r = rows;
            do {
                if (rfb_run_differs(src, sh, bpr, word))
                    return 0;
                src += e->row_stride;
                sh  += e->row_stride;
            } while (--r);
        }
    }

    return 1;
}

/* Take the tile-plane.  ONE read of the source into raw; the XOR, the shadow
 * write and the wire bytes all come from that single copy, so a screen drawn
 * on underneath cannot desynchronise the shadow from what was sent. */
/* Answers whether the shadow it replaced was all zero, which with keep_xor
 * means the XOR plane IS the raw plane: a tile's first appearance, every
 * tile of a session's first frame.  The caller then codes it once, not
 * twice. */
static int rfb_take_plane(const rfb_u8 *src, rfb_u8 *sh, rfb_u8 *raw,
                          rfb_u8 *xb, rfb_u32 bpr, rfb_u32 tw, rfb_u32 th,
                          int keep_xor, int word)
{
    rfb_u32 r = th;
    rfb_u32 was = 0;

    if (word) {
        const rfb_u32 words = tw >> 2;
        const rfb_u32 tail = tw & 3u;

        do {
            const rfb_u32 *sw = (const rfb_u32 *)(const void *)src;
            rfb_u32 *dw = (rfb_u32 *)(void *)sh;
            rfb_u32 *ww = (rfb_u32 *)(void *)raw;
            rfb_u32 n = words;

            if (keep_xor) {
                rfb_u32 *xw = (rfb_u32 *)(void *)xb;
                while (n--) {
                    rfb_u32 sv = *sw++;
                    rfb_u32 dv = *dw;
                    was |= dv;
                    *xw++ = sv ^ dv;
                    *dw++ = sv;
                    *ww++ = sv;
                }
                xb = (rfb_u8 *)(void *)xw;
            } else {
                while (n--) {
                    rfb_u32 sv = *sw++;
                    *dw++ = sv;
                    *ww++ = sv;
                }
            }
            raw = (rfb_u8 *)(void *)ww;

            if (tail) {
                const rfb_u8 *s = (const rfb_u8 *)(const void *)sw;
                rfb_u8 *d = (rfb_u8 *)(void *)dw;
                rfb_u32 c = tail;
                while (c--) {
                    rfb_u8 sv = *s++;
                    if (keep_xor) {
                        was |= *d;
                        *xb++ = (rfb_u8)(sv ^ *d);
                    }
                    *d++ = sv;
                    *raw++ = sv;
                }
            }
            src += bpr; sh += bpr;
        } while (--r);
        return keep_xor && was == 0;
    }

    do {
        const rfb_u8 *s = src;
        rfb_u8 *d = sh;
        rfb_u32 c = tw;
        while (c--) {
            rfb_u8 sv = *s++;
            if (keep_xor) {
                was |= *d;
                *xb++ = (rfb_u8)(sv ^ *d);
            }
            *d++ = sv;
            *raw++ = sv;
        }
        src += bpr; sh += bpr;
    } while (--r);
    return keep_xor && was == 0;
}

/* PackBits in the format's unit.  raw_len is bytes; on RGB565 it is even
 * (tile_w is) and the buffers are four-aligned, which is what the sixteen-bit
 * reads need. */
static rfb_u32 rfb_pb(int unit16, const rfb_u8 *in, rfb_u32 raw_len,
                      rfb_u8 *out, rfb_u32 cap, int min_run)
{
    if (unit16)
        return rfb_packbits16((const rfb_u16 *)(const void *)in,
                              raw_len >> 1, out, cap, min_run);
    return rfb_packbits(in, raw_len, out, cap, min_run);
}

/* A plane that did not change but must be sent anyway (no RFB_F_PLANEMASK).
 * The shadow already holds the bytes, and the XOR against them is zero. */
static void rfb_take_clean(const rfb_u8 *sh, rfb_u8 *raw, rfb_u8 *xb,
                           rfb_u32 bpr, rfb_u32 tw, rfb_u32 th, int keep_xor)
{
    rfb_u32 r = th;

    do {
        memcpy(raw, sh, (size_t)tw);
        raw += tw;
        if (keep_xor) {
            memset(xb, 0, (size_t)tw);
            xb += tw;
        }
        sh += bpr;
    } while (--r);
}

/* Can the whole frame use the longword path: true when the plane bases, the
 * row stride and the tile width are all multiples of four. */
static int rfb_words_ok(const rfb_encoder *e, const rfb_u8 *const *planes)
{
    unsigned long bits = (unsigned long)e->row_stride
                       | (unsigned long)e->g.tile_w
                       | (unsigned long)e->plane_stride
                       | (unsigned long)e->shadow
                       | (unsigned long)e->xorbuf
                       | (unsigned long)e->rawbuf;
    rfb_u32 p;

    for (p = 0; p < e->nplanes; p++)
        bits |= (unsigned long)planes[p];

    return ((bits & 3ul) == 0ul);
}

/* --------------------------------------------------------- scroll probe --- */

typedef struct {
    rfb_u16 x0, w, y0, h;
    rfb_s16 dy;
} rfb_copy;

/* Which column blocks of a row are byte-identical.  Bit b set = block b
 * matched.  Runs n_samples * (n_cand + 1) times a frame, so keep every
 * constant hoisted out of the loop. */
static rfb_u32 rfb_blk_match(const rfb_u8 *a, const rfb_u8 *b, rfb_u32 bpr,
                             rfb_u32 blk, rfb_u32 nblk)
{
    const rfb_u32 words = blk >> 2;
    const int aligned = ((((unsigned long)a) | ((unsigned long)b) | blk)
                         & 3ul) == 0ul;
    rfb_u32 mask = 0, bit = 1u, whole, left = bpr;

    /* Blocks that are wholly inside the row, and the short one at the end. */
    whole = (nblk && bpr / blk >= nblk) ? nblk : bpr / blk;

    if (aligned) {
        const rfb_u32 *pa = (const rfb_u32 *)(const void *)a;
        const rfb_u32 *pb = (const rfb_u32 *)(const void *)b;
        rfb_u32 i = whole;

        while (i--) {
            rfb_u32 w = words;
            rfb_u32 acc = 0;

            while (w--)
                acc |= *pa++ ^ *pb++;
            if (!acc)
                mask |= bit;
            bit <<= 1;
        }
        a = (const rfb_u8 *)(const void *)pa;
        b = (const rfb_u8 *)(const void *)pb;
        left = bpr - whole * blk;
    } else {
        rfb_u32 i = whole;

        while (i--) {
            rfb_u32 n = blk;
            rfb_u32 acc = 0;

            while (n--)
                acc |= (rfb_u32)(*a++ ^ *b++);
            if (!acc)
                mask |= bit;
            bit <<= 1;
        }
        left = bpr - whole * blk;
    }

    if (whole < nblk && left) {
        rfb_u32 acc = 0;

        while (left--)
            acc |= (rfb_u32)(*a++ ^ *b++);
        if (!acc)
            mask |= bit;
    }
    return mask;
}

static rfb_u32 rfb_probe_pass(rfb_encoder *e, const rfb_u8 *src,
                              const rfb_s16 *cand, rfb_u32 ncand,
                              rfb_u32 *chg, rfb_u32 *out_nsamp)
{
    const rfb_u32 bpr = e->g.bytes_per_row;
    const rfb_u32 stride = e->row_stride;
    const rfb_u32 h = e->g.height;
    const rfb_u32 nsl = e->n_samples;
    const rfb_u32 blk = e->blk_bytes;
    const rfb_u32 nblk = e->n_blk;
    const rfb_u32 keep = (nblk >= 32u) ? 0xFFFFFFFFu : ((1u << nblk) - 1u);
    const rfb_u8 *srow = src;
    const rfb_u8 *shrow = e->shadow;
    const rfb_u32 sstep = (rfb_u32)e->probe_step * stride;
    rfb_u32 y, c, nsamp = 0, dirty = 0;

    for (y = 0; y < h && nsamp < nsl; y += e->probe_step, nsamp++) {
        rfb_u32 *pm = e->probe_mask + nsamp;
        rfb_u32 cm;

        memcpy(e->probe_row, srow, (size_t)bpr);
        e->st.probe_bytes += bpr;

        cm = ~rfb_blk_match(e->probe_row, shrow, bpr, blk, nblk);
        cm &= keep;
        chg[nsamp] = cm;
        if (cm)
            dirty++;

        for (c = 0; c < ncand; c++, pm += nsl) {
            rfb_s32 sy = (rfb_s32)y + cand[c];
            rfb_u32 mask = 0;
            if (sy >= 0 && (rfb_u32)sy < h)
                mask = rfb_blk_match(e->probe_row,
                                     e->shadow + (rfb_u32)sy * stride, bpr,
                                     blk, nblk);
            *pm = mask;
        }
        srow += sstep;
        shrow += sstep;
    }
    e->st.probes++;
    *out_nsamp = nsamp;
    return dirty;
}

/* Enough moved last frame for something to be scrolling.  Under it nothing can
 * be, and the backoff is not spent on a pass that was never going to look. */
static int rfb_scroll_maybe(const rfb_encoder *e)
{
    return (e->last_dirty >= e->scroll.busy_tiles) || e->last_copy;
}

/* Whether the next band 0 will look for a scroll.  Exported because the probe
 * samples rows the whole height of the screen, so a caller that reads its
 * pixels off a graphics card has to know whether this pass needs the frame in
 * one moment or only the band it is about to encode.  It answers the state as
 * it stands and changes none of it: the backoff is spent by the encode. */
int rfb_scroll_probe_due(const rfb_encoder *e)
{
    if (!e || !(e->flags & RFB_F_COPYRECT))
        return 0;
    if (!(e->flags & RFB_F_SCROLL_ADAPTIVE))
        return 1;
    if (!rfb_scroll_maybe(e))
        return 0;

    return e->backoff_left ? 0 : 1;
}

static int rfb_detect_scroll(rfb_encoder *e, const rfb_u8 *src, rfb_copy *cp)
{
    const rfb_u32 bpr = e->g.bytes_per_row;
    const rfb_u32 h = e->g.height;
    const rfb_u32 nblk = e->n_blk;
    rfb_u32 *chg = e->probe_mask + (rfb_u32)e->scroll.n_cand * e->n_samples;
    rfb_s16 sticky[1];
    const rfb_s16 *cand = e->scroll.cand;
    rfb_u32 ncand = e->scroll.n_cand;
    rfb_u32 s, c, b;
    rfb_u32 dirtyblk[RFB_PROBE_MAX_BLK], scrollblk[RFB_PROBE_MAX_BLK];
    rfb_u32 nsamp = 0, dirty, changed = 0;
    rfb_u32 best = 0, best_score = 0;
    rfb_u32 lo, hi, run, run_start, best_run, best_start;
    const rfb_u32 *bestmask;
    rfb_u32 sel;
    int retried = 0;

    if (ncand == 0 || nblk == 0)
        return 0;

    /* Try last frame's winning offset alone first; fall back to the full
     * candidate list only when it fails to explain what moved. */
    if (e->last_copy && e->last_dy != 0) {
        sticky[0] = e->last_dy;
        cand = sticky;
        ncand = 1;
    }

again:
    dirty = rfb_probe_pass(e, src, cand, ncand, chg, &nsamp);

    if (nsamp < 4 || dirty * 4u < nsamp)   /* too little moved to be a scroll */
        return 0;

    changed = 0;
    for (s = 0; s < nsamp; s++) {
        rfb_u32 m = chg[s];
        while (m) { changed++; m &= m - 1u; }
    }

    /* Score a candidate on blocks that changed and still matched at the
     * offset.  A score on matches alone elects any offset under which a static
     * background agrees with itself, and that is most of the screen. */
    best = 0; best_score = 0;
    {
        const rfb_u32 *pm = e->probe_mask;
        for (c = 0; c < ncand; c++, pm += e->n_samples) {
            rfb_u32 score = 0;
            for (s = 0; s < nsamp; s++) {
                rfb_u32 m = pm[s] & chg[s];
                while (m) { score++; m &= m - 1u; }
            }
            if (score > best_score) { best_score = score; best = c; }
        }
    }

    if (best_score * 100u < changed * e->scroll.match_pct && !retried &&
        cand != e->scroll.cand) {
        /* The shortcut did not explain the frame.  Use the full list. */
        cand = e->scroll.cand;
        ncand = e->scroll.n_cand;
        retried = 1;
        goto again;
    }
    if (best_score == 0)
        return 0;

    /* A column joins the copy only when it changes often AND the offset
     * explains the change; static columns must be left out. */
    bestmask = e->probe_mask + best * e->n_samples;
    lo = nblk; hi = 0;
    for (b = 0; b < nblk; b++) {
        rfb_u32 bitb = 1u << b;
        dirtyblk[b] = 0; scrollblk[b] = 0;
        for (s = 0; s < nsamp; s++) {
            if (chg[s] & bitb) {
                dirtyblk[b]++;
                if (bestmask[s] & bitb)
                    scrollblk[b]++;
            }
        }
        if (dirtyblk[b] * 8u >= nsamp &&
            scrollblk[b] * 100u >= dirtyblk[b] * e->scroll.match_pct) {
            if (b < lo) lo = b;
            hi = b;
        }
    }
    if (lo > hi)
        return 0;

    sel = 0;
    for (b = lo; b <= hi; b++)
        sel |= 1u << b;

    /* Longest run of consecutive samples where the span mostly matched.  An
     * imprecise band is safe: the copy is only a prediction. */
    {
        rfb_u32 nsel = hi - lo + 1u;
        rfb_u32 hit_need = (nsel * e->scroll.match_pct + 99u) / 100u;
        if (hit_need == 0)
            hit_need = 1;
        run = 0; run_start = 0; best_run = 0; best_start = 0;
        for (s = 0; s < nsamp; s++) {
            rfb_u32 m = bestmask[s] & sel;
            rfb_u32 hits = 0;
            while (m) { hits++; m &= m - 1u; }
            if (hits >= hit_need) {
                if (run == 0)
                    run_start = s;
                run++;
                if (run > best_run) { best_run = run; best_start = run_start; }
            } else {
                run = 0;
            }
        }
    }
    if (best_run < 2)
        return 0;

    cp->x0 = (rfb_u16)(lo * e->blk_bytes);
    cp->w  = (rfb_u16)((hi + 1u) * e->blk_bytes - cp->x0);
    if ((rfb_u32)cp->x0 + cp->w > bpr)
        cp->w = (rfb_u16)(bpr - cp->x0);
    cp->y0 = (rfb_u16)(best_start * e->probe_step);
    cp->h  = (rfb_u16)(best_run * e->probe_step);
    cp->dy = cand[best];

    /* Clip so both ends of the move stay on the screen. */
    if (cp->dy > 0) {
        if ((rfb_u32)cp->y0 + cp->h + cp->dy > h) {
            if ((rfb_u32)cp->y0 + cp->dy >= h)
                return 0;
            cp->h = (rfb_u16)(h - cp->y0 - cp->dy);
        }
    } else {
        rfb_u32 back = (rfb_u32)(-cp->dy);
        if (cp->y0 < back) {
            if ((rfb_u32)cp->h <= back - cp->y0)
                return 0;
            cp->h = (rfb_u16)(cp->h - (back - cp->y0));
            cp->y0 = (rfb_u16)back;
        }
        if ((rfb_u32)cp->y0 + cp->h > h)
            cp->h = (rfb_u16)(h - cp->y0);
    }
    if (cp->h < e->scroll.min_band || cp->w == 0)
        return 0;
    return 1;
}

static void rfb_apply_copy(rfb_encoder *e, const rfb_copy *cp)
{
    const rfb_u32 stride = e->row_stride;
    rfb_u32 p, r;

    for (p = 0; p < e->nplanes; p++) {
        rfb_u8 *plane = e->shadow + p * e->plane_stride;
        if (cp->dy > 0) {
            for (r = 0; r < cp->h; r++) {
                rfb_u32 dstr = cp->y0 + r;
                rfb_u32 srcr = dstr + (rfb_u32)cp->dy;
                memmove(plane + dstr * stride + cp->x0,
                        plane + srcr * stride + cp->x0, (size_t)cp->w);
            }
        } else {
            for (r = cp->h; r > 0; r--) {
                rfb_u32 dstr = cp->y0 + r - 1u;
                rfb_u32 srcr = (rfb_u32)((rfb_s32)dstr + cp->dy);
                memmove(plane + dstr * stride + cp->x0,
                        plane + srcr * stride + cp->x0, (size_t)cp->w);
            }
        }
    }
    e->st.shadow_move += (rfb_u32)cp->h * cp->w * e->nplanes;
}

/* --------------------------------------------------------------- frame --- */

long rfb_encode_frame(rfb_encoder *e, const rfb_u8 *src,
                      rfb_u8 *out, rfb_u32 out_cap)
{
    const rfb_u8 *planes[RFB_MAX_DEPTH];
    rfb_u32 p;

    if (!e || !src)
        return RFB_E_GEOM;
    if (e->nplanes == 0 || e->nplanes > RFB_MAX_DEPTH)
        return RFB_E_GEOM;

    for (p = 0; p < e->nplanes; p++)
        planes[p] = src + p * e->plane_stride;

    return rfb_encode_frame_planes(e, planes, out, out_cap);
}

long rfb_encode_frame_planes(rfb_encoder *e, const rfb_u8 *const *planes,
                             rfb_u8 *out, rfb_u32 out_cap)
{
    if (!e)
        return RFB_E_GEOM;
    return rfb_encode_band(e, planes, out, out_cap, 0, e->tiles_y);
}

long rfb_encode_band(rfb_encoder *e, const rfb_u8 *const *planes,
                     rfb_u8 *out, rfb_u32 out_cap,
                     rfb_u16 ty0, rfb_u16 ty1)
{
    int first, last;

    rfb_u32 bpr, depth, tb, tile_row;
    int keep_xor, min_run, word, chunky, unit16;
    rfb_out o;
    rfb_u32 ty, tx, p, y0, top = 0, tile_index, walk0;
    rfb_u32 dirty_tiles = 0;
    rfb_u32 dirty_plane[RFB_MAX_DEPTH];
    int fresh_plane[RFB_MAX_DEPTH];     /* shadow was zero: XOR is RAW */
    rfb_u8 *sh_plane[RFB_MAX_DEPTH];
    rfb_u8 *raw_plane[RFB_MAX_DEPTH];
    rfb_u8 *xor_plane[RFB_MAX_DEPTH];
    int did_copy = 0;

    if (!e || !planes || !out)
        return RFB_E_GEOM;
    if (ty1 > e->tiles_y || ty0 > ty1)
        return RFB_E_GEOM;

    /* A band is a whole frame message carrying only tile rows [ty0, ty1).
     * Per-FRAME work hangs off `first` and `last`: the scroll probe and its
     * copy op go in the first band, the frame counters close in the last. */
    first = (ty0 == 0);
    last  = (ty1 >= e->tiles_y);

    bpr = e->g.bytes_per_row;
    /* The plane count, not the depth: a chunky source is one plane of bytes,
     * and its depth is about the far end, nothing here. */
    depth = e->nplanes;
    chunky = RFB_FMT_IS_CHUNKY(e->g.format) ? 1 : 0;
    unit16 = (rfb_pb_unit(e->g.format) == 2u);
    tb = (rfb_u32)e->g.tile_w * e->g.tile_h;
    keep_xor = (e->flags & RFB_F_XOR) ? 1 : 0;
    min_run = (e->flags & RFB_F_RLE2) ? 2 : 3;

    word = rfb_words_ok(e, planes);
    tile_row = (rfb_u32)e->g.tile_h * e->row_stride;

    o.buf = out; o.cap = out_cap; o.len = 0; o.over = 0;
    rfb_put8(&o, RFB_VERSION);
    rfb_put8(&o, 0);
    rfb_put16(&o, e->seq++);

    if (first && (e->flags & RFB_F_COPYRECT)) {
        int probe = rfb_scroll_probe_due(e);

        /* The backoff doubles after each miss and resets on a hit.  Spending
         * a unit of it is the one thing rfb_scroll_probe_due() must not do,
         * so the pass it refused for the backoff spends it here. */
        if (!probe && (e->flags & RFB_F_SCROLL_ADAPTIVE) && e->backoff_left &&
            rfb_scroll_maybe(e))
            e->backoff_left--;

        if (probe) {
            rfb_copy cp;
            if (rfb_detect_scroll(e, planes[0], &cp)) {
                rfb_put8(&o, RFB_OP_COPY);
                rfb_put16(&o, cp.x0);
                rfb_put16(&o, cp.w);
                rfb_put16(&o, cp.y0);
                rfb_put16(&o, cp.h);
                rfb_put16(&o, (rfb_u32)(rfb_u16)cp.dy);
                rfb_apply_copy(e, &cp);
                e->st.copies++;
                e->st.copy_rows += cp.h;
                e->last_dy = cp.dy;
                e->backoff = 0;
                e->backoff_left = 0;
                e->miss_run = 0;
                e->since_copy = 0;
                did_copy = 1;
            } else if (e->scroll.max_backoff) {
                /* Three misses in a row before backing off, and only for a
                 * screen that scrolled recently.  Anything else backs off
                 * from the first miss. */
                rfb_u32 tolerate = (e->since_copy < 64u) ? 3u : 1u;

                if (e->miss_run < 255u)
                    e->miss_run++;
                if (e->miss_run >= tolerate) {
                    rfb_u32 b = e->backoff ? e->backoff * 2u : 1u;
                    if (b > e->scroll.max_backoff)
                        b = e->scroll.max_backoff;
                    e->backoff = (rfb_u8)b;
                    e->backoff_left = e->backoff;
                }
            }
        }
    }

    for (p = 0; p < depth; p++) {
        sh_plane[p]  = e->shadow + p * e->plane_stride;
        raw_plane[p] = e->rawbuf + p * tb;
        xor_plane[p] = e->xorbuf + p * tb;
    }

    /* Ask the whole band first; rfb_band_clean() reads exactly the bytes the
     * tile walk would have.  Banded calls ONLY -- on a whole-screen pass this
     * measured cheaper but raised keystroke-to-frame latency. */
    walk0 = ty0;
    if (!(first && last) && rfb_band_clean(e, planes, ty0, ty1, word)) {
        e->st.tiles_scanned += (rfb_u32)(ty1 - ty0) * e->tiles_x;
        walk0 = ty1;
    }

    y0 = (rfb_u32)walk0 * tile_row;
    top = (rfb_u32)walk0 * e->g.tile_h;
    tile_index = (rfb_u32)walk0 * e->tiles_x;
    for (ty = walk0; ty < ty1; ty++, y0 += tile_row) {
        rfb_u32 th = e->g.height - top;
        rfb_u32 x0 = 0;

        if (th > e->g.tile_h)
            th = e->g.tile_h;
        top += e->g.tile_h;

        for (tx = 0; tx < e->tiles_x; tx++, tile_index++,
                                       x0 += e->g.tile_w) {
            rfb_u32 tw = bpr - x0;
            rfb_u32 off = y0 + x0;
            rfb_u32 raw_len, mask = 0;

            if (tw > e->g.tile_w)
                tw = e->g.tile_w;
            /* Both fit a word, so this is a MULU.W and not a helper call. */
            raw_len = (rfb_u32)((rfb_u16)tw * (rfb_u16)th);
            e->st.tiles_scanned++;

            /* Ask first, write nothing.  A tile that nobody drew on ends here. */
            for (p = 0; p < depth; p++) {
                dirty_plane[p] = (rfb_u32)rfb_cmp_plane(
                        planes[p] + off, sh_plane[p] + off,
                        e->row_stride, tw, th, word);
                if (dirty_plane[p])
                    mask |= 1u << p;
            }
            if (mask == 0)
                continue;

            dirty_tiles++;
            e->st.tiles_dirty++;
            if (!(e->flags & RFB_F_PLANEMASK))
                mask = (1u << depth) - 1u;

            for (p = 0; p < depth; p++) {
                fresh_plane[p] = 0;
                if (!(mask & (1u << p)))
                    continue;
                if (dirty_plane[p])
                    fresh_plane[p] = rfb_take_plane(
                                   planes[p] + off, sh_plane[p] + off,
                                   raw_plane[p], xor_plane[p],
                                   e->row_stride, tw, th, keep_xor, word);
                else
                    rfb_take_clean(sh_plane[p] + off,
                                   raw_plane[p], xor_plane[p],
                                   e->row_stride, tw, th, keep_xor);
            }

            if (chunky) {
                rfb_put8(&o, RFB_OP_TILE8);
                rfb_put16(&o, tile_index);
            } else {
                rfb_put8(&o, RFB_OP_TILE);
                rfb_put16(&o, tile_index);
                rfb_put8(&o, (rfb_u8)mask);
            }

            for (p = 0; p < depth; p++) {
                rfb_u32 best_len, la, lb;
                rfb_u8 *raw = raw_plane[p];
                rfb_u8 *best_ptr;
                int best_code;

                if (!(mask & (1u << p)))
                    continue;

                best_code = RFB_CODE_RAW;
                best_len = raw_len;
                best_ptr = raw;

                if (e->flags & RFB_F_BESTOF) {
                    /* Each candidate is capped at what is already the best, so
                     * a losing one stops early instead of finishing.  A plane
                     * whose shadow was zero has an XOR equal to its RAW, so
                     * the second candidate would be the first one over again:
                     * that is every tile of a first frame, which is the frame
                     * a viewer waits for. */
                    if (keep_xor) {
                        la = rfb_pb(unit16, xor_plane[p], raw_len,
                                    e->pb_a, best_len - 1u, min_run);
                        if (la != RFB_PB_FAIL) {
                            best_code = RFB_CODE_PB_XOR;
                            best_len = la;
                            best_ptr = e->pb_a;
                        }
                    }
                    if ((e->flags & RFB_F_PACKBITS) && !fresh_plane[p]) {
                        lb = rfb_pb(unit16, raw, raw_len,
                                    e->pb_b, best_len - 1u, min_run);
                        if (lb != RFB_PB_FAIL) {
                            best_code = RFB_CODE_PB_RAW;
                            best_len = lb;
                            best_ptr = e->pb_b;
                        }
                    }
                } else if (keep_xor) {
                    la = rfb_pb(unit16, xor_plane[p], raw_len, e->pb_a,
                                RFB_PB_BOUND(raw_len), min_run);
                    if (la != RFB_PB_FAIL) {
                        best_code = RFB_CODE_PB_XOR;
                        best_len = la;
                        best_ptr = e->pb_a;
                    }
                } else if (e->flags & RFB_F_PACKBITS) {
                    lb = rfb_pb(unit16, raw, raw_len, e->pb_b,
                                RFB_PB_BOUND(raw_len), min_run);
                    if (lb != RFB_PB_FAIL) {
                        best_code = RFB_CODE_PB_RAW;
                        best_len = lb;
                        best_ptr = e->pb_b;
                    }
                }

                rfb_put8(&o, (rfb_u8)best_code);
                if (best_code == RFB_CODE_RAW) {
                    e->st.code_raw++;
                } else {
                    rfb_put16(&o, best_len);
                    if (best_code == RFB_CODE_PB_RAW)
                        e->st.code_pb_raw++;
                    else
                        e->st.code_pb_xor++;
                }
                rfb_putblk(&o, best_ptr, best_len);
                e->st.planes_sent++;
            }

            if (o.over)
                return RFB_E_OVERFLOW;
        }
    }

    rfb_put8(&o, RFB_OP_END);
    if (o.over)
        return RFB_E_OVERFLOW;

    /* Bytes out are this message's and are counted whichever band it was. */
    e->st.bytes_out += o.len;

    /* Dirty tiles and the copied flag describe the SCREEN pass, not the band,
       so they accumulate until the last band closes them. */
    e->band_dirty += dirty_tiles;
    if (did_copy)
        e->band_copy = 1;

    if (!last)
        return (long)o.len;

    e->st.src_bytes += e->frame_bytes;
    if (!e->band_copy && e->since_copy < 255u)
        e->since_copy++;
    e->last_dirty = (rfb_u16)(e->band_dirty > 0xFFFFu ? 0xFFFFu
                                                      : e->band_dirty);
    e->last_copy = e->band_copy;
    e->band_dirty = 0;
    e->band_copy = 0;
    e->st.frames++;
    return (long)o.len;
}
