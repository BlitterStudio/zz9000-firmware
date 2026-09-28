/*
 * Source guards for host-side mailbox arbitration.
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

static const char *find_definition(const char *source, const char *name)
{
  const char *pos;

  pos = source;
  while ((pos = strstr(pos, name)) != 0) {
    const char *brace = strchr(pos, '{');
    const char *semicolon = strchr(pos, ';');
    if (brace && (!semicolon || brace < semicolon)) {
      return pos;
    }
    pos++;
  }

  return 0;
}

static int function_contains(const char *source, const char *name,
                             const char *needle)
{
  const char *start;
  const char *end;
  const char *hit;

  start = find_definition(source, name);
  if (!start) {
    printf("%s: definition not found\n", name);
    return 0;
  }

  end = strstr(start + 1, "\nint ");
  if (!end) end = strstr(start + 1, "\nstatic ");
  if (!end) end = source + strlen(source);

  hit = strstr(start, needle);
  if (hit && hit < end) {
    return 1;
  }

  printf("%s: missing %s\n", name, needle);
  return 0;
}

static int source_contains(const char *source, const char *needle)
{
  if (strstr(source, needle)) {
    return 1;
  }
  printf("missing %s\n", needle);
  return 0;
}

static int source_not_contains(const char *source, const char *needle)
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
  int ok = 1;

  if (argc != 2) {
    printf("usage: %s <zz9k_host.c>\n", argv[0]);
    return 2;
  }

  source = read_file(argv[1]);
  if (!source) {
    printf("failed to read %s\n", argv[1]);
    return 2;
  }

  ok &= source_contains(source, "static int zz9k_mailbox_lock");
  ok &= source_contains(source, "static void zz9k_mailbox_unlock");
  ok &= source_contains(source, "FindSemaphore(");
  ok &= source_contains(source, "AddSemaphore(");
  ok &= source_contains(source, "ObtainSemaphore(");
  ok &= source_contains(source, "ReleaseSemaphore(");
  ok &= source_contains(source,
                        "ZZ9K_MAILBOX_SEMAPHORE_NAME \"zz9000.sdk.mailbox\"");
  ok &= source_not_contains(source, "ZZ9K_SYNC_COOKIE_FLAG");
  ok &= source_not_contains(source, "ZZ9K_PENDING_SYNC_MAX");
  ok &= source_not_contains(source, "zz9k_pending_sync_add_locked");
  ok &= source_not_contains(source, "zz9k_poll_sync_completion_locked");
  ok &= source_not_contains(source, "zz9000.sdk.mailbox.v2");
  ok &= source_not_contains(source,
                            "(void)zz9k_arm_completion_irq(*ctx);");
  ok &= source_not_contains(source, "ZZ9K_NO_IRQ_WAIT");
  ok &= function_contains(source, "zz9k_call",
                          "lock_status = zz9k_mailbox_lock(ctx);");
  ok &= function_contains(source, "zz9k_call",
                          "zz9k_mailbox_unlock(ctx);");
  ok &= function_contains(source, "zz9k_call",
                          "status = zz9k_consume_next_completion_locked(ctx, reply);");

  free(source);
  return ok ? 0 : 1;
}
