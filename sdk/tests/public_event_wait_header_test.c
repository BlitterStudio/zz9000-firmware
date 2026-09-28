/*
 * Source guard for the public AmigaOS event timer helper header.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path)
{
  FILE *file;
  long length;
  char *data;

  file = fopen(path, "rb");
  if (!file) {
    return 0;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return 0;
  }
  length = ftell(file);
  if (length < 0) {
    fclose(file);
    return 0;
  }
  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return 0;
  }

  data = (char *)malloc((size_t)length + 1U);
  if (!data) {
    fclose(file);
    return 0;
  }
  if (fread(data, 1U, (size_t)length, file) != (size_t)length) {
    free(data);
    fclose(file);
    return 0;
  }

  data[length] = '\0';
  fclose(file);
  return data;
}

static int expect_contains(const char *source, const char *needle)
{
  if (strstr(source, needle)) {
    return 1;
  }

  printf("missing %s\n", needle);
  return 0;
}

static int expect_not_contains(const char *source, const char *needle)
{
  if (!strstr(source, needle)) {
    return 1;
  }

  printf("unexpected %s\n", needle);
  return 0;
}

int main(int argc, char **argv)
{
  char *source;
  int ok;

  if (argc != 2) {
    printf("usage: %s <amiga/include/zz9k/event_wait.h>\n", argv[0]);
    return 2;
  }

  source = read_file(argv[1]);
  if (!source) {
    printf("failed to read %s\n", argv[1]);
    return 2;
  }

  ok = 1;
  ok &= expect_contains(source, "#ifndef ZZ9K_EVENT_WAIT_H");
  ok &= expect_contains(source, "#include <devices/timer.h>");
  ok &= expect_contains(source, "#include <proto/exec.h>");
  ok &= expect_contains(source, "#include \"zz9k/library.h\"");
  ok &= expect_contains(source, "typedef struct ZZ9KEventTimer");
  ok &= expect_contains(source, "static inline int zz9k_event_timer_open");
  ok &= expect_contains(source, "static inline void zz9k_event_timer_start");
  ok &= expect_contains(source, "static inline void zz9k_event_timer_finish");
  ok &= expect_contains(source, "static inline void zz9k_event_timer_close");
  ok &= expect_contains(source, "static inline int zz9k_event_drain_async_port");
  ok &= expect_contains(source, "static inline int zz9k_event_drain_async_ports");
  ok &= expect_contains(source, "static inline int zz9k_event_wait_async_port");
  ok &= expect_contains(source, "static inline int zz9k_event_wait_async_ports");
  ok &= expect_contains(source, "static inline int zz9k_event_wait_signal");
  ok &= expect_contains(source, "static inline int zz9k_event_wait_msg");
  ok &= expect_contains(source, "struct MsgPort **reply_ports");
  ok &= expect_contains(source, "uint32_t port_count");
  ok &= expect_contains(source, "ZZ9KAsyncRequest **asyncs");
  ok &= expect_contains(source, "ULONG signal_mask");
  ok &= expect_contains(source, "message->mn_Length != (uint16_t)sizeof(ZZ9KAsyncRequest)");
  ok &= expect_contains(source, "Empty reply-port signals are legal; keep waiting.");
  ok &= expect_contains(source, "for (;;) {");
  ok &= expect_contains(source, "continue;");
  ok &= expect_contains(source, "ZZ9K_STATUS_NO_MEMORY");
  ok &= expect_contains(source, "if (!timer || !reply_port || !async)");
  ok &= expect_contains(source, "if (!async->queued && !async->completed)");
  ok &= expect_not_contains(source, "|| !async->queued)");
  ok &= expect_contains(source, "OpenDevice");
  ok &= expect_contains(source, "TR_ADDREQUEST");
  ok &= expect_contains(source, "AbortIO");
  ok &= expect_contains(source, "WaitIO");
  ok &= expect_contains(source, "Wait(");
  ok &= expect_contains(source, "GetMsg");
  ok &= expect_contains(source, "ZZ9K_STATUS_TIMEOUT");
  ok &= expect_contains(source, "ZZ9K_STATUS_CANCELLED");
  ok &= expect_contains(source, "ZZ9K_STATUS_INTERNAL_ERROR");

  free(source);
  return ok ? 0 : 1;
}
