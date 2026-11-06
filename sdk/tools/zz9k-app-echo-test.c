/*
 * Hardware smoke client for zz9k.library's ARM app client API.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/ports.h>
#include <exec/types.h>
#include <proto/exec.h>
#include <dos/dos.h>
#include <proto/zz9k.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zz9k/text.h>

struct Library *ZZ9KBase;

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

int main(int argc, char **argv)
{
  ZZ9KAppHandle *app = 0;
  struct MsgPort *port = 0;
  const char *app_argv[1] = { "echo" };
  ZZ9KAppMessage sent;
  ZZ9KAppMessage received;
  uint32_t signal;
  uint32_t count = 300U;
  uint32_t sent_count = 0U;
  uint32_t received_count = 0U;
  uint32_t i;
  ZZ9KAppStatusInfo final_status;
  const char *step = "open";
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
  ZZ9KBase = OpenLibrary((CONST_STRPTR)ZZ9K_LIBRARY_NAME,
                         ZZ9K_LIBRARY_VERSION);
  if (!ZZ9KBase) {
    puts("zz9k-app-echo-test: zz9k.library is not installed");
    return 20;
  }
  if (ZZ9KBase->lib_Revision < ZZ9K_LIBRARY_MIN_REVISION_APP_CLIENT) {
    puts("zz9k-app-echo-test: zz9k.library app client is too old");
    CloseLibrary(ZZ9KBase);
    return 20;
  }
  port = CreateMsgPort();
  if (!port) {
    CloseLibrary(ZZ9KBase);
    return 20;
  }
  trace("open");
  status = ZZ9KAppOpen(&app, port);
  if (status != ZZ9K_STATUS_OK) goto fail;
  step = "load";
  trace(step);
  status = ZZ9KAppLoadFile(app, argv[1]);
  if (status != ZZ9K_STATUS_OK) goto fail;
  step = "start";
  trace(step);
  status = ZZ9KAppStart(app, app_argv, 1U);
  if (status != ZZ9K_STATUS_OK) goto fail;
  trace("signal");
  signal = ZZ9KAppMessageSignal(app);
  if (!signal) {
    status = ZZ9K_STATUS_INTERNAL_ERROR;
    goto fail;
  }
  while (sent_count < count) {
    ULONG signals;

    for (i = 0U; i < sizeof(sent.bytes); i++) {
      sent.bytes[i] = (uint8_t)(sent_count * 31U + i);
    }
    step = "send";
    if (sent_count < 2U || sent_count % 100U == 0U) trace(step);
    status = ZZ9KAppSendMessage(app, &sent);
    if (status != ZZ9K_STATUS_OK) goto fail;
    sent_count++;
    signals = Wait(signal | SIGBREAKF_CTRL_C);
    if (signals & SIGBREAKF_CTRL_C) {
      status = ZZ9K_STATUS_CANCELLED;
      goto fail;
    }
    while (GetMsg(port)) {
      /* Notification payload is advisory; drain messages before Close. */
    }
    step = "receive";
    status = ZZ9KAppGetMessage(app, &received);
    if (status != ZZ9K_STATUS_OK || memcmp(sent.bytes, received.bytes,
                                            sizeof(sent.bytes)) != 0) {
      if (status == ZZ9K_STATUS_OK) status = ZZ9K_STATUS_INTERNAL_ERROR;
      goto fail;
    }
    received_count++;
  }
  step = "stop";
  trace(step);
  status = ZZ9KAppStop(app);
  if (status != ZZ9K_STATUS_OK) goto fail;
  for (;;) {
    ULONG signals = Wait(signal | SIGBREAKF_CTRL_C);

    if (signals & SIGBREAKF_CTRL_C) {
      status = ZZ9K_STATUS_CANCELLED;
      goto fail;
    }
    while (GetMsg(port)) {}
    status = ZZ9KAppStatus(app, &final_status);
    if (status != ZZ9K_STATUS_OK) goto fail;
    if (final_status.state == ZZ9K_APP_STATE_IDLE) break;
  }
  if (final_status.fault_code != ZZ9K_APP_FAULT_NONE ||
      final_status.exit_code != 0) {
    status = ZZ9K_STATUS_INTERNAL_ERROR;
    goto fail;
  }
  trace("close");
  ZZ9KAppClose(app);
  DeleteMsgPort(port);
  trace("close library");
  CloseLibrary(ZZ9KBase);
  printf("zz9k-app-echo-test: PASS (%lu messages)\n", (unsigned long)count);
  return 0;

fail:
  printf("zz9k-app-echo-test: FAIL at %s after %lu messages: %s (%d)\n", step,
         (unsigned long)received_count, zz9k_status_text(status), status);
  if (app && ZZ9KAppStatus(app, &final_status) == ZZ9K_STATUS_OK) {
    printf("app state %lu fault %lu exit %ld generation %lu pc %08lx\n",
           (unsigned long)final_status.state, (unsigned long)final_status.fault_code,
           (long)final_status.exit_code, (unsigned long)final_status.generation,
           (unsigned long)final_status.pc);
  }
  if (port) {
    while (GetMsg(port)) {}
  }
  trace("close");
  if (app) ZZ9KAppClose(app);
  if (port) DeleteMsgPort(port);
  trace("close library");
  CloseLibrary(ZZ9KBase);
  return 20;
}
