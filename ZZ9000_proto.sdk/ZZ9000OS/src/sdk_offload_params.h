/*
 * ZZ9000 dual-core task scheduler -- decompress task descriptor helper.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host-compilable (no xil_ or firmware includes) so the pack/unpack of
 * struct decompress_op_params into taskq_desc_t::op_params is unit-tested
 * off-target, mirroring scheduler.h/scheduler.c. Shared by the producer
 * (Task 6) and the ARM-side executor in sdk_mailbox.c.
 */
#ifndef ZZ9K_SDK_OFFLOAD_PARAMS_H
#define ZZ9K_SDK_OFFLOAD_PARAMS_H

#include <stdint.h>
#include "scheduler.h"

struct decompress_op_params {
  uint32_t algorithm;
  uint32_t flags;
};

/* op_params is 4-byte aligned within taskq_desc_t, so the struct-pointer
 * cast below is safe -- same pattern the crypto op-params structs use. */
static inline void decompress_fill_desc(taskq_desc_t *d,
    uint32_t in_addr, uint32_t in_len, uint32_t out_addr, uint32_t out_cap,
    uint32_t algorithm, uint32_t flags)
{
  struct decompress_op_params *p = (struct decompress_op_params *)d->op_params;
  d->opcode = TASKQ_OP_DECOMPRESS;
  d->in_addr = in_addr;
  d->in_len = in_len;
  d->out_addr = out_addr;
  d->out_cap = out_cap;
  p->algorithm = algorithm;
  p->flags = flags;
}

#endif /* ZZ9K_SDK_OFFLOAD_PARAMS_H */
