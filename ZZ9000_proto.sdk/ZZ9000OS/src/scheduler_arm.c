/*
 * ZZ9000 dual-core task scheduler -- ARM-only glue.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 *
 * This file carries the Cortex-A9 / Xilinx-BSP integration: SCU/SMP
 * coherency bring-up, the coherent shared control block, the core-1 worker,
 * and the core-0 poll. The portable state machine it drives lives in
 * scheduler.c (host-tested); a focused host seam covers worker initialization.

 */
#define SCHEDULER_CORE1_FALLBACK_BODY() do { \
  scheduler_core1_halt_before_recover(core1_halt, zzapp_recover_halted_core1); \
  sdk_compression_reclaim_core1_decode(); \
  zzapp_services_reclaim_halted_audio(); \
  zzm_runtime_quarantine_module(ZZM_MODULE_MINIGL); \
  g_core1_started = 0; \
} while (0)
#if defined(TASKQ_HOST_TEST) && \
    (defined(SCHEDULER_PUBLISH_HOST_TEST) || defined(SCHEDULER_CLEAN_HOST_TEST))

#include "scheduler.h"

static taskq_shared_t *publish_shared;
static uint32_t publish_doorbells;

static void scheduler_publish_host_doorbell(void *unused)
{
  (void)unused;
  publish_doorbells++;
}

void scheduler_core1_publish_host_test_bind(taskq_shared_t *shared)
{
  publish_shared = shared;
  publish_doorbells = 0U;
}

uint32_t scheduler_core1_publish_host_test_doorbells(void)
{
  return publish_doorbells;
}

static void scheduler_core1_wake(taskq_shared_t *shared)
{
  if (shared)
    (void)taskq_doorbell_request(&shared->service_pending,
                                 scheduler_publish_host_doorbell, 0);
}

void scheduler_core1_publish(void)
{
  scheduler_core1_publish_work(publish_shared, scheduler_core1_wake);
}
#if defined(SCHEDULER_CLEAN_HOST_TEST)
static taskq_shared_t *clean_shared;
static uint32_t clean_delay_us;
static uint32_t clean_elapsed_us;
static int clean_acknowledge;

static int scheduler_clean_host_available(void *unused)
{
  (void)unused;
  return 1;
}

static void scheduler_clean_host_publish(void *unused)
{
  (void)unused;
}
static void scheduler_clean_host_cache(void *unused)
{
  (void)unused;
}


static void scheduler_clean_host_wait(void *unused, uint32_t usecs)
{
  (void)unused;
  clean_elapsed_us += usecs;
  if (clean_acknowledge && clean_elapsed_us >= clean_delay_us &&
      clean_shared->core1_current_slot < 0 &&
      clean_shared->core1_clean_request) {
    clean_shared->core1_clean_request = 0U;
    clean_shared->core1_clean_done = 1U;
  }
}

void scheduler_core1_clean_host_test_bind(taskq_shared_t *shared,
                                          uint32_t delay_us, int acknowledge)
{
  clean_shared = shared;
  clean_delay_us = delay_us;
  clean_elapsed_us = 0U;
  clean_acknowledge = acknowledge;
}

uint32_t scheduler_core1_clean_host_test_elapsed(void)
{
  return clean_elapsed_us;
}

void scheduler_core1_service_clean_request(void)
{
  scheduler_core1_service_clean_request_with(
      clean_shared, scheduler_clean_host_cache, scheduler_clean_host_publish, 0);
}

int scheduler_core1_clean_dcache(void)
{
  return scheduler_core1_clean_dcache_wait(
      clean_shared, scheduler_clean_host_available, scheduler_clean_host_publish,
      scheduler_clean_host_wait, 0,
      SCHED_CORE1_CLEAN_WAIT_US / SCHED_CORE1_CLEAN_POLL_US);
}
#endif


#elif defined(TASKQ_HOST_TEST) && defined(SCHEDULER_ARM_FALLBACK_HOST_TEST)

#include "scheduler.h"
#include "zzm_format.h"

static volatile uint32_t g_core1_started;

extern void core1_halt(void);
extern void zzapp_recover_halted_core1(void);
extern void sdk_compression_reclaim_core1_decode(void);
extern void zzapp_services_reclaim_halted_audio(void);
extern void zzm_runtime_quarantine_module(uint32_t module);

static void scheduler_core1_fallback_recover(void)
{
  SCHEDULER_CORE1_FALLBACK_BODY();
}

void scheduler_core1_fallback_host_test(void)
{
  scheduler_core1_fallback_recover();
}

#elif defined(TASKQ_HOST_TEST) && defined(MINIGL_HOST_TEST)

#include "scheduler.h"
#include "scheduler_core1_minigl_init.h"

void scheduler_core1_host_test_init(taskq_shared_t *sh)
{
  scheduler_core1_minigl_init(sh);
}

#elif !defined(TASKQ_HOST_TEST)

#include "scheduler.h"
#include "memorymap.h"
#include "sdk_mailbox.h"
#include "sdk_compression.h"
#include "sdk_image_stream.h"
#include "sdk_video_stream.h"
#include "sdk_media_session.h"
#include "sdk_smp_lock.h"
#include "card_pool.h"
#include "l2_maint.h"
#include "surface_allocator.h"
#include "zzm_loader.h"
#include "zzm_minigl_host.h"
#include "core2.h"
#include "interrupt.h"
#include "sleep.h"
#include "xil_types.h"
#include "xil_mmu.h"
#include "xil_cache.h"
#include "xil_cache_l.h"
#include "xpseudo_asm.h"
#include "xreg_cortexa9.h"
#include "scheduler_core1_minigl_init.h"
#include "zzapp_as.h"
#include "zzapp_kernel.h"
#include "zzapp_runtime.h"
#include "zzapp_services.h"
#include "zzm_runtime.h"
#include <stdio.h>
#if ZZAPP_PERF_COUNTERS
#include "xtime_l.h"
#endif

#define SCHED_CORE1_DOORBELL_SGI 15U
#define SCHED_CORE1_DOORBELL_CPU 2U

/* The system thread keeps the established 1 MiB core-1 stack. */
static uint8_t scheduler_app_stacks[ZZAPP_KERNEL_APP_SLOTS]
                                   [ZZAPP_KERNEL_STACK_BYTES]
  __attribute__((aligned(8)));

static void scheduler_core1_reclaim_if_requested(taskq_shared_t *sh);

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
     TASKQ_OP_IMAGE_ANIMATION_FRAME_NEXT == SDK_OP_IMAGE_ANIMATION_FRAME_NEXT &&
     TASKQ_OP_IMAGE_ANIMATION_FRAME_PRESENT == SDK_OP_IMAGE_ANIMATION_FRAME_PRESENT &&
     TASKQ_OP_IMAGE_ANIMATION_FRAME_RETIRE == SDK_OP_IMAGE_ANIMATION_FRAME_RETIRE &&
     TASKQ_OP_IMAGE_ANIMATION_RESTART == SDK_OP_IMAGE_ANIMATION_RESTART &&
     TASKQ_OP_AUDIO_STREAM_FEED == SDK_OP_AUDIO_STREAM_FEED &&
     TASKQ_OP_AUDIO_STREAM_READ == SDK_OP_AUDIO_STREAM_READ &&
     TASKQ_OP_AUDIO_STREAM_CLOSE == SDK_OP_AUDIO_STREAM_CLOSE &&
     TASKQ_OP_VIDEO_SESSION_WRITE == SDK_OP_VIDEO_SESSION_WRITE &&
     TASKQ_OP_VIDEO_SESSION_DECODE == SDK_OP_VIDEO_SESSION_DECODE &&
     TASKQ_OP_VIDEO_SESSION_CLOSE == SDK_OP_VIDEO_SESSION_CLOSE &&
     TASKQ_OP_MEDIA_SESSION_WRITE == SDK_OP_MEDIA_SESSION_WRITE &&
     TASKQ_OP_MEDIA_SESSION_DECODE == SDK_OP_MEDIA_SESSION_DECODE &&
     TASKQ_OP_MEDIA_SESSION_AUDIO_READ ==
       SDK_OP_MEDIA_SESSION_AUDIO_READ &&
     TASKQ_OP_MEDIA_SESSION_CLOSE == SDK_OP_MEDIA_SESSION_CLOSE) ? 1 : -1];

/* The image-session table shares the coherent region with the queue
 * control block; keep them from overlapping. */
typedef char sched_sessions_offset_check[
    (sizeof(taskq_shared_t) <= SDK_IMAGE_SESSIONS_OFFSET) ? 1 : -1];

/* Core-0 view of scheduler run state. The taskq watchdog trips core 1 off
 * after repeated faults; g_core1_started is set by the worker on entry. */
static taskq_watchdog_t g_sched_watchdog;
static volatile uint32_t g_core1_started;

static void scheduler_core1_fallback_recover(void)
{
  SCHEDULER_CORE1_FALLBACK_BODY();
}

taskq_shared_t *scheduler_shared(void)
{
  return (taskq_shared_t *)SDK_TASKQ_REGION_ADDRESS;
}

void zzapp_recover_external_locks(void)
{
  /* One list, used by cold restart, the watchdog and the single-core
   * fallback, always before context retirement and teardown. */
  card_pool_reset_lock(&card_pool);
  l2_maint_reset_core1_lock();
  zzapp_services_reset_core1_lock();
  sdk_smp_lock_reset_malloc();
  sdk_image_stream_reset_locks();
  zzm_loader_reset_session_lock();
  zzapp_as_reset_lock();
  zzapp_mem_reset_live_lock();
  zzapp_mem_reconcile_halted();
  surface_allocator_reset_lock();
  zzm_runtime_reset_heap_lock();
}

void scheduler_boot_init(void)
{
  taskq_shared_t *sh = scheduler_shared();

  /* Doorbell and perf words live in uninitialised DDR. Clear them before
   * core 1 starts, including when performance counters are compiled out. */
  taskq_shared_boot_init(sh);
  zzapp_kernel_stack_paint((uint8_t *)(uintptr_t)SDK_CORE1_SYSTEM_STACK_BASE,
                           SDK_CORE1_SYSTEM_STACK_SIZE);
  {
    uint32_t slot;
    for (slot = 0; slot < ZZAPP_KERNEL_APP_SLOTS; slot++)
      zzapp_kernel_stack_paint(scheduler_app_stacks[slot],
                               sizeof(scheduler_app_stacks[slot]));
  }
  taskq_watchdog_init(&g_sched_watchdog, 3u);
  zzapp_boot_init();
  g_core1_started = 0;
}

int scheduler_core1_available(void)
{
  return (g_core1_started != 0 &&
          taskq_watchdog_core1_enabled(&g_sched_watchdog)) ? 1 : 0;
}

static void scheduler_send_doorbell(void *unused)
{
  (void)unused;
  dsb();
  (void)XScuGic_SoftwareIntr(interrupt_get_intc(),
                              SCHED_CORE1_DOORBELL_SGI,
                              SCHED_CORE1_DOORBELL_CPU);
  __asm__ __volatile__("sev" ::: "memory");
}

#if ZZAPP_PERF_COUNTERS
uint32_t scheduler_perf_stamp(void)
{
  XTime stamp;

  XTime_GetTime(&stamp);
  return (uint32_t)stamp;
}

uint32_t taskq_perf_enqueue_stamp(void)
{
  return scheduler_perf_stamp();
}

static void scheduler_perf_publish_doorbell(taskq_shared_t *sh)
{
  sh->perf_doorbell_stamp = scheduler_perf_stamp();
  (void)__atomic_add_fetch(&sh->perf_sgi_count, 1U, __ATOMIC_RELEASE);
}
#endif

static void scheduler_core1_wake(taskq_shared_t *sh)
{
#if ZZAPP_PERF_COUNTERS
  uint32_t expected = 0U;

  /* State 2 reserves the transition while core 0 publishes the timestamp.
   * Core 1 never services state 2; release state 1 publishes work and its
   * matching doorbell sample together. */
  if (!__atomic_compare_exchange_n(&sh->service_pending, &expected, 2U, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return;
  scheduler_perf_publish_doorbell(sh);
  __atomic_store_n(&sh->service_pending, 1U, __ATOMIC_RELEASE);
  scheduler_send_doorbell(NULL);
#else
  (void)taskq_doorbell_request(&sh->service_pending,
                               scheduler_send_doorbell, NULL);
#endif
}
void scheduler_core1_publish(void)
{
  scheduler_core1_publish_work(scheduler_shared(), scheduler_core1_wake);
}


/* The SGI is a doorbell, not a work body. Mark service due and let the
 * exception return path select the system thread before another app runs. */
static void scheduler_core1_doorbell_irq(void *unused)
{
  (void)unused;
  zzapp_kernel_service_due(zzapp_runtime_kernel());
}

void scheduler_core1_enable_doorbell(void)
{
  XScuGic *intc = interrupt_get_intc();

  (void)XScuGic_Connect(intc, SCHED_CORE1_DOORBELL_SGI,
                         (Xil_InterruptHandler)scheduler_core1_doorbell_irq,
                         NULL);
  XScuGic_Enable(intc, SCHED_CORE1_DOORBELL_SGI);
}

uint32_t scheduler_core1_stack_high_water(uint32_t slot)
{
  if (slot >= ZZAPP_KERNEL_APP_SLOTS)
    return zzapp_kernel_stack_used(
        (const uint8_t *)(uintptr_t)SDK_CORE1_SYSTEM_STACK_BASE,
        SDK_CORE1_SYSTEM_STACK_SIZE);
  return zzapp_kernel_stack_used(scheduler_app_stacks[slot],
                                 sizeof(scheduler_app_stacks[slot]));
}

void *scheduler_core1_app_stack_top(uint32_t slot)
{
  if (slot >= ZZAPP_KERNEL_APP_SLOTS)
    return NULL;
  return scheduler_app_stacks[slot] + sizeof(scheduler_app_stacks[slot]);
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

/*
 * Core-local MMU + D-cache enable for core 1. Xil_EnableMMU() is not safe
 * here: its Xil_DCacheInvalidate()/Xil_ICacheInvalidate() also invalidate
 * the shared PL310 L2 by way (xil_cache.c Xil_L2CacheInvalidate), which
 * discards every dirty L2 line core 0 holds at that moment -- core 0 keeps
 * running through core 1's boot and every cold restart. Core 0 enabled and
 * owns the L2; core 1 only needs its own L1s, TLB and branch predictor clean
 * of reset garbage before it turns translation and caching on.
 */
static void core1_enable_mmu_local(void)
{
  u32 sctlr;

  Xil_L1DCacheInvalidate();
  Xil_L1ICacheInvalidate();
  mtcp(XREG_CP15_INVAL_UTLB_UNLOCKED, 0U);
  mtcp(XREG_CP15_INVAL_BRANCH_ARRAY, 0U);
  dsb();
  isb();

  sctlr = mfcp(XREG_CP15_SYS_CONTROL);
  sctlr |= XREG_CP15_CONTROL_M_BIT | XREG_CP15_CONTROL_C_BIT;
  mtcp(XREG_CP15_SYS_CONTROL, sctlr);
  dsb();
  isb();
}

void scheduler_coherency_init_core1(void)
{
  /* Core 1 is entered straight from the CPU1 reset vector (core1_loop),
   * bypassing boot.S, so it starts with MMU + caches + SMP OFF. Bring it up to
   * match core 0. Unlike core 0, every domain is Client: app-table AP bits
   * must protect both DDR (domain 15) and the domain-0 device, FPGA and OCM
   * mappings, and domain 14 keeps the firmware-text L2 pages read-only. */
  u32 ttbr0 = (u32)(UINTPTR)zzapp_as_template() | TASKQ_TTBR0_ATTR;
  uint32_t contextidr = 0U;

  __asm__ __volatile__("mcr p15, 0, %0, c13, c0, 1\n\tisb"
                       : : "r" (contextidr) : "memory");

  mtcp(XREG_CP15_TTBR0, ttbr0);
  dsb();
  mtcp(XREG_CP15_INVAL_UTLB_UNLOCKED, 0U);
  mtcp(XREG_CP15_INVAL_BRANCH_ARRAY, 0U);
  mtcp(XREG_CP15_DOMAIN_ACCESS_CTRL, 0x55555555U);
  dsb();
  isb();

  taskq_set_smp_bit();

  core1_enable_mmu_local();                        /* enable MMU + D-cache */

  /* boot.S enables the I-cache on core 0 (SCTLR = M|C|I), but core 1 enters
   * from its reset vector and bypasses boot.S; core1_enable_mmu_local() sets
   * only M|C. Without SCTLR.I every instruction fetch goes to L2/DDR -- measured
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

  if (status == SDK_TASK_IN_PROGRESS)
    taskq_requeue(&sh->queue, slot);
  else
    taskq_complete(&sh->queue, slot, status, payload, (uint16_t)len);
  return status;
}

#if ZZAPP_PERF_COUNTERS

static void scheduler_perf_sync_doorbell(taskq_shared_t *sh,
					 zzapp_kernel_t *kernel)
{
	static uint32_t seen_sgis;
	static uint32_t seen_work_publications;
	uint32_t published = __atomic_load_n(&sh->perf_sgi_count,
					    __ATOMIC_ACQUIRE);
	uint32_t work_publications =
		__atomic_load_n(&sh->perf_work_publications, __ATOMIC_ACQUIRE);

	while (seen_sgis != published) {
		zzapp_kernel_perf_sgi_raised(kernel, sh->perf_doorbell_stamp);
		seen_sgis++;
	}
	zzapp_kernel_perf_work_published(kernel,
					 work_publications - seen_work_publications);
	seen_work_publications = work_publications;
}

static int scheduler_perf_consume_doorbell(taskq_shared_t *sh,
					   zzapp_kernel_t *kernel)
{
	if (__atomic_load_n(&sh->service_pending, __ATOMIC_ACQUIRE) == 2U)
		return 0;
	scheduler_perf_sync_doorbell(sh, kernel);
	if (__atomic_load_n(&sh->service_pending, __ATOMIC_ACQUIRE) == 2U)
		return 0;
	/* A producer may publish between the first acquire and this state
	 * observation. Re-acquire the count before any service work begins. */
	scheduler_perf_sync_doorbell(sh, kernel);
	zzapp_kernel_perf_sgi_started(kernel, scheduler_perf_stamp());
	return 1;
}

static zzapp_perf_service_t scheduler_perf_class(const taskq_desc_t *desc)
{
	uint32_t opcode = desc->opcode;

	if (opcode == TASKQ_OP_DECODE_MP3 || opcode == TASKQ_OP_AUDIO_STREAM_FEED ||
	    opcode == TASKQ_OP_AUDIO_STREAM_READ ||
	    opcode == TASKQ_OP_AUDIO_STREAM_CLOSE ||
	    opcode == TASKQ_OP_MEDIA_SESSION_AUDIO_READ)
		return ZZAPP_PERF_SERVICE_AUDIO;
	return ZZAPP_PERF_SERVICE_SDK;
}
#endif
/* Bounded service body used between app calls. It never sleeps: while an
 * app is live the next timer tick or cooperative yield returns here. */
void scheduler_core1_service_once(void)
{
  taskq_shared_t *sh = scheduler_shared();
  sched_worker_action_t action;
  int slot;

#if ZZAPP_PERF_COUNTERS
  if (!scheduler_perf_consume_doorbell(sh, zzapp_runtime_kernel()))
    return;
#endif
  slot = taskq_claim_any(&sh->queue);
  if (slot >= 0) {
#if ZZAPP_PERF_COUNTERS
    zzapp_perf_service_t service = scheduler_perf_class(&sh->queue.descs[slot]);
    uint32_t stamp = scheduler_perf_stamp();

    zzapp_kernel_perf_sdk_started(zzapp_runtime_kernel(),
                                  sh->queue.descs[slot].perf_enqueue_stamp,
                                  stamp);
    zzapp_kernel_perf_service_begin(zzapp_runtime_kernel(), service, stamp);
#endif
    sh->core1_current_slot = slot;
    (void)scheduler_run_slot(&sh->queue.descs[slot]);
    sh->core1_current_slot = -1;
    sh->tasks_on_core1++;
#if ZZAPP_PERF_COUNTERS
    zzapp_kernel_perf_service_end(zzapp_runtime_kernel(), service,
                                  scheduler_perf_stamp());
#endif
  }
  action = sh->minigl_reset_request != 0U ? SCHED_WORKER_ACTION_RESET :
                                           SCHED_WORKER_ACTION_MINIGL;
  if (scheduler_minigl_submission_action(sh, action) ==
      SCHED_WORKER_ACTION_MINIGL) {
#if ZZAPP_PERF_COUNTERS
    uint32_t stamp = scheduler_perf_stamp();
    zzapp_kernel_perf_service_begin(zzapp_runtime_kernel(),
                                    ZZAPP_PERF_SERVICE_MINIGL, stamp);
#endif
    sh->minigl_in_flight = 1;
    dmb();
    zzm_minigl_service_tick(0);
    sh->minigl_in_flight = 0;
    dmb();
#if ZZAPP_PERF_COUNTERS
    zzapp_kernel_perf_service_end(zzapp_runtime_kernel(),
                                  ZZAPP_PERF_SERVICE_MINIGL,
                                  scheduler_perf_stamp());
#endif
  }
}

static uint32_t scheduler_doorbell_drain(void *context, uint32_t batch)
{
  taskq_shared_t *sh = context;
  uint32_t done = 0;

  while (done < batch && taskq_has_active(&sh->queue)) {
    scheduler_core1_service_once();
    done++;
  }
  return done;
}

static int scheduler_doorbell_more(void *context)
{
  return taskq_has_active(&((taskq_shared_t *)context)->queue);
}

void scheduler_core1_service_from_app(void)
{
  taskq_shared_t *sh = scheduler_shared();

  /* One queued task per app tick, as before; the step also runs the MiniGL
   * tick. Without queued work only the MiniGL tick runs. */
  if (sh->service_pending &&
      taskq_doorbell_drain(&sh->service_pending, scheduler_doorbell_drain,
                           scheduler_doorbell_more, sh, 1U) != 0U)
    return;
  scheduler_core1_service_once();
}

static int scheduler_core1_kernel_lock_or_yield(void)
{
  zzapp_kernel_t *kernel = zzapp_runtime_kernel();

  if (zzapp_kernel_system_lock(kernel) == 0)
    return 1;
  /* No worker task is active while yielding an app-owned kernel lock, so
   * servicing a pending L1 clean cannot interrupt task-local state. */
  scheduler_core1_service_clean_request();
  zzapp_kernel_service_due(kernel);
  __asm__ __volatile__("svc #0" ::: "memory");
  return 0;
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

  for (;;) {
    if (scheduler_core1_kernel_lock_or_yield())
      break;
  }
  scheduler_core1_minigl_init(sh);
  sdk_mailbox_reset_jpeg_tasks();
  (void)zzapp_kernel_system_unlock(zzapp_runtime_kernel());

  for (;;) {
    int reset_req;
    int task_slot;
    int task_avail;
    int mgl_avail;
    int mgl_bound;
    int mgl_progress = 0;
    int apps_runnable;
    int should_wfe;
    uint32_t drained = 0U;
    sched_worker_action_t action;

    if (!scheduler_core1_kernel_lock_or_yield())
      continue;
    zzapp_kernel_set_current_thread(-1);
#if ZZAPP_PERF_COUNTERS
    if (!scheduler_perf_consume_doorbell(sh, zzapp_runtime_kernel())) {
      (void)zzapp_kernel_system_unlock(zzapp_runtime_kernel());
      continue;
    }
#endif
    if (sh->core1_park_request) {
      (void)zzapp_kernel_system_unlock(zzapp_runtime_kernel());
      scheduler_core1_park_if_requested();
      continue;
    }

    /* Checked before every claim: once core 0 asks, no further task runs. */
    scheduler_core1_reclaim_if_requested(sh);
    scheduler_core1_service_clean_request();
    if (sh->service_pending)
      drained = taskq_doorbell_drain(&sh->service_pending,
                                     scheduler_doorbell_drain,
                                     scheduler_doorbell_more, sh, 1U);
#if ZZAPP_PERF_COUNTERS
    zzapp_kernel_perf_service_begin(zzapp_runtime_kernel(),
                                    ZZAPP_PERF_SERVICE_LAUNCHER,
                                    scheduler_perf_stamp());
#endif
    zzapp_worker_hook();
#if ZZAPP_PERF_COUNTERS
    zzapp_kernel_perf_service_end(zzapp_runtime_kernel(),
                                  ZZAPP_PERF_SERVICE_LAUNCHER,
                                  scheduler_perf_stamp());
#endif
    if (drained != 0U) {
      (void)zzapp_kernel_system_unlock(zzapp_runtime_kernel());
#if ZZAPP_PERF_COUNTERS
      zzapp_kernel_perf_print_reports(zzapp_runtime_kernel());
#endif
      if (zzapp_kernel_apps_runnable(zzapp_runtime_kernel()))
        __asm__ __volatile__("svc #0" ::: "memory");
      continue;
    }

    reset_req = (sh->minigl_reset_request != 0);
    task_slot = reset_req ? -1 : taskq_claim_any(&sh->queue);
    task_avail = (task_slot >= 0);
    mgl_avail = zzm_minigl_has_work();
    mgl_bound = zzm_minigl_worker_registered();
    action = sched_worker_decide(reset_req, task_avail, mgl_avail, mgl_bound);
    action = scheduler_minigl_submission_action(sh, action);

    switch (action) {
    case SCHED_WORKER_ACTION_RESET: {
      /* A reset step can wait for a committed RasterIX fence. Publish that
       * interval so core 0's mailbox quiesce cannot reclaim its buffers. */
      int result;

      sh->minigl_in_flight = 1;
      dmb();
      result = zzm_minigl_reset_step();
      sh->minigl_in_flight = 0;
      dmb();
      if (result != ZZM_MINIGL_RESET_BUSY) {
        sh->minigl_reset_request = 0;
        sh->minigl_reset_ack = result == ZZM_MINIGL_RESET_FAILED ?
                               SCHED_MINIGL_RESET_ACK_FAILED :
                               SCHED_MINIGL_RESET_ACK_DONE;
        dmb();
      }
      break;
    }

    case SCHED_WORKER_ACTION_TASK: {
#if ZZAPP_PERF_COUNTERS
      zzapp_perf_service_t service =
          scheduler_perf_class(&sh->queue.descs[task_slot]);
      uint32_t stamp = scheduler_perf_stamp();

      zzapp_kernel_perf_sdk_started(zzapp_runtime_kernel(),
                                    sh->queue.descs[task_slot].perf_enqueue_stamp,
                                    stamp);
      zzapp_kernel_perf_service_begin(zzapp_runtime_kernel(), service, stamp);
#endif
      sh->core1_current_slot = task_slot;
      (void)scheduler_run_slot(&sh->queue.descs[task_slot]);
      sh->core1_current_slot = -1;
      sh->tasks_on_core1++;
#if ZZAPP_PERF_COUNTERS
      zzapp_kernel_perf_service_end(zzapp_runtime_kernel(), service,
                                    scheduler_perf_stamp());
#endif
      break;
    }

    case SCHED_WORKER_ACTION_MINIGL:
    case SCHED_WORKER_ACTION_POLL_IDLE: {
#if ZZAPP_PERF_COUNTERS
      uint32_t stamp = scheduler_perf_stamp();
      zzapp_kernel_perf_service_begin(zzapp_runtime_kernel(),
                                      ZZAPP_PERF_SERVICE_MINIGL, stamp);
#endif
      sh->minigl_in_flight = 1;
      dmb();
      mgl_progress = zzm_minigl_service_tick(0);
      sh->minigl_in_flight = 0;
      dmb();
#if ZZAPP_PERF_COUNTERS
      zzapp_kernel_perf_service_end(zzapp_runtime_kernel(),
                                    ZZAPP_PERF_SERVICE_MINIGL,
                                    scheduler_perf_stamp());
#endif
      break;
    }

    case SCHED_WORKER_ACTION_WFE:
    default:
      break;
    }
    should_wfe = sched_worker_may_wfe(action, mgl_progress,
                                      taskq_has_active(&sh->queue),
                                      sh->minigl_reset_request != 0,
                                      sh->minigl_doorbell != 0,
                                      zzm_minigl_worker_registered());
    /* An app parked on host data (a file reply, console input, a flip) is
     * woken by this loop's next service pass. The launcher's reply raises
     * no interrupt, so idling in WFE would add up to one 5 ms tick to every
     * request; keep polling while any wait is registered. */
    if (should_wfe) {
      uint32_t slot;

      for (slot = 0U; slot < ZZAPP_KERNEL_APP_SLOTS; slot++) {
        if (zzapp_services_wait_registered(slot)) {
          should_wfe = 0;
          break;
        }
      }
    }

    (void)zzapp_kernel_system_unlock(zzapp_runtime_kernel());
#if ZZAPP_PERF_COUNTERS
    zzapp_kernel_perf_print_reports(zzapp_runtime_kernel());
#endif
    /* A runnable app gets the core back at once. Idling in WFE while an
     * app could run left it waiting for the next 5 ms tick after every
     * service pass, which made swquake take minutes to load (kit 66). */
    apps_runnable = zzapp_kernel_apps_runnable(zzapp_runtime_kernel());
    if (apps_runnable)
      __asm__ __volatile__("svc #0" ::: "memory");
    else if (should_wfe) {
      zzapp_kernel_idle_begin(zzapp_runtime_kernel());
      __asm__ __volatile__("wfe" ::: "memory");
      zzapp_kernel_idle_end(zzapp_runtime_kernel());
    }
  }
}

void scheduler_request_minigl_reset(void)
{
  taskq_shared_t *sh = scheduler_shared();

  sh->minigl_reset_ack = 0;
  dmb();
  sh->minigl_reset_request = 1;
  dsb();
  __asm__ __volatile__("sev" ::: "memory");
}

void scheduler_cancel_minigl_reset(void)
{
  taskq_shared_t *sh = scheduler_shared();

  /* The restarted worker must not also run the reset. */
  sh->minigl_reset_request = 0;
  dsb();
  if (scheduler_core1_available())
    core1_cold_restart();
}


/*
 * Core 1 side of the reset-time reclaim. Core 0 asks only after the quiesce,
 * so no task is running and this core holds neither the malloc lock nor the
 * image decode-state lock: it frees its own tracked decode blocks exactly as
 * a cold restart's reclaim would, but with coherent caches and no CPU reset.
 */
static void scheduler_core1_reclaim_if_requested(taskq_shared_t *sh)
{
  if (!sh->core1_reclaim_request)
    return;
  sh->core1_reclaim_request = 0;
  (void)sdk_compression_reclaim_core1_decode();
  dsb();
  sh->core1_reclaim_done = 1;
  dsb();
  __asm__ __volatile__("sev" ::: "memory");
}

#define SCHED_RECLAIM_SPINS 2000U   /* x 10 us */
/* Clean runs only from the Amiga-reset pool handoff; keep it inside that
 * path's 150-ms reset budget without extending reclaim's normal deadline. */

int scheduler_core1_reclaim(void)
{
  taskq_shared_t *sh = scheduler_shared();
  uint32_t spins;

  if (!scheduler_core1_available() || !sh->core1_alive ||
      sh->core1_current_slot >= 0 || sh->core1_restart_request)
    return -1;
  sh->core1_reclaim_done = 0;
  sh->core1_reclaim_request = 1;
  dsb();
  __asm__ __volatile__("sev" ::: "memory");
  for (spins = 0U; spins < SCHED_RECLAIM_SPINS && !sh->core1_reclaim_done;
       spins++)
    usleep(10);
  if (sh->core1_reclaim_done)
    return 0;
  sh->core1_reclaim_request = 0;
  dsb();
  printf("[sched] core 1 did not reclaim; cold restart\n");
  return -1;
}

/* Core 1 cleans only after the shared helper has established that no worker
 * task is active and no restart is pending. */
static void scheduler_clean_cache(void *unused)
{
  (void)unused;
  Xil_L1DCacheFlush();
}

static int scheduler_clean_available(void *unused)
{
  (void)unused;
  return scheduler_core1_available();
}

static void scheduler_clean_publish(void *unused)
{
  (void)unused;
  dsb();
  __asm__ __volatile__("sev" ::: "memory");
}

void scheduler_core1_service_clean_request(void)
{
  scheduler_core1_service_clean_request_with(
      scheduler_shared(), scheduler_clean_cache, scheduler_clean_publish, NULL);
}

static void scheduler_clean_wait(void *unused, uint32_t usecs)
{
  (void)unused;
  usleep(usecs);
}

int scheduler_core1_clean_dcache(void)
{
  int result = scheduler_core1_clean_dcache_wait(
      scheduler_shared(), scheduler_clean_available, scheduler_clean_publish,
      scheduler_clean_wait, NULL,
      SCHED_CORE1_CLEAN_WAIT_US / SCHED_CORE1_CLEAN_POLL_US);

  if (result != 0)
    printf("[sched] core 1 did not clean its cache; cold restart\n");
  return result;
}

/*
 * Core 1 side of the cold-restart handshake (taskq_shared_t). Write back every
 * dirty L1 line -- heap metadata and decoder state allocated on this core --
 * so core 0 can free those blocks once this core is held in reset, then
 * acknowledge and stay parked. The L1-only clean is per-core (the PL310 L2 is
 * shared and already visible to core 0), so it cannot race core 0's own cache
 * maintenance.
 */
void scheduler_core1_park_if_requested(void)
{
  taskq_shared_t *sh = scheduler_shared();

  if (!sh->core1_park_request)
    return;
  Xil_L1DCacheFlush();
  dsb();
  sh->core1_parked = 1;
  dsb();
  __asm__ __volatile__("sev" ::: "memory");
  for (;;)
    __asm__ __volatile__("wfe" ::: "memory");
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
  zzapp_core0_poll();

  if (sh->core1_restart_request) {
    sh->core1_restart_request = 0;
    dmb();  /* publish the clear before the reset re-enters the worker */
    taskq_watchdog_on_fault(&g_sched_watchdog);
    /* Poison may take the card pool before either branch retires apps. */
    card_pool_reset_lock(&card_pool);
    sdk_image_stream_poison_core1_sessions();
    sdk_video_stream_poison_core1_sessions();
    sdk_media_session_poison_core1();
    sdk_mailbox_poison_core1_audio_streams();
    if (taskq_watchdog_core1_enabled(&g_sched_watchdog)) {
      core1_cold_restart();
    } else {
      /* Same order as a cold restart: every lock, then contexts, then
       * teardown. The helper owns that order; only non-lock reclaim
       * remains here because core 1 will not run it. */
      scheduler_core1_fallback_recover();
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

/*
 * A quiesce cap bounds reset while allowing MiniGL's 100-ms RasterIX drain
 * to complete before the scheduler cold-restarts core 1.
 */

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
void scheduler_resume_minigl_submissions(void)
{
  taskq_shared_t *sh = scheduler_shared();

  sh->minigl_submission_admission = 1U;
  dmb();
  sh->minigl_submission_ack = 0U;
  dmb();
  scheduler_core1_publish();
}

void scheduler_quiesce_for_reset(void)
{
  taskq_shared_t *sh = scheduler_shared();
  uint32_t spins;
  int timed_out;

  if (!scheduler_core1_available()) {
    sh->minigl_submission_admission = 0U;
    dmb();
    if (++sh->minigl_submission_generation == 0U)
      ++sh->minigl_submission_generation;
    sh->minigl_submission_ack = sh->minigl_submission_generation;
    return;
  }
  sh->minigl_submission_admission = 0U;
  dmb();
  if (++sh->minigl_submission_generation == 0U)
    ++sh->minigl_submission_generation;
  sh->minigl_submission_ack = 0U;
  dmb();
  scheduler_core1_publish();

  for (spins = 0; spins < SCHED_QUIESCE_SPINS; spins++) {
    if (scheduler_minigl_quiesce_done(sh))
      break;
    usleep(SCHED_QUIESCE_US);
  }

  /* Clear the queue BEFORE any restart, so a restarted worker finds no QUEUED
   * descriptor to claim. Capture the timeout first -- taskq_init would mask it.
   * taskq_init leaves zero QUEUED slots, and the worker only ever claims QUEUED,
   * so it cannot pick up a stale task and write into buffers the mailbox is
   * about to reuse. */
  timed_out = !scheduler_minigl_quiesce_done(sh);
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
