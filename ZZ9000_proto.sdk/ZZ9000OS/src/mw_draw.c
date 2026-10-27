/*
 * ZZ9000 MiniGL Wire Protocol v9 Draw Decoder & Dispatch
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mw_draw.h"
#include <string.h>

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

    for (i = 0; i < total_words; ++i)
        out_verts[i] = mw_bef(payload + 8 + i * 4);

    *out_mode = mode;
    *out_count = count;
    return 1;
}

int mw_decode_draw_packed(const uint8_t *payload, size_t bytes,
                          float *out_verts, uint32_t *out_mode, uint32_t *out_count)
{
    uint32_t mode, count, format, position, color, tex0, tex1, vertex_normal;
    const uint8_t *q;
    float constant_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float constant_tex0[4]  = {0.0f, 0.0f, 0.0f, 1.0f};
    float constant_tex1[4]  = {0.0f, 0.0f, 0.0f, 1.0f};
    float constant_normal[3]= {0.0f, 0.0f, 1.0f};
    uint32_t i, c, u;

    if (bytes < 12)
        return 0;

    mode = mw_be32(payload);
    count = mw_be32(payload + 4);
    format = mw_be32(payload + 8);

    position = ((format >> MW_PACK_POSITION) & 3U) + 2U;
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

    for (i = 0; i < count; ++i) {
        float *dst = out_verts + i * MW_OBJECT_WORDS;

        /* Position */
        dst[0] = mw_bef(q); q += 4;
        dst[1] = mw_bef(q); q += 4;
        dst[2] = (position >= 3U) ? mw_bef(q) : 0.0f;
        if (position >= 3U) q += 4;
        dst[3] = (position == 4U) ? mw_bef(q) : 1.0f;
        if (position == 4U) q += 4;

        /* Color */
        if (color == 1U) {
            uint32_t rgba = mw_be32(q); q += 4;
            dst[4] = (float)((rgba >> 24) & 0xffU) * (1.0f / 255.0f);
            dst[5] = (float)((rgba >> 16) & 0xffU) * (1.0f / 255.0f);
            dst[6] = (float)((rgba >> 8)  & 0xffU) * (1.0f / 255.0f);
            dst[7] = (float)( rgba        & 0xffU) * (1.0f / 255.0f);
        } else if (color == 2U) {
            for (c = 0; c < 4; ++c) { dst[4 + c] = mw_bef(q); q += 4; }
        } else {
            for (c = 0; c < 4; ++c) dst[4 + c] = constant_color[c];
        }

        /* Tex0 and Tex1 */
        for (u = 0; u < 2; ++u) {
            uint32_t t_mode = u ? tex1 : tex0;
            const float *t_const = u ? constant_tex1 : constant_tex0;
            float *tdst = dst + 8 + 4 * u;

            if (t_mode == 1U) {
                tdst[0] = mw_bef(q); q += 4;
                tdst[1] = mw_bef(q); q += 4;
                tdst[2] = 0.0f;
                tdst[3] = 1.0f;
            } else if (t_mode == 2U) {
                for (c = 0; c < 4; ++c) { tdst[c] = mw_bef(q); q += 4; }
            } else {
                for (c = 0; c < 4; ++c) tdst[c] = t_const[c];
            }
        }

        /* Normal */
        if (vertex_normal) {
            dst[16] = mw_bef(q); q += 4;
            dst[17] = mw_bef(q); q += 4;
            dst[18] = mw_bef(q); q += 4;
        } else {
            dst[16] = constant_normal[0];
            dst[17] = constant_normal[1];
            dst[18] = constant_normal[2];
        }
    }

    *out_mode = mode;
    *out_count = count;
    return 1;
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
