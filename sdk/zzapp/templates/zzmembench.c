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

#define BENCH_MAX_BYTES       (8u << 20)
#define BENCH_ROUNDS          8u
#define BENCH_ALU_ADDS        20000000u
#define BENCH_STACK_BYTES     ((512u << 10) + 64u)
#define PL310_BASE             0xf8f02000u
#define PL310_ID               0x000u
#define PL310_AUX              0x104u
#define PL310_PREFETCH         0xf60u
#define PL310_POWER            0xf80u
#define DDRC_BASE              0xf8006000u
#define PL310_DOUBLE_LINEFILL  (1u << 30)
#define PL310_INCR_LINEFILL    (1u << 23)
#define PL310_PREFETCH_OFFSET  0x1fu

static uint8_t bench_bss[BENCH_MAX_BYTES + 64u];
static volatile uint32_t bench_sink;

struct seq_result {
    uint32_t mib_s;
    uint32_t cycles_per_kib;
};

struct chase_result {
    uint32_t cycles_per_load[4];
};

static uint8_t *align64(void *value)
{
    return (uint8_t *)(((uintptr_t)value + 63u) & ~(uintptr_t)63u);
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
    pmcr |= 1u;
    pmcr &= ~8u;
    __asm__ __volatile__("mcr p15, 0, %0, c9, c12, 0" : : "r"(pmcr));
    __asm__ __volatile__("mcr p15, 0, %0, c9, c12, 1" : : "r"(0x80000000u));
    __asm__ __volatile__("isb" ::: "memory");
}

static uint32_t cpsr_read(void)
{
    uint32_t value;

    __asm__ __volatile__("mrs %0, cpsr" : "=r"(value));
    return value;
}

static uint32_t sctlr_read(void)
{
    uint32_t value;

    __asm__ __volatile__("mrc p15, 0, %0, c1, c0, 0" : "=r"(value));
    return value;
}

static uint32_t actlr_read(void)
{
    uint32_t value;

    __asm__ __volatile__("mrc p15, 0, %0, c1, c0, 1" : "=r"(value));
    return value;
}

static uint32_t mmio_read(uint32_t address)
{
    return *(volatile const uint32_t *)(uintptr_t)address;
}

static void mmio_write(uint32_t address, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)address = value;
    __asm__ __volatile__("dsb sy\n\tisb" ::: "memory");
}

static uint32_t elapsed32(uint32_t begin, uint32_t end)
{
    return end - begin;
}

static struct seq_result seq_words(uint8_t *memory, uint32_t bytes, int write)
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

static struct seq_result seq_ldrd(uint8_t *memory, uint32_t bytes)
{
    uint32_t begin_cycles;
    uint32_t elapsed_cycles;
    uint64_t begin_us;
    uint64_t elapsed_us;
    uint64_t sum = 0u;
    uint32_t round;
    uint32_t offset;
    uint64_t total = (uint64_t)bytes * BENCH_ROUNDS;
    struct seq_result result = {0u, 0u};

    begin_us = zz_time_us();
    begin_cycles = pmu_cycles();
    for (round = 0u; round < BENCH_ROUNDS; round++) {
        for (offset = 0u; offset < bytes; offset += 8u) {
            uint64_t value;

            __asm__ __volatile__("ldrd %Q0, %R0, [%1]"
                                 : "=&r"(value)
                                 : "r"(memory + offset) : "memory");
            sum += value;
        }
    }
    elapsed_cycles = elapsed32(begin_cycles, pmu_cycles());
    elapsed_us = zz_time_us() - begin_us;
    bench_sink ^= (uint32_t)sum;
    if (elapsed_us != 0u)
        result.mib_s = (uint32_t)((total * 1000000u) /
                                  (elapsed_us * 1024u * 1024u));
    result.cycles_per_kib = (uint32_t)(((uint64_t)elapsed_cycles * 1024u) / total);
    return result;
}

static struct seq_result seq_line_store(uint8_t *memory, uint32_t bytes)
{
    uint32_t begin_cycles;
    uint32_t elapsed_cycles;
    uint64_t begin_us;
    uint64_t elapsed_us;
    uint32_t round;
    uint32_t offset;
    uint64_t total = (uint64_t)bytes * BENCH_ROUNDS;
    struct seq_result result = {0u, 0u};

    begin_us = zz_time_us();
    begin_cycles = pmu_cycles();
    for (round = 0u; round < BENCH_ROUNDS; round++) {
        uint32_t lo = round;
        uint32_t hi = ~round;

        for (offset = 0u; offset < bytes; offset += 32u)
            __asm__ __volatile__(
                "str %1, [%0, #0]\n\t"
                "str %2, [%0, #4]\n\t"
                "str %1, [%0, #8]\n\t"
                "str %2, [%0, #12]\n\t"
                "str %1, [%0, #16]\n\t"
                "str %2, [%0, #20]\n\t"
                "str %1, [%0, #24]\n\t"
                "str %2, [%0, #28]"
                : : "r"(memory + offset), "r"(lo), "r"(hi) : "memory");
    }
    elapsed_cycles = elapsed32(begin_cycles, pmu_cycles());
    elapsed_us = zz_time_us() - begin_us;
    if (elapsed_us != 0u)
        result.mib_s = (uint32_t)((total * 1000000u) /
                                  (elapsed_us * 1024u * 1024u));
    result.cycles_per_kib = (uint32_t)(((uint64_t)elapsed_cycles * 1024u) / total);
    return result;
}

static void chase_prepare(uint8_t *memory, uint32_t bytes)
{
    volatile uint32_t *words = (volatile uint32_t *)(void *)memory;
    uint32_t index;
    uint32_t count = bytes / (uint32_t)sizeof(*words);

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

static struct chase_result chase_all(uint8_t *memory, uint32_t max_bytes)
{
    static const uint32_t sizes[4] = {4096u, 65536u, 1u << 20, 8u << 20};
    struct chase_result result;
    uint32_t i;

    for (i = 0u; i < 4u; i++)
        result.cycles_per_load[i] = sizes[i] <= max_bytes ?
            chase_measure(memory, sizes[i]) : 0u;
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

static void append_hex(char **at, char *end, uint32_t value)
{
    static const char digits[] = "0123456789abcdef";
    uint32_t shift;

    append_text(at, end, "0x");
    for (shift = 28u;; shift -= 4u) {
        append_char(at, end, digits[(value >> shift) & 15u]);
        if (shift == 0u)
            break;
    }
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
                          uint8_t *memory, uint32_t max_bytes)
{
    struct seq_result read = seq_words(memory, max_bytes, 0);
    struct seq_result write = seq_words(memory, max_bytes, 1);
    struct chase_result chase = chase_all(memory, max_bytes);

    append_text(at, end, name);
    append_text(at, end, "  ");
    append_seq(at, end, &read, &write);
    append_text(at, end, "  ");
    append_chase(at, end, &chase);
    append_char(at, end, '\n');
}

static void append_stream(char **at, char *end, const char *name, uint8_t *memory)
{
    struct seq_result words_read = seq_words(memory, BENCH_MAX_BYTES, 0);
    struct seq_result ldrd_read = seq_ldrd(memory, BENCH_MAX_BYTES);
    struct seq_result line_write = seq_line_store(memory, BENCH_MAX_BYTES);
    struct chase_result chase = chase_all(memory, BENCH_MAX_BYTES);

    append_text(at, end, name);
    append_text(at, end, " wrdR ");
    append_seq(at, end, &words_read, &line_write);
    append_text(at, end, " ldrdR ");
    append_u32(at, end, ldrd_read.mib_s);
    append_char(at, end, '/');
    append_u32(at, end, ldrd_read.cycles_per_kib);
    append_text(at, end, " chase ");
    append_chase(at, end, &chase);
    append_char(at, end, '\n');
}

static int wants_tune(int argc, char **argv)
{
    int i;

    for (i = 0; i < argc; i++)
        if (argv[i] != NULL && argv[i][0] == '-' && argv[i][1] == 't' &&
            argv[i][2] == 'u' && argv[i][3] == 'n' && argv[i][4] == 'e' &&
            argv[i][5] == '\0')
            return 1;
    return 0;
}

static void append_config(char **at, char *end)
{
    append_text(at, end, "cpu cpsr ");
    append_hex(at, end, cpsr_read());
    append_text(at, end, " sctlr ");
    append_hex(at, end, sctlr_read());
    append_text(at, end, " actlr ");
    append_hex(at, end, actlr_read());
    append_char(at, end, '\n');
    append_text(at, end, "pl310 id ");
    append_hex(at, end, mmio_read(PL310_BASE + PL310_ID));
    append_text(at, end, " aux ");
    append_hex(at, end, mmio_read(PL310_BASE + PL310_AUX));
    append_text(at, end, " prefetch ");
    append_hex(at, end, mmio_read(PL310_BASE + PL310_PREFETCH));
    append_text(at, end, " power ");
    append_hex(at, end, mmio_read(PL310_BASE + PL310_POWER));
    append_char(at, end, '\n');
    append_text(at, end, "ddrc ctrl/hpr/lpr/page/go2c ");
    append_hex(at, end, mmio_read(DDRC_BASE + 0x00u));
    append_char(at, end, '/');
    append_hex(at, end, mmio_read(DDRC_BASE + 0x08u));
    append_char(at, end, '/');
    append_hex(at, end, mmio_read(DDRC_BASE + 0x0cu));
    append_char(at, end, '/');
    append_hex(at, end, mmio_read(DDRC_BASE + 0x60u));
    append_char(at, end, '/');
    append_hex(at, end, mmio_read(DDRC_BASE + 0x64u));
    append_char(at, end, '\n');
    append_text(at, end, "ddrc rd-priority p0-p3 ");
    append_hex(at, end, mmio_read(DDRC_BASE + 0x218u));
    append_char(at, end, '/');
    append_hex(at, end, mmio_read(DDRC_BASE + 0x21cu));
    append_char(at, end, '/');
    append_hex(at, end, mmio_read(DDRC_BASE + 0x220u));
    append_char(at, end, '/');
    append_hex(at, end, mmio_read(DDRC_BASE + 0x224u));
    append_char(at, end, '\n');
}

/* PL310 Prefetch Control settings to compare.  Offsets must be 0-7, 15, 23
 * or 31 (DDI 0246F 3.3.14).  The L1 prefetch bit made no difference on the
 * card, so it stays as configured. */
static void append_tune(char **at, char *end, uint8_t *memory)
{
    static const struct {
        uint32_t offset;
        uint32_t extra;
    } settings[] = {
        {0u, 0u},
        {3u, 0u},
        {7u, 0u},
        {15u, 0u},
        {7u, PL310_DOUBLE_LINEFILL},
        {15u, PL310_DOUBLE_LINEFILL},
        {15u, PL310_DOUBLE_LINEFILL | PL310_INCR_LINEFILL},
    };
    uint32_t original_prefetch = mmio_read(PL310_BASE + PL310_PREFETCH);
    uint32_t base = original_prefetch & ~(PL310_PREFETCH_OFFSET |
                                          PL310_DOUBLE_LINEFILL |
                                          PL310_INCR_LINEFILL);
    uint32_t i;

    append_text(at, end, "tune: PL310 prefetch control values\n");
    for (i = 0u; i < sizeof(settings) / sizeof(settings[0]); i++) {
        uint32_t value = base | settings[i].offset | settings[i].extra;

        mmio_write(PL310_BASE + PL310_PREFETCH, value);
        if (mmio_read(PL310_BASE + PL310_PREFETCH) != value) {
            append_text(at, end, "tune unavailable: prefetch write rejected\n");
            break;
        }
        append_text(at, end, "tune p ");
        append_hex(at, end, value);
        append_char(at, end, ' ');
        append_stream(at, end, "bss", memory);
    }
    mmio_write(PL310_BASE + PL310_PREFETCH, original_prefetch);
    append_text(at, end, "tune restored p ");
    append_hex(at, end, mmio_read(PL310_BASE + PL310_PREFETCH));
    append_char(at, end, '\n');
}

/* zz_log truncates one call at 1024 bytes and the firmware passes at most
 * 8 lines per second to the UART: send the report one line per call,
 * 150 ms apart.  The firmware adds the line ending. */
static void log_lines(const char *text)
{
    while (*text != '\0') {
        const char *nl = text;

        while (*nl != '\0' && *nl != '\n')
            nl++;
        zz_log(text, (uint32_t)(nl - text));
        zz_sleep_us(150000u);
        text = *nl == '\n' ? nl + 1 : nl;
    }
}

int zz_main(int argc, char **argv)
{
    char report[4096];
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

    pmu_enable();
    cycles_per_add = alu_measure(&alu_cycles, &mhz);
    stack_raw = (uint8_t *)alloca(BENCH_STACK_BYTES);
    stack = align64(stack_raw);
    heap_raw = (uint8_t *)malloc(BENCH_MAX_BYTES + 64u);
    if (heap_raw == NULL) {
        append_text(&at, end, "zzmembench: malloc failed\n");
        *at = '\0';
        zz_log(report, (uint32_t)(at - report));
        return ZZ_ENOMEM;
    }
    heap = align64(heap_raw);

    append_text(&at, end, "zzmembench PMU CCNT (warm random pointer chase)\n");
    append_config(&at, end);
    append_text(&at, end, "clock ");
    append_u32(&at, end, mhz);
    append_text(&at, end, " MHz; alu ");
    append_u32(&at, end, BENCH_ALU_ADDS);
    append_text(&at, end, " adds ");
    append_u32(&at, end, alu_cycles);
    append_text(&at, end, " cycles, ");
    append_u32(&at, end, cycles_per_add);
    append_text(&at, end, " c/add\n");
    append_text(&at, end, "region R/W MiB/s R/W cyc/KiB chase cyc/load (4K/64K/1M/8M)\n");
    append_region(&at, end, "bss  ", bench_bss, BENCH_MAX_BYTES);
    append_region(&at, end, "stack", stack, 512u << 10);
    append_region(&at, end, "heap ", heap, BENCH_MAX_BYTES);

    display_rc = zz_display_open(640u, 480u, ZZ_FMT_RGB565, 0u, &display);
    if (display_rc == ZZ_OK && display.back != NULL &&
        display.pitch >= 640u * 2u && display.height >= 480u) {
        uint32_t span = display.pitch * display.height;
        uint32_t bytes = 1u;

        while (bytes * 2u <= span)
            bytes *= 2u;
        append_region(&at, end, "p96  ", (uint8_t *)display.back, bytes);
        (void)zz_display_close(display.id);
    } else {
        append_text(&at, end, "p96 unavailable rc ");
        append_u32(&at, end, (uint32_t)display_rc);
        append_char(&at, end, '\n');
        if (display_rc == ZZ_OK)
            (void)zz_display_close(display.id);
    }
    append_stream(&at, end, "stream", bench_bss);
    if (wants_tune(argc, argv))
        append_tune(&at, end, bench_bss);
    append_text(&at, end,
                "firmware cached buffer unavailable: app ABI intentionally exposes none\n");
    *at = '\0';
    log_lines(report);
    free(heap_raw);
    return 0;
}
