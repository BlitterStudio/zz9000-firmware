/*
 * Minimal 68k runner contract for opt-in offload experiments.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_M68K_RUNNER_H
#define ZZ9K_M68K_RUNNER_H

#include <stdint.h>

#define ZZ9K_M68K_CCR_C 0x0001U
#define ZZ9K_M68K_CCR_V 0x0002U
#define ZZ9K_M68K_CCR_Z 0x0004U
#define ZZ9K_M68K_CCR_N 0x0008U
#define ZZ9K_M68K_CCR_X 0x0010U

typedef enum ZZ9KM68KStatus {
  ZZ9K_M68K_STATUS_RTS = 0,
  ZZ9K_M68K_STATUS_LIMIT = 1,
  ZZ9K_M68K_STATUS_ILLEGAL = 2,
  ZZ9K_M68K_STATUS_OOB = 3,
  ZZ9K_M68K_STATUS_BAD_ARGUMENT = 4
} ZZ9KM68KStatus;

typedef struct ZZ9KM68KState {
  uint32_t d[8];
  uint32_t a[8];
  uint32_t pc;
  uint16_t sr;
} ZZ9KM68KState;

typedef struct ZZ9KM68KResult {
  int status;
  uint32_t instructions;
  uint32_t fault_pc;
  uint16_t opcode;
} ZZ9KM68KResult;

void zz9k_m68k_state_init(ZZ9KM68KState *state);

int zz9k_m68k_run(ZZ9KM68KState *state,
                  uint8_t *memory,
                  uint32_t memory_size,
                  uint32_t instruction_limit,
                  ZZ9KM68KResult *result);

#endif
