/*
 * Host unit tests for the decompress task descriptor helper
 * (sdk_offload_params.h): pack/unpack symmetry + dispatch class.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdk_offload_params.h"
#include "scheduler.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

#define SDK_COMPRESSION_ZLIB 2U

static void test_decompress_fill_desc_packs_fields(void)
{
  taskq_desc_t d;
  const struct decompress_op_params *p;

  memset(&d, 0, sizeof d);
  decompress_fill_desc(&d, 0x11000000u, 4096u, 0x12000000u, 65536u,
                       SDK_COMPRESSION_ZLIB, 0u);

  assert(d.opcode == TASKQ_OP_DECOMPRESS);
  assert(d.in_addr == 0x11000000u && d.in_len == 4096u);
  assert(d.out_addr == 0x12000000u && d.out_cap == 65536u);

  p = (const struct decompress_op_params *)d.op_params;
  assert(p->algorithm == SDK_COMPRESSION_ZLIB && p->flags == 0u);

  printf("test_decompress_fill_desc_packs_fields OK\n");
}

static void test_decompress_class_is_long(void)
{
  assert(taskq_class_for_opcode(TASKQ_OP_DECOMPRESS, 999999u) == TASK_LONG);
  printf("test_decompress_class_is_long OK\n");
}

int main(void)
{
  test_decompress_fill_desc_packs_fields();
  test_decompress_class_is_long();
  printf("all offload_params tests passed\n");
  return 0;
}
