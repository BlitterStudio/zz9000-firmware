/*
 * Bounded diagnostic for SDK completion IRQ transport.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/caps.h"
#include "zz9k/sdk.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__amigaos__) || defined(__amiga__) || defined(__AMIGA__) || \
    defined(__VBCC__)
#define ZZ9K_IRQTEST_AMIGA 1
#else
#define ZZ9K_IRQTEST_AMIGA 0
#endif

#if ZZ9K_IRQTEST_AMIGA
#include <dos/dos.h>
#include <exec/exec.h>
#include <exec/interrupts.h>
#include <exec/tasks.h>
#include <hardware/intbits.h>
#include <proto/dos.h>
#include <proto/exec.h>
#endif

#define IRQTEST_STATUS_POLLS 200000UL
#define IRQTEST_COMPLETION_POLLS 1000UL

typedef struct ZZ9KIRQTestState {
  ZZ9KContext *ctx;
#if ZZ9K_IRQTEST_AMIGA
  struct Interrupt irq;
  struct Task *task;
  BYTE signal;
  ULONG signal_mask;
  int int_bit;
#endif
  volatile uint32_t irq_hits;
  volatile uint16_t last_irq_status;
} ZZ9KIRQTestState;

static void print_status(const char *prefix, int status)
{
  printf("zz9k-irqtest: %s: %s (%d)\n",
         prefix, zz9k_status_name(status), status);
}

#if ZZ9K_IRQTEST_AMIGA
static uint32_t zz9k_irqtest_isr(ZZ9KIRQTestState *state asm("a1"))
{
  uint16_t irq_status = 0;

  if (!state || zz9k_interrupt_status(state->ctx, &irq_status) !=
                    ZZ9K_STATUS_OK) {
    return 0;
  }

  state->last_irq_status = irq_status;
  if ((irq_status & ZZ9K_INTERRUPT_SDK) == 0U) {
    return 0;
  }

  zz9k_completion_irq_ack(state->ctx);
  state->irq_hits++;
  if (state->task && state->signal_mask != 0UL) {
    Signal(state->task, state->signal_mask);
  }
  return 1;
}

static int zz9k_irqtest_install_irq(ZZ9KIRQTestState *state,
                                    ZZ9KContext *ctx)
{
  memset(state, 0, sizeof(*state));
  state->ctx = ctx;
  state->signal = AllocSignal(-1);
  if (state->signal < 0) {
    return ZZ9K_STATUS_NO_MEMORY;
  }

  state->task = FindTask(0);
  state->signal_mask = 1UL << state->signal;
  /* INT2 vs INT6: ENV:ZZ9K_INT2, then the ZZ9000.CFG `int2` key
   * (shared decision in zz9k_sdk_use_int2, same as zz9k.library). */
  state->int_bit = zz9k_sdk_use_int2(ctx) ? INTB_PORTS : INTB_EXTER;
  state->irq.is_Node.ln_Type = NT_INTERRUPT;
  state->irq.is_Node.ln_Pri = 126;
  state->irq.is_Node.ln_Name = "ZZ9000 SDK IRQ";
  state->irq.is_Data = state;
  state->irq.is_Code = (void *)zz9k_irqtest_isr;

  Forbid();
  AddIntServer(state->int_bit, &state->irq);
  Permit();
  return ZZ9K_STATUS_OK;
}

static void zz9k_irqtest_remove_irq(ZZ9KIRQTestState *state)
{
  if (!state) {
    return;
  }

  if (state->irq.is_Node.ln_Type == NT_INTERRUPT) {
    Forbid();
    RemIntServer(state->int_bit, &state->irq);
    Permit();
    state->irq.is_Node.ln_Type = 0;
  }
  if (state->signal >= 0) {
    FreeSignal(state->signal);
    state->signal = -1;
  }
}
#else
static int zz9k_irqtest_install_irq(ZZ9KIRQTestState *state,
                                    ZZ9KContext *ctx)
{
  memset(state, 0, sizeof(*state));
  state->ctx = ctx;
  return ZZ9K_STATUS_UNSUPPORTED;
}

static void zz9k_irqtest_remove_irq(ZZ9KIRQTestState *state)
{
  (void)state;
}
#endif

int main(void)
{
  static const uint8_t payload[4] = { 'i', 'r', 'q', '!' };
  ZZ9KContext *ctx = 0;
  ZZ9KCaps caps;
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  ZZ9KIRQTestState irq_state;
  uint32_t request_id = 0;
  uint32_t status_polls;
  uint32_t completion_polls;
  int irq_enabled = 0;
  int irq_installed = 0;
  int status;
  int rc = 1;

  memset(&irq_state, 0, sizeof(irq_state));
#if ZZ9K_IRQTEST_AMIGA
  irq_state.signal = -1;
#endif

  printf("zz9k-irqtest: opening SDK mailbox\n");
  fflush(stdout);
  status = zz9k_open(&ctx);
  if (status != ZZ9K_STATUS_OK) {
    print_status("open failed", status);
    return 1;
  }

  printf("zz9k-irqtest: querying capabilities\n");
  fflush(stdout);
  status = zz9k_query_caps(ctx, &caps);
  if (status != ZZ9K_STATUS_OK) {
    print_status("query caps failed", status);
    goto out;
  }
  if ((caps.capability_bits & ZZ9K_CAP_IRQ_COMPLETION) == 0U) {
    printf("zz9k-irqtest: firmware does not advertise irq-completion\n");
    goto out;
  }

  printf("zz9k-irqtest: installing interrupt server\n");
  fflush(stdout);
  status = zz9k_irqtest_install_irq(&irq_state, ctx);
  if (status != ZZ9K_STATUS_OK) {
    print_status("install interrupt server failed", status);
    goto out;
  }
  irq_installed = 1;
#if ZZ9K_IRQTEST_AMIGA
  printf("zz9k-irqtest: using INT%u\n",
         irq_state.int_bit == INTB_PORTS ? 2U : 6U);
  fflush(stdout);
#endif

  printf("zz9k-irqtest: enabling completion irq\n");
  fflush(stdout);
  status = zz9k_completion_irq_enable(ctx, 1);
  if (status != ZZ9K_STATUS_OK) {
    print_status("enable completion irq failed", status);
    goto out;
  }
  irq_enabled = 1;

  printf("zz9k-irqtest: clearing stale sdk irq\n");
  fflush(stdout);
  status = zz9k_completion_irq_ack(ctx);
  if (status != ZZ9K_STATUS_OK) {
    print_status("clear stale completion irq failed", status);
    goto out;
  }

  memset(&request, 0, sizeof(request));
  status = zz9k_request_ping(&request, payload, sizeof(payload));
  if (status != ZZ9K_STATUS_OK) {
    print_status("build ping failed", status);
    goto out;
  }

  printf("zz9k-irqtest: submitting ping\n");
  fflush(stdout);
  status = zz9k_submit(ctx, &request, &request_id);
  if (status != ZZ9K_STATUS_QUEUED) {
    print_status("submit failed", status);
    goto out;
  }

  printf("zz9k-irqtest: waiting for sdk interrupt server\n");
  fflush(stdout);
  for (status_polls = 0; status_polls < IRQTEST_STATUS_POLLS;
       status_polls++) {
    if (irq_state.irq_hits != 0U) {
      break;
    }
  }
  if (irq_state.irq_hits == 0U) {
    printf("zz9k-irqtest: timeout waiting for SDK interrupt server\n");
    goto out;
  }

  printf("zz9k-irqtest: polling completion\n");
  fflush(stdout);
  memset(&reply, 0, sizeof(reply));
  for (completion_polls = 0; completion_polls < IRQTEST_COMPLETION_POLLS;
       completion_polls++) {
    status = zz9k_poll(ctx, &reply);
    if (status == ZZ9K_STATUS_OK && reply.request_id == request_id) {
      break;
    }
    if (status != ZZ9K_STATUS_BUSY && status != ZZ9K_STATUS_OK) {
      print_status("poll completion failed", status);
      goto out;
    }
  }

  printf("zz9k-irqtest: acking sdk interrupt\n");
  fflush(stdout);
  status = zz9k_completion_irq_ack(ctx);
  if (status != ZZ9K_STATUS_OK) {
    print_status("ack completion irq failed", status);
    goto out;
  }

  if (reply.request_id != request_id) {
    printf("zz9k-irqtest: timeout waiting for matching completion\n");
    goto out;
  }
  if (reply.status != ZZ9K_STATUS_OK) {
    print_status("ping completion failed", reply.status);
    goto out;
  }

  printf("irqtest ok status_polls=%lu completion_polls=%lu "
         "request_id=%lu irq_hits=%lu status=0x%04x\n",
         (unsigned long)status_polls,
         (unsigned long)completion_polls,
         (unsigned long)request_id,
         (unsigned long)irq_state.irq_hits,
         (unsigned)irq_state.last_irq_status);
  rc = 0;

out:
  if (irq_enabled) {
    printf("zz9k-irqtest: disabling completion irq\n");
    fflush(stdout);
    status = zz9k_completion_irq_enable(ctx, 0);
    if (status != ZZ9K_STATUS_OK) {
      print_status("disable completion irq failed", status);
      rc = 1;
    }
  }
  if (irq_installed) {
    printf("zz9k-irqtest: removing interrupt server\n");
    fflush(stdout);
    zz9k_irqtest_remove_irq(&irq_state);
  }
  zz9k_close(ctx);
  return rc;
}
