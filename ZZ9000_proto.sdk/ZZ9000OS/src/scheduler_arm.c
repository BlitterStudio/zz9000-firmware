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
#include "sdk_image_stream.h"
#include "sdk_video_stream.h"
#include "sdk_media_session.h"
#include "core2.h"
#include "sleep.h"
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

/* Same drift guard for the decompress opcode. */
typedef char sched_decompress_opcode_drift_check[
    (TASKQ_OP_DECOMPRESS == SDK_OP_DECOMPRESS) ? 1 : -1];

/* Same drift guard for the image- and audio-service opcodes. */
typedef char sched_image_opcode_drift_check[
    (TASKQ_OP_SCALE_IMAGE == SDK_OP_SCALE_IMAGE &&
     TASKQ_OP_SCALE_IMAGE_CLIPPED == SDK_OP_SCALE_IMAGE_CLIPPED &&
     TASKQ_OP_DECODE_JPEG == SDK_OP_DECODE_JPEG &&
     TASKQ_OP_DECODE_MP3 == SDK_OP_DECODE_MP3 &&
     TASKQ_OP_IMAGE_SESSION_FEED == SDK_OP_IMAGE_SESSION_FEED &&
     TASKQ_OP_IMAGE_SESSION_CLOSE == SDK_OP_IMAGE_SESSION_CLOSE &&
     TASKQ_OP_AUDIO_STREAM_FEED == SDK_OP_AUDIO_STREAM_FEED &&
     TASKQ_OP_AUDIO_STREAM_READ == SDK_OP_AUDIO_STREAM_READ &&
     TASKQ_OP_VIDEO_SESSION_WRITE == SDK_OP_VIDEO_SESSION_WRITE &&
     TASKQ_OP_VIDEO_SESSION_DECODE == SDK_OP_VIDEO_SESSION_DECODE &&
     TASKQ_OP_VIDEO_SESSION_CLOSE == SDK_OP_VIDEO_SESSION_CLOSE &&
     TASKQ_OP_MEDIA_SESSION_WRITE == SDK_OP_MEDIA_SESSION_WRITE &&
     TASKQ_OP_MEDIA_SESSION_DECODE == SDK_OP_MEDIA_SESSION_DECODE &&
     TASKQ_OP_MEDIA_SESSION_CLOSE == SDK_OP_MEDIA_SESSION_CLOSE) ? 1 : -1];

/* The image-session table shares the coherent region with the queue
 * control block; keep them from overlapping. */
typedef char sched_sessions_offset_check[
    (sizeof(taskq_shared_t) <= SDK_IMAGE_SESSIONS_OFFSET) ? 1 : -1];

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
  sh->tasks_on_core1 = 0;   /* observability counters live in uninitialised DDR */
  sh->tasks_on_core0 = 0;   /* until zeroed here -- must not read back as garbage */
  taskq_watchdog_init(&g_sched_watchdog, 3u);
  g_core1_started = 0;
}

int scheduler_core1_available(void)
{
  return (g_core1_started != 0 &&
          taskq_watchdog_core1_enabled(&g_sched_watchdog)) ? 1 : 0;
}

/* Bounded wait for core 1's worker to come up, ~100 ms total. */
#define SCHED_CORE1_BOOT_SPINS   1000u
#define SCHED_CORE1_BOOT_US       100u

void scheduler_confirm_core1_boot(void)
{
  taskq_shared_t *sh = scheduler_shared();
  uint32_t spins;

  /*
   * Core 0 calls this once, after arm_app_init() has launched core 1. The
   * worker publishes core1_alive in the coherent shared block on entry
   * (scheduler_core1_worker); wait a bounded time for it. Only once seen do we
   * set g_core1_started, which gates scheduler_core1_available() and hence the
   * whole async offload path. If core 1 never checks in (absent, or wedged
   * before the worker), we stay in single-core mode and every task runs inline.
   */
  for (spins = 0; spins < SCHED_CORE1_BOOT_SPINS; spins++) {
    if (sh->core1_alive != 0) {
      g_core1_started = 1;
      return;
    }
    usleep(SCHED_CORE1_BOOT_US);
  }
  /* timeout: leave g_core1_started = 0 -> single-core fallback */
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

  /* boot.S enables the I-cache on core 0 (SCTLR = M|C|I), but core 1 enters
   * from its reset vector and bypasses boot.S; Xil_EnableMMU() sets only
   * M|C. Without SCTLR.I every instruction fetch goes to L2/DDR -- measured
   * ~10-30x slowdown on tight decode loops (LZH batch chunks blew the 68k's
   * 5 s sync wait). L1-only variant on purpose: Xil_ICacheEnable() would
   * also re-run the shared PL310 L2 enable that core 0 already performed. */
  Xil_L1ICacheEnable();

  Xil_SetTlbAttributes(SDK_TASKQ_REGION_ADDRESS, NORM_WB_CACHE);
}

/*
 * Run one claimed task's compute on the calling core and mark it DONE. This is
 * the scheduler's compute-dispatch seam: the queue/worker/harvest machinery is
 * opcode-agnostic, and this function hands the task to the opcode-dispatched
 * offload executor, sdk_mailbox_run_offload_task (sdk_mailbox.c), which routes
 * it to its service handler. Today that executor wires only the crypto
 * service (sdk_mailbox_run_crypto_task); later phases extend its dispatch to
 * decompression, image, and MP3 offload without changing this function.
 *
 * The service handler owns all data-buffer cache maintenance, so this runs
 * correctly on whichever core executes it: core 1 in the normal async path, or
 * core 0 on the inline/fallback and SHORT-drain paths (scheduler_core0_poll). A
 * task that ran to completion -- including a legitimate failure such as an AEAD
 * auth mismatch -- is a DONE task carrying its exact SDK_STATUS_*; only a
 * core-1 hardware fault (handled in core2.c) marks a slot FAILED. For crypto the
 * completion payload length is decided inside sdk_mailbox_run_offload_task:
 * one SDKCryptoResultPayload (== TASKQ_RESULT_PAYLOAD bytes) on success, none
 * on error, matching the pre-scheduler handlers byte-for-byte.
 */
static uint16_t scheduler_run_slot(taskq_desc_t *d)
{
  taskq_shared_t *sh = scheduler_shared();
  uint8_t payload[TASKQ_RESULT_PAYLOAD];
  uint32_t len = 0;
  int slot = (int)(d - sh->queue.descs);
  uint16_t status = sdk_mailbox_run_offload_task(d, payload, &len);
  taskq_complete(&sh->queue, slot, status, payload, (uint16_t)len);
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
    sh->tasks_on_core1++;   /* proof: this core executed a crypto task */
  }
}

/*
 * Core-0 poll, called once per main-loop iteration. The queue, worker, harvest,
 * and completion posting here are opcode-agnostic -- they carry whatever task
 * the worker ran. Phase 1 wires only the crypto service to the queue (see
 * scheduler_run_slot); later phases add image/compression and MP3 offload
 * without changing this machinery. Three jobs:
 *   1. Harvest tasks the worker finished (DONE/FAILED) and post their deferred
 *      completions to the Amiga via sdk_mailbox_post_deferred. A DONE task
 *      carries its exact result status; a FAILED task (core-1 fault) posts the
 *      generic SDK_STATUS_INTERNAL_ERROR so the Amiga falls back to software.
 *      Each successful DONE clears the fault watchdog. If the completion ring
 *      is full the task stays harvested and is retried on the next poll.
 *   2. Service a core-1 fault: if the worker faulted it marked its in-flight
 *      slot FAILED (harvested above) and set core1_restart_request. Trip the
 *      watchdog; while it still has core 1 enabled, cold-restart the core. Once
 *      the watchdog trips (too many consecutive faults) leave core 1 down and
 *      fall to single-core mode.
 *   3. Opportunistically drain one SHORT task inline in idle windows -- only
 *      when core 1 is enabled and neither a Zorro register request nor display
 *      work is pending this iteration (taskq_should_drain). This soaks up
 *      latency-sensitive work without stealing cycles from Amiga-facing I/O.
 */
void scheduler_core0_poll(int zorro_pending, int display_pending)
{
  taskq_shared_t *sh = scheduler_shared();
  int slot;

  while ((slot = taskq_harvest(&sh->queue)) >= 0) {
    taskq_desc_t *d = &sh->queue.descs[slot];
    if (!sdk_mailbox_task_gen_ok(slot)) {
      /* Task submitted under a previous mailbox lifetime (the mailbox was
       * re-initialised while it was in flight). Drop it without posting so its
       * stale request_id/user_cookie never lands in the new completion ring. */
      taskq_release(&sh->queue, slot);
      continue;
    }
    int failed = (d->state == TASK_FAILED);
    uint16_t status = failed ? SDK_STATUS_INTERNAL_ERROR : d->result_status;
    if (!sdk_mailbox_post_deferred(d->request_id, d->user_cookie,
                                   (uint16_t)d->opcode, status,
                                   d->result_payload, d->result_len)) {
      break;  /* completion ring full -> retry next poll */
    }
    if (!failed) {
      taskq_watchdog_on_success(&g_sched_watchdog);
    }
    taskq_release(&sh->queue, slot);
  }

  if (sh->core1_restart_request) {
    sh->core1_restart_request = 0;
    dmb();  /* publish the clear before the reset re-enters the worker */
    taskq_watchdog_on_fault(&g_sched_watchdog);
    /* Core-1-affine image sessions lost their codec heap objects with the
     * fault (cold_restart's reclaim frees them; on permanent disable they
     * are parked-unreachable). Drop the dangling references in BOTH
     * branches so core 0 never destroys against reclaimed memory. Audio
     * streams hold no heap objects, but their embedded decoder state may
     * be mid-frame; mark them faulted so feeds/reads fail cleanly. */
    sdk_image_stream_poison_core1_sessions();
    sdk_video_stream_poison_core1_sessions();
    sdk_media_session_poison_core1();
    sdk_mailbox_poison_core1_audio_streams();
    if (taskq_watchdog_core1_enabled(&g_sched_watchdog)) {
      core1_cold_restart();
    } else {
      g_core1_started = 0;  /* give up on core 1 -> single-core fallback */
    }
  }

  if (!scheduler_core1_available()) {
    /*
     * Single-core fallback: core 1 is absent or the watchdog disabled it. New
     * crypto/decompress requests already compute inline in the front
     * (crypto_dispatch, decompress_dispatch) or return BUSY
     * (handle_decompress_batch), so the queue only holds tasks stranded when
     * core 1 died mid-flight. Drain them ALL (claim_any, not claim_short) so
     * nothing is left QUEUED forever -- but TASK_LONG and TASK_SHORT are NOT
     * handled the same way here. core 0 IS the main loop: it must keep
     * answering every 68k register-window bus cycle (the FPGA withholds
     * DTACK until the ARM responds -- no timeout) and kicking the ~12.9s
     * whole-PS watchdog (main.c:415), so a stranded TASK_LONG task (a
     * batched or whole-file LZH decode) must NEVER run inline here -- that is
     * the exact main-loop stall/hard-crash this branch fixes. Fail it
     * instead, the same way core2.c's fault path fails an in-flight slot on
     * a core-1 exception (taskq_fail, core2.c:121); the harvest loop above
     * already maps every FAILED task to SDK_STATUS_INTERNAL_ERROR, so the
     * 68k client falls back to its per-member/software decode path.
     * TASK_SHORT (crypto) is sub-millisecond, so it is still safe to run
     * inline on the main loop.
     */
    while ((slot = taskq_claim_any(&sh->queue)) >= 0) {
      taskq_desc_t *d = &sh->queue.descs[slot];
      if (d->cls == (uint32_t)TASK_LONG) {
        taskq_fail(&sh->queue, slot, 0);
      } else {
        (void)scheduler_run_slot(d);
        sh->tasks_on_core0++;
      }
    }
  } else if (taskq_should_drain(zorro_pending, display_pending, 1)) {
    /* Opportunistic SHORT drain in idle windows while core 1 is healthy. */
    slot = taskq_claim_short(&sh->queue);
    if (slot >= 0) {
      (void)scheduler_run_slot(&sh->queue.descs[slot]);
      sh->tasks_on_core0++;
    }
  }
}

/* Bounded quiesce wait: crypto tasks are sub-millisecond, so 5000 * 10us = 50ms
 * is far beyond a normal drain. If a task is still active when the cap expires
 * it is either wedged or a legitimately long op mid-write -- either way core 1
 * is force-reset (see below) rather than left to race the mailbox teardown. */
#define SCHED_QUIESCE_SPINS  5000u
#define SCHED_QUIESCE_US       10u

/*
 * Quiesce the scheduler before a mailbox teardown (sdk_mailbox_init on Amiga
 * reset / firmware-update). Core 1's worker writes each task's result into the
 * task's resolved data buffers; if the mailbox frees and re-hands-out those
 * buffers while a task is still in flight, the late write corrupts the new
 * owner even though the stale completion is dropped by the generation tag. Wait
 * for every queued/in-flight task to finish, then re-init the queue so no task
 * survives into the next mailbox lifetime.
 *
 * If a task outlives the bounded wait (a wedged worker, or a genuinely long
 * hash/stream/AEAD), we must NOT just reinit and free its buffers -- the worker
 * still owns those addresses and would write into memory the mailbox is about
 * to reuse. Cold-reset core 1 first: that halts the worker mid-op, guaranteeing
 * no write survives into the next lifetime. The worker re-enters core1_loop and
 * parks on the now-empty queue; core 0's next scheduler_boot path / poll brings
 * it back. Draining is the fast path; the reset is the safety net.
 *
 * Race-free without a worker handshake: this runs on core 0's main loop, the
 * sole task producer, so once the queue reads empty no new task can appear.
 */
void scheduler_quiesce_for_reset(void)
{
  taskq_shared_t *sh = scheduler_shared();
  uint32_t spins;
  int timed_out;

  if (!scheduler_core1_available())
    return;   /* no core-1 worker -> no in-flight writes to drain */

  for (spins = 0; spins < SCHED_QUIESCE_SPINS; spins++) {
    if (!taskq_has_active(&sh->queue))
      break;
    usleep(SCHED_QUIESCE_US);
  }

  /* Clear the queue BEFORE any restart, so a restarted worker finds no QUEUED
   * descriptor to claim. Capture the timeout first -- taskq_init would mask it.
   * taskq_init leaves zero QUEUED slots, and the worker only ever claims QUEUED,
   * so it cannot pick up a stale task and write into buffers the mailbox is
   * about to reuse. */
  timed_out = taskq_has_active(&sh->queue);
  taskq_init(&sh->queue);
  sh->core1_current_slot = -1;

  if (timed_out) {
    /* A task outlived the wait and is still writing its buffers. Cold-reset
     * core 1 to halt it before those buffers are freed. The queue is already
     * cleared above and core1_cold_restart flushes the whole D-cache before
     * releasing CPU1, so the restarted worker re-enters onto the empty queue. */
    core1_cold_restart();
    sh->core1_restart_request = 0;   /* consumed here; don't double-restart */
  }
}

#endif /* TASKQ_HOST_TEST */
