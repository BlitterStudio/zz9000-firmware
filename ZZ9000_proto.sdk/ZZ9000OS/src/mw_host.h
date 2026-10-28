/*
 * ZZ9000 MiniGL Wire Protocol v9 Host Interface
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MW_HOST_H
#define MW_HOST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mw_ctx mw_ctx;

/* Allocates a new renderer context (NULL on failure). */
mw_ctx  *mw_host_create(uint32_t width, uint32_t height);
/* Firmware service variant with a stable card-pool owner. */
mw_ctx  *mw_host_create_owned(uint32_t width, uint32_t height, uint32_t owner);

/* Submits a wire-v9 command packet.
 * Status: 0 ok, 0xfffffffe INVALID, 0xfffffffd CAPACITY,
 *         0xfffffffc LOST, 0xfffffffb FAILED. */
uint32_t mw_host_submit(mw_ctx *c, const uint8_t *packet, uint32_t bytes);
/* As mw_host_submit(), also returns the command that failed execution or
 * validation (zero only when framing fails before an opcode is available). */
uint32_t mw_host_submit_report(mw_ctx *c, const uint8_t *packet, uint32_t bytes,
                               uint32_t *failed_opcode);

/* Executes a synchronous request. */
uint32_t mw_host_request(mw_ctx *c, const uint8_t *request, uint32_t bytes,
                         uint8_t *reply, uint32_t reply_capacity, uint32_t *reply_bytes);

/* Reads back latest presented frame as top-row-first RGBA8. */
uint32_t mw_host_readback_rgba8(mw_ctx *c, uint8_t *out, uint32_t capacity);
/* Reads rows [first_row, first_row + row_count) of the latest frame as
 * top-row-first RGBA8. */
uint32_t mw_host_readback_latest_rows(mw_ctx *c, uint8_t *out,
                                      uint32_t capacity, uint32_t first_row,
                                      uint32_t row_count);

/* Destroys renderer context and frees allocated memory. */
void     mw_host_destroy(mw_ctx *c);

#ifdef __cplusplus
}
#endif

#endif /* MW_HOST_H */
