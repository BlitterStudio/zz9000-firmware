/*
 * ZZ9000 dual-core task scheduler -- Phase 0 coherency stress harness.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Built only when SCHED_STRESS_TEST is defined. Turns the whole firmware into a
 * two-core torture test that proves, on real silicon, that:
 *   - the SCU keeps the task-queue region coherent between the two cores,
 *   - the LDREX/STREX claim (taskq_cas_u32) works cross-core, including under
 *     contention (core 0 and core 1 both claim), and
 *   - the release/acquire ordering (taskq_store_release) is honoured.
 *
 * Protocol: core 0 stamps each descriptor with a pattern P
 * (in_addr = request_id = user_cookie = P) and enqueues it. A consumer (core 1
 * always, core 0 opportunistically) claims the slot, checks the three stamped
 * words agree (a coherency read check), writes P ^ 0xFFFFFFFF into the result
 * payload, and completes. Core 0 harvests and verifies the transform. Any
 * disagreement increments a mismatch counter reported over serial.
 *
 * GATE: zero mismatches over a long run (target: millions of claims).
 */
#ifndef TASKQ_HOST_TEST
#ifdef SCHED_STRESS_TEST

#include "scheduler.h"
#include "memorymap.h"
#include "watchdog.h"
#include <stdio.h>
#include <stdint.h>

#define STRESS_SHARED        ((taskq_shared_t *)SDK_TASKQ_REGION_ADDRESS)
#define STRESS_XFORM         0xFFFFFFFFU
#define STRESS_REPORT_CLAIMS 1000000UL
#define STRESS_HEARTBEAT_ROUNDS 50000000UL
#define STRESS_CORE0_DRAIN_BUDGET 4

static void stress_put_u32(volatile uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint32_t stress_get_u32(const volatile uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* A consumer runs one claimed slot: verify core 0's stamp, write the transform. */
static void stress_run_slot(taskq_shared_t *sh, int slot)
{
  taskq_desc_t *d = &sh->queue.descs[slot];
  uint8_t payload[4];
  uint32_t in = d->in_addr;
  uint16_t st = (d->request_id == in && d->user_cookie == in) ? 0 : 1;
  stress_put_u32(payload, in ^ STRESS_XFORM);
  taskq_complete(&sh->queue, slot, st, payload, 4);
}

void scheduler_stress_init(void)
{
  taskq_shared_t *sh = STRESS_SHARED;
  taskq_init(&sh->queue);
  sh->core1_current_slot = -1;
  sh->core1_restart_request = 0;
  sh->core1_fault_code = 0;
  sh->core1_alive = 0;
}

/* Core 1: sole dedicated consumer -- claim, verify, transform, complete. */
void scheduler_stress_core1(void)
{
  taskq_shared_t *sh = STRESS_SHARED;
  sh->core1_alive = 1;
  for (;;) {
    int slot = taskq_claim_any(&sh->queue);
    if (slot < 0) {
      continue;
    }
    stress_run_slot(sh, slot);
  }
}

/* Core 0: producer + opportunistic consumer + verifier + serial reporter. */
void scheduler_stress_core0(void)
{
  taskq_shared_t *sh = STRESS_SHARED;
  uint32_t round = 0, seq = 0;
  uint32_t claims = 0, mismatches = 0;
  uint32_t next_report = STRESS_REPORT_CLAIMS;
  uint32_t next_heartbeat = STRESS_HEARTBEAT_ROUNDS;

  printf("[stress] core0 producer starting; region=%p cap=%u\n",
         (void *)sh, (unsigned)TASKQ_CAPACITY);

  for (;;) {
    int slot;
    int budget;

    /* This harness owns core 0 and never returns to the main loop, so it must
     * service the private watchdog itself or the board resets every ~12.9 s.
     * If core 0 itself ever wedges, the watchdog still fires (intended). */
    watchdog_kick();

    /* Fill every currently-free slot with a fresh stamped pattern. */
    for (;;) {
      uint32_t p = taskq_stress_fill(round, seq);
      int s = taskq_enqueue(&sh->queue, 0u, TASK_SHORT, p, 0u, 0u, 0u, p, p, NULL);
      if (s < 0) {
        break;              /* queue full */
      }
      seq++;
    }

    /* Opportunistic core-0 drain: contend with core 1 on the CAS. */
    budget = STRESS_CORE0_DRAIN_BUDGET;
    while (budget-- > 0 && (slot = taskq_claim_short(&sh->queue)) >= 0) {
      stress_run_slot(sh, slot);
    }

    /* Harvest + verify everything a consumer finished. */
    while ((slot = taskq_harvest(&sh->queue)) >= 0) {
      taskq_desc_t *d = &sh->queue.descs[slot];
      uint32_t want = d->in_addr ^ STRESS_XFORM;
      uint32_t got = stress_get_u32(d->result_payload);
      if (d->result_status != 0 || got != want) {
        mismatches++;
      }
      claims++;
      taskq_release(&sh->queue, slot);
    }

    round++;
    if (claims >= next_report) {
      printf("[stress] rounds=%lu claims=%lu mismatches=%lu\n",
             (unsigned long)round, (unsigned long)claims,
             (unsigned long)mismatches);
      next_report += STRESS_REPORT_CLAIMS;
    } else if (round >= next_heartbeat) {
      printf("[stress] heartbeat rounds=%lu claims=%lu mismatches=%lu\n",
             (unsigned long)round, (unsigned long)claims,
             (unsigned long)mismatches);
      next_heartbeat += STRESS_HEARTBEAT_ROUNDS;
    }
  }
}

#endif /* SCHED_STRESS_TEST */
#endif /* TASKQ_HOST_TEST */
