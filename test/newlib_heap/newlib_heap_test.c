/* Host regression tests for the firmware newlib break override. */
/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#include "xil_types.h"
#include "newlib_heap.h"

caddr_t _sbrk(s32 increment);
extern uint8_t _heap_start[];

#define HEAP_BYTES 1024U

uint8_t heap_bytes[HEAP_BYTES];

/* Model the linker-provided heap bounds used by newlib_heap.c. */
__asm__(".globl _heap_start\n"
        "_heap_start = heap_bytes\n"
        ".globl _heap_end\n"
        "_heap_end = heap_bytes + 1024\n");

static int lock_depth;

void sdk_smp_lock_malloc_acquire(void)
{
	lock_depth++;
}

void sdk_smp_lock_malloc_release(void)
{
	lock_depth--;
}

static int checks;
static int failures;

#define CHECK(expr) do { \
	checks++; \
	if (!(expr)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
	} \
} while (0)

static void check_usage(uint32_t expected_used, uint32_t expected_peak)
{
	uint32_t used = UINT32_MAX;
	uint32_t peak = UINT32_MAX;

	newlib_heap_usage(&used, &peak);
	CHECK(used == expected_used);
	CHECK(peak == expected_peak);
	CHECK(lock_depth == 0);
}

int main(void)
{
	caddr_t start = (caddr_t)_heap_start;
	caddr_t before_failure;

	CHECK(_sbrk(0) == start);
	check_usage(0U, 0U);

	CHECK(_sbrk(128) == start);
	check_usage(128U, 128U);
	CHECK(_sbrk(256) == start + 128);
	check_usage(384U, 384U);

	CHECK(_sbrk(-64) == start + 384);
	check_usage(320U, 384U);
	CHECK(_sbrk((s32)(HEAP_BYTES - 320U)) == start + 320);
	check_usage(HEAP_BYTES, HEAP_BYTES);

	before_failure = _sbrk(0);
	errno = 0;
	CHECK(_sbrk(1) == (caddr_t)-1);
	CHECK(errno == ENOMEM);
	CHECK(_sbrk(0) == before_failure);
	check_usage(HEAP_BYTES, HEAP_BYTES);

	CHECK(_sbrk(-(s32)HEAP_BYTES) == start + HEAP_BYTES);
	check_usage(0U, HEAP_BYTES);
	before_failure = _sbrk(0);
	errno = 0;
	CHECK(_sbrk(-1) == (caddr_t)-1);
	CHECK(errno == ENOMEM);
	CHECK(_sbrk(0) == before_failure);
	check_usage(0U, HEAP_BYTES);

	printf("%d checks, %d failures\n", checks, failures);
	return failures != 0;
}
