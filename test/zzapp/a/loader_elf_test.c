#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "zzapp.h"
#include "zzapp_runtime.h"
#include "host_card_pool.h"
int zzapp_loader_have_image(void);
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
#define ZERO_CHUNK_FRAMES 2U
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

static int load_end(uint8_t image[IMAGE_BYTES])
{
    int rc;
    uint32_t passes = 0U;

    assert(zzapp_loader_begin() == ZZ_OK);
    assert(zzapp_loader_chunk(0, image, IMAGE_BYTES) == ZZ_OK);
    do {
        rc = zzapp_loader_end();
        if (rc == ZZAPP_LOADER_IN_PROGRESS) {
            assert(zzapp_test_loader_zero_last_frames() != 0U);
            assert(zzapp_test_loader_zero_last_frames() <=
                   ZERO_CHUNK_FRAMES);
            assert(zzapp_test_loader_mapping_calls() == 0U);
        }
        passes++;
    } while (rc == ZZAPP_LOADER_IN_PROGRESS);
    if (rc == ZZ_OK)
        assert(successful_loads++ != 0U || passes > 1U);
    return rc;
}

int main(void)
{
    uint8_t image[IMAGE_BYTES];
    uint32_t card;
    uint32_t i;
    const zzapp_api *api;
    zzapp_heap_info mem;
    host_card_pool_reset(256U << 20);
    zzapp_boot_init();
    assert(sizeof(zzapp_proc_slot(0)->image_frames) /
           sizeof(zzapp_proc_slot(0)->image_frames[0]) == ZZAPP_MEM_MAX_FRAMES);
    make_image(image);
    assert(load_end(image) == ZZ_OK);
    assert(zzapp_test_loader_mapping_calls() != 0U);
    card = zzapp_test_arena_card();
    assert(card == 0xc0000000u);
    assert(get32(app_memory(0U, LOAD_OFF)) == card + LOAD_OFF);
    for (i = 0U; i < (1U << 20); i++)
        assert(((uint8_t *)(uintptr_t)zzapp_proc_slot(0)->heap_frames[3])[i] ==
               0U);
    assert(zzapp_proc_slot(0)->memory.committed == ZZAPP_ARENA_BYTES);
    zzapp_loader_release();
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
