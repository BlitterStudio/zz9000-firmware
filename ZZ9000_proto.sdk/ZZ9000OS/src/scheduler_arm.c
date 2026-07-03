/*
 * ZZ9000 dual-core task scheduler -- ARM-only glue.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file carries the Cortex-A9 / Xilinx-BSP integration that cannot be
 * host-tested: SCU/SMP coherency bring-up, the coherent shared control block,
 * the core-1 worker, and the core-0 poll. The portable state machine it drives
 * lives in scheduler.c (host-tested). Compiled out of the host test build.
 */
#ifndef TASKQ_HOST_TEST

#include "scheduler.h"
#include "memorymap.h"
#include "xil_types.h"
#include "xil_mmu.h"
#include "xil_cache.h"
#include "xpseudo_asm.h"
#include "xreg_cortexa9.h"

/* BSP flat 1 MB-section translation table (translation_table.S). */
extern u32 MMUTable;

/* TTBR0 page-table-walk attributes: outer cacheable write-back, matching the
 * value boot.S programs on core 0 (0x5B). */
#define TASKQ_TTBR0_ATTR 0x5BU

static void taskq_set_smp_bit(void)
{
  /* ACTLR (CP15 c1,c0,1): bit 6 = SMP (participate in SCU coherency),
   * bit 0 = cache/TLB maintenance broadcast. Set before enabling the D-cache. */
  u32 actlr = mfcp(XREG_CP15_AUX_CONTROL);
  actlr |= (1U << 6) | (1U << 0);
  mtcp(XREG_CP15_AUX_CONTROL, actlr);
  dsb();
  isb();
}

void scheduler_coherency_init_core0(void)
{
  /* Core 0 already runs with MMU + D-cache + SMP (BSP boot.S). Only mark the
   * task-queue region Normal, Inner-Shareable, cacheable write-back in the
   * shared translation table so the SCU keeps it coherent with core 1. This
   * runs at boot before core 1 is started. */
  Xil_SetTlbAttributes(SDK_TASKQ_REGION_ADDRESS, NORM_WB_CACHE);
}

void scheduler_coherency_init_core1(void)
{
  /* Core 1 is entered straight from the CPU1 reset vector (core1_loop),
   * bypassing boot.S, so it starts with MMU + caches + SMP OFF. Bring it up to
   * match core 0 so cross-core LDREX/STREX and SCU coherency work:
   *   1. point TTBR0 at the shared translation table (same table as core 0),
   *   2. set the domain access control (all domains manager),
   *   3. set ACTLR.SMP before enabling the D-cache,
   *   4. enable MMU + D-cache (Xil_EnableMMU invalidates I/D caches first),
   *   5. refresh the shareable attribute so this core's TLB reflects it.
   * The region was already marked shareable by scheduler_coherency_init_core0()
   * at boot, before this core started. */
  u32 ttbr0 = (u32)(UINTPTR)&MMUTable | TASKQ_TTBR0_ATTR;

  mtcp(XREG_CP15_TTBR0, ttbr0);
  mtcp(XREG_CP15_DOMAIN_ACCESS_CTRL, 0xFFFFFFFFU); /* all domains: manager */
  dsb();
  isb();

  taskq_set_smp_bit();

  Xil_EnableMMU();                                 /* enable MMU + D-cache */

  Xil_SetTlbAttributes(SDK_TASKQ_REGION_ADDRESS, NORM_WB_CACHE);
}

#endif /* TASKQ_HOST_TEST */
