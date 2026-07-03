/*
 * ZZ9000 dual-core task scheduler -- portable core interface.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This header and scheduler.c are deliberately free of xil_ and firmware
 * includes so the queue/claim/harvest/policy/watchdog state machine is unit-tested
 * off-target (system cc, -DTASKQ_HOST_TEST). ARM-only glue lives in
 * scheduler_arm.c / scheduler_stress.c.
 */
#ifndef ZZ9K_SCHEDULER_H
#define ZZ9K_SCHEDULER_H

#include <stdint.h>

#define TASKQ_CAPACITY         16u
#define TASKQ_SHORT_MAX_BYTES  4096u
#define TASKQ_RESULT_PAYLOAD   48u
#define TASKQ_OP_PARAM_BYTES   48u   /* opaque per-task input params (firmware) */

/* Crypto opcodes mirrored from sdk_mailbox.h. scheduler_arm.c carries
 * _Static_assert drift guards proving these match the firmware definitions. */
#define TASKQ_OP_CRYPTO_HASH   0x0800u
#define TASKQ_OP_CRYPTO_STREAM 0x0801u
#define TASKQ_OP_CRYPTO_AEAD   0x0802u
#define TASKQ_OP_CRYPTO_KX     0x0803u
#define TASKQ_OP_CRYPTO_VERIFY 0x0804u

typedef enum {
  TASK_FREE    = 0,
  TASK_QUEUED  = 1,
  TASK_CLAIMED = 2,
  TASK_DONE    = 3,
  TASK_FAILED  = 4
} taskq_state_t;

typedef enum {
  TASK_SHORT = 0,
  TASK_LONG  = 1
} taskq_class_t;

typedef struct {
  volatile uint32_t state;      /* taskq_state_t; CAS target */
  uint32_t cls;                 /* taskq_class_t */
  uint32_t opcode;
  uint32_t in_addr;             /* card DDR address of input buffer */
  uint32_t in_len;
  uint32_t out_addr;            /* card DDR address of output buffer */
  uint32_t out_cap;
  uint32_t request_id;
  uint32_t user_cookie;
  uint16_t result_status;       /* SDK_STATUS_* to post on completion */
  uint16_t result_len;          /* completion payload length */
  uint8_t  result_payload[TASKQ_RESULT_PAYLOAD];
  uint8_t  op_params[TASKQ_OP_PARAM_BYTES]; /* producer-filled input params */
} taskq_desc_t;

typedef struct {
  taskq_desc_t descs[TASKQ_CAPACITY];
  uint32_t     enqueue_cursor;  /* single-producer hint (core 0 only) */
} taskq_t;

typedef struct {                /* coherent control block at the queue region */
  taskq_t          queue;
  volatile int32_t  core1_current_slot;     /* slot core 1 is running; -1 idle */
  volatile uint32_t core1_restart_request;  /* core 1 sets on fault; core 0 clears */
  volatile uint32_t core1_fault_code;       /* last fault code */
  volatile uint32_t core1_alive;            /* worker sets 1 on entry */
} taskq_shared_t;

typedef struct {
  uint32_t consecutive_faults;
  uint32_t fault_threshold;
  int      core1_enabled;
} taskq_watchdog_t;

/* ---- queue lifecycle (portable) ---- */
void taskq_init(taskq_t *q);
int  taskq_enqueue(taskq_t *q, uint32_t opcode, taskq_class_t cls,
                   uint32_t in_addr, uint32_t in_len,
                   uint32_t out_addr, uint32_t out_cap,
                   uint32_t request_id, uint32_t user_cookie,
                   const void *op_params);
int  taskq_claim_any(taskq_t *q);
int  taskq_claim_short(taskq_t *q);
void taskq_complete(taskq_t *q, int slot, uint16_t status,
                    const uint8_t *payload, uint16_t payload_len);
void taskq_fail(taskq_t *q, int slot, uint16_t status);
int  taskq_harvest(taskq_t *q);
void taskq_release(taskq_t *q, int slot);

/* ---- dispatch policy (portable) ---- */
taskq_class_t taskq_class_for_opcode(uint32_t opcode, uint32_t in_len);
int  taskq_should_drain(int zorro_pending, int display_pending, int core1_enabled);

/* ---- fault watchdog (portable) ---- */
void taskq_watchdog_init(taskq_watchdog_t *w, uint32_t threshold);
void taskq_watchdog_on_fault(taskq_watchdog_t *w);
void taskq_watchdog_on_success(taskq_watchdog_t *w);
int  taskq_watchdog_core1_enabled(const taskq_watchdog_t *w);

/* ---- coherency stress model (portable; Phase 0 harness uses these) ---- */
uint32_t taskq_stress_fill(uint32_t seed, uint32_t index);
int      taskq_stress_check(uint32_t seed, uint32_t index, uint32_t value);

/* ---- atomic primitive seam ---- */
int taskq_cas_u32(volatile uint32_t *p, uint32_t expected, uint32_t desired);
#ifdef TASKQ_HOST_TEST
extern int taskq_test_force_cas_fail;  /* >0: force the next N CAS calls to fail */
#endif

#ifndef TASKQ_HOST_TEST
/* ARM-only glue (scheduler_arm.c). Not visible to the host test build. */
void scheduler_coherency_init_core0(void); /* core 0: mark region shareable */
void scheduler_coherency_init_core1(void); /* core 1: full MMU/cache/SMP bring-up */
taskq_shared_t *scheduler_shared(void);    /* the coherent control block */
void scheduler_boot_init(void);            /* core 0: init queue + watchdog at boot */
int  scheduler_core1_available(void);      /* core 1 started and not watchdog-disabled */
void scheduler_core1_worker(void);         /* core 1: dedicated task worker; never returns */
void scheduler_core0_poll(int zorro_pending, int display_pending); /* core 0: harvest+post+drain */
#endif

#if defined(SCHED_STRESS_TEST) && !defined(TASKQ_HOST_TEST)
/* Phase 0 coherency torture harness (scheduler_stress.c). Turns the whole
 * firmware into a two-core stress test; none of these return. */
void scheduler_stress_init(void);   /* core 0: init the shared block (pre core-1) */
void scheduler_stress_core0(void);  /* core 0: producer + drain + verify + report */
void scheduler_stress_core1(void);  /* core 1: dedicated consumer */
#endif

#endif /* ZZ9K_SCHEDULER_H */
