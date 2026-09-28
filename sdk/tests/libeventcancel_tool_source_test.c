/*
 * Source guard for the event-driven zz9k.library cancellation smoke test.
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
    printf("usage: %s <zz9k-libeventcancel.c>\n", argv[0]);
    return 2;
  }

  source = read_file(argv[1]);
  if (!source) {
    printf("failed to read %s\n", argv[1]);
    return 2;
  }

  ok = 1;
  ok &= expect_contains(source, "#include \"zz9k/event_wait.h\"");
  ok &= expect_not_contains(source, "#include \"zz9k-event-wait.h\"");
  ok &= expect_contains(source, "#include \"proto/zz9k.h\"");
  ok &= expect_contains(source, "ZZ9K_LIBRARY_MIN_REVISION_EVENT_DISPATCHER");
  ok &= expect_contains(source, "ZZ9K_EVENT_CANCEL_COUNT");
  ok &= expect_contains(source, "ZZ9KCallAsyncBatchMsg");
  ok &= expect_contains(source, "ZZ9KCancelAsync");
  ok &= expect_contains(source, "partial batch drain");
  ok &= expect_contains(source, "CreateMsgPort");
  ok &= expect_contains(source, "zz9k_event_wait_async_port");
  ok &= expect_not_contains(source, "zz9k_event_timer_start");
  ok &= expect_not_contains(source, "zz9k_event_timer_finish");
  ok &= expect_not_contains(source, "Wait(");
  ok &= expect_contains(source, "cancel event ok");
  ok &= expect_not_contains(source, "typedef struct EventTimer");
  ok &= expect_not_contains(source, "static int open_timer");
  ok &= expect_not_contains(source, "ZZ9KPoll(");
  ok &= expect_not_contains(source, "ZZ9KWaitAsync(");

  free(source);
  return ok ? 0 : 1;
}
