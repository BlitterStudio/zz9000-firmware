/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host stand-ins for the firmware decode heap, plus the card pool backing
 * the WebP session arenas. Both count toward the attempt counter and the
 * failure injection: card_pool_alloc is linked with --wrap, so the real
 * pool runs and this file only observes and fails its calls. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "card_pool.h"
#include "host_card_pool.h"
#include "sdk_decode_reclaim.h"

#define HOST_RUNTIME_POOL_BYTES (96U << 20)

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

static void note_peak(void)
{
  size_t now = allocated_bytes + host_card_pool_used();

  if (now > peak_allocated_bytes)
    peak_allocated_bytes = now;
}

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
      note_peak();
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
  host_card_pool_reset(HOST_RUNTIME_POOL_BYTES);
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
  return allocated_bytes + host_card_pool_used();
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

uint32_t __real_card_pool_alloc(card_pool_t *pool, uint32_t size,
                                uint32_t owner);
uint32_t __wrap_card_pool_alloc(card_pool_t *pool, uint32_t size,
                                uint32_t owner)
{
  uint32_t addr;

  ++allocation_attempts;
  if (fail_on_attempt != 0U && allocation_attempts == fail_on_attempt)
    return 0U;
  if (pool == &card_pool && card_pool.range_count == 0U)
    host_card_pool_reset(HOST_RUNTIME_POOL_BYTES);
  addr = __real_card_pool_alloc(pool, size, owner);
  note_peak();
  return addr;
}

void smp_raw_spin_lock(volatile uint32_t *word)
{
  (void)word;
}

void smp_raw_spin_unlock(volatile uint32_t *word)
{
  (void)word;
}
