/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "sdk_decode_reclaim.h"

#define HOST_RUNTIME_MAX_ALLOCATIONS 128U

struct host_allocation {
  void *ptr;
  size_t size;
};

static struct host_allocation allocations[HOST_RUNTIME_MAX_ALLOCATIONS];
static unsigned allocation_attempts;
static unsigned fail_on_attempt;
static size_t allocated_bytes;
static size_t peak_allocated_bytes;

static void record_allocation(void *ptr, size_t size)
{
  unsigned i;

  if (!ptr)
    return;
  for (i = 0U; i < HOST_RUNTIME_MAX_ALLOCATIONS; ++i) {
    if (!allocations[i].ptr) {
      allocations[i].ptr = ptr;
      allocations[i].size = size;
      allocated_bytes += size;
      if (allocated_bytes > peak_allocated_bytes)
        peak_allocated_bytes = allocated_bytes;
      return;
    }
  }
}

static void forget_allocation(void *ptr)
{
  unsigned i;

  for (i = 0U; i < HOST_RUNTIME_MAX_ALLOCATIONS; ++i) {
    if (allocations[i].ptr == ptr) {
      allocated_bytes -= allocations[i].size;
      allocations[i].ptr = 0;
      allocations[i].size = 0U;
      return;
    }
  }
}

void host_runtime_reset(void)
{
  unsigned i;

  allocation_attempts = 0U;
  fail_on_attempt = 0U;
  allocated_bytes = 0U;
  peak_allocated_bytes = 0U;
  for (i = 0U; i < HOST_RUNTIME_MAX_ALLOCATIONS; ++i) {
    allocations[i].ptr = 0;
    allocations[i].size = 0U;
  }
}

void host_runtime_fail_on_allocation(unsigned attempt)
{
  fail_on_attempt = attempt;
}

unsigned host_runtime_allocation_attempts(void)
{
  return allocation_attempts;
}

size_t host_runtime_allocated_bytes(void)
{
  return allocated_bytes;
}

size_t host_runtime_peak_allocated_bytes(void)
{
  return peak_allocated_bytes;
}

void *sdk_decode_heap_alloc(size_t size)
{
  void *ptr;

  ++allocation_attempts;
  if (fail_on_attempt != 0U && allocation_attempts == fail_on_attempt)
    return 0;
  ptr = malloc(size);
  record_allocation(ptr, size);
  sdk_decode_track(ptr);
  return ptr;
}

void sdk_decode_heap_free(void *ptr)
{
  sdk_decode_untrack(ptr);
  forget_allocation(ptr);
  free(ptr);
}

void smp_raw_spin_lock(volatile uint32_t *word)
{
  (void)word;
}

void smp_raw_spin_unlock(volatile uint32_t *word)
{
  (void)word;
}
