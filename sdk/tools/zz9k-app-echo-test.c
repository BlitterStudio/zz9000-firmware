/*
 * Hardware smoke client for the ARM app runtime through aar.library: open a
 * handle, load, start, send 32-byte messages and check that the echo app
 * returns each one unchanged, stop, close.
 *
 * usage: zz9k-app-echo-test <echo.elf> [message-count]
 *
 * The queues are polled. The library keeps the lease itself.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <dos/dos.h>
#include <exec/types.h>
#include <proto/aar.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEND_TIMEOUT_TICKS 500U   /* 10 s of Delay(1) */
#define RECV_SPINS 2000U          /* empty polls before the first Delay */

struct Library *AARBase;

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

static const char *aar_text(LONG rc)
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
  case AAR_E_OWNER: return "not the owner";
  default: return "error";
  }
}

int main(int argc, char **argv)
{
  AARHandle *app = 0;
  AARStatusInfo st;
  const char *app_argv[1] = { "echo" };
  AARMessage sent;
  AARMessage received;
  uint32_t count = 300U;
  uint32_t sent_count = 0U;
  uint32_t received_count = 0U;
  ULONG reason = 0U;
  uint32_t i;
  const char *step = "open";
  LONG status;

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
  AARBase = OpenLibrary((CONST_STRPTR)AAR_LIB_NAME, AAR_LIB_VERSION);
  if (!AARBase) {
    puts("zz9k-app-echo-test: cannot open aar.library");
    return 20;
  }
  status = AAROpen(&app, 0, &reason);
  if (status != AAR_OK) {
    app = 0;
    goto fail;
  }
  step = "load";
  trace(step);
  status = AARLoadFile(app, argv[1]);
  if (status != AAR_OK) goto fail;
  step = "start";
  trace(step);
  status = AARStart(app, app_argv, 1U);
  if (status != AAR_OK) goto fail;

  while (sent_count < count) {
    uint32_t spins = 0U;
    uint32_t ticks = 0U;

    for (i = 0U; i < sizeof(sent.bytes); i++) {
      sent.bytes[i] = (UBYTE)(sent_count * 31U + i);
    }
    step = "send";
    if (sent_count < 2U || sent_count % 100U == 0U) trace(step);
    status = AARSendMessage(app, &sent);
    if (status != AAR_OK) goto fail;
    sent_count++;
    step = "receive";
    for (;;) {
      status = AARGetMessage(app, &received);
      if (status == AAR_OK) break;
      if (status != AAR_E_BUSY) goto fail;
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
    if (memcmp(sent.bytes, received.bytes, sizeof(sent.bytes)) != 0) {
      status = AAR_E_IO;
      step = "compare";
      goto fail;
    }
    received_count++;
  }
  step = "stop";
  trace(step);
  status = AARStop(app);
  if (status != AAR_OK) goto fail;
  {
    uint32_t ticks;

    for (ticks = 0U;; ticks++) {
      st.size = sizeof(st);
      status = AARStatus(app, &st);
      if (status != AAR_OK) goto fail;
      if (st.state == AAR_STATE_IDLE || st.state == AAR_STATE_FAULTED) break;
      if (ticks > SEND_TIMEOUT_TICKS) {
        status = AAR_E_TIMEDOUT;
        step = "wait for the app to end";
        goto fail;
      }
      Delay(1);
    }
  }
  if (st.fault_class != AAR_FAULT_NONE || st.exit_code != 0) {
    status = AAR_E_IO;
    step = "app status";
    goto fail;
  }
  trace("close");
  (void)AARClose(app);
  CloseLibrary(AARBase);
  printf("zz9k-app-echo-test: PASS (%lu messages)\n", (unsigned long)count);
  return 0;

fail:
  printf("zz9k-app-echo-test: FAIL at %s after %lu messages: %s (%ld)\n", step,
         (unsigned long)received_count, aar_text(status), (long)status);
  if (status == AAR_E_NORESOURCE) {
    printf("the card refused the claim, reason %lu\n", (unsigned long)reason);
  }
  if (app) {
    st.size = sizeof(st);
    if (AARStatus(app, &st) == AAR_OK) {
      printf("app state %lu fault %lu exit %ld reason %lu pc %08lx\n",
             (unsigned long)st.state, (unsigned long)st.fault_class,
             (long)st.exit_code, (unsigned long)st.reason,
             (unsigned long)st.fault_pc.lo);
    }
    trace("close");
    (void)AARClose(app);
  }
  CloseLibrary(AARBase);
  return 20;
}
