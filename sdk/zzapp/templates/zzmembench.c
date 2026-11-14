/*
 * Core-1 memory bandwidth benchmark for a USR-mode ZZ9000 app.
 * Timed loops stay in app memory and the report goes out through zz_log.
 * PMU, system control registers, and device MMIO are not available to apps.
 *
 * SPDX-License-Identifier: MIT
 */
#include "zzapp.h"
#include "zzapp_note.h"

ZZAPP_NOTE(ZZAPP_NOTE_FLAG_DISPLAY, 0)

#include <alloca.h>
#include <stdint.h>
#include <stdlib.h>

#define BENCH_MAX_BYTES   (8u << 20)
#define BENCH_ROUNDS      8u
#define BENCH_ALU_ADDS    20000000u
#define BENCH_STACK_BYTES ((512u << 10) + 64u)

static uint8_t bench_bss[BENCH_MAX_BYTES + 64u];
static volatile uint32_t bench_sink;

struct seq_result {
    uint32_t mib_s;
};

struct chase_result {
    uint32_t ns_per_load[4];
};

static uint8_t *align64(void *value)
{
    return (uint8_t *)(((uintptr_t)value + 63u) & ~(uintptr_t)63u);
}

static struct seq_result seq_words(uint8_t *memory, uint32_t bytes, int write)
{
    volatile uint32_t *words = (volatile uint32_t *)(void *)memory;
    uint32_t count = bytes / (uint32_t)sizeof(*words);
    uint32_t sum = 0u;
    uint64_t begin_us;
    uint64_t elapsed_us;
    uint32_t round;
    uint32_t index;
    uint64_t total = (uint64_t)bytes * BENCH_ROUNDS;
    struct seq_result result = {0u};

    begin_us = zz_time_us();
    for (round = 0u; round < BENCH_ROUNDS; round++) {
        if (write) {
            for (index = 0u; index < count; index++)
                words[index] = index + round;
        } else {
            for (index = 0u; index < count; index++)
                sum += words[index];
        }
    }
    elapsed_us = zz_time_us() - begin_us;
    bench_sink ^= sum;
    if (elapsed_us != 0u)
        result.mib_s = (uint32_t)((total * 1000000u) /
                                  (elapsed_us * 1024u * 1024u));
    return result;
}

static struct seq_result seq_ldrd(uint8_t *memory, uint32_t bytes)
{
    uint64_t begin_us;
    uint64_t elapsed_us;
    uint64_t sum = 0u;
    uint32_t round;
    uint32_t offset;
    uint64_t total = (uint64_t)bytes * BENCH_ROUNDS;
    struct seq_result result = {0u};

    begin_us = zz_time_us();
    for (round = 0u; round < BENCH_ROUNDS; round++) {
        for (offset = 0u; offset < bytes; offset += 8u) {
            uint64_t value;

            __asm__ __volatile__("ldrd %Q0, %R0, [%1]"
                                 : "=&r"(value)
                                 : "r"(memory + offset) : "memory");
            sum += value;
        }
    }
    elapsed_us = zz_time_us() - begin_us;
    bench_sink ^= (uint32_t)sum;
    if (elapsed_us != 0u)
        result.mib_s = (uint32_t)((total * 1000000u) /
                                  (elapsed_us * 1024u * 1024u));
    return result;
}

static struct seq_result seq_line_store(uint8_t *memory, uint32_t bytes)
{
    uint64_t begin_us;
    uint64_t elapsed_us;
    uint32_t round;
    uint32_t offset;
    uint64_t total = (uint64_t)bytes * BENCH_ROUNDS;
    struct seq_result result = {0u};

    begin_us = zz_time_us();
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
    elapsed_us = zz_time_us() - begin_us;
    if (elapsed_us != 0u)
        result.mib_s = (uint32_t)((total * 1000000u) /
                                  (elapsed_us * 1024u * 1024u));
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
    uint64_t begin_us;
    uint64_t elapsed_us;
    uint32_t step;

    chase_prepare(memory, bytes);
    begin_us = zz_time_us();
    for (step = 0u; step < steps; step++)
        index = words[index & (bytes / 4u - 1u)];
    elapsed_us = zz_time_us() - begin_us;
    bench_sink ^= index;
    if (elapsed_us == 0u)
        return 0u;
    return (uint32_t)((elapsed_us * 1000u) / steps);
}

static struct chase_result chase_all(uint8_t *memory, uint32_t max_bytes)
{
    static const uint32_t sizes[4] = {4096u, 65536u, 1u << 20, 8u << 20};
    struct chase_result result;
    uint32_t i;

    for (i = 0u; i < 4u; i++)
        result.ns_per_load[i] = sizes[i] <= max_bytes ?
            chase_measure(memory, sizes[i]) : 0u;
    return result;
}

static void alu_measure(uint64_t *elapsed_us_out)
{
    uint32_t value = 1u;
    uint64_t begin_us;
    uint32_t i;

    begin_us = zz_time_us();
    for (i = 0u; i < BENCH_ALU_ADDS; i++)
        __asm__ __volatile__("add %0, %0, %1" : "+r"(value) : "r"(i) : "cc");
    *elapsed_us_out = zz_time_us() - begin_us;
    bench_sink ^= value;
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

static void append_u64(char **at, char *end, uint64_t value)
{
    char digits[20];
    uint32_t count = 0u;

    do {
        digits[count++] = (char)('0' + (uint32_t)(value % 10u));
        value /= 10u;
    } while (value != 0u);
    while (count != 0u)
        append_char(at, end, digits[--count]);
}

static void append_seq(char **at, char *end, const struct seq_result *read,
                       const struct seq_result *write)
{
    append_u64(at, end, read->mib_s);
    append_char(at, end, '/');
    append_u64(at, end, write->mib_s);
}

static void append_chase(char **at, char *end, const struct chase_result *chase)
{
    uint32_t i;

    for (i = 0u; i < 4u; i++) {
        if (i != 0u)
            append_char(at, end, '/');
        append_u64(at, end, chase->ns_per_load[i]);
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
    append_u64(at, end, words_read.mib_s);
    append_text(at, end, " ldrdR ");
    append_u64(at, end, ldrd_read.mib_s);
    append_text(at, end, " lineW ");
    append_u64(at, end, line_write.mib_s);
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
    uint64_t alu_us;

    alu_measure(&alu_us);
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

    append_text(&at, end, "zzmembench bandwidth (zz_time_us)\n");
    append_text(&at, end, "cpu/pl310/ddrc not available to apps\n");
    append_text(&at, end, "alu ");
    append_u64(&at, end, BENCH_ALU_ADDS);
    append_text(&at, end, " adds in ");
    append_u64(&at, end, alu_us);
    append_text(&at, end, " us\n");
    append_text(&at, end, "region R/W MiB/s chase ns/load (4K/64K/1M/8M)\n");
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
        append_u64(&at, end, (uint32_t)display_rc);
        append_char(&at, end, '\n');
        if (display_rc == ZZ_OK)
            (void)zz_display_close(display.id);
    }
    append_stream(&at, end, "stream", bench_bss);
    if (wants_tune(argc, argv))
        append_text(&at, end, "tune not available to apps\n");
    append_text(&at, end,
                "firmware cached buffer unavailable: app ABI intentionally exposes none\n");
    *at = '\0';
    log_lines(report);
    free(heap_raw);
    return 0;
}
