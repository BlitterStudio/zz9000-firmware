/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host stand-ins for the firmware decode heap: counts and can fail
 * allocations, records live bytes, and registers every block with the real
 * sdk_decode_reclaim.c tracker exactly as core 1 does (smp_cpu_id() == 1). */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "sdk_decode_reclaim.h"
#include "host_runtime.h"

#define HOST_RUNTIME_MAX_ALLOCATIONS 256U

struct host_allocation {
  void *ptr;
  size_t size;
};

static struct host_allocation allocations[HOST_RUNTIME_MAX_ALLOCATIONS];
static unsigned allocation_attempts;
static unsigned fail_on_attempt;
static size_t allocated_bytes;
static size_t peak_allocated_bytes;
static unsigned live_blocks;

void host_runtime_reset(void)
{
  unsigned i;

  allocation_attempts = 0U;
  fail_on_attempt = 0U;
  allocated_bytes = 0U;
  peak_allocated_bytes = 0U;
  live_blocks = 0U;
  for (i = 0U; i < HOST_RUNTIME_MAX_ALLOCATIONS; ++i) {
    allocations[i].ptr = 0;
    allocations[i].size = 0U;
  }
}

void host_runtime_fail_on_allocation(unsigned attempt) { fail_on_attempt = attempt; }
unsigned host_runtime_allocation_attempts(void) { return allocation_attempts; }
size_t host_runtime_allocated_bytes(void) { return allocated_bytes; }
size_t host_runtime_peak_allocated_bytes(void) { return peak_allocated_bytes; }
unsigned host_runtime_live_blocks(void) { return live_blocks; }

void *sdk_decode_heap_alloc(size_t size)
{
  unsigned i;
  void *ptr;

  ++allocation_attempts;
  if (fail_on_attempt != 0U && allocation_attempts == fail_on_attempt)
    return 0;
  ptr = malloc(size);
  if (!ptr)
    return 0;
  for (i = 0U; i < HOST_RUNTIME_MAX_ALLOCATIONS; ++i) {
    if (!allocations[i].ptr) {
      allocations[i].ptr = ptr;
      allocations[i].size = size;
      break;
    }
  }
  allocated_bytes += size;
  if (allocated_bytes > peak_allocated_bytes)
    peak_allocated_bytes = allocated_bytes;
  ++live_blocks;
  sdk_decode_track(ptr);
  return ptr;
}

void sdk_decode_heap_free(void *ptr)
{
  unsigned i;

  if (!ptr)
    return;
  sdk_decode_untrack(ptr);
  for (i = 0U; i < HOST_RUNTIME_MAX_ALLOCATIONS; ++i) {
    if (allocations[i].ptr == ptr) {
      allocated_bytes -= allocations[i].size;
      allocations[i].ptr = 0;
      allocations[i].size = 0U;
      break;
    }
  }
  --live_blocks;
  free(ptr);
}

/* Decode work runs on the core-1 worker in firmware. The card pool's SMP
 * lock has nothing to contend with in this single-threaded host. */
int smp_cpu_id(void) { return 1; }
uint32_t smp_local_irq_save(void) { return 0U; }
void smp_local_irq_restore(uint32_t s) { (void)s; }
void smp_raw_spin_lock(volatile uint32_t *w) { *w = 1U; }
void smp_raw_spin_unlock(volatile uint32_t *w) { *w = 0U; }
