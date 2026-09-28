/*
 * Real zz9k.library smoke test for AmigaOS.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "proto/zz9k.h"
#include "zz9k/sdk.h"
#include <proto/exec.h>
#include <stdio.h>

struct Library *ZZ9KBase;

static void print_capability_name(int *first, const char *name)
{
  if (!*first) {
    printf(",");
  }
  printf("%s", name);
  *first = 0;
}

static void print_capability_names(uint32_t capability_bits)
{
  uint32_t remaining;
  uint32_t count;
  uint32_t i;
  int first;

  printf("Capability names: ");
  remaining = capability_bits;
  count = zz9k_known_capability_count();
  first = 1;

  for (i = 0; i < count; i++) {
    uint32_t bit;
    const char *name;

    bit = zz9k_known_capability_bit(i);
    if (bit == 0U || (capability_bits & bit) == 0U) {
      continue;
    }
    name = zz9k_capability_name(bit);
    if (name) {
      print_capability_name(&first, name);
    }
    remaining &= ~bit;
  }

  if (remaining != 0U) {
    char unknown[12];
    sprintf(unknown, "0x%08lx", (unsigned long)remaining);
    print_capability_name(&first, unknown);
  }

  if (first) {
    printf("none");
  }
  printf("\n");
}

int main(void)
{
  ZZ9KCaps caps;
  ZZ9KDiagInfo diag;
  int status;

  printf("zz9k-libtest: opening zz9k.library\n");
  ZZ9KBase = OpenLibrary((CONST_STRPTR)ZZ9K_LIBRARY_NAME,
                         ZZ9K_LIBRARY_VERSION);
  if (!ZZ9KBase) {
    printf("open failed\n");
    return 20;
  }

  printf("open ok base=0x%08lx version=%u revision=%u\n",
         (unsigned long)ZZ9KBase, ZZ9KBase->lib_Version,
         ZZ9KBase->lib_Revision);
  printf("querying capabilities\n");

  status = ZZ9KQueryCaps(&caps);
  if (status != ZZ9K_STATUS_OK) {
    printf("query caps failed: %s (%d)\n", zz9k_status_text(status),
           status);
    CloseLibrary(ZZ9KBase);
    return 10;
  }

  printf("SDK ABI %lu.%lu caps=0x%08lx queue=%lu completion=%lu\n",
         (unsigned long)caps.abi_major, (unsigned long)caps.abi_minor,
         (unsigned long)caps.capability_bits,
         (unsigned long)caps.request_ring_entries,
         (unsigned long)caps.completion_ring_entries);
  print_capability_names(caps.capability_bits);

  status = ZZ9KReadDiag(&diag);
  if (status == ZZ9K_STATUS_OK) {
    printf("diag: completed=%lu failed=%lu last_status=%s (%lu) "
           "heap_free=%lu largest=%lu\n",
           (unsigned long)diag.requests_completed,
           (unsigned long)diag.requests_failed,
           zz9k_status_text((int)diag.last_status),
           (unsigned long)diag.last_status,
           (unsigned long)diag.shared_heap_free,
           (unsigned long)diag.shared_heap_largest_free);
  } else {
    printf("diag failed: %s (%d)\n", zz9k_status_text(status), status);
  }

  CloseLibrary(ZZ9KBase);
  return status == ZZ9K_STATUS_OK ? 0 : 10;
}
