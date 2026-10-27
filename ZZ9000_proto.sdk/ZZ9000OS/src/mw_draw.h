/*
 * ZZ9000 MiniGL Wire Protocol v9 Draw Decoder & Dispatch
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MW_DRAW_H
#define MW_DRAW_H

#include <stdint.h>
#include <stddef.h>
#include "mw_types.h"
#include "mw_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decodes drawing command attributes into 19-float object-space vertices:
 * [X, Y, Z, W,  R, G, B, A,  S0, T0, P0, Q0,  S1, T1, P1, Q1,  Nx, Ny, Nz]
 */

int mw_decode_draw_object(const uint8_t *payload, size_t bytes,
                          float *out_verts, uint32_t *out_mode, uint32_t *out_count);

int mw_decode_draw_packed(const uint8_t *payload, size_t bytes,
                          float *out_verts, uint32_t *out_mode, uint32_t *out_count);

int mw_decode_draw_lowered(const uint8_t *payload, size_t bytes,
                           float *out_verts, uint32_t *out_mode, uint32_t *out_count,
                           uint32_t *out_flags, const uint8_t **out_edge_mask);

int mw_decode_draw_array_raw(const uint8_t *payload, size_t bytes,
                             float *out_verts, uint32_t *out_mode, uint32_t *out_count);

typedef struct mw_clip_vert {
    float x, y, z, w;
} mw_clip_vert;

static inline mw_clip_vert mw_transform_pt(const float m[16], const float *p)
{
    mw_clip_vert v;
    v.x = m[0] * p[0] + m[4] * p[1] + m[8]  * p[2] + m[12] * p[3];
    v.y = m[1] * p[0] + m[5] * p[1] + m[9]  * p[2] + m[13] * p[3];
    v.z = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14] * p[3];
    v.w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15] * p[3];
    return v;
}

#define MW_NEAR_W 1e-6f

static inline float mw_clipped_area(const float mvp[16], float vp_w, float vp_h,
                                    const float *a, const float *b, const float *c)
{
    mw_clip_vert in[9], out[9];
    unsigned n, m, i;
    float twice;

    in[0] = mw_transform_pt(mvp, a);
    in[1] = mw_transform_pt(mvp, b);
    in[2] = mw_transform_pt(mvp, c);

    /* Clip w >= near_w */
    m = 0;
    for (i = 0; i < 3; ++i) {
        const mw_clip_vert *s = &in[i];
        const mw_clip_vert *e = &in[(i + 1 == 3) ? 0 : (i + 1)];
        float ds = s->w - MW_NEAR_W;
        float de = e->w - MW_NEAR_W;
        if ((ds >= 0.0f) != (de >= 0.0f)) {
            float t = (ds != de) ? (ds / (ds - de)) : 0.0f;
            mw_clip_vert v;
            v.x = s->x + t * (e->x - s->x);
            v.y = s->y + t * (e->y - s->y);
            v.z = s->z + t * (e->z - s->z);
            v.w = MW_NEAR_W;
            out[m++] = v;
        }
        if (de >= 0.0f)
            out[m++] = *e;
    }
    if (m < 3) return 0.0f;

    /* Clip z + w >= 0 */
    n = 0;
    for (i = 0; i < m; ++i) {
        const mw_clip_vert *s = &out[i];
        const mw_clip_vert *e = &out[(i + 1 == m) ? 0 : (i + 1)];
        float ds = s->z + s->w;
        float de = e->z + e->w;
        if ((ds >= 0.0f) != (de >= 0.0f)) {
            float t = (ds != de) ? (ds / (ds - de)) : 0.0f;
            mw_clip_vert v;
            v.x = s->x + t * (e->x - s->x);
            v.y = s->y + t * (e->y - s->y);
            v.z = s->z + t * (e->z - s->z);
            v.w = s->w + t * (e->w - s->w);
            if (v.w < MW_NEAR_W) v.w = MW_NEAR_W;
            in[n++] = v;
        }
        if (de >= 0.0f)
            in[n++] = *e;
    }
    if (n < 3) return 0.0f;

    twice = 0.0f;
    for (i = 0; i < n; ++i) {
        const mw_clip_vert *p1 = &in[i];
        const mw_clip_vert *p2 = &in[(i + 1 == n) ? 0 : (i + 1)];
        twice += (p1->x / p1->w) * (p2->y / p2->w) - (p2->x / p2->w) * (p1->y / p1->w);
    }
    return 0.125f * twice * vp_w * vp_h;
}

size_t mw_expand_lowered(uint32_t mode, const float *in, size_t count, float *out, int flat);

size_t mw_outline_lowered(uint32_t mode, const float *in, size_t count,
                          const uint8_t *mask, float *out);

#ifdef __cplusplus
}
#endif

#endif /* MW_DRAW_H */
