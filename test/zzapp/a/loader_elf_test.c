#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "zzapp.h"
#include "zzapp_runtime.h"
#include "host_card_pool.h"
#include "xil_cache.h"
#include "zzapp_as.h"
int zzapp_loader_have_image(void);
uint32_t zzapp_test_loader_flush_count(void);
int zzapp_test_loader_flush_range(uint32_t index, uintptr_t *addr,
                                  uint32_t *bytes);
uint32_t zzapp_test_loader_flush_dropped(void);
uint32_t zzapp_test_loader_reloc_entries_last(void);
uint32_t zzapp_test_loader_work_last_kind(void);
static uint32_t selected_slot;
int smp_cpu_id(void) { return 0; }
void smp_raw_spin_lock(volatile uint32_t *word) { *word = 1U; }
int smp_raw_spin_try_lock(volatile uint32_t *word)
{
    if (*word)
        return 0;
    *word = 1U;
    return 1;
}
uint32_t smp_local_irq_save(void) { return 0U; }
void smp_local_irq_restore(uint32_t state) { (void)state; }
struct xil_cache_mock_state g_xil_cache_mock;
void smp_raw_spin_unlock(volatile uint32_t *word) { *word = 0U; }

uint32_t zzapp_services_current_slot(void)
{
    return selected_slot;
}
static uint8_t service_window[ZZAPP_WINDOW_BYTES];
static int start_pending;
static int dynamic_started;
static uint32_t start_argc;
static int argv_started;
static uint32_t launcher_memory_limit;
static int start_entered_result = ZZ_OK;
static uint32_t start_entered_generation;

int zzapp_services_start_entered(uint32_t generation)
{
    start_entered_generation = generation;
    return start_entered_result;
}


uint32_t zzapp_services_memory_limit(void)
{
    return launcher_memory_limit;
}


uint8_t *zzapp_services_window(void)
{
    return service_window;
}

int zzapp_services_take_start(uint32_t *argc)
{
    if (!start_pending)
        return 0;
    start_pending = 0;
    *argc = start_argc;
    return 1;
}

void zzapp_host_heap(void **base, uint32_t *bytes);

static void argv_entry(const void *api_ptr, int argc, char **argv)
{
    void *base;
    uint32_t bytes;

    (void)api_ptr;
    assert(argc == 1);
    assert(strcmp(argv[0], "argv-test") == 0);
    zzapp_host_heap(&base, &bytes);
    assert(base > (void *)argv[0]);
    assert(bytes != 0u);
    argv_started = 1;
}
static int rejected_started;

static void rejected_entry(const void *api_ptr, int argc, char **argv)
{
    (void)api_ptr;
    (void)argc;
    (void)argv;
    rejected_started = 1;
}



static void dynamic_entry(const void *api_ptr, int argc, char **argv)
{
    const zzapp_api *api = api_ptr;
    void *base;
    void *old_break;
    uint32_t bytes;

    assert(argc == 1);
    assert(argv != NULL);
    assert(strcmp(argv[0], "dynamic-argv") == 0);
    zzapp_host_heap(&base, &bytes);
    assert(base > (void *)argv[0]);
    assert(bytes != 0u);
    assert(api->heap_grow(1u << 20, &old_break) == ZZ_OK);
    assert(old_break != NULL);
    assert(api->heap_shrink(1u << 20, NULL) == ZZ_OK);
    dynamic_started = 1;
}

static int boundary_started;
static int boundary_argc;
static char *boundary_argv0;
static uint32_t boundary_frames;

static void boundary_entry(const void *api_ptr, int argc, char **argv)
{
    (void)api_ptr;
    boundary_argc = argc;
    boundary_argv0 = argv ? argv[0] : (char *)1;
    boundary_frames = zzapp_proc_slot(0)->heap_frame_count;
    boundary_started = 1;
}

static void crossing_entry(const void *api_ptr, int argc, char **argv)
{
    (void)api_ptr;
    assert(argc == 1);
    assert(argv != NULL && argv[0] != NULL);
    assert(strcmp(argv[0], "frame-crossing-arg") == 0);
    boundary_started = 1;
}



#define IMAGE_BYTES 0x200u
#define PHOFF 52u
#define LOAD_OFF 0x100u
#define NOTE_OFF 0x0a0u
#define RELA_OFF 0x0d0u
#define LOADER_UPLOAD_CHUNK_BYTES (64U * 1024U)
#define LOADER_ZERO_CHUNK_BYTES (32U * 1024U)
#define LOADER_COPY_CHUNK_BYTES (48U * 1024U)
#define LOADER_MAP_CHUNK_BYTES (96U * 1024U)
#define LOADER_RELOC_PREPARE_ENTRIES 512U
#define LOADER_RELOC_APPLY_ENTRIES 256U
/* Mirror of the loader's pass kinds (zzapp_loader.c). */
#define TEST_WORK_KIND_ZERO  1U
#define TEST_WORK_KIND_COPY  2U
#define TEST_WORK_KIND_MAP   3U
#define TEST_WORK_KIND_RELOC 4U
#define TEST_WORK_KIND_APPLY 5U
#define TEST_WORK_KIND_ALLOC 6U
#define SHOFF 0x180u

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint8_t *app_memory(uint32_t slot, uint32_t offset)
{
    zzapp_proc_t *proc = zzapp_proc_slot(slot);

    return (uint8_t *)(uintptr_t)(proc->image_frames[offset >> 20] +
                                  (offset & ((1U << 20) - 1U)));
}


static void make_image(uint8_t image[IMAGE_BYTES])
{
    uint8_t *ph;
    uint8_t *note;
    uint8_t *sh;

    memset(image, 0, IMAGE_BYTES);
    image[0] = 0x7f; image[1] = 'E'; image[2] = 'L'; image[3] = 'F';
    image[4] = 1; image[5] = 1; image[6] = 1;
    put16(image + 16, 3);                 /* ET_DYN */
    put16(image + 18, 40);                /* EM_ARM */
    put32(image + 24, LOAD_OFF);
    put32(image + 28, PHOFF);
    put32(image + 32, SHOFF);
    put32(image + 36, 0x400);             /* hard-float */
    put16(image + 40, 52);
    put16(image + 42, 32);
    put16(image + 44, 2);
    put16(image + 46, 40);
    put16(image + 48, 1);

    ph = image + PHOFF;
    put32(ph + 0, 1);                     /* PT_LOAD */
    put32(ph + 4, LOAD_OFF);
    put32(ph + 8, LOAD_OFF);
    put32(ph + 16, 4);
    put32(ph + 20, 8);
    put32(ph + 24, 1);                    /* PF_X */
    ph += 32;
    put32(ph + 0, 4);                     /* PT_NOTE */
    put32(ph + 4, NOTE_OFF);
    put32(ph + 16, 44);

    note = image + NOTE_OFF;
    put32(note + 0, 6);
    put32(note + 4, 24);
    put32(note + 8, ZZAPP_NOTE_TYPE);
    memcpy(note + 12, "ZZAPP", 6);
    put32(note + 20, ZZAPP_NOTE_MAGIC);

    put32(image + RELA_OFF + 0, LOAD_OFF);
    put32(image + RELA_OFF + 4, 23);      /* R_ARM_RELATIVE, symbol zero */
    put32(image + RELA_OFF + 8, LOAD_OFF);
    sh = image + SHOFF;
    put32(sh + 4, 4);                     /* SHT_RELA */
    put32(sh + 16, RELA_OFF);
    put32(sh + 20, 12);
    put32(sh + 36, 12);
}
static uint32_t successful_loads;

static void assert_pass_work_bounded(void)
{
    uint32_t zero = zzapp_test_loader_zero_last_bytes();
    uint32_t work = zzapp_test_loader_work_last_bytes();
    uint32_t kind = zzapp_test_loader_work_last_kind();

    /* XTime spans use COUNTS_PER_SECOND = CPU/2, about 333 MHz on the
     * 666 MHz CPU. These are the production loader's measured pass bounds. */
    assert(zero <= LOADER_ZERO_CHUNK_BYTES);
    assert(zzapp_test_loader_reloc_entries_last() <=
           LOADER_RELOC_APPLY_ENTRIES);
    if (work == 0U)
        return;
    switch (kind) {
    case TEST_WORK_KIND_ZERO:
        assert(work <= LOADER_ZERO_CHUNK_BYTES);
        break;
    case TEST_WORK_KIND_COPY:
        assert(work <= LOADER_COPY_CHUNK_BYTES);
        break;
    case TEST_WORK_KIND_MAP:
        assert(work <= LOADER_MAP_CHUNK_BYTES);
        break;
    case TEST_WORK_KIND_RELOC:
        assert(work <= LOADER_RELOC_PREPARE_ENTRIES * 16U);
        break;
    case TEST_WORK_KIND_APPLY:
        assert(work <= LOADER_RELOC_APPLY_ENTRIES * 16U);
        break;
    case TEST_WORK_KIND_ALLOC:
        break;
    default:
        assert(0);
    }
}

static int load_end(uint8_t image[IMAGE_BYTES])
{
    int rc;
    uint32_t passes = 0U;

    assert(zzapp_loader_begin() == ZZ_OK);
    assert(zzapp_loader_chunk(0, image, IMAGE_BYTES) == ZZ_OK);
    do {
        rc = zzapp_loader_end();
        if (rc == ZZAPP_LOADER_IN_PROGRESS)
            assert_pass_work_bounded();
        passes++;
    } while (rc == ZZAPP_LOADER_IN_PROGRESS);
    if (rc == ZZ_OK)
        assert(successful_loads++ != 0U || passes > 1U);
    return rc;
}

static void test_large_load_is_bounded_and_exact(void)
{
    const uint32_t filesz = 2U << 20;
    const uint32_t memsz = 3U << 20;
    const uint32_t image_bytes = LOAD_OFF + filesz;
    uint8_t *image = malloc(image_bytes);
    uint32_t offset;
    uint32_t passes = 0U;
    uint32_t max_work = 0U;
    uint32_t max_zero = 0U;
    int rc;

    assert(image != NULL);
    memset(image, 0, image_bytes);
    make_image(image);
    put32(image + PHOFF + 16U, filesz);
    put32(image + PHOFF + 20U, memsz);
    put32(image + NOTE_OFF + 28U, 3U);
    put32(image + NOTE_OFF + 40U, 1U);
    put32(image + SHOFF + 4U, 0U);
    for (offset = 0U; offset < filesz; offset++)
        image[LOAD_OFF + offset] = (uint8_t)(offset * 29U + 17U);

    assert(zzapp_loader_begin() == ZZ_OK);
    for (offset = 0U; offset < image_bytes;) {
        uint32_t chunk = image_bytes - offset;

        if (chunk > LOADER_UPLOAD_CHUNK_BYTES)
            chunk = LOADER_UPLOAD_CHUNK_BYTES;
        assert(zzapp_loader_chunk(offset, image + offset, chunk) == ZZ_OK);
        offset += chunk;
    }
    do {
        rc = zzapp_loader_end();
        if (rc == ZZAPP_LOADER_IN_PROGRESS) {
            assert_pass_work_bounded();
            if (zzapp_test_loader_work_last_bytes() > max_work)
                max_work = zzapp_test_loader_work_last_bytes();
            if (zzapp_test_loader_zero_last_bytes() > max_zero)
                max_zero = zzapp_test_loader_zero_last_bytes();
        }
        passes++;
    } while (rc == ZZAPP_LOADER_IN_PROGRESS);
    assert(rc == ZZ_OK);
    /* The per-pass budget splits this 2 MiB image with 1 MiB of .bss
     * across many passes of every phase. */
    assert(passes > 30U);
    assert(max_zero > LOADER_ZERO_CHUNK_BYTES / 2U);
    printf("large load: %u passes, max work %u bytes, max zero %u bytes\n",
           passes, max_work, max_zero);
    assert(memcmp(app_memory(0U, LOAD_OFF), image + LOAD_OFF, filesz) == 0);
    for (offset = 0U; offset < memsz - filesz; offset++)
        assert(app_memory(0U, LOAD_OFF + filesz)[offset] == 0U);
    zzapp_loader_release();
    free(image);
}

static void test_uniform_megabyte_segments_map_as_sections(void)
{
    const uint32_t text_bytes = 1U << 20;
    const uint32_t image_bytes = LOAD_OFF + text_bytes;
    const uint32_t section_memory = (7U << 12) | (1U << 16) |
                                    (1U << 3) | (1U << 2);
    uint8_t *image = calloc(1U, image_bytes);
    uint8_t *load;
    uint8_t *note;
    const uint32_t *l1;
    zzapp_proc_t *proc;
    uint32_t offset;
    int rc;

    assert(image != NULL);
    make_image(image);
    memcpy(image + PHOFF + 64U, image + PHOFF + 32U, 32U);
    put16(image + 44U, 3U);
    put32(image + 24U, 0U);
    load = image + PHOFF;
    put32(load + 8U, 0U);
    put32(load + 16U, text_bytes);
    put32(load + 20U, text_bytes);
    put32(load + 24U, 5U);
    load += 32U;
    put32(load + 0U, 1U);
    put32(load + 4U, 0U);
    put32(load + 8U, 1U << 20);
    put32(load + 16U, 0U);
    put32(load + 20U, 1U << 20);
    put32(load + 24U, 6U);
    put32(image + SHOFF + 4U, 0U);
    note = image + NOTE_OFF;
    put32(note + 28U, 3U);
    put32(note + 40U, 1U);

    assert(zzapp_loader_begin() == ZZ_OK);
    for (offset = 0U; offset < image_bytes;) {
        uint32_t count = image_bytes - offset;

        if (count > LOADER_UPLOAD_CHUNK_BYTES)
            count = LOADER_UPLOAD_CHUNK_BYTES;
        assert(zzapp_loader_chunk(offset, image + offset, count) == ZZ_OK);
        offset += count;
    }
    do {
        rc = zzapp_loader_end();
        if (rc == ZZAPP_LOADER_IN_PROGRESS)
            assert_pass_work_bounded();
    } while (rc == ZZAPP_LOADER_IN_PROGRESS);
    assert(rc == ZZ_OK);
    proc = zzapp_proc_slot(0U);
    l1 = zzapp_as_l1(&proc->as);
    assert((l1[ZZAPP_AS_WINDOW_BASE >> 20] & 3U) == 2U);
    assert((l1[(ZZAPP_AS_WINDOW_BASE >> 20) + 1U] & 3U) == 2U);
    assert((l1[ZZAPP_AS_WINDOW_BASE >> 20] & 0xfff00000U) ==
           proc->image_frames[0]);
    assert((l1[(ZZAPP_AS_WINDOW_BASE >> 20) + 1U] & 0xfff00000U) ==
           proc->image_frames[1]);
    assert((l1[ZZAPP_AS_WINDOW_BASE >> 20] & (3U << 10)) == (2U << 10));
    assert((l1[ZZAPP_AS_WINDOW_BASE >> 20] & (1U << 4)) == 0U);
    assert((l1[(ZZAPP_AS_WINDOW_BASE >> 20) + 1U] & (3U << 10)) ==
           (3U << 10));
    assert((l1[(ZZAPP_AS_WINDOW_BASE >> 20) + 1U] & (1U << 4)) != 0U);
    assert((l1[ZZAPP_AS_WINDOW_BASE >> 20] & (1U << 17)) != 0U);
    assert((l1[(ZZAPP_AS_WINDOW_BASE >> 20) + 1U] & (1U << 17)) != 0U);
    assert((l1[ZZAPP_AS_WINDOW_BASE >> 20] & section_memory) ==
           (0x15de6U & section_memory));
    assert((l1[(ZZAPP_AS_WINDOW_BASE >> 20) + 1U] & section_memory) ==
           (0x15de6U & section_memory));
    zzapp_loader_release();
    free(image);
}

static void test_page_mapped_flush_work_is_bounded(void)
{
    const uint32_t segment_bytes = 512U * 1024U;
    const uint32_t image_bytes = LOAD_OFF + segment_bytes;
    uint8_t *image = malloc(image_bytes);
    const uint32_t *l1;
    uint32_t offset;
    uint32_t seen_flushes;
    int rc;

    assert(image != NULL);
    make_image(image);
    memcpy(image + PHOFF + 64U, image + PHOFF + 32U, 32U);
    put16(image + 44U, 3U);
    put16(image + 48U, 0U);
    put32(image + PHOFF + 8U, 0U);
    put32(image + PHOFF + 16U, segment_bytes);
    put32(image + PHOFF + 20U, segment_bytes);
    put32(image + PHOFF + 24U, 5U);
    put32(image + PHOFF + 32U, 1U);
    put32(image + PHOFF + 36U, 0U);
    put32(image + PHOFF + 40U, segment_bytes);
    put32(image + PHOFF + 48U, 0U);
    put32(image + PHOFF + 52U, segment_bytes);
    put32(image + PHOFF + 56U, 6U);
    put32(image + NOTE_OFF + 28U, 3U);
    for (offset = 0U; offset < segment_bytes; offset++)
        image[LOAD_OFF + offset] = (uint8_t)(offset * 29U + 17U);

    assert(zzapp_loader_begin() == ZZ_OK);
    for (offset = 0U; offset < image_bytes;) {
        uint32_t count = image_bytes - offset;

        if (count > LOADER_UPLOAD_CHUNK_BYTES)
            count = LOADER_UPLOAD_CHUNK_BYTES;
        assert(zzapp_loader_chunk(offset, image + offset, count) == ZZ_OK);
        offset += count;
    }
    seen_flushes = zzapp_test_loader_flush_count();
    do {
        uint32_t flushes;
        uint32_t flushed = 0U;
        uint32_t f;

        rc = zzapp_loader_end();
        flushes = zzapp_test_loader_flush_count();
        for (f = seen_flushes; f < flushes; f++) {
            uintptr_t addr;
            uint32_t bytes;

            assert(zzapp_test_loader_flush_range(f, &addr, &bytes));
            (void)addr;
            flushed += bytes;
        }
        assert(flushed <= LOADER_MAP_CHUNK_BYTES);
        if (flushed != 0U) {
            assert(zzapp_test_loader_work_last_kind() ==
                   TEST_WORK_KIND_MAP);
            assert(zzapp_test_loader_work_last_bytes() == flushed);
        }
        seen_flushes = flushes;
    } while (rc == ZZAPP_LOADER_IN_PROGRESS);
    assert(rc == ZZ_OK);
    assert(seen_flushes != 0U);
    assert(zzapp_test_loader_flush_dropped() == 0U);
    l1 = zzapp_as_l1(&zzapp_proc_slot(0U)->as);
    assert((l1[ZZAPP_AS_WINDOW_BASE >> 20] & 3U) == 1U);
    assert(memcmp(app_memory(0U, 0U), image + LOAD_OFF, segment_bytes) == 0);
    assert(app_memory(0U, segment_bytes)[0] == 0U);
    zzapp_loader_release();
    free(image);
}

static void make_relocation_stress_image(uint8_t *image, uint32_t bytes,
                                         uint32_t relocation_count)
{
    uint32_t shoff = RELA_OFF + relocation_count * 12U;
    uint32_t i;
    uint8_t *sh;

    assert(bytes >= shoff + 40U);
    make_image(image);
    memset(image + RELA_OFF, 0, relocation_count * 12U);
    memset(image + shoff, 0, 40U);
    put32(image + 32U, shoff);
    put32(image + PHOFF + 16U, bytes - LOAD_OFF);
    put32(image + PHOFF + 20U, 3U << 20);
    put32(image + NOTE_OFF + 28U, 3U);
    for (i = 0U; i < relocation_count; i++) {
        uint8_t *rel = image + RELA_OFF + i * 12U;

        put32(rel + 0U, LOAD_OFF);
        put32(rel + 4U, 23U);
        put32(rel + 8U, LOAD_OFF);
    }
    sh = image + shoff;
    put32(sh + 4U, 4U);
    put32(sh + 16U, RELA_OFF);
    put32(sh + 20U, relocation_count * 12U);
    put32(sh + 36U, 12U);
}

static void test_relocation_allocation_and_zero_use_separate_passes(void)
{
    const uint32_t relocation_count = 1517U;
    const uint32_t bytes = RELA_OFF + relocation_count * 12U + 40U;
    uint8_t *image = malloc(bytes);
    uint32_t offset;
    uint32_t pass;
    int rc;

    assert(image != NULL);
    zzapp_boot_init();
    make_relocation_stress_image(image, bytes, relocation_count);
    assert(zzapp_loader_begin() == ZZ_OK);
    for (offset = 0U; offset < bytes;) {
        uint32_t chunk = bytes - offset;

        if (chunk > LOADER_UPLOAD_CHUNK_BYTES)
            chunk = LOADER_UPLOAD_CHUNK_BYTES;
        assert(zzapp_loader_chunk(offset, image + offset, chunk) == ZZ_OK);
        offset += chunk;
    }

    for (pass = 0U; pass < 3U; pass++) {
        uint32_t entries = pass < 2U ? LOADER_RELOC_PREPARE_ENTRIES :
                           relocation_count -
                               2U * LOADER_RELOC_PREPARE_ENTRIES;

        rc = zzapp_loader_end();
        assert(rc == ZZAPP_LOADER_IN_PROGRESS);
        assert(zzapp_test_loader_work_last_kind() == TEST_WORK_KIND_RELOC);
        assert(zzapp_test_loader_work_last_bytes() == entries * 16U);
        assert(zzapp_test_loader_zero_last_bytes() == 0U);
    }

    rc = zzapp_loader_end();
    assert(rc == ZZAPP_LOADER_IN_PROGRESS);
    assert(zzapp_test_loader_work_last_kind() == TEST_WORK_KIND_ALLOC);
    assert(zzapp_test_loader_work_last_bytes() != 0U);
    assert(zzapp_test_loader_zero_last_bytes() == 0U);

    rc = zzapp_loader_end();
    assert(rc == ZZAPP_LOADER_IN_PROGRESS);
    assert(zzapp_test_loader_work_last_kind() == TEST_WORK_KIND_ZERO);
    assert(zzapp_test_loader_zero_last_bytes() == LOADER_ZERO_CHUNK_BYTES);

    assert(zzapp_loader_abort() == ZZ_OK);
    free(image);
}

static void test_abort_releases_each_loader_slice(void)
{
    const uint32_t relocation_count = 2U * 4096U + 1U;
    const uint32_t bytes = RELA_OFF + relocation_count * 12U + 40U;
    uint8_t *image = malloc(bytes);
    uint32_t abort_after = 0U;
    uint32_t aborted = 0U;
    int rc = ZZAPP_LOADER_IN_PROGRESS;

    assert(image != NULL);
    while (rc == ZZAPP_LOADER_IN_PROGRESS && aborted < 512U) {
        uint32_t in_progress = 0U;
        uint32_t offset;

        zzapp_boot_init();
        make_relocation_stress_image(image, bytes, relocation_count);
        assert(zzapp_loader_begin() == ZZ_OK);
        for (offset = 0U; offset < bytes;) {
            uint32_t chunk = bytes - offset;

            if (chunk > LOADER_UPLOAD_CHUNK_BYTES)
                chunk = LOADER_UPLOAD_CHUNK_BYTES;
            assert(zzapp_loader_chunk(offset, image + offset, chunk) == ZZ_OK);
            offset += chunk;
        }
        do {
            rc = zzapp_loader_end();
            if (rc == ZZAPP_LOADER_IN_PROGRESS &&
                in_progress++ == abort_after) {
                assert(zzapp_loader_abort() == ZZ_OK);
                assert(zzapp_test_arena_mem() == NULL);
                assert(zzapp_proc_slot(0)->memory.committed == 0U);
                aborted++;
                break;
            }
        } while (rc == ZZAPP_LOADER_IN_PROGRESS);
        abort_after++;
    }
    /* Covers relocation extraction, frame zero/copy/map/cache and relocation
     * application slices; every interrupted load must release its arena. */
    assert(rc == ZZ_OK);
    zzapp_loader_release();
    free(image);
}

static void test_relocation_validation_is_sliced(void)
{
    const uint32_t relocation_count = 2U * LOADER_RELOC_PREPARE_ENTRIES + 1U;
    const uint32_t bytes = RELA_OFF + relocation_count * 12U + 40U;
    uint8_t *image = malloc(bytes);
    uint32_t in_progress = 0U;
    uint32_t offset;
    int rc;

    /* The malformed entry sits in the third extraction chunk: a loader
     * that validates every entry in parse_elf's single pass rejects the
     * image on the first zzapp_loader_end() call, before any relocation
     * work has been reported. */
    assert(image != NULL);
    zzapp_boot_init();
    make_relocation_stress_image(image, bytes, relocation_count);
    put32(image + RELA_OFF + (relocation_count - 1U) * 12U + 4U, 22U);
    assert(zzapp_loader_begin() == ZZ_OK);
    for (offset = 0U; offset < bytes;) {
        uint32_t chunk = bytes - offset;

        if (chunk > LOADER_UPLOAD_CHUNK_BYTES)
            chunk = LOADER_UPLOAD_CHUNK_BYTES;
        assert(zzapp_loader_chunk(offset, image + offset, chunk) == ZZ_OK);
        offset += chunk;
    }
    do {
        rc = zzapp_loader_end();
        if (rc == ZZAPP_LOADER_IN_PROGRESS)
            in_progress++;
    } while (rc == ZZAPP_LOADER_IN_PROGRESS);
    assert(rc == ZZ_EINVAL);
    assert(in_progress >= 2U);
    assert(zzapp_test_arena_mem() == NULL);
    assert(zzapp_loader_state() == ZZAPP_ST_IDLE);
    free(image);
}

#define SPAN_PACKED_RELOCS 1600U
#define SPAN_STRIDED_RELOCS 448U
#define SPAN_RELOC_COUNT (SPAN_PACKED_RELOCS + SPAN_STRIDED_RELOCS)

static uint32_t span_reloc_offset(uint32_t i)
{
    if (i < SPAN_PACKED_RELOCS)
        return LOAD_OFF + i * 8U;
    return (1U << 20) + (i - SPAN_PACKED_RELOCS) * 0x1000U;
}

static void make_span_relocation_image(uint8_t *image, uint32_t bytes)
{
    uint32_t shoff = RELA_OFF + SPAN_RELOC_COUNT * 12U;
    uint32_t i;
    uint8_t *sh;

    assert(bytes >= shoff + 40U);
    make_image(image);
    put32(image + 32U, shoff);
    put32(image + PHOFF + 16U, bytes - LOAD_OFF);
    put32(image + PHOFF + 20U, 3U << 20);
    put32(image + NOTE_OFF + 28U, 3U);
    for (i = 0U; i < SPAN_RELOC_COUNT; i++) {
        uint8_t *rel = image + RELA_OFF + i * 12U;
        uint32_t off = span_reloc_offset(i);

        put32(rel + 0U, off);
        put32(rel + 4U, 23U);
        put32(rel + 8U, off);
    }
    sh = image + shoff;
    put32(sh + 4, 4);
    put32(sh + 16, RELA_OFF);
    put32(sh + 20, SPAN_RELOC_COUNT * 12U);
    put32(sh + 36, 12);
}

static void test_relocation_passes_are_bounded_and_flushed(void)
{
    const uint32_t bytes = RELA_OFF + SPAN_RELOC_COUNT * 12U + 40U;
    uint8_t *image = malloc(bytes);
    uint32_t applied_total = 0U;
    uint32_t apply_flushes = 0U;
    uint32_t seen_flushes = 0U;
    uint32_t max_entries = 0U;
    uint32_t max_span = 0U;
    uint32_t card;
    uint32_t offset;
    uint32_t i;
    int rc = ZZAPP_LOADER_IN_PROGRESS;

    assert(image != NULL);
    zzapp_boot_init();
    make_span_relocation_image(image, bytes);
    assert(zzapp_loader_begin() == ZZ_OK);
    for (offset = 0U; offset < bytes;) {
        uint32_t chunk = bytes - offset;

        if (chunk > LOADER_UPLOAD_CHUNK_BYTES)
            chunk = LOADER_UPLOAD_CHUNK_BYTES;
        assert(zzapp_loader_chunk(offset, image + offset, chunk) == ZZ_OK);
        offset += chunk;
    }
    while (rc == ZZAPP_LOADER_IN_PROGRESS) {
        uint32_t entries;
        uint32_t flushes;
        uint32_t j;

        rc = zzapp_loader_end();
        entries = zzapp_test_loader_reloc_entries_last();
        flushes = zzapp_test_loader_flush_count();
        if (entries > max_entries)
            max_entries = entries;
        assert(entries <= LOADER_RELOC_APPLY_ENTRIES);
        if (entries != 0U) {
            /* Every word written this pass is flushed this pass, after
             * the write: the flush may not slip to a later pass, or the
             * app's I-side could read a stale word. */
            apply_flushes += flushes - seen_flushes;
            for (j = 0U; j < entries; j++) {
                uintptr_t word = (uintptr_t)app_memory(
                    0U, span_reloc_offset(applied_total + j));
                uint32_t f;
                int covered = 0;

                for (f = seen_flushes; f < flushes; f++) {
                    uintptr_t addr;
                    uint32_t len;

                    assert(zzapp_test_loader_flush_range(f, &addr, &len));
                    if (len > max_span)
                        max_span = len;
                    if (word >= addr && word + 4U <= addr + len)
                        covered = 1;
                }
                assert(covered);
            }
            applied_total += entries;
        }
        seen_flushes = flushes;
    }
    assert(rc == ZZ_OK);
    assert(applied_total == SPAN_RELOC_COUNT);
    assert(zzapp_test_loader_flush_dropped() == 0U);
    assert(max_entries > 1U);
    /* Packed relocations share one flush per span instead of one per
     * word: fewer than half as many flush calls as relocated words. */
    assert(apply_flushes * 2U < SPAN_RELOC_COUNT);
    /* No span grew wider than one pass of packed words plus slack. */
    assert(max_span <= LOADER_RELOC_APPLY_ENTRIES * 8U + 64U);
    card = zzapp_test_arena_card();
    for (i = 0U; i < SPAN_RELOC_COUNT; i++) {
        uint32_t off = span_reloc_offset(i);

        assert(get32(app_memory(0U, off)) == card + off);
    }
    printf("span relocs: %u entries, %u flushes, max span %u bytes, "
           "max entries/pass %u\n",
           SPAN_RELOC_COUNT, apply_flushes, max_span, max_entries);
    zzapp_loader_release();
    free(image);
}


int main(void)
{
    uint8_t image[IMAGE_BYTES];
    uint32_t card;
    uint32_t i;
    const zzapp_api *api;
    zzapp_heap_info mem;
    uint32_t bsp[4096] = { 0U };
    host_card_pool_reset(256U << 20);
    zzapp_as_init(bsp);
    zzapp_boot_init();
    assert(sizeof(zzapp_proc_slot(0)->image_frames) /
           sizeof(zzapp_proc_slot(0)->image_frames[0]) == ZZAPP_MEM_MAX_FRAMES);
    make_image(image);
    assert(load_end(image) == ZZ_OK);
    /* This host build uses the production address-space implementation. */
    card = zzapp_test_arena_card();
    assert(card == 0xc0000000u);
    assert(get32(app_memory(0U, LOAD_OFF)) == card + LOAD_OFF);
    for (i = 0U; i < (1U << 20); i++)
        assert(((uint8_t *)(uintptr_t)zzapp_proc_slot(0)->heap_frames[3])[i] ==
               0U);
    assert(zzapp_proc_slot(0)->memory.committed == ZZAPP_ARENA_BYTES);
    zzapp_loader_release();
    zzapp_boot_init();
    test_uniform_megabyte_segments_map_as_sections();
    zzapp_boot_init();
    test_page_mapped_flush_work_is_bounded();
    zzapp_boot_init();
    test_large_load_is_bounded_and_exact();
    test_abort_releases_each_loader_slice();
    test_relocation_validation_is_sliced();
    test_relocation_allocation_and_zero_use_separate_passes();
    test_relocation_passes_are_bounded_and_flushed();
    zzapp_boot_init();
    make_image(image);
    assert(zzapp_loader_begin() == ZZ_OK);
    assert(zzapp_loader_chunk(0, image, IMAGE_BYTES) == ZZ_OK);
    while (zzapp_loader_end() == ZZAPP_LOADER_IN_PROGRESS)
        ;
    assert(zzapp_loader_state() == ZZAPP_ST_LOADING);
    zzapp_loader_release();
    assert(zzapp_test_arena_mem() == NULL);
    assert(zzapp_proc_slot(0)->memory.committed == 0U);

    /* Every suspension point aborts cleanly, including the first. */
    zzapp_boot_init();
    make_image(image);
    assert(zzapp_loader_begin() == ZZ_OK);
    assert(zzapp_loader_chunk(0, image, IMAGE_BYTES) == ZZ_OK);
    assert(zzapp_loader_end() == ZZAPP_LOADER_IN_PROGRESS);
    assert(zzapp_loader_abort() == ZZ_OK);
    assert(zzapp_test_arena_mem() == NULL);
    assert(zzapp_proc_slot(0)->memory.committed == 0U);

    /* Stock GNU ld emits SHT_REL: its addend is the target word, not a
     * third relocation-field as in RELA. */
    zzapp_boot_init();
    make_image(image);
    put32(image + RELA_OFF + 8, 0u);
    put32(image + SHOFF + 4, 9u);          /* SHT_REL */
    put32(image + SHOFF + 20, 8u);
    put32(image + SHOFF + 36, 8u);
    put32(image + LOAD_OFF, LOAD_OFF);
    assert(load_end(image) == ZZ_OK);
    card = zzapp_test_arena_card();
    assert(get32(app_memory(0U, LOAD_OFF)) == card + LOAD_OFF);
    zzapp_loader_release();
    /* REL's implicit addend is read from its virtual target after PT_LOAD
     * placement, not from the source-file offset. */
    zzapp_boot_init();
    make_image(image);
    put32(image + 24, LOAD_OFF + 0x20u);
    put32(image + PHOFF + 8, LOAD_OFF + 0x20u);
    put32(image + RELA_OFF, LOAD_OFF + 0x20u);
    put32(image + RELA_OFF + 8, 0u);
    put32(image + SHOFF + 4, 9u);          /* SHT_REL */
    put32(image + SHOFF + 20, 8u);
    put32(image + SHOFF + 36, 8u);
    put32(image + LOAD_OFF, LOAD_OFF + 0x20u);
    assert(load_end(image) == ZZ_OK);
    card = zzapp_test_arena_card();
    assert(get32(app_memory(0U, LOAD_OFF + 0x20u)) ==
           card + LOAD_OFF + 0x20u);
    zzapp_loader_release();

    zzapp_boot_init(); make_image(image);
    put32(image + SHOFF + 4, 9u);          /* SHT_REL */
    put32(image + SHOFF + 20, 8u);
    put32(image + SHOFF + 36, 4u);         /* malformed entry size */
    assert(load_end(image) == ZZ_EINVAL);

    zzapp_boot_init(); make_image(image);
    put32(image + SHOFF + 4, 9u);          /* SHT_REL */
    put32(image + SHOFF + 20, 8u);
    put32(image + SHOFF + 36, 8u);
    put32(image + RELA_OFF + 4, 2u);       /* unsupported relocation type */
    assert(load_end(image) == ZZ_EINVAL);

    zzapp_boot_init(); make_image(image);
    put32(image + RELA_OFF + 4, 0x100u | 23u); /* nonzero symbol index */
    assert(load_end(image) == ZZ_EINVAL);

    zzapp_boot_init(); make_image(image);
    put32(image + 24, 0u);
    assert(load_end(image) == ZZ_OK);
    assert(zzapp_loader_have_image());
    zzapp_loader_release();

    zzapp_boot_init(); make_image(image);
    put32(image + RELA_OFF + 4, 2);
    assert(load_end(image) == ZZ_EINVAL);

    zzapp_boot_init(); make_image(image);
    put32(image + PHOFF + 24, 3);
    assert(load_end(image) == ZZ_EINVAL);

    zzapp_boot_init(); make_image(image);
    put32(image + NOTE_OFF + 24, 2);
    assert(load_end(image) == ZZ_EINVAL);

    zzapp_boot_init(); make_image(image);
    put32(image + NOTE_OFF + 28u, 3u);
    assert(load_end(image) == ZZ_OK);
    assert(zzapp_proc_slot(0)->memory.committed == (2u << 20));
    zzapp_proc_slot(0)->heap_base = 0x00100000u;
    zzapp_proc_slot(0)->heap_start = 0x00100000u;
    zzapp_proc_slot(0)->heap_break = 0x00100000u;
    zzapp_proc_slot(0)->heap_limit = 0x01000000u;
    api = zzapp_api_table();
    assert(api->heap_grow(3u << 20, NULL) == ZZ_OK);
    assert(zzapp_proc_slot(0)->heap_break == 0x00400000u);
    assert(api->heap_info(&mem) == ZZ_OK);
    assert(mem.committed == (5u << 20));
    assert(api->heap_shrink(2u << 20, NULL) == ZZ_OK);
    assert(zzapp_proc_slot(0)->heap_break == 0x00200000u);
    zzapp_proc_slot(0)->heap_base = 0u;
    zzapp_proc_slot(0)->heap_start = 0u;
    zzapp_proc_slot(0)->heap_break = 0u;
    zzapp_proc_slot(0)->heap_limit = 32u << 20;
    zzapp_proc_slot(0)->memory.committed = 0u;
    zzapp_proc_slot(0)->memory.limit = 16u << 20;
    for (card = 0; card < 16u; card++)
        assert(api->heap_grow(1u << 20, NULL) == ZZ_OK);
    assert(api->heap_grow(1u << 20, NULL) == ZZ_ENOMEM);
    assert(zzapp_proc_slot(0)->heap_break == (16u << 20));
    zzapp_loader_release();
    /*
     * A START can expire after the worker has claimed it.  The host must
     * validate the generation before publishing RUNNING or entering it.
     */
    zzapp_boot_init();
    make_image(image);
    assert(load_end(image) == ZZ_OK);
    start_argc = 0U;
    rejected_started = 0;
    start_entered_result = ZZ_EIO;
    zzapp_test_set_entry(rejected_entry);
    start_pending = 1;
    zzapp_worker_hook();
    assert(!rejected_started);
    /* A raced, non-quarantined start rejection finishes in IDLE: the slot
     * reopens for the next claim instead of wedging in STOPPING. */
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    assert(start_entered_generation == zzapp_current_generation());
    start_entered_result = ZZ_OK;

    zzapp_boot_init();
    make_image(image);
    assert(load_end(image) == ZZ_OK);
    zzapp_put_be32(service_window + ZZAPP_ARGV_OFF + ZZAPP_ARGV_OFFS_OFF,
                   ZZAPP_ARGV_OFF + ZZAPP_ARGV_STR_OFF);
    memcpy(service_window + ZZAPP_ARGV_OFF + ZZAPP_ARGV_STR_OFF, "argv-test", 10u);
    start_argc = 1u;
    argv_started = 0;
    zzapp_test_set_entry(argv_entry);
    start_pending = 1;
    zzapp_worker_hook();
    assert(argv_started);

    make_image(image);
    put32(image + PHOFF + 20u, (1u << 20) - LOAD_OFF);
    put32(image + NOTE_OFF + 28u, 3u);
    put32(image + NOTE_OFF + 40u, 1u);
    zzapp_put_be32(service_window + ZZAPP_ARGV_OFF + ZZAPP_ARGV_OFFS_OFF,
                   ZZAPP_ARGV_OFF + ZZAPP_ARGV_STR_OFF);
    memcpy(service_window + ZZAPP_ARGV_OFF + ZZAPP_ARGV_STR_OFF,
           "dynamic-argv", 13u);
    start_argc = 1u;
    assert(load_end(image) == ZZ_OK);
    zzapp_test_set_entry(dynamic_entry);
    start_pending = 1;
    dynamic_started = 0;
    zzapp_worker_hook();
    assert(dynamic_started);

    zzapp_boot_init(); make_image(image);
    launcher_memory_limit = 16u << 20;
    put32(image + NOTE_OFF + 28u, 3u);
    assert(load_end(image) == ZZ_OK);
    assert(zzapp_proc_slot(0)->memory.limit == launcher_memory_limit);
    zzapp_proc_slot(0)->heap_base = 0u;
    zzapp_proc_slot(0)->heap_start = 0u;
    zzapp_proc_slot(0)->heap_break = 0u;
    zzapp_proc_slot(0)->heap_limit = 32u << 20;
    api = zzapp_api_table();
    for (card = 0; card < 14u; card++)
        assert(api->heap_grow(1u << 20, NULL) == ZZ_OK);
    assert(api->heap_grow(1u << 20, NULL) == ZZ_ENOMEM);
    zzapp_loader_release();
    launcher_memory_limit = 0u;

    zzapp_boot_init(); make_image(image);
    put32(image + NOTE_OFF + 28u, 3u);
    put32(image + NOTE_OFF + 40u, 600u);
    assert(load_end(image) == ZZ_ENOMEM);
    assert(zzapp_loader_state() == ZZAPP_ST_IDLE);

    /* API 0.3 image ending on a frame with no reserved heap must still
     * start, including argc 0: the terminating argv pointer needs a frame. */
    zzapp_boot_init();
    make_image(image);
    put32(image + PHOFF + 20u, (1u << 20) - LOAD_OFF);
    put32(image + NOTE_OFF + 28u, 3u);
    put32(image + NOTE_OFF + 40u, 0u);
    assert(load_end(image) == ZZ_OK);
    assert(zzapp_proc_slot(0)->heap_frame_count == 0u);
    boundary_started = 0;
    start_argc = 0u;
    zzapp_test_set_entry(boundary_entry);
    start_pending = 1;
    zzapp_worker_hook();
    assert(boundary_started);
    assert(boundary_argc == 0);
    assert(boundary_argv0 == NULL);
    assert(boundary_frames >= 1u);

    /* An argv string that starts in one frame and ends in the next. */
    zzapp_boot_init();
    make_image(image);
    put32(image + PHOFF + 20u, 0xFFFE0u - LOAD_OFF);
    put32(image + NOTE_OFF + 28u, 3u);
    put32(image + NOTE_OFF + 40u, 1u);
    assert(load_end(image) == ZZ_OK);
    zzapp_put_be32(service_window + ZZAPP_ARGV_OFF + ZZAPP_ARGV_OFFS_OFF,
                   ZZAPP_ARGV_OFF + ZZAPP_ARGV_STR_OFF);
    memcpy(service_window + ZZAPP_ARGV_OFF + ZZAPP_ARGV_STR_OFF,
           "frame-crossing-arg", 19u);
    boundary_started = 0;
    start_argc = 1u;
    zzapp_test_set_entry(crossing_entry);
    start_pending = 1;
    zzapp_worker_hook();
    assert(boundary_started);
    return 0;
}
