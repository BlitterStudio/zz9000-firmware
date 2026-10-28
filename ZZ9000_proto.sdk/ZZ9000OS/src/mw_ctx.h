/*
 * ZZ9000 MiniGL Wire Protocol v9 Context
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MW_CTX_H
#define MW_CTX_H

#include <stdint.h>
#include <stddef.h>
#include "mw_host.h"
#include "mw_types.h"
#include "mw_state.h"
#include "mw_texture.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mw_ctx {
    uint32_t width;
    uint32_t height;
    mw_state state;
    /* First asynchronous command error; packet transport remains healthy. */
    uint32_t error;
    mw_texture_manager texmgr;
    uint8_t  palette[256][4];
    uint32_t palette_width;
    uint32_t *color_buffers[3];
    uint8_t  *depth_buffer;
    uint8_t  *texture_pool;
    uint32_t texture_pool_bytes;

    float *vertex_scratch;
    uint32_t vertex_scratch_capacity;

    uint32_t handle;
    uint32_t generation;
    uint32_t card_pool_owner;
    uint32_t color_buffer_addresses[3];
    uint32_t depth_buffer_address;
    uint32_t texture_pool_address;
    uint32_t vertex_scratch_address;
    uint8_t card_pool_backed;

    /* BLIT / CAPTURE state for fast path */
    uint8_t  *presented_span_address;
    uint8_t   presented_request[24 + 16 * MW_BLIT_MAX_RECTS];
    uint32_t  presented_request_len;
    uint8_t  *presented_span;
    uint32_t  presented_copy_address;
    uint32_t  presented_span_size;
    uint32_t  presented_span_capacity;
    uint8_t   presented_valid;
};

#ifdef __cplusplus
}
#endif

#endif /* MW_CTX_H */
