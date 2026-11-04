/*
 * Core-1 memory/cache benchmark for the trusted ZZ9000 ARM app runtime.
 * It keeps all timed loops in app memory and emits one table through zz_log.
 *
 * SPDX-License-Identifier: MIT
 */
#include "zzapp.h"

#include <alloca.h>
#include <stdint.h>
#include <stdlib.h>

#define BENCH_MAX_BYTES  (8u << 20)
#define BENCH_ROUNDS     8u
#define BENCH_ALU_ADDS   20000000u
#define BENCH_STACK_BYTES ((10u << 20) + 64u)

static uint8_t bench_bss[BENCH_MAX_BYTES + 64u];
static volatile uint32_t bench_sink;

struct seq_result {
    uint32_t mib_s;
    uint32_t cycles_per_kib;
};

struct chase_result {
    uint32_t cycles_per_load[4];
};

static uint32_t align64(uintptr_t value)
{
    return (uint32_t)((value + 63u) & ~(uintptr_t)63u);
}

static uint32_t pmu_cycles(void)
{
    uint32_t value;

    __asm__ __volatile__("isb\n\tmrc p15, 0, %0, c9, c13, 0"
                         : "=r"(value) : : "memory");
    return value;
}

static void pmu_enable(void)
{
    uint32_t pmcr;

    __asm__ __volatile__("mrc p15, 0, %0, c9, c12, 0" : "=r"(pmcr));
    pmcr |= 1u;              /* PMCR.E: enable counters. */
    pmcr &= ~8u;             /* PMCR.D: do not divide CCNT by 64. */
    __asm__ __volatile__("mcr p15, 0, %0, c9, c12, 0" : : "r"(pmcr));
    __asm__ __volatile__("mcr p15, 0, %0, c9, c12, 1" : : "r"(0x80000000u));
    __asm__ __volatile__("isb" ::: "memory");
}

static uint32_t elapsed32(uint32_t begin, uint32_t end)
{
    return end - begin;
}

static struct seq_result seq_measure(uint8_t *memory, uint32_t bytes, int write)
{
    volatile uint32_t *words = (volatile uint32_t *)(void *)memory;
    uint32_t count = bytes / (uint32_t)sizeof(*words);
    uint32_t sum = 0u;
    uint32_t begin_cycles;
    uint32_t elapsed_cycles;
    uint64_t begin_us;
    uint64_t elapsed_us;
    uint32_t round;
    uint32_t index;
    uint64_t total = (uint64_t)bytes * BENCH_ROUNDS;
    struct seq_result result = {0u, 0u};

    begin_us = zz_time_us();
    begin_cycles = pmu_cycles();
    for (round = 0u; round < BENCH_ROUNDS; round++) {
        if (write) {
            for (index = 0u; index < count; index++)
                words[index] = index + round;
        } else {
            for (index = 0u; index < count; index++)
                sum += words[index];
        }
    }
    elapsed_cycles = elapsed32(begin_cycles, pmu_cycles());
    elapsed_us = zz_time_us() - begin_us;
    bench_sink ^= sum;
    if (elapsed_us != 0u)
        result.mib_s = (uint32_t)((total * 1000000u) /
                                  (elapsed_us * 1024u * 1024u));
    if (total != 0u)
        result.cycles_per_kib =
            (uint32_t)(((uint64_t)elapsed_cycles * 1024u) / total);
    return result;
}

static void chase_prepare(uint8_t *memory, uint32_t bytes)
{
    volatile uint32_t *words = (volatile uint32_t *)(void *)memory;
    uint32_t index;
    uint32_t count = bytes / (uint32_t)sizeof(*words);

    /* A full-period LCG over a power-of-two word count: every link is a
     * dependent, non-linear next address, so no read can be overlapped. */
    for (index = 0u; index < count; index++)
        words[index] = index * 1664525u + 1013904223u;
}

static uint32_t chase_measure(uint8_t *memory, uint32_t bytes)
{
    volatile uint32_t *words = (volatile uint32_t *)(void *)memory;
    uint32_t steps = bytes <= (64u << 10) ? 1048576u : 262144u;
    uint32_t index = 0u;
    uint32_t begin;
    uint32_t elapsed;
    uint32_t step;

    chase_prepare(memory, bytes);
    begin = pmu_cycles();
    for (step = 0u; step < steps; step++)
        index = words[index & (bytes / 4u - 1u)];
    elapsed = elapsed32(begin, pmu_cycles());
    bench_sink ^= index;
    return elapsed / steps;
}

static struct chase_result chase_all(uint8_t *memory)
{
    static const uint32_t sizes[4] = {4096u, 65536u, 1u << 20, 8u << 20};
    struct chase_result result;
    uint32_t i;

    for (i = 0u; i < 4u; i++)
        result.cycles_per_load[i] = chase_measure(memory, sizes[i]);
    return result;
}

static uint32_t alu_measure(uint32_t *cycles_out, uint32_t *mhz_out)
{
    uint32_t value = 1u;
    uint32_t begin_cycles;
    uint32_t elapsed_cycles;
    uint64_t begin_us;
    uint64_t elapsed_us;
    uint32_t i;

    begin_us = zz_time_us();
    begin_cycles = pmu_cycles();
    for (i = 0u; i < BENCH_ALU_ADDS; i++)
        __asm__ __volatile__("add %0, %0, %1" : "+r"(value) : "r"(i) : "cc");
    elapsed_cycles = elapsed32(begin_cycles, pmu_cycles());
    elapsed_us = zz_time_us() - begin_us;
    bench_sink ^= value;
    *cycles_out = elapsed_cycles;
    *mhz_out = elapsed_us == 0u ? 0u : (uint32_t)(elapsed_cycles / elapsed_us);
    return elapsed_cycles / BENCH_ALU_ADDS;
}

static void append_char(char **at, char *end, char c)
{
    if (*at < end)
        *(*at)++ = c;
}

static void append_text(char **at, char *end, const char *text)
{
    while (*text != '\0')
        append_char(at, end, *text++);
}

static void append_u32(char **at, char *end, uint32_t value)
{
    char digits[10];
    uint32_t count = 0u;

    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    while (count != 0u)
        append_char(at, end, digits[--count]);
}

static void append_seq(char **at, char *end, const struct seq_result *read,
                       const struct seq_result *write)
{
    append_u32(at, end, read->mib_s);
    append_char(at, end, '/');
    append_u32(at, end, write->mib_s);
    append_char(at, end, ' ');
    append_u32(at, end, read->cycles_per_kib);
    append_char(at, end, '/');
    append_u32(at, end, write->cycles_per_kib);
}

static void append_chase(char **at, char *end, const struct chase_result *chase)
{
    uint32_t i;

    for (i = 0u; i < 4u; i++) {
        if (i != 0u)
            append_char(at, end, '/');
        append_u32(at, end, chase->cycles_per_load[i]);
    }
}

static void append_region(char **at, char *end, const char *name,
                          uint8_t *memory)
{
    struct seq_result read = seq_measure(memory, BENCH_MAX_BYTES, 0);
    struct seq_result write = seq_measure(memory, BENCH_MAX_BYTES, 1);
    struct chase_result chase = chase_all(memory);

    append_text(at, end, name);
    append_text(at, end, "  ");
    append_seq(at, end, &read, &write);
    append_text(at, end, "  ");
    append_chase(at, end, &chase);
    append_char(at, end, '\n');
}

int zz_main(int argc, char **argv)
{
    char report[1024];
    char *at = report;
    char *const end = report + sizeof(report) - 1u;
    uint8_t *stack_raw;
    uint8_t *stack;
    uint8_t *heap_raw;
    uint8_t *heap;
    zzapp_surface display = {0};
    int display_rc;
    uint32_t alu_cycles;
    uint32_t mhz;
    uint32_t cycles_per_add;

    (void)argc;
    (void)argv;
    pmu_enable();
    cycles_per_add = alu_measure(&alu_cycles, &mhz);
    stack_raw = (uint8_t *)alloca(BENCH_STACK_BYTES);
    stack = (uint8_t *)(uintptr_t)align64((uintptr_t)stack_raw);
    heap_raw = (uint8_t *)malloc(BENCH_MAX_BYTES + 64u);
    if (heap_raw == NULL) {
        append_text(&at, end, "zzmembench: malloc failed\n");
        *at = '\0';
        zz_log(report, (uint32_t)(at - report));
        return ZZ_ENOMEM;
    }
    heap = (uint8_t *)(uintptr_t)align64((uintptr_t)heap_raw);

    append_text(&at, end, "zzmembench PMU CCNT (warm random pointer chase)\n");
    append_text(&at, end, "clock ");
    append_u32(&at, end, mhz);
    append_text(&at, end, " MHz; alu ");
    append_u32(&at, end, BENCH_ALU_ADDS);
    append_text(&at, end, " adds ");
    append_u32(&at, end, alu_cycles);
    append_text(&at, end, " cycles, ");
    append_u32(&at, end, cycles_per_add);
    append_text(&at, end, " c/add\n");
    append_text(&at, end, "region  R/W MiB/s  R/W cyc/KiB  chase cyc/load (4K/64K/1M/8M)\n");
    append_region(&at, end, "bss  ", bench_bss);
    append_region(&at, end, "stack", stack);
    append_region(&at, end, "heap ", heap);

    display_rc = zz_display_open(2048u, 1024u, ZZ_FMT_BGRA8888, 0u, &display);
    if (display_rc == ZZ_OK && display.back != NULL &&
        display.pitch >= 2048u * 4u && display.height >= 1024u) {
        append_region(&at, end, "p96  ", (uint8_t *)display.back);
        (void)zz_display_close(display.id);
    } else {
        append_text(&at, end, "p96   unavailable rc ");
        append_u32(&at, end, (uint32_t)display_rc);
        append_char(&at, end, '\n');
        if (display_rc == ZZ_OK)
            (void)zz_display_close(display.id);
    }
    append_text(&at, end,
                "firmware cached buffer unavailable: app ABI intentionally exposes none\n");
    *at = '\0';
    zz_log(report, (uint32_t)(at - report));
    free(heap_raw);
    return 0;
}
