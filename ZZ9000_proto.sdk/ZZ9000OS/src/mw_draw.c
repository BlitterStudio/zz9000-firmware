/*
 * ZZ9000 MiniGL Wire Protocol v9 Draw Decoder & Dispatch
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mw_draw.h"
#include <string.h>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

/* Float words move as 32-bit integers: no core-to-VFP move per word on ARM,
 * and every bit, NaNs included, arrives as sent. */
typedef uint32_t __attribute__((may_alias)) mw_word_alias_t;

int mw_decode_draw_object(const uint8_t *payload, size_t bytes,
                          float *out_verts, uint32_t *out_mode, uint32_t *out_count)
{
    uint32_t mode, count, total_words, i;

    if (bytes < 8)
        return 0;

    mode = mw_be32(payload);
    count = mw_be32(payload + 4);
    total_words = count * MW_OBJECT_WORDS;

    if (bytes < 8 + total_words * 4)
        return 0;

    {
        mw_word_alias_t *out = (mw_word_alias_t *)out_verts;

        for (i = 0; i < total_words; ++i)
            out[i] = mw_be32(payload + 8 + i * 4);
    }

    *out_mode = mode;
    *out_count = count;
    return 1;
}

/* Vertices are expanded to MW_OBJECT_WORDS words by copying one template of
 * the per-draw constants and defaults over each destination and then
 * overwriting the attributes the wire carries per vertex.  Everything moves
 * as 32-bit integers, so no float passes through a register (a core-to-VFP
 * move per word on the ARM target) and every bit, NaNs included, arrives as
 * sent. */
static void mw_unpack_packed_vertices(const uint8_t *q, uint32_t count, uint32_t format,
                                      const float *constant_color, const float *constant_tex0,
                                      const float *constant_tex1, const float *constant_normal,
                                      float *out_verts)
{
    uint32_t position = ((format >> MW_PACK_POSITION) & 3U) + 2U;
    uint32_t color = (format >> MW_PACK_COLOR) & 3U;
    uint32_t tex0 = (format >> MW_PACK_TEX0) & 3U;
    uint32_t tex1 = (format >> MW_PACK_TEX1) & 3U;
    uint32_t vertex_normal = (format >> MW_PACK_NORMAL) & 1U;
    uint32_t tmpl[MW_OBJECT_WORDS] = { 0U };
    const float one = 1.0f;
    const float zero = 0.0f;
    uint32_t i;

    memcpy(&tmpl[2], &zero, sizeof(uint32_t));
    memcpy(&tmpl[3], &one, sizeof(uint32_t));
    if (color == 0U)
        memcpy(&tmpl[4], constant_color, 4U * sizeof(uint32_t));
    if (tex0 == 0U)
        memcpy(&tmpl[8], constant_tex0, 4U * sizeof(uint32_t));
    else if (tex0 == 1U) {
        memcpy(&tmpl[10], &zero, sizeof(uint32_t));
        memcpy(&tmpl[11], &one, sizeof(uint32_t));
    }
    if (tex1 == 0U)
        memcpy(&tmpl[12], constant_tex1, 4U * sizeof(uint32_t));
    else if (tex1 == 1U) {
        memcpy(&tmpl[14], &zero, sizeof(uint32_t));
        memcpy(&tmpl[15], &one, sizeof(uint32_t));
    }
    if (!vertex_normal)
        memcpy(&tmpl[16], constant_normal, 3U * sizeof(uint32_t));
#if defined(__ARM_NEON)
    /* The first sixteen template words stay in vector registers; the loop
     * stores them, then overwrites what the wire carries.  The normal's
     * three words are copied separately so that no store passes the end of
     * the last vertex. */
    const uint32x4_t template0 = vld1q_u32(&tmpl[0]);
    const uint32x4_t template1 = vld1q_u32(&tmpl[4]);
    const uint32x4_t template2 = vld1q_u32(&tmpl[8]);
    const uint32x4_t template3 = vld1q_u32(&tmpl[12]);
#endif

    for (i = 0; i < count; ++i) {
        mw_word_alias_t *dst = (mw_word_alias_t *)(out_verts + i * MW_OBJECT_WORDS);
        uint32_t w;

#if defined(__ARM_NEON)
        vst1q_u32((uint32_t *)dst, template0);
        vst1q_u32((uint32_t *)dst + 4, template1);
        vst1q_u32((uint32_t *)dst + 8, template2);
        vst1q_u32((uint32_t *)dst + 12, template3);
        dst[16] = tmpl[16];
        dst[17] = tmpl[17];
        dst[18] = tmpl[18];
#else
        memcpy(dst, tmpl, sizeof(tmpl));
#endif

        /* Position */
        dst[0] = mw_be32(q);
        dst[1] = mw_be32(q + 4);
        q += 8;
        if (position >= 3U) {
            dst[2] = mw_be32(q);
            q += 4;
        }
        if (position == 4U) {
            dst[3] = mw_be32(q);
            q += 4;
        }

        /* Color */
        if (color == 1U) {
            uint32_t rgba = mw_be32(q); q += 4;
            float r = (float)((rgba >> 24) & 0xffU) * (1.0f / 255.0f);
            float g = (float)((rgba >> 16) & 0xffU) * (1.0f / 255.0f);
            float b = (float)((rgba >> 8)  & 0xffU) * (1.0f / 255.0f);
            float a = (float)( rgba        & 0xffU) * (1.0f / 255.0f);

            memcpy(&dst[4], &r, sizeof(r));
            memcpy(&dst[5], &g, sizeof(g));
            memcpy(&dst[6], &b, sizeof(b));
            memcpy(&dst[7], &a, sizeof(a));
        } else if (color == 2U) {
            for (w = 0; w < 4U; ++w, q += 4) dst[4 + w] = mw_be32(q);
        }

        /* Tex0 and Tex1 */
        if (tex0 == 1U) {
            dst[8] = mw_be32(q);
            dst[9] = mw_be32(q + 4);
            q += 8;
        } else if (tex0 == 2U) {
            for (w = 0; w < 4U; ++w, q += 4) dst[8 + w] = mw_be32(q);
        }
        if (tex1 == 1U) {
            dst[12] = mw_be32(q);
            dst[13] = mw_be32(q + 4);
            q += 8;
        } else if (tex1 == 2U) {
            for (w = 0; w < 4U; ++w, q += 4) dst[12 + w] = mw_be32(q);
        }

        /* Normal */
        if (vertex_normal) {
            dst[16] = mw_be32(q);
            dst[17] = mw_be32(q + 4);
            dst[18] = mw_be32(q + 8);
            q += 12;
        }
    }
}

int mw_decode_draw_packed(const uint8_t *payload, size_t bytes,
                          float *out_verts, uint32_t *out_mode, uint32_t *out_count)
{
    uint32_t mode, count, format, color, tex0, tex1, vertex_normal;
    uint32_t constants, words;
    uint64_t expected;
    const uint8_t *q;
    float constant_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float constant_tex0[4]  = {0.0f, 0.0f, 0.0f, 1.0f};
    float constant_tex1[4]  = {0.0f, 0.0f, 0.0f, 1.0f};
    float constant_normal[3]= {0.0f, 0.0f, 1.0f};
    uint32_t c;

    if (bytes < 12)
        return 0;

    mode = mw_be32(payload);
    count = mw_be32(payload + 4);
    format = mw_be32(payload + 8);

    if (mode > 6 || !count || count > MW_MAX_VERTICES ||
        (mode == 1 && (count % 2 != 0)) ||
        (mode == 4 && (count % 3 != 0)))
        return 0;

    if (!mw_packed_layout(format, &constants, &words))
        return 0;

    expected = 12 + 4 * ((uint64_t)constants + (uint64_t)count * words);
    if (bytes != expected)
        return 0;

    color = (format >> MW_PACK_COLOR) & 3U;
    tex0 = (format >> MW_PACK_TEX0) & 3U;
    tex1 = (format >> MW_PACK_TEX1) & 3U;
    vertex_normal = (format >> MW_PACK_NORMAL) & 1U;

    q = payload + 12;

    if (!color) {
        for (c = 0; c < 4; ++c) { constant_color[c] = mw_bef(q); q += 4; }
    }
    if (!tex0) {
        for (c = 0; c < 4; ++c) { constant_tex0[c] = mw_bef(q); q += 4; }
    }
    if (!tex1) {
        for (c = 0; c < 4; ++c) { constant_tex1[c] = mw_bef(q); q += 4; }
    }
    if (!vertex_normal) {
        for (c = 0; c < 3; ++c) { constant_normal[c] = mw_bef(q); q += 4; }
    }

    mw_unpack_packed_vertices(q, count, format, constant_color, constant_tex0,
                              constant_tex1, constant_normal, out_verts);

    *out_mode = mode;
    *out_count = count;
    return 1;
}

static int mw_decode_draw_packed_indexed_impl(const uint8_t *payload,
                                              size_t bytes, float *scratch,
                                              uint32_t *out_mode,
                                              uint32_t *out_count,
                                              int prevalidate)
{
    uint32_t mode, v_count, idx_count, format;
    uint32_t constants, words;
    uint64_t expected;
    const uint8_t *q, *idx_ptr;
    float constant_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float constant_tex0[4]  = {0.0f, 0.0f, 0.0f, 1.0f};
    float constant_tex1[4]  = {0.0f, 0.0f, 0.0f, 1.0f};
    float constant_normal[3]= {0.0f, 0.0f, 1.0f};
    uint32_t color, tex0, tex1, vertex_normal, c, i;
    float *unique_verts;

    if (bytes < 16)
        return 0;

    mode = mw_be32(payload);
    v_count = mw_be32(payload + 4);
    idx_count = mw_be32(payload + 8);
    format = mw_be32(payload + 12);

    if (mode > 6 || !v_count || v_count > MW_MAX_VERTICES ||
        !idx_count || idx_count > MW_MAX_VERTICES ||
        (mode == 1 && (idx_count % 2 != 0)) ||
        (mode == 4 && (idx_count % 3 != 0)))
        return 0;

    if (!mw_packed_layout(format, &constants, &words))
        return 0;

    expected = 16 + 4 * ((uint64_t)constants + (uint64_t)v_count * words) +
               (((uint64_t)idx_count * 2 + 3) & ~3ULL);
    if (bytes != expected)
        return 0;

    /* The standalone decoder rejects a bad index before writing anything;
     * the parse visitor runs after mw_validate_packet_report, which has
     * already proven every index < v_count, so the validated entry point
     * skips that extra pass over the index array and keeps the bound in
     * the copy loop as a safety net. */
    if (prevalidate) {
        idx_ptr = payload + 16 + 4 * (constants + v_count * words);
        for (i = 0; i < idx_count; ++i) {
            if (mw_be16(idx_ptr + i * 2) >= v_count)
                return 0;
        }
    }

    color = (format >> MW_PACK_COLOR) & 3U;
    tex0 = (format >> MW_PACK_TEX0) & 3U;
    tex1 = (format >> MW_PACK_TEX1) & 3U;
    vertex_normal = (format >> MW_PACK_NORMAL) & 1U;

    q = payload + 16;
    if (!color) {
        for (c = 0; c < 4; ++c) { constant_color[c] = mw_bef(q); q += 4; }
    }
    if (!tex0) {
        for (c = 0; c < 4; ++c) { constant_tex0[c] = mw_bef(q); q += 4; }
    }
    if (!tex1) {
        for (c = 0; c < 4; ++c) { constant_tex1[c] = mw_bef(q); q += 4; }
    }
    if (!vertex_normal) {
        for (c = 0; c < 3; ++c) { constant_normal[c] = mw_bef(q); q += 4; }
    }

    unique_verts = scratch + idx_count * MW_OBJECT_WORDS;
    mw_unpack_packed_vertices(q, v_count, format, constant_color, constant_tex0,
                              constant_tex1, constant_normal, unique_verts);

    idx_ptr = payload + 16 + 4 * (constants + v_count * words);
    for (i = 0; i < idx_count; ++i) {
        uint16_t idx = mw_be16(idx_ptr + i * 2);

        if (idx >= v_count)
            return 0;
        memcpy(scratch + i * MW_OBJECT_WORDS,
               unique_verts + (uint32_t)idx * MW_OBJECT_WORDS,
               MW_OBJECT_BYTES);
    }

    *out_mode = mode;
    *out_count = idx_count;
    return 1;
}

int mw_decode_draw_packed_indexed(const uint8_t *payload, size_t bytes,
                                  float *scratch, uint32_t *out_mode, uint32_t *out_count)
{
    return mw_decode_draw_packed_indexed_impl(payload, bytes, scratch,
                                              out_mode, out_count, 1);
}

int mw_decode_draw_packed_indexed_validated(const uint8_t *payload, size_t bytes,
                                            float *scratch, uint32_t *out_mode,
                                            uint32_t *out_count)
{
    return mw_decode_draw_packed_indexed_impl(payload, bytes, scratch,
                                              out_mode, out_count, 0);
}

int mw_decode_draw_lowered(const uint8_t *payload, size_t bytes,
                           float *out_verts, uint32_t *out_mode, uint32_t *out_count,
                           uint32_t *out_flags, const uint8_t **out_edge_mask)
{
    uint32_t mode, count, flags, total_words, i;

    if (bytes < 16)
        return 0;

    mode = mw_be32(payload);
    count = mw_be32(payload + 4);
    flags = mw_be32(payload + 12);
    total_words = count * MW_OBJECT_WORDS;

    if (bytes < 16 + total_words * 4)
        return 0;

    for (i = 0; i < total_words; ++i)
        out_verts[i] = mw_bef(payload + 16 + i * 4);

    *out_mode = mode;
    *out_count = count;
    *out_flags = flags;
    *out_edge_mask = (flags & MW_LOWERED_EDGE_MASK) ?
                     (payload + 16 + total_words * 4) : NULL;
    return 1;
}

typedef struct mw_raw_stream {
    const uint8_t *data;
    float constant[4];
    uint32_t components;
    uint32_t type;
    uint32_t element;
} mw_raw_stream;

static float mw_raw_scalar_component(const uint8_t *p, uint32_t type)
{
    switch (type) {
    case 0x1400: return (float)(int8_t)p[0];
    case 0x1401: return (float)p[0];
    case 0x1402: return (float)(int16_t)(((uint32_t)p[0] << 8) | p[1]);
    case 0x1403: return (float)(((uint32_t)p[0] << 8) | p[1]);
    case 0x1404: return (float)(int32_t)mw_be32(p);
    case 0x1405: return (float)mw_be32(p);
    case 0x1406: return mw_bef(p);
    case 0x140a: {
        uint64_t bits = 0;
        unsigned i;
        double val;
        for (i = 0; i < 8; ++i) bits = (bits << 8) | p[i];
        memcpy(&val, &bits, sizeof(val));
        return (float)val;
    }
    default: return 0.0f;
    }
}

int mw_decode_draw_array_raw(const uint8_t *payload, size_t bytes,
                             float *out_verts, uint32_t *out_mode, uint32_t *out_count)
{
    uint32_t mode, count, streams_count, index_type;
    mw_raw_stream streams[7];
    const uint8_t *q;
    const uint8_t *index_data;
    uint32_t i, s, c;

    if (bytes < 32)
        return 0;

    memset(streams, 0, sizeof(streams));
    mode = mw_be32(payload);
    count = mw_be32(payload + 4);
    streams_count = mw_be32(payload + 16);
    index_type = mw_be32(payload + 20);

    q = payload + 32;
    for (i = 0; i < streams_count; ++i) {
        uint32_t slot = mw_be32(q);
        uint32_t comps = mw_be32(q + 4);
        uint32_t type = mw_be32(q + 8);
        uint32_t elem = mw_be32(q + 12);
        uint32_t data_len = mw_be32(q + 16);
        q += 20;

        if (slot < 7) {
            streams[slot].components = comps;
            streams[slot].type = type;
            streams[slot].element = elem;
            if (data_len) {
                streams[slot].data = q;
                q += (data_len + 3U) & ~3U;
            } else {
                for (c = 0; c < 4; ++c)
                    streams[slot].constant[c] = mw_bef(q + c * 4);
                q += 16;
            }
        }
    }
    index_data = index_type ? q : NULL;

    for (i = 0; i < count; ++i) {
        float *dst = out_verts + i * MW_OBJECT_WORDS;
        uint32_t idx = i;
        static const unsigned slots_to_check[5] = {0U, 1U, 4U, 5U, 6U};

        if (index_data) {
            if (index_type == 0x1401)
                idx = index_data[i];
            else if (index_type == 0x1403)
                idx = ((uint32_t)index_data[i * 2] << 8) | index_data[i * 2 + 1];
            else if (index_type == 0x1405)
                idx = mw_be32(index_data + i * 4);
        }

        /* Default object vertex */
        dst[0] = 0.0f; dst[1] = 0.0f; dst[2] = 0.0f; dst[3] = 1.0f; /* pos */
        dst[4] = 1.0f; dst[5] = 1.0f; dst[6] = 1.0f; dst[7] = 1.0f; /* color */
        dst[8] = 0.0f; dst[9] = 0.0f; dst[10] = 0.0f; dst[11] = 1.0f; /* tex0 */
        dst[12] = 0.0f; dst[13] = 0.0f; dst[14] = 0.0f; dst[15] = 1.0f; /* tex1 */
        dst[16] = 0.0f; dst[17] = 0.0f; dst[18] = 1.0f; /* normal */

        for (s = 0; s < 5; ++s) {
            unsigned slot = slots_to_check[s];
            const mw_raw_stream *st = &streams[slot];
            const uint8_t *v = st->data ? (st->data + idx * st->element) : NULL;
            float *target = (slot == 0) ? dst :
                            (slot == 1) ? (dst + 4) :
                            (slot == 4) ? (dst + 8) :
                            (slot == 5) ? (dst + 12) : (dst + 16);

            if (!st->components && !st->data)
                continue;

            if (st->type == MW_RAW_UBYTE_ARGB && v) {
                target[0] = (float)v[1] * (1.0f / 255.0f);
                target[1] = (float)v[2] * (1.0f / 255.0f);
                target[2] = (float)v[3] * (1.0f / 255.0f);
                target[3] = (float)v[0] * (1.0f / 255.0f);
                continue;
            }
            if (st->type == MW_RAW_UBYTE_BGRA && v) {
                target[0] = (float)v[2] * (1.0f / 255.0f);
                target[1] = (float)v[1] * (1.0f / 255.0f);
                target[2] = (float)v[0] * (1.0f / 255.0f);
                target[3] = (float)v[3] * (1.0f / 255.0f);
                continue;
            }

            for (c = 0; c < st->components; ++c) {
                float val;
                if (st->data) {
                    uint32_t scalar_bytes = st->element / st->components;
                    val = mw_raw_scalar_component(v + c * scalar_bytes, st->type);
                    if (slot == 1 || slot == 6) {
                        if (st->type == 0x1400)
                            val = (val <= -128.0f) ? -1.0f : val / 127.0f;
                        else if (st->type == 0x1401)
                            val = val / 255.0f;
                        else if (st->type == 0x1402)
                            val = (val <= -32768.0f) ? -1.0f : val / 32767.0f;
                        else if (st->type == 0x1403)
                            val = val / 65535.0f;
                        else if (st->type == 0x1404)
                            val = (val <= -2147483648.0f) ? -1.0f : val / 2147483647.0f;
                        else if (st->type == 0x1405)
                            val = val / 4294967295.0f;
                    }
                } else {
                    val = st->constant[c];
                }
                target[c] = val;
            }
        }
    }

    *out_mode = mode;
    *out_count = count;
    return 1;
}

static void mw_copy_vert(float *out, size_t to, const float *in, size_t from)
{
    memcpy(out + to * MW_OBJECT_WORDS, in + from * MW_OBJECT_WORDS, MW_OBJECT_BYTES);
}

static void mw_emit_tri(float *out, size_t *n, const float *in,
                        size_t a, size_t b, size_t c, int flat)
{
    mw_copy_vert(out, *n, in, a);
    mw_copy_vert(out, *n + 1, in, b);
    mw_copy_vert(out, *n + 2, in, c);
    if (flat) {
        unsigned k;
        for (k = 0; k < 3; ++k)
            memcpy(out + (*n + k) * MW_OBJECT_WORDS + 4,
                   in + c * MW_OBJECT_WORDS + 4, 4 * sizeof(float));
    }
    *n += 3;
}

size_t mw_expand_lowered(uint32_t mode, const float *in, size_t count, float *out, int flat)
{
    size_t n = 0;
    size_t i;

    if (mode <= 3) { /* POINTS, LINES, LINE_LOOP, LINE_STRIP */
        memcpy(out, in, count * MW_OBJECT_BYTES);
        return count;
    }

    if (mode == 4) { /* GL_TRIANGLES */
        for (i = 0; i + 2 < count; i += 3)
            mw_emit_tri(out, &n, in, i, i + 1, i + 2, flat);
    } else if (mode == 5) { /* GL_TRIANGLE_STRIP */
        for (i = 2; i < count; ++i) {
            size_t a = (i & 1) ? (i - 1) : (i - 2);
            size_t b = (i & 1) ? (i - 2) : (i - 1);
            mw_emit_tri(out, &n, in, a, b, i, flat);
        }
    } else if (mode == 6 || mode == 9) { /* GL_TRIANGLE_FAN / GL_POLYGON */
        for (i = 2; i < count; ++i)
            mw_emit_tri(out, &n, in, 0, i - 1, i, flat);
    } else if (mode == 7) { /* GL_QUADS */
        for (i = 0; i + 3 < count; i += 4) {
            mw_emit_tri(out, &n, in, i, i + 1, i + 2, flat);
            mw_emit_tri(out, &n, in, i, i + 2, i + 3, flat);
        }
    } else if (mode == 8) { /* GL_QUAD_STRIP */
        for (i = 0; i + 3 < count; i += 2) {
            mw_emit_tri(out, &n, in, i, i + 1, i + 3, flat);
            mw_emit_tri(out, &n, in, i, i + 3, i + 2, flat);
        }
    }
    return n;
}

static int mw_mask_edge(const uint8_t *mask, size_t i)
{
    if (!mask) return 1;
    return (mw_be32(mask + (i / 32) * 4) & (1U << (i % 32))) != 0;
}

static void mw_emit_line(float *out, size_t *n, const float *in,
                         size_t a, size_t b, const uint8_t *mask)
{
    if (mw_mask_edge(mask, a)) {
        mw_copy_vert(out, (*n)++, in, a);
        mw_copy_vert(out, (*n)++, in, b);
    }
}

size_t mw_outline_lowered(uint32_t mode, const float *in, size_t count,
                          const uint8_t *mask, float *out)
{
    size_t n = 0;
    size_t i;

    if (mode == 4) { /* GL_TRIANGLES */
        for (i = 0; i + 2 < count; i += 3) {
            mw_emit_line(out, &n, in, i, i + 1, mask);
            mw_emit_line(out, &n, in, i + 1, i + 2, mask);
            mw_emit_line(out, &n, in, i + 2, i, mask);
        }
    } else if (mode == 5) { /* GL_TRIANGLE_STRIP */
        for (i = 2; i < count; ++i) {
            size_t a = (i & 1) ? (i - 1) : (i - 2);
            size_t b = (i & 1) ? (i - 2) : (i - 1);
            mw_emit_line(out, &n, in, a, b, mask);
            mw_emit_line(out, &n, in, b, i, mask);
            mw_emit_line(out, &n, in, i, a, mask);
        }
    } else if (mode == 6) { /* GL_TRIANGLE_FAN */
        for (i = 2; i < count; ++i) {
            mw_emit_line(out, &n, in, 0, i - 1, mask);
            mw_emit_line(out, &n, in, i - 1, i, mask);
            mw_emit_line(out, &n, in, i, 0, mask);
        }
    } else if (mode == 7) { /* GL_QUADS */
        for (i = 0; i + 3 < count; i += 4) {
            mw_emit_line(out, &n, in, i, i + 1, mask);
            mw_emit_line(out, &n, in, i + 1, i + 2, mask);
            mw_emit_line(out, &n, in, i + 2, i + 3, mask);
            mw_emit_line(out, &n, in, i + 3, i, mask);
        }
    } else if (mode == 8) { /* GL_QUAD_STRIP */
        for (i = 0; i + 3 < count; i += 2) {
            mw_emit_line(out, &n, in, i, i + 1, mask);
            mw_emit_line(out, &n, in, i + 1, i + 3, mask);
            mw_emit_line(out, &n, in, i + 3, i + 2, mask);
            mw_emit_line(out, &n, in, i + 2, i, mask);
        }
    } else if (mode == 9) { /* GL_POLYGON */
        for (i = 0; i < count; ++i)
            mw_emit_line(out, &n, in, i, (i + 1 == count) ? 0 : (i + 1), mask);
    } else {
        memcpy(out, in, count * MW_OBJECT_BYTES);
        n = count;
    }
    return n;
}
