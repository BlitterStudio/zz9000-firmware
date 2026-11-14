/*
 * Source guard for resident zz9k.library serialization.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zz9k/library_vectors.h"

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

  end = strstr(start + 1, "\nstatic ");
  if (!end) {
    end = source + strlen(source);
  }

  hit = strstr(start, needle);
  return hit && hit < end;
}

/* The resident jump table must provide one vector per LVO the headers
 * advertise (4 standard + ZZ9K_LVO_FUNCTION_COUNT public), or a client
 * gated on lib_Revision jumps past the initialized table. */
static int expect_vector_table_complete(const char *source)
{
  const char *table;
  const char *end;
  const char *pos;
  int count;

  table = strstr(source, "zz9k_lib_vectors[] = {");
  if (!table) {
    printf("zz9k_lib_vectors table not found\n");
    return 0;
  }
  end = strstr(table, "};");
  if (!end) {
    printf("zz9k_lib_vectors table not terminated\n");
    return 0;
  }

  count = 0;
  pos = table;
  while ((pos = strstr(pos, "(APTR)zz9k_lib_")) != 0 && pos < end) {
    count++;
    pos++;
  }

  if (count != 4 + ZZ9K_LVO_FUNCTION_COUNT) {
    printf("zz9k_lib_vectors has %d entries, headers advertise %d\n",
           count, 4 + ZZ9K_LVO_FUNCTION_COUNT);
    return 0;
  }

  return 1;
}

static int expect_public_call_locked(const char *source, const char *name)
{
  int ok;

  ok = 1;
  if (!function_contains(source, name, "zz9k_lib_enter(base)")) {
    printf("%s: missing zz9k_lib_enter(base)\n", name);
    ok = 0;
  }
  if (!function_contains(source, name, "zz9k_lib_leave(base)")) {
    printf("%s: missing zz9k_lib_leave(base)\n", name);
    ok = 0;
  }

  return ok;
}

int main(int argc, char **argv)
{
  static const char *public_calls[] = {
    "zz9k_lib_query_caps",
    "zz9k_lib_query_service",
    "zz9k_lib_ping",
    "zz9k_lib_call",
    "zz9k_lib_call_async",
    "zz9k_lib_call_async_batch",
    "zz9k_lib_poll",
    "zz9k_lib_alloc_shared",
    "zz9k_lib_free_shared",
    "zz9k_lib_mem_fill",
    "zz9k_lib_mem_copy",
    "zz9k_lib_alloc_surface",
    "zz9k_lib_alloc_surface_ex",
    "zz9k_lib_free_surface",
    "zz9k_lib_map_framebuffer_surface",
    "zz9k_lib_scale_image",
    "zz9k_lib_read_diag",
    "zz9k_lib_call_async_msg",
    "zz9k_lib_call_async_batch_msg",
    "zz9k_lib_cancel_async",
    "zz9k_lib_wait_async",
    "zz9k_lib_wait_async_batch",
    "zz9k_lib_decode_image",
    "zz9k_lib_crypto_hash",
    "zz9k_lib_crypto_hash_batch",
    "zz9k_lib_crypto_stream",
    "zz9k_lib_crypto_stream_batch",
    "zz9k_lib_crypto_aead",
    "zz9k_lib_crypto_aead_batch",
    "zz9k_lib_crypto_kx",
    "zz9k_lib_fill_surface",
    "zz9k_lib_copy_surface"
  };
  char *source;
  size_t i;
  int failures;

  if (argc != 2) {
    printf("usage: %s <zz9k_library_resident.c>\n", argv[0]);
    return 2;
  }

  source = read_file(argv[1]);
  if (!source) {
    printf("failed to read %s\n", argv[1]);
    return 2;
  }

  failures = 0;
  if (!strstr(source, "static int zz9k_lib_enter")) {
    printf("missing zz9k_lib_enter helper\n");
    failures++;
  }
  if (!strstr(source, "static void zz9k_lib_leave")) {
    printf("missing zz9k_lib_leave helper\n");
    failures++;
  }
  if (!function_contains(source, "zz9k_lib_close",
                         "ObtainSemaphore(&base->lock)")) {
    printf("zz9k_lib_close: missing semaphore guard\n");
    failures++;
  }

  if (!expect_vector_table_complete(source)) {
    failures++;
  }

  for (i = 0; i < sizeof(public_calls) / sizeof(public_calls[0]); i++) {
    if (!expect_public_call_locked(source, public_calls[i])) {
      failures++;
    }
  }

  free(source);
  return failures ? 1 : 0;
}
