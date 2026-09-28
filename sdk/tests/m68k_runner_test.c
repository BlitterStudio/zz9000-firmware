/*
 * Contract tests for the opt-in 68k offload runner prototype.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k-m68k-runner.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void put_be16(uint8_t *memory, uint32_t offset, uint16_t value)
{
  memory[offset + 0U] = (uint8_t)(value >> 8);
  memory[offset + 1U] = (uint8_t)value;
}

static void put_be32(uint8_t *memory, uint32_t offset, uint32_t value)
{
  memory[offset + 0U] = (uint8_t)(value >> 24);
  memory[offset + 1U] = (uint8_t)(value >> 16);
  memory[offset + 2U] = (uint8_t)(value >> 8);
  memory[offset + 3U] = (uint8_t)value;
}

static int run_program(ZZ9KM68KState *state,
                       uint8_t *memory,
                       uint32_t memory_size,
                       uint32_t instruction_limit,
                       ZZ9KM68KResult *result)
{
  return zz9k_m68k_run(
      state, memory, memory_size, instruction_limit, result);
}

static int test_moveq_sets_data_register_and_ccr(void)
{
  uint8_t memory[64];
  ZZ9KM68KState state;
  ZZ9KM68KResult result;
  int status;

  memset(memory, 0, sizeof(memory));
  zz9k_m68k_state_init(&state);
  state.a[7] = 32U;
  put_be16(memory, 0U, 0x70ffU); /* moveq #-1,d0 */
  put_be16(memory, 2U, 0x4e75U); /* rts */
  put_be32(memory, 32U, 0x00000040UL);

  status = run_program(&state, memory, sizeof(memory), 8U, &result);
  if (status != ZZ9K_M68K_STATUS_RTS) return 1;
  if (state.d[0] != 0xffffffffUL) return 2;
  if ((state.sr & ZZ9K_M68K_CCR_N) == 0U) return 3;
  if ((state.sr & ZZ9K_M68K_CCR_Z) != 0U) return 4;
  if ((state.sr & (ZZ9K_M68K_CCR_V | ZZ9K_M68K_CCR_C)) != 0U) return 5;
  if (state.pc != 0x40UL || state.a[7] != 36U) return 6;
  if (result.instructions != 2U) return 7;
  return 0;
}

static int test_addq_and_subq_long_update_data_register(void)
{
  uint8_t memory[64];
  ZZ9KM68KState state;
  ZZ9KM68KResult result;
  int status;

  memset(memory, 0, sizeof(memory));
  zz9k_m68k_state_init(&state);
  state.a[7] = 32U;
  put_be16(memory, 0U, 0x7005U); /* moveq #5,d0 */
  put_be16(memory, 2U, 0x5680U); /* addq.l #3,d0 */
  put_be16(memory, 4U, 0x5380U); /* subq.l #1,d0 */
  put_be16(memory, 6U, 0x4e75U); /* rts */
  put_be32(memory, 32U, 0x00000040UL);

  status = run_program(&state, memory, sizeof(memory), 8U, &result);
  if (status != ZZ9K_M68K_STATUS_RTS) return 1;
  if (state.d[0] != 7U) return 2;
  if ((state.sr & (ZZ9K_M68K_CCR_N | ZZ9K_M68K_CCR_Z |
                   ZZ9K_M68K_CCR_V | ZZ9K_M68K_CCR_C)) != 0U) {
    return 3;
  }
  if (result.instructions != 4U) return 4;
  return 0;
}

static int test_big_endian_absolute_long_load_and_store(void)
{
  uint8_t memory[96];
  ZZ9KM68KState state;
  ZZ9KM68KResult result;
  int status;

  memset(memory, 0, sizeof(memory));
  zz9k_m68k_state_init(&state);
  state.a[7] = 64U;
  put_be16(memory, 0U, 0x7012U); /* moveq #0x12,d0 */
  put_be16(memory, 2U, 0x23c0U); /* move.l d0,$00000020.l */
  put_be32(memory, 4U, 0x00000020UL);
  put_be16(memory, 8U, 0x2239U); /* move.l $00000020.l,d1 */
  put_be32(memory, 10U, 0x00000020UL);
  put_be16(memory, 14U, 0x4e75U); /* rts */
  put_be32(memory, 64U, 0x00000050UL);

  status = run_program(&state, memory, sizeof(memory), 8U, &result);
  if (status != ZZ9K_M68K_STATUS_RTS) return 1;
  if (memory[32] != 0U || memory[33] != 0U ||
      memory[34] != 0U || memory[35] != 0x12U) {
    return 2;
  }
  if (state.d[1] != 0x12U) return 3;
  if (result.instructions != 4U) return 4;
  return 0;
}

static int test_zero_result_sets_z_and_clears_n(void)
{
  uint8_t memory[32];
  ZZ9KM68KState state;
  ZZ9KM68KResult result;
  int status;

  memset(memory, 0, sizeof(memory));
  zz9k_m68k_state_init(&state);
  state.a[7] = 16U;
  put_be16(memory, 0U, 0x7000U); /* moveq #0,d0 */
  put_be16(memory, 2U, 0x4e75U); /* rts */
  put_be32(memory, 16U, 0x00000020UL);

  status = run_program(&state, memory, sizeof(memory), 4U, &result);
  if (status != ZZ9K_M68K_STATUS_RTS) return 1;
  if ((state.sr & ZZ9K_M68K_CCR_Z) == 0U) return 2;
  if ((state.sr & ZZ9K_M68K_CCR_N) != 0U) return 3;
  return 0;
}

static int test_illegal_instruction_reports_fault_pc_and_opcode(void)
{
  uint8_t memory[8];
  ZZ9KM68KState state;
  ZZ9KM68KResult result;
  int status;

  memset(memory, 0, sizeof(memory));
  zz9k_m68k_state_init(&state);
  put_be16(memory, 0U, 0x4afcU); /* illegal */

  status = run_program(&state, memory, sizeof(memory), 4U, &result);
  if (status != ZZ9K_M68K_STATUS_ILLEGAL) return 1;
  if (result.fault_pc != 0U || result.opcode != 0x4afcU) return 2;
  if (result.instructions != 0U) return 3;
  return 0;
}

static int test_fetch_out_of_bounds_is_reported(void)
{
  uint8_t memory[1];
  ZZ9KM68KState state;
  ZZ9KM68KResult result;
  int status;

  memset(memory, 0, sizeof(memory));
  zz9k_m68k_state_init(&state);

  status = run_program(&state, memory, sizeof(memory), 4U, &result);
  if (status != ZZ9K_M68K_STATUS_OOB) return 1;
  if (result.fault_pc != 0U || result.instructions != 0U) return 2;
  return 0;
}

static int test_instruction_limit_stops_after_executed_instruction(void)
{
  uint8_t memory[32];
  ZZ9KM68KState state;
  ZZ9KM68KResult result;
  int status;

  memset(memory, 0, sizeof(memory));
  zz9k_m68k_state_init(&state);
  state.a[7] = 16U;
  put_be16(memory, 0U, 0x7005U); /* moveq #5,d0 */
  put_be16(memory, 2U, 0x4e75U); /* rts */
  put_be32(memory, 16U, 0x00000020UL);

  status = run_program(&state, memory, sizeof(memory), 1U, &result);
  if (status != ZZ9K_M68K_STATUS_LIMIT) return 1;
  if (state.d[0] != 5U || state.pc != 2U) return 2;
  if (result.instructions != 1U) return 3;
  return 0;
}

static int test_checksum_loop_uses_postincrement_and_dbra(void)
{
  uint8_t memory[96];
  ZZ9KM68KState state;
  ZZ9KM68KResult result;
  int status;

  memset(memory, 0, sizeof(memory));
  zz9k_m68k_state_init(&state);
  state.a[0] = 48U;
  state.a[7] = 64U;
  memory[48] = 1U;
  memory[49] = 2U;
  memory[50] = 3U;
  memory[51] = 4U;
  put_be16(memory, 0U, 0x7000U); /* moveq #0,d0 */
  put_be16(memory, 2U, 0x7203U); /* moveq #3,d1 */
  put_be16(memory, 4U, 0xd018U); /* add.b (a0)+,d0 */
  put_be16(memory, 6U, 0x51c9U); /* dbra d1,loop */
  put_be16(memory, 8U, 0xfffaU); /* loop at pc 4 */
  put_be16(memory, 10U, 0x4e75U); /* rts */
  put_be32(memory, 64U, 0x00000060UL);

  status = run_program(&state, memory, sizeof(memory), 16U, &result);
  if (status != ZZ9K_M68K_STATUS_RTS) return 1;
  if ((state.d[0] & 0xffU) != 10U) return 2;
  if (state.d[1] != 0x0000ffffUL) return 3;
  if (state.a[0] != 52U) return 4;
  if (state.pc != 0x60U || state.a[7] != 68U) return 5;
  if ((state.sr & (ZZ9K_M68K_CCR_N | ZZ9K_M68K_CCR_Z |
                   ZZ9K_M68K_CCR_V | ZZ9K_M68K_CCR_C)) != 0U) {
    return 6;
  }
  if (result.instructions != 11U) return 7;
  return 0;
}

static int test_checksum_loop_respects_instruction_limit(void)
{
  uint8_t memory[96];
  ZZ9KM68KState state;
  ZZ9KM68KResult result;
  int status;

  memset(memory, 0, sizeof(memory));
  zz9k_m68k_state_init(&state);
  state.a[0] = 48U;
  state.a[7] = 64U;
  memory[48] = 1U;
  memory[49] = 2U;
  memory[50] = 3U;
  memory[51] = 4U;
  put_be16(memory, 0U, 0x7000U); /* moveq #0,d0 */
  put_be16(memory, 2U, 0x7203U); /* moveq #3,d1 */
  put_be16(memory, 4U, 0xd018U); /* add.b (a0)+,d0 */
  put_be16(memory, 6U, 0x51c9U); /* dbra d1,loop */
  put_be16(memory, 8U, 0xfffaU); /* loop at pc 4 */
  put_be16(memory, 10U, 0x4e75U); /* rts */
  put_be32(memory, 64U, 0x00000060UL);

  status = run_program(&state, memory, sizeof(memory), 5U, &result);
  if (status != ZZ9K_M68K_STATUS_LIMIT) return 1;
  if ((state.d[0] & 0xffU) != 3U) return 2;
  if ((state.d[1] & 0xffffU) != 2U) return 3;
  if (state.a[0] != 50U) return 4;
  if (state.pc != 6U) return 5;
  if (result.instructions != 5U) return 6;
  return 0;
}

static int test_memcpy_loop_uses_source_and_destination_postincrement(void)
{
  uint8_t memory[128];
  ZZ9KM68KState state;
  ZZ9KM68KResult result;
  int status;

  memset(memory, 0, sizeof(memory));
  zz9k_m68k_state_init(&state);
  state.a[0] = 80U;
  state.a[1] = 88U;
  state.a[7] = 112U;
  state.sr = ZZ9K_M68K_CCR_X | ZZ9K_M68K_CCR_C | ZZ9K_M68K_CCR_V;
  memory[80] = 0x11U;
  memory[81] = 0x22U;
  memory[82] = 0x80U;
  memory[83] = 0x00U;
  put_be16(memory, 0U, 0x7203U); /* moveq #3,d1 */
  put_be16(memory, 2U, 0x12d8U); /* move.b (a0)+,(a1)+ */
  put_be16(memory, 4U, 0x51c9U); /* dbra d1,loop */
  put_be16(memory, 6U, 0xfffaU); /* loop at pc 2 */
  put_be16(memory, 8U, 0x4e75U); /* rts */
  put_be32(memory, 112U, 0x00000078UL);

  status = run_program(&state, memory, sizeof(memory), 16U, &result);
  if (status != ZZ9K_M68K_STATUS_RTS) return 1;
  if (memory[88] != 0x11U || memory[89] != 0x22U ||
      memory[90] != 0x80U || memory[91] != 0x00U) {
    return 2;
  }
  if (state.a[0] != 84U || state.a[1] != 92U) return 3;
  if (state.d[1] != 0x0000ffffUL) return 4;
  if (state.pc != 0x78U || state.a[7] != 116U) return 5;
  if ((state.sr & ZZ9K_M68K_CCR_X) == 0U) return 6;
  if ((state.sr & ZZ9K_M68K_CCR_Z) == 0U) return 7;
  if ((state.sr & (ZZ9K_M68K_CCR_N | ZZ9K_M68K_CCR_V |
                   ZZ9K_M68K_CCR_C)) != 0U) {
    return 8;
  }
  if (result.instructions != 10U) return 9;
  return 0;
}

static int test_memcpy_loop_respects_instruction_limit(void)
{
  uint8_t memory[128];
  ZZ9KM68KState state;
  ZZ9KM68KResult result;
  int status;

  memset(memory, 0, sizeof(memory));
  zz9k_m68k_state_init(&state);
  state.a[0] = 80U;
  state.a[1] = 88U;
  state.a[7] = 112U;
  memory[80] = 0x11U;
  memory[81] = 0x22U;
  memory[82] = 0x80U;
  memory[83] = 0x00U;
  put_be16(memory, 0U, 0x7203U); /* moveq #3,d1 */
  put_be16(memory, 2U, 0x12d8U); /* move.b (a0)+,(a1)+ */
  put_be16(memory, 4U, 0x51c9U); /* dbra d1,loop */
  put_be16(memory, 6U, 0xfffaU); /* loop at pc 2 */
  put_be16(memory, 8U, 0x4e75U); /* rts */
  put_be32(memory, 112U, 0x00000078UL);

  status = run_program(&state, memory, sizeof(memory), 4U, &result);
  if (status != ZZ9K_M68K_STATUS_LIMIT) return 1;
  if (memory[88] != 0x11U || memory[89] != 0x22U) return 2;
  if (memory[90] != 0U || memory[91] != 0U) return 3;
  if (state.a[0] != 82U || state.a[1] != 90U) return 4;
  if ((state.d[1] & 0xffffU) != 2U) return 5;
  if (state.pc != 4U) return 6;
  if (result.instructions != 4U) return 7;
  return 0;
}

int main(void)
{
  int rc;

  rc = test_moveq_sets_data_register_and_ccr();
  if (rc) {
    printf("test_moveq_sets_data_register_and_ccr failed: %d\n", rc);
    return 10 + rc;
  }
  rc = test_addq_and_subq_long_update_data_register();
  if (rc) {
    printf("test_addq_and_subq_long_update_data_register failed: %d\n", rc);
    return 20 + rc;
  }
  rc = test_big_endian_absolute_long_load_and_store();
  if (rc) {
    printf("test_big_endian_absolute_long_load_and_store failed: %d\n", rc);
    return 30 + rc;
  }
  rc = test_zero_result_sets_z_and_clears_n();
  if (rc) {
    printf("test_zero_result_sets_z_and_clears_n failed: %d\n", rc);
    return 40 + rc;
  }
  rc = test_illegal_instruction_reports_fault_pc_and_opcode();
  if (rc) {
    printf("test_illegal_instruction_reports_fault_pc_and_opcode failed: %d\n",
           rc);
    return 50 + rc;
  }
  rc = test_fetch_out_of_bounds_is_reported();
  if (rc) {
    printf("test_fetch_out_of_bounds_is_reported failed: %d\n", rc);
    return 60 + rc;
  }
  rc = test_instruction_limit_stops_after_executed_instruction();
  if (rc) {
    printf("test_instruction_limit_stops_after_executed_instruction failed: %d\n",
           rc);
    return 70 + rc;
  }
  rc = test_checksum_loop_uses_postincrement_and_dbra();
  if (rc) {
    printf("test_checksum_loop_uses_postincrement_and_dbra failed: %d\n", rc);
    return 80 + rc;
  }
  rc = test_checksum_loop_respects_instruction_limit();
  if (rc) {
    printf("test_checksum_loop_respects_instruction_limit failed: %d\n", rc);
    return 90 + rc;
  }
  rc = test_memcpy_loop_uses_source_and_destination_postincrement();
  if (rc) {
    printf("test_memcpy_loop_uses_source_and_destination_postincrement failed: %d\n",
           rc);
    return 100 + rc;
  }
  rc = test_memcpy_loop_respects_instruction_limit();
  if (rc) {
    printf("test_memcpy_loop_respects_instruction_limit failed: %d\n", rc);
    return 110 + rc;
  }

  printf("m68k_runner_test: all checks passed\n");
  return 0;
}
