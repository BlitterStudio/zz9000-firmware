/*
 * Minimal 68k runner contract for opt-in offload experiments.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k-m68k-runner.h"

#include <string.h>

static int zz9k_m68k_read_be16(const uint8_t *memory,
                               uint32_t memory_size,
                               uint32_t offset,
                               uint16_t *value)
{
  if (!memory || !value || offset > memory_size ||
      2U > memory_size - offset) {
    return 0;
  }
  *value = ((uint16_t)memory[offset] << 8) |
           (uint16_t)memory[offset + 1U];
  return 1;
}

static int zz9k_m68k_read_u8(const uint8_t *memory,
                             uint32_t memory_size,
                             uint32_t offset,
                             uint8_t *value)
{
  if (!memory || !value || offset >= memory_size) {
    return 0;
  }
  *value = memory[offset];
  return 1;
}

static int zz9k_m68k_write_u8(uint8_t *memory,
                              uint32_t memory_size,
                              uint32_t offset,
                              uint8_t value)
{
  if (!memory || offset >= memory_size) {
    return 0;
  }
  memory[offset] = value;
  return 1;
}

static int zz9k_m68k_read_be32(const uint8_t *memory,
                               uint32_t memory_size,
                               uint32_t offset,
                               uint32_t *value)
{
  if (!memory || !value || offset > memory_size ||
      4U > memory_size - offset) {
    return 0;
  }
  *value = ((uint32_t)memory[offset] << 24) |
           ((uint32_t)memory[offset + 1U] << 16) |
           ((uint32_t)memory[offset + 2U] << 8) |
           (uint32_t)memory[offset + 3U];
  return 1;
}

static int zz9k_m68k_write_be32(uint8_t *memory,
                                uint32_t memory_size,
                                uint32_t offset,
                                uint32_t value)
{
  if (!memory || offset > memory_size || 4U > memory_size - offset) {
    return 0;
  }
  memory[offset + 0U] = (uint8_t)(value >> 24);
  memory[offset + 1U] = (uint8_t)(value >> 16);
  memory[offset + 2U] = (uint8_t)(value >> 8);
  memory[offset + 3U] = (uint8_t)value;
  return 1;
}

static int zz9k_m68k_fetch_word(ZZ9KM68KState *state,
                                const uint8_t *memory,
                                uint32_t memory_size,
                                uint16_t *opcode)
{
  if (!zz9k_m68k_read_be16(memory, memory_size, state->pc, opcode)) {
    return 0;
  }
  state->pc += 2U;
  return 1;
}

static int zz9k_m68k_fetch_long(ZZ9KM68KState *state,
                                const uint8_t *memory,
                                uint32_t memory_size,
                                uint32_t *value)
{
  if (!zz9k_m68k_read_be32(memory, memory_size, state->pc, value)) {
    return 0;
  }
  state->pc += 4U;
  return 1;
}

static void zz9k_m68k_set_logic_flags(ZZ9KM68KState *state, uint32_t value)
{
  state->sr &= (uint16_t)~(ZZ9K_M68K_CCR_N | ZZ9K_M68K_CCR_Z |
                          ZZ9K_M68K_CCR_V | ZZ9K_M68K_CCR_C);
  if (value == 0U) {
    state->sr |= ZZ9K_M68K_CCR_Z;
  }
  if ((value & 0x80000000UL) != 0U) {
    state->sr |= ZZ9K_M68K_CCR_N;
  }
}

static void zz9k_m68k_set_logic_byte_flags(ZZ9KM68KState *state,
                                           uint8_t value)
{
  state->sr &= (uint16_t)~(ZZ9K_M68K_CCR_N | ZZ9K_M68K_CCR_Z |
                          ZZ9K_M68K_CCR_V | ZZ9K_M68K_CCR_C);
  if (value == 0U) {
    state->sr |= ZZ9K_M68K_CCR_Z;
  }
  if ((value & 0x80U) != 0U) {
    state->sr |= ZZ9K_M68K_CCR_N;
  }
}

static void zz9k_m68k_set_add_flags(ZZ9KM68KState *state,
                                    uint32_t src,
                                    uint32_t dst,
                                    uint32_t result)
{
  uint16_t flags = 0U;

  if (result == 0U) {
    flags |= ZZ9K_M68K_CCR_Z;
  }
  if ((result & 0x80000000UL) != 0U) {
    flags |= ZZ9K_M68K_CCR_N;
  }
  if (((~(dst ^ src) & (dst ^ result)) & 0x80000000UL) != 0U) {
    flags |= ZZ9K_M68K_CCR_V;
  }
  if (result < dst) {
    flags |= ZZ9K_M68K_CCR_C | ZZ9K_M68K_CCR_X;
  }
  state->sr &= (uint16_t)~(ZZ9K_M68K_CCR_X | ZZ9K_M68K_CCR_N |
                          ZZ9K_M68K_CCR_Z | ZZ9K_M68K_CCR_V |
                          ZZ9K_M68K_CCR_C);
  state->sr |= flags;
}

static void zz9k_m68k_set_add_byte_flags(ZZ9KM68KState *state,
                                         uint8_t src,
                                         uint8_t dst,
                                         uint8_t result)
{
  uint16_t flags = 0U;
  uint32_t wide = (uint32_t)dst + (uint32_t)src;

  if (result == 0U) {
    flags |= ZZ9K_M68K_CCR_Z;
  }
  if ((result & 0x80U) != 0U) {
    flags |= ZZ9K_M68K_CCR_N;
  }
  if (((~(dst ^ src) & (dst ^ result)) & 0x80U) != 0U) {
    flags |= ZZ9K_M68K_CCR_V;
  }
  if (wide > 0xffU) {
    flags |= ZZ9K_M68K_CCR_C | ZZ9K_M68K_CCR_X;
  }
  state->sr &= (uint16_t)~(ZZ9K_M68K_CCR_X | ZZ9K_M68K_CCR_N |
                          ZZ9K_M68K_CCR_Z | ZZ9K_M68K_CCR_V |
                          ZZ9K_M68K_CCR_C);
  state->sr |= flags;
}

static void zz9k_m68k_set_sub_flags(ZZ9KM68KState *state,
                                    uint32_t src,
                                    uint32_t dst,
                                    uint32_t result)
{
  uint16_t flags = 0U;

  if (result == 0U) {
    flags |= ZZ9K_M68K_CCR_Z;
  }
  if ((result & 0x80000000UL) != 0U) {
    flags |= ZZ9K_M68K_CCR_N;
  }
  if ((((dst ^ src) & (dst ^ result)) & 0x80000000UL) != 0U) {
    flags |= ZZ9K_M68K_CCR_V;
  }
  if (dst < src) {
    flags |= ZZ9K_M68K_CCR_C | ZZ9K_M68K_CCR_X;
  }
  state->sr &= (uint16_t)~(ZZ9K_M68K_CCR_X | ZZ9K_M68K_CCR_N |
                          ZZ9K_M68K_CCR_Z | ZZ9K_M68K_CCR_V |
                          ZZ9K_M68K_CCR_C);
  state->sr |= flags;
}

static int zz9k_m68k_set_fault(ZZ9KM68KResult *result,
                               int status,
                               uint32_t fault_pc,
                               uint16_t opcode)
{
  result->status = status;
  result->fault_pc = fault_pc;
  result->opcode = opcode;
  return status;
}

void zz9k_m68k_state_init(ZZ9KM68KState *state)
{
  if (state) {
    memset(state, 0, sizeof(*state));
  }
}

int zz9k_m68k_run(ZZ9KM68KState *state,
                  uint8_t *memory,
                  uint32_t memory_size,
                  uint32_t instruction_limit,
                  ZZ9KM68KResult *result)
{
  ZZ9KM68KResult local_result;

  if (!result) {
    result = &local_result;
  }
  memset(result, 0, sizeof(*result));
  if (!state || !memory) {
    return zz9k_m68k_set_fault(
        result, ZZ9K_M68K_STATUS_BAD_ARGUMENT, 0U, 0U);
  }
  while (1) {
    uint32_t pc = state->pc;
    uint16_t opcode;

    if (result->instructions >= instruction_limit) {
      result->status = ZZ9K_M68K_STATUS_LIMIT;
      return result->status;
    }
    if (!zz9k_m68k_fetch_word(state, memory, memory_size, &opcode)) {
      return zz9k_m68k_set_fault(
          result, ZZ9K_M68K_STATUS_OOB, pc, 0U);
    }

    if ((opcode & 0xf100U) == 0x7000U) {
      uint32_t reg = (opcode >> 9) & 7U;
      int8_t imm = (int8_t)(uint8_t)opcode;

      state->d[reg] = (uint32_t)(int32_t)imm;
      zz9k_m68k_set_logic_flags(state, state->d[reg]);
    } else if ((opcode & 0xf0c0U) == 0x5080U &&
               ((opcode >> 3) & 7U) == 0U) {
      uint32_t reg = opcode & 7U;
      uint32_t quick = (opcode >> 9) & 7U;
      uint32_t dst = state->d[reg];
      uint32_t value;

      if (quick == 0U) {
        quick = 8U;
      }
      if ((opcode & 0x0100U) != 0U) {
        value = dst - quick;
        state->d[reg] = value;
        zz9k_m68k_set_sub_flags(state, quick, dst, value);
      } else {
        value = dst + quick;
        state->d[reg] = value;
        zz9k_m68k_set_add_flags(state, quick, dst, value);
      }
    } else if ((opcode & 0xf1f8U) == 0xd018U) {
      uint32_t data_reg = (opcode >> 9) & 7U;
      uint32_t addr_reg = opcode & 7U;
      uint8_t src;
      uint8_t dst;
      uint8_t value;

      if (!zz9k_m68k_read_u8(memory, memory_size, state->a[addr_reg],
                             &src)) {
        return zz9k_m68k_set_fault(
            result, ZZ9K_M68K_STATUS_OOB, pc, opcode);
      }
      state->a[addr_reg]++;
      dst = (uint8_t)state->d[data_reg];
      value = (uint8_t)(dst + src);
      state->d[data_reg] = (state->d[data_reg] & 0xffffff00UL) |
                           (uint32_t)value;
      zz9k_m68k_set_add_byte_flags(state, src, dst, value);
    } else if ((opcode & 0xf1f8U) == 0x10d8U) {
      uint32_t src_reg = opcode & 7U;
      uint32_t dst_reg = (opcode >> 9) & 7U;
      uint8_t value;

      if (!zz9k_m68k_read_u8(memory, memory_size, state->a[src_reg],
                             &value) ||
          !zz9k_m68k_write_u8(memory, memory_size, state->a[dst_reg],
                              value)) {
        return zz9k_m68k_set_fault(
            result, ZZ9K_M68K_STATUS_OOB, pc, opcode);
      }
      state->a[src_reg]++;
      state->a[dst_reg]++;
      zz9k_m68k_set_logic_byte_flags(state, value);
    } else if ((opcode & 0xfff8U) == 0x23c0U) {
      uint32_t reg = opcode & 7U;
      uint32_t address;

      if (!zz9k_m68k_fetch_long(state, memory, memory_size, &address) ||
          !zz9k_m68k_write_be32(memory, memory_size, address,
                                state->d[reg])) {
        return zz9k_m68k_set_fault(
            result, ZZ9K_M68K_STATUS_OOB, pc, opcode);
      }
    } else if ((opcode & 0xf1ffU) == 0x2039U) {
      uint32_t reg = (opcode >> 9) & 7U;
      uint32_t address;

      if (!zz9k_m68k_fetch_long(state, memory, memory_size, &address) ||
          !zz9k_m68k_read_be32(memory, memory_size, address,
                               &state->d[reg])) {
        return zz9k_m68k_set_fault(
            result, ZZ9K_M68K_STATUS_OOB, pc, opcode);
      }
      zz9k_m68k_set_logic_flags(state, state->d[reg]);
    } else if (opcode == 0x4e75U) {
      uint32_t return_pc;

      if (!zz9k_m68k_read_be32(memory, memory_size, state->a[7],
                               &return_pc)) {
        return zz9k_m68k_set_fault(
            result, ZZ9K_M68K_STATUS_OOB, pc, opcode);
      }
      state->a[7] += 4U;
      state->pc = return_pc;
      result->instructions++;
      result->status = ZZ9K_M68K_STATUS_RTS;
      return result->status;
    } else if ((opcode & 0xfff8U) == 0x51c8U) {
      uint32_t reg = opcode & 7U;
      uint16_t displacement_word;
      int16_t displacement;
      uint32_t upper = state->d[reg] & 0xffff0000UL;
      uint16_t counter;

      if (!zz9k_m68k_fetch_word(state, memory, memory_size,
                                &displacement_word)) {
        return zz9k_m68k_set_fault(
            result, ZZ9K_M68K_STATUS_OOB, pc, opcode);
      }
      displacement = (int16_t)displacement_word;
      counter = (uint16_t)((state->d[reg] - 1U) & 0xffffU);
      state->d[reg] = upper | (uint32_t)counter;
      if (counter != 0xffffU) {
        state->pc = (uint32_t)((int32_t)state->pc + (int32_t)displacement);
      }
    } else {
      state->pc = pc;
      return zz9k_m68k_set_fault(
          result, ZZ9K_M68K_STATUS_ILLEGAL, pc, opcode);
    }

    result->instructions++;
  }
}
