/*
 * Strong replacement for the BSP's weak _sbrk.  The heap bounds are linker
 * symbols; the counters describe live break consumption, not its reservation.
 */
#include <errno.h>
#include <sys/types.h>
#include <stdint.h>
#include "xil_types.h"
#include "sdk_smp_lock.h"
#include "newlib_heap.h"

extern uint8_t _heap_start[];
extern uint8_t _heap_end[];

static uint8_t *heap_break;
static volatile uint32_t heap_used;
static volatile uint32_t heap_peak;

caddr_t _sbrk(s32 increment)
{
    uint8_t *previous;
    uint8_t *next;
    uint32_t decrement;

    if (heap_break == 0)
        heap_break = _heap_start;
    previous = heap_break;
    if (increment >= 0) {
        if ((uint32_t)increment > (uint32_t)(_heap_end - heap_break)) {
            errno = ENOMEM;
            return (caddr_t)-1;
        }
    } else {
        decrement = (uint32_t)(-(increment + 1)) + 1U;
        if (decrement > (uint32_t)(heap_break - _heap_start)) {
            errno = ENOMEM;
            return (caddr_t)-1;
        }
    }
    next = heap_break + increment;
    heap_break = next;
    heap_used = (uint32_t)(next - _heap_start);
    if (heap_used > heap_peak)
        heap_peak = heap_used;
    return (caddr_t)previous;
}

void newlib_heap_usage(uint32_t *used, uint32_t *peak)
{
    sdk_smp_lock_malloc_acquire();
    if (used)
        *used = heap_used;
    if (peak)
        *peak = heap_peak;
    sdk_smp_lock_malloc_release();
}
