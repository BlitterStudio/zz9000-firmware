/*
 * Unit checks for the sliced sleep wait. Includes sleep.c directly and
 * replaces the host clock/stop/pump seams; SDL_Delay stays real, so a
 * wrong slice size shows up as wall-clock time.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "../src/sleep.c"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static uint64_t script[8];
static int use_script;
static int clock_reads;
static int stop_after; /* stop once host_stop_requested passes this count */
static int stop_calls;
static int pumps;

uint64_t host_time_us(void)
{
    if (use_script)
        return script[clock_reads < 8 ? clock_reads++ : 7];
    {
        struct timespec ts;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
    }
}

int host_stop_requested(void)
{
    stop_calls++;
    return stop_after != 0 && stop_calls > stop_after;
}

void host_pump(void)
{
    pumps++;
}

static void reset_mocks(void)
{
    memset(script, 0, sizeof script);
    use_script = 0;
    clock_reads = 0;
    stop_after = 0;
    stop_calls = 0;
    pumps = 0;
}

static uint64_t now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* The deadline passing between the check and the wait must not wrap into a
 * days-long delay: the clock reads 0 for the comparison and 2 after. */
static void test_deadline_passed_between_reads(void)
{
    reset_mocks();
    use_script = 1;
    script[0] = 0;
    script[1] = 2;
    script[2] = 2;
    host_sleep_until(1);
    assert(clock_reads >= 2);
}

/* A far deadline must still wake for a stop request within a few slices. */
static void test_stop_wakes_long_sleep(void)
{
    uint64_t start;
    uint64_t elapsed;

    reset_mocks();
    stop_after = 3;
    start = now_us();
    host_sleep_until(start + 1000000ull);
    elapsed = now_us() - start;
    assert(elapsed < 500000ull);
}

int main(void)
{
    test_deadline_passed_between_reads();
    test_stop_wakes_long_sleep();
    puts("sleep_unit: ok");
    return 0;
}
