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
#include "sdk_mailbox.h"
#include "xil_types.h"
#include "xil_mmu.h"
#include "xil_cache.h"
#include "xpseudo_asm.h"
#include "xreg_cortexa9.h"

/* BSP flat 1 MB-section translation table (translation_table.S). */
extern u32 MMUTable;

/* Compile-time guard: the crypto opcodes the scheduler policy mirrors in
 * scheduler.h must match the firmware's authoritative definitions. */
typedef char sched_opcode_drift_check[
    (TASKQ_OP_CRYPTO_HASH   == SDK_OP_CRYPTO_HASH   &&
     TASKQ_OP_CRYPTO_STREAM == SDK_OP_CRYPTO_STREAM &&
     TASKQ_OP_CRYPTO_AEAD   == SDK_OP_CRYPTO_AEAD   &&
     TASKQ_OP_CRYPTO_KX     == SDK_OP_CRYPTO_KX     &&
     TASKQ_OP_CRYPTO_VERIFY == SDK_OP_CRYPTO_VERIFY) ? 1 : -1];

/* Core-0 view of scheduler run state. The taskq watchdog trips core 1 off
 * after repeated faults; g_core1_started is set by the worker on entry. */
static taskq_watchdog_t g_sched_watchdog;
static volatile uint32_t g_core1_started;

taskq_shared_t *scheduler_shared(void)
{
  return (taskq_shared_t *)SDK_TASKQ_REGION_ADDRESS;
}

void scheduler_boot_init(void)
{
  taskq_shared_t *sh = scheduler_shared();
  taskq_init(&sh->queue);
  sh->core1_current_slot = -1;
  sh->core1_restart_request = 0;
  sh->core1_fault_code = 0;
  sh->core1_alive = 0;
  taskq_watchdog_init(&g_sched_watchdog, 3u);
  g_core1_started = 0;
}

int scheduler_core1_available(void)
{
  return (g_core1_started != 0 &&
          taskq_watchdog_core1_enabled(&g_sched_watchdog)) ? 1 : 0;
}

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

/*
 * Run one claimed task's compute on the calling core and mark it DONE.
 *
 * The compute (sdk_mailbox_run_crypto_task) owns all data-buffer cache
 * maintenance, so this runs correctly on whichever core executes it: core 1 in
 * the normal async path, or core 0 on the inline/fallback and SHORT-drain paths
 * (scheduler_core0_poll). A crypto op that ran to completion -- including a
 * legitimate failure such as an AEAD auth mismatch -- is a DONE task carrying
 * its exact SDK_STATUS_*; only a core-1 hardware fault (handled in core2.c)
 * marks a slot FAILED. The completion payload is always one
 * SDKCryptoResultPayload (== TASKQ_RESULT_PAYLOAD bytes) on success, none on
 * error, matching the pre-scheduler handlers byte-for-byte.
 */
static uint16_t scheduler_run_slot(taskq_desc_t *d)
{
  taskq_shared_t *sh = scheduler_shared();
  uint8_t payload[TASKQ_RESULT_PAYLOAD];
  int slot = (int)(d - sh->queue.descs);
  uint16_t status = sdk_mailbox_run_crypto_task((uint16_t)d->opcode,
                                                d->op_params, payload);
  uint16_t len = (status == SDK_STATUS_OK) ? (uint16_t)TASKQ_RESULT_PAYLOAD : 0u;
  taskq_complete(&sh->queue, slot, status, payload, len);
  return status;
}

/*
 * Core-1 dedicated worker. Entered from core1_loop (core2.c) after coherency
 * bring-up; never returns. Publishes core1_alive in the coherent shared block
 * as the boot-start confirmation core 0 waits on (scheduler_core1_available),
 * then claims and runs tasks. Idles on WFE; the core-0 enqueue path issues SEV
 * after a release-store so a sleeping worker wakes promptly. core1_current_slot
 * is set around execution so a fault on core 1 can attribute the in-flight slot
 * (core2.c fault path). The worker touches only the task queue and the task's
 * resolved data buffers -- never Zorro, the mailbox, the doorbell, or display.
 */
void scheduler_core1_worker(void)
{
  taskq_shared_t *sh = scheduler_shared();

  sh->core1_alive = 1;
  dmb();  /* make the alive flag observable to core 0 before we sleep on WFE */

  for (;;) {
    int slot = taskq_claim_any(&sh->queue);
    if (slot < 0) {
      __asm__ __volatile__("wfe" ::: "memory");
      continue;
    }
    sh->core1_current_slot = slot;
    (void)scheduler_run_slot(&sh->queue.descs[slot]);
    sh->core1_current_slot = -1;
  }
}

#endif /* TASKQ_HOST_TEST */
