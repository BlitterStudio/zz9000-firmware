/*
 * Hardware smoke client for the ARM app runtime through the client transport
 * module (aar_xport_zz9000): claim, load, start, send 32-byte messages and
 * check that the echo app returns each one unchanged, stop, release.
 *
 * usage: zz9k-app-echo-test <echo.elf> [message-count]
 *
 * The queues are polled; the lease is renewed from the polling loops.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <dos/dos.h>
#include <exec/types.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aar_xport_zz9000.h"

#define SEND_TIMEOUT_TICKS 500U   /* 10 s of Delay(1) */
#define RECV_SPINS 2000U          /* empty polls before the first Delay */

static aar_xport app;

/* Each step is printed before the call, so a crash names the call that
 * did not return. */
static void trace(const char *step)
{
  printf("zz9k-app-echo-test: %s\n", step);
  fflush(stdout);
}

static void usage(const char *name)
{
  printf("usage: %s <echo.elf> [message-count]\n", name);
}

static const char *aar_text(int rc)
{
  switch (rc) {
  case AAR_OK: return "ok";
  case AAR_E_IO: return "io error";
  case AAR_E_INVAL: return "invalid";
  case AAR_E_NOMEM: return "no memory";
  case AAR_E_BUSY: return "busy";
  case AAR_E_NOENT: return "not found";
  case AAR_E_NOSYS: return "unsupported";
  case AAR_E_TIMEDOUT: return "timed out";
  case AAR_E_NORESOURCE: return "refused by the card";
  case AAR_E_STALE: return "claim gone";
  default: return "error";
  }
}

int main(int argc, char **argv)
{
  aar_xport_board board;
  const char *app_argv[1] = { "echo" };
  uint8_t sent[AAR_MSG_BYTES];
  uint8_t received[AAR_MSG_BYTES];
  aar_xport_status st;
  uint32_t count = 300U;
  uint32_t sent_count = 0U;
  uint32_t received_count = 0U;
  uint32_t reason = 0U;
  uint32_t i;
  const char *step = "open";
  int board_open = 0;
  int claimed = 0;
  int status;

  if (argc < 2 || argc > 3) {
    usage(argv[0]);
    return 10;
  }
  if (argc == 3) {
    char *end = 0;
    unsigned long value = strtoul(argv[2], &end, 10);

    if (!end || *end || value == 0UL || value > 65536UL) {
      usage(argv[0]);
      return 10;
    }
    count = (uint32_t)value;
  }
  trace("open");
  status = aar_xport_amiga_board_open(&board);
  if (status != AAR_OK) {
    puts("zz9k-app-echo-test: no Zorro III board, zz9k.library or mmu.library");
    return 20;
  }
  board_open = 1;
  status = aar_xport_open(&app, &board, &reason);
  if (status != AAR_OK) goto fail;
  claimed = 1;
  step = "load";
  trace(step);
  status = aar_xport_load_file(&app, argv[1], 0U);
  if (status != AAR_OK) goto fail;
  step = "start";
  trace(step);
  status = aar_xport_start(&app, app_argv, 1U);
  if (status != AAR_OK) goto fail;

  while (sent_count < count) {
    uint32_t spins = 0U;
    uint32_t ticks = 0U;

    for (i = 0U; i < sizeof(sent); i++) {
      sent[i] = (uint8_t)(sent_count * 31U + i);
    }
    step = "send";
    if (sent_count < 2U || sent_count % 100U == 0U) trace(step);
    status = aar_xport_send_message(&app, sent);
    if (status != AAR_OK) goto fail;
    sent_count++;
    step = "receive";
    for (;;) {
      status = aar_xport_get_message(&app, received);
      if (status < 0) goto fail;
      if (status == 1) break;
      (void)aar_xport_lease_renew(&app);
      if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
        status = AAR_E_IO;
        step = "cancelled";
        goto fail;
      }
      if (++spins >= RECV_SPINS) {
        spins = 0U;
        if (++ticks > SEND_TIMEOUT_TICKS) {
          status = AAR_E_TIMEDOUT;
          goto fail;
        }
        Delay(1);
      }
    }
    if (memcmp(sent, received, sizeof(sent)) != 0) {
      status = AAR_E_IO;
      step = "compare";
      goto fail;
    }
    received_count++;
  }
  step = "stop";
  trace(step);
  status = aar_xport_stop(&app);
  if (status != AAR_OK) goto fail;
  {
    uint32_t ticks;

    for (ticks = 0U;; ticks++) {
      status = aar_xport_status_get(&app, &st);
      if (status != AAR_OK) goto fail;
      if (st.state == AAR_STATE_IDLE || st.state == AAR_STATE_FAULTED) break;
      if (ticks > SEND_TIMEOUT_TICKS) {
        status = AAR_E_TIMEDOUT;
        step = "wait for the app to end";
        goto fail;
      }
      (void)aar_xport_lease_renew(&app);
      Delay(1);
    }
  }
  if (st.fault_class != AAR_FAULT_NONE || st.exit_code != 0) {
    status = AAR_E_IO;
    step = "app status";
    goto fail;
  }
  trace("release");
  (void)aar_xport_release(&app);
  aar_xport_amiga_board_close(&board);
  printf("zz9k-app-echo-test: PASS (%lu messages)\n", (unsigned long)count);
  return 0;

fail:
  printf("zz9k-app-echo-test: FAIL at %s after %lu messages: %s (%d)\n", step,
         (unsigned long)received_count, aar_text(status), status);
  if (status == AAR_E_NORESOURCE) {
    printf("the card refused the claim, reason %lu\n",
           (unsigned long)(reason ? reason : aar_xport_reason(&app)));
  }
  if (claimed && aar_xport_status_get(&app, &st) == AAR_OK) {
    printf("app state %lu fault %lu exit %ld handle %08lx pc %08lx\n",
           (unsigned long)st.state, (unsigned long)st.fault_class,
           (long)st.exit_code, (unsigned long)st.handle,
           (unsigned long)st.fault_pc);
  }
  trace("release");
  if (claimed) (void)aar_xport_release(&app);
  if (board_open) aar_xport_amiga_board_close(&board);
  return 20;
}
