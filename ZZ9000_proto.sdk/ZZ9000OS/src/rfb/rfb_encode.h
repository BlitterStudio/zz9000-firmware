/* Remote-framebuffer frame encoder.  Portable C99, no allocation per frame.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ZZ9K_RFB_ENCODE_H
#define ZZ9K_RFB_ENCODE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Own typedefs rather than <stdint.h>: this must build under every m68k
 * toolchain the project supports. */
typedef unsigned char  rfb_u8;
typedef unsigned short rfb_u16;
typedef unsigned int   rfb_u32;   /* int, not long: long is 64 bits on the host */
typedef signed short   rfb_s16;
typedef signed int     rfb_s32;

#define RFB_VERSION      1

#define RFB_OP_END       0x00
#define RFB_OP_COPY      0x01
#define RFB_OP_TILE      0x02
#define RFB_OP_TILE8     0x03   /* either chunky format: no plane mask */

#define RFB_CODE_RAW     0
#define RFB_CODE_PB_RAW  1
#define RFB_CODE_PB_XOR  2

/* What a pixel means.  Zero is planar, so a caller that clears its rfb_geom
 * gets the Amiga BitMap it always got.  Three separate questions come off this
 * one field: layout (RFB_FMT_IS_CHUNKY), bytes a pixel (RFB_FMT_PIXEL_BYTES)
 * and how many colours (rfb_pal_colours) -- which is NOT 1 << depth. */
#define RFB_FMT_PLANAR   0      /* depth one-bit planes, pixel is an index  */
#define RFB_FMT_CLUT8    1      /* one plane, a byte is an index            */
#define RFB_FMT_RGB565   2      /* one plane, two bytes are a colour        */
#define RFB_FMT_HAM6     3      /* 6 planes, 16 base colours, 4-bit modify  */
#define RFB_FMT_HAM8     4      /* 8 planes, 64 base colours, 6-bit modify  */
#define RFB_FMT_EHB      5      /* 6 planes, 32 colours, 32..63 are halved  */

/* Planes, not bits.  The three chipset modes are NOT chunky: they are the
 * Amiga BitMap, and only the last step from index to colour is theirs. */
#define RFB_FMT_IS_CHUNKY(f) ((f) == RFB_FMT_CLUT8 || (f) == RFB_FMT_RGB565)

/* Source bytes one pixel occupies, and 0 on a planar format, where it is not
 * a whole number. */
#define RFB_FMT_PIXEL_BYTES(f) \
    ((f) == RFB_FMT_CLUT8 ? 1u : ((f) == RFB_FMT_RGB565 ? 2u : 0u))

/* The longest `pal` there is, which is what sizes a caller's buffer.  Not the
 * ceiling on rfb_geom.depth: RFB_FMT_RGB565 is 16 deep and has no palette. */
#define RFB_MAX_DEPTH    8

/* What rfb_geom.depth is on the truecolour format, which is the one place a
 * depth is neither a plane count nor a palette width. */
#define RFB_RGB565_DEPTH 16
#define RFB_MAX_TILE_W   64   /* bytes */
#define RFB_MAX_TILE_H   64   /* rows */
#define RFB_MAX_CAND     16

/* Encoder layers, each switchable on its own.  The shipping set is
 * RFB_F_BASELINE. */
#define RFB_F_PACKBITS         0x0001u  /* PB_RAW is allowed */
#define RFB_F_XOR              0x0002u  /* PB_XOR is allowed */
#define RFB_F_PLANEMASK        0x0004u  /* send only the planes that changed */
#define RFB_F_BESTOF           0x0008u  /* pick the smallest allowed code */
#define RFB_F_COPYRECT         0x0010u  /* detect a vertical scroll */
#define RFB_F_SCROLL_ADAPTIVE  0x0020u  /* probe only after a busy frame */
#define RFB_F_RLE2             0x0040u  /* PackBits runs start at 2, not 3 */
/* BMF_INTERLEAVED: a plane advances by bytes_per_row and a row by
 * bytes_per_row * depth.  Nothing on the wire changes. */
#define RFB_F_INTERLEAVED      0x0080u

#define RFB_F_BASELINE (RFB_F_PACKBITS | RFB_F_XOR | RFB_F_PLANEMASK | \
                        RFB_F_BESTOF)

#define RFB_E_GEOM       (-1L)   /* geometry rejected */
#define RFB_E_BUFFER     (-2L)   /* shadow or scratch too small */
#define RFB_E_OVERFLOW   (-3L)   /* output capacity exhausted */

typedef struct {
    rfb_u16 width;          /* pixels, for the receiver; not used to index */
    rfb_u16 height;         /* rows */
    rfb_u16 bytes_per_row;  /* from the BitMap; one plane's row either way */
    rfb_u8  depth;          /* 1..8 planar, 8 CLUT8, 16 RGB565, 6/8/6 HAM
                               and EHB, where it is still the plane count  */
    rfb_u8  tile_w;         /* tile width in BYTES, 1..RFB_MAX_TILE_W */
    rfb_u8  tile_h;         /* tile height in rows, 1..RFB_MAX_TILE_H */
    rfb_u8  format;         /* one of the three RFB_FMT_ */
} rfb_geom;

/* Counters.  src_bytes and probe_bytes are the two that matter on the Amiga:
 * both are reads of the live bitmap, which is chip RAM. */
typedef struct {
    rfb_u32 frames;
    rfb_u32 bytes_out;
    rfb_u32 tiles_scanned;
    rfb_u32 tiles_dirty;
    rfb_u32 planes_sent;
    rfb_u32 code_raw;
    rfb_u32 code_pb_raw;
    rfb_u32 code_pb_xor;
    rfb_u32 copies;
    rfb_u32 copy_rows;
    rfb_u32 probes;         /* frames the scroll detector ran on */
    rfb_u32 src_bytes;      /* chip reads, tile pass */
    rfb_u32 probe_bytes;    /* chip reads, scroll probe */
    rfb_u32 shadow_move;    /* shadow bytes moved for copy ops */
} rfb_stats;

typedef struct {
    rfb_u8  probe_step;     /* sample every Nth row, 1..16 */
    rfb_u8  match_pct;      /* rows that must match for a candidate to win */
    rfb_u16 min_band;       /* fewest rows worth a copy op */
    rfb_u8  n_cand;
    rfb_s16 cand[RFB_MAX_CAND];  /* row offsets to try */
    rfb_u16 busy_tiles;     /* RFB_F_SCROLL_ADAPTIVE threshold */
    rfb_u8  max_backoff;    /* frames skipped after a miss, 0 = never skip */
} rfb_scroll_cfg;

typedef struct {
    rfb_geom g;
    rfb_u32  flags;
    rfb_u8   nplanes;       /* g.depth planar, 1 chunky.  What the walk uses */
    rfb_u16  tiles_x;
    rfb_u16  tiles_y;
    rfb_u32  plane_bytes;   /* bytes_per_row * height */
    rfb_u32  frame_bytes;   /* plane_bytes * depth */
    rfb_u32  row_stride;    /* source bytes from one row to the next */
    rfb_u32  plane_stride;  /* source bytes from one plane to the next */
    rfb_u16  seq;

    rfb_u8  *shadow;
    rfb_u8  *scratch;
    rfb_u32  scratch_len;

    /* Carved out of scratch by rfb_encoder_init(). */
    rfb_u8  *xorbuf;        /* depth * tile_w * tile_h */
    rfb_u8  *rawbuf;        /* depth * tile_w * tile_h */
    rfb_u8  *pb_a;
    rfb_u8  *pb_b;
    rfb_u8  *probe_row;     /* one source row, RFB_F_COPYRECT only */
    rfb_u32 *probe_mask;    /* n_cand * n_samples column-match masks */

    rfb_scroll_cfg scroll;
    rfb_u16  probe_step;    /* effective, after the sample-count clamp */
    rfb_u16  n_samples;
    rfb_u16  blk_bytes;     /* probe column block width */
    rfb_u16  n_blk;
    rfb_u16  last_dirty;    /* dirty tiles in the previous frame */
    rfb_u8   last_copy;
    rfb_s16  last_dy;       /* the offset that worked, tried first next time */
    rfb_u8   backoff;       /* frames to skip after a probe found nothing */
    rfb_u8   backoff_left;
    rfb_u8   miss_run;      /* consecutive probes that found nothing */
    rfb_u8   since_copy;    /* frames since a copy, saturating at 255 */

    /* Accumulated across the bands of one screen pass, because they describe
     * the pass and not the message.  Closed and cleared by the last band. */
    rfb_u32  band_dirty;
    rfb_u8   band_copy;

    rfb_stats st;
} rfb_encoder;

/* How many planes the source actually has: g->depth, or 1 for a chunky
 * format.  A caller sizing its own buffers wants this and not the depth. */
rfb_u8  rfb_planes(const rfb_geom *g);

/* How many entries the `pal` word carries for this geometry, and 0 when the
 * format has no palette.  NOT 1 << depth, which is right only where the planes
 * hold a palette index.  No `pal` word is sent at all on a format answering 0. */
rfb_u32 rfb_pal_colours(const rfb_geom *g);

rfb_u32 rfb_shadow_size(const rfb_geom *g);
rfb_u32 rfb_scratch_size(const rfb_geom *g, rfb_u32 flags,
                         const rfb_scroll_cfg *cfg);
rfb_u32 rfb_worst_case_frame(const rfb_geom *g);

/* Fills cfg with the defaults: every 4th row sampled, 75% of sampled rows must
 * match, 32-row floor, offsets 8 16 24 32 4 2 1 -8 -16. */
void rfb_scroll_defaults(rfb_scroll_cfg *cfg);

/* cfg may be NULL for the defaults.  shadow must be zeroed for the first
 * frame, which then codes as a delta from the all-zero screen a receiver
 * starts from. */
long rfb_encoder_init(rfb_encoder *e, const rfb_geom *g, rfb_u32 flags,
                      const rfb_scroll_cfg *cfg,
                      rfb_u8 *shadow, rfb_u32 shadow_len,
                      rfb_u8 *scratch, rfb_u32 scratch_len);

long rfb_encode_frame(rfb_encoder *e, const rfb_u8 *src,
                      rfb_u8 *out, rfb_u32 out_cap);

/* For a source whose planes are not one block: planes[p] is the base of plane
 * p and rows within it are row_stride apart.  A tile that differs is copied
 * ONCE into rawbuf, so the shadow and the wire cannot disagree however much the
 * screen moves under a caller that holds no drawing lock. */
/* One BAND of a frame: the same message carrying only the tiles of tile rows
 * [ty0, ty1).  Bands are expected in order, 0 to tiles_y, and two screen passes
 * must not be interleaved: the scroll probe runs on the first band and the
 * counters that describe a pass are closed by the last. */
long rfb_encode_band(rfb_encoder *e, const rfb_u8 *const *planes,
                     rfb_u8 *out, rfb_u32 out_cap,
                     rfb_u16 ty0, rfb_u16 ty1);

long rfb_encode_frame_planes(rfb_encoder *e, const rfb_u8 *const *planes,
                             rfb_u8 *out, rfb_u32 out_cap);

/* Whether the next band 0 will look for a scroll.  The probe samples rows the
 * whole height of the screen, so a caller reading its pixels back off a
 * graphics card asks this to decide whether the pass needs the frame in one
 * moment or only the band it is about to encode.  Reads state, changes none. */
int rfb_scroll_probe_due(const rfb_encoder *e);

/* PackBits, exposed because the decoder and the tests want the same pair.
 * Two pairs: the unit a count counts is a byte, or on RFB_FMT_RGB565 a
 * two-byte pixel (rfb_pb_unit()).  The 16 pair takes n in UNITS from an even
 * address and answers bytes; the unpacker's out_n is bytes.  Both packers
 * answer RFB_PB_FAIL when the output would pass cap, and when the first
 * quarter of the input has no run in it (see rfb_encode.c). */
rfb_u32 rfb_packbits(const rfb_u8 *in, rfb_u32 n, rfb_u8 *out, rfb_u32 cap,
                     int min_run);
long rfb_unpackbits(const rfb_u8 *in, rfb_u32 n, rfb_u8 *out, rfb_u32 out_n);
rfb_u32 rfb_packbits16(const rfb_u16 *in, rfb_u32 n, rfb_u8 *out, rfb_u32 cap,
                       int min_run);
long rfb_unpackbits16(const rfb_u8 *in, rfb_u32 n, rfb_u8 *out, rfb_u32 out_n);

/* The PackBits unit for a format: 2 on RFB_FMT_RGB565, else 1.  A tile of an
 * RGB565 screen is whole pixels -- rfb_encoder_init() refuses an odd tile_w
 * there -- so every plane a PB code carries is a whole number of units. */
#define rfb_pb_unit(format) (((format) == RFB_FMT_RGB565) ? 2u : 1u)

#ifdef __cplusplus
}
#endif

#endif /* ZZ9K_RFB_ENCODE_H */
