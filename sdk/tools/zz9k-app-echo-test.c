/*
 * Hardware smoke client for zz9k.library's ARM app client API.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/ports.h>
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/zz9k.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Library *ZZ9KBase;

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
  status = ZZ9KAppOpen(&app, port);
  if (status != ZZ9K_STATUS_OK) goto fail;
  status = ZZ9KAppLoadFile(app, argv[1]);
  if (status != ZZ9K_STATUS_OK) goto fail;
  status = ZZ9KAppStart(app, app_argv, 1U);
  if (status != ZZ9K_STATUS_OK) goto fail;
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
    status = ZZ9KAppGetMessage(app, &received);
    if (status != ZZ9K_STATUS_OK || memcmp(sent.bytes, received.bytes,
                                            sizeof(sent.bytes)) != 0) {
      if (status == ZZ9K_STATUS_OK) status = ZZ9K_STATUS_INTERNAL_ERROR;
      goto fail;
    }
    received_count++;
  }
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
  ZZ9KAppClose(app);
  DeleteMsgPort(port);
  CloseLibrary(ZZ9KBase);
  printf("zz9k-app-echo-test: PASS (%lu messages)\n", (unsigned long)count);
  return 0;

fail:
  printf("zz9k-app-echo-test: FAIL (%d)\n", status);
  if (port) {
    while (GetMsg(port)) {}
  }
  if (app) ZZ9KAppClose(app);
  if (port) DeleteMsgPort(port);
  CloseLibrary(ZZ9KBase);
  return 20;
}
