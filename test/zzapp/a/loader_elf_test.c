#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "zzapp.h"
#include "zzapp_runtime.h"
int zzapp_loader_have_image(void);
static uint32_t selected_slot;

uint32_t zzapp_services_current_slot(void)
{
    return selected_slot;
}
static uint8_t service_window[ZZAPP_WINDOW_BYTES];
static int start_pending;
static int dynamic_started;

uint8_t *zzapp_services_window(void)
{
    return service_window;
}

int zzapp_services_take_start(uint32_t *argc)
{
    if (!start_pending)
        return 0;
    start_pending = 0;
    *argc = 0U;
    return 1;
}

void zzapp_host_heap(void **base, uint32_t *bytes);

static void dynamic_entry(const void *api_ptr, int argc, char **argv)
{
    const zzapp_api *api = api_ptr;
    void *base;
    void *old_break;
    uint32_t bytes;

    assert(argc == 0);
    assert(argv != NULL);
    zzapp_host_heap(&base, &bytes);
    assert(base != NULL);
    assert(bytes >= (1u << 20));
    assert(api->heap_grow(1u << 20, &old_break) == ZZ_OK);
    assert(old_break != NULL);
    assert(api->heap_shrink(1u << 20, NULL) == ZZ_OK);
    dynamic_started = 1;
}



#define IMAGE_BYTES 0x200u
#define PHOFF 52u
#define LOAD_OFF 0x100u
#define NOTE_OFF 0x0a0u
#define RELA_OFF 0x0d0u
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

static int load_end(uint8_t image[IMAGE_BYTES])
{
    assert(zzapp_loader_begin() == ZZ_OK);
    assert(zzapp_loader_chunk(0, image, IMAGE_BYTES) == ZZ_OK);
    return zzapp_loader_end();
}

int main(void)
{
    uint8_t image[IMAGE_BYTES];
    uint32_t card;
    const zzapp_api *api;
    zzapp_heap_info mem;
    zzapp_boot_init();
    make_image(image);
    assert(load_end(image) == ZZ_OK);
    card = zzapp_test_arena_card();
    assert(card == 0xc0000000u);
    assert(get32(zzapp_test_arena_mem() + LOAD_OFF) == card + LOAD_OFF);
    assert(zzapp_proc_slot(0)->memory.committed == ZZAPP_ARENA_BYTES);
    selected_slot = 1U;
    make_image(image);
    assert(load_end(image) == ZZ_OK);
    assert(zzapp_proc_slot(1)->owner != zzapp_proc_slot(0)->owner);
    zzapp_loader_release();
    selected_slot = 0U;
    zzapp_loader_release();

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
    assert(get32(zzapp_test_arena_mem() + LOAD_OFF) == card + LOAD_OFF);
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
    assert(get32(zzapp_test_arena_mem() + LOAD_OFF + 0x20u) ==
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
    put32(image + NOTE_OFF + 28u, 3u);
    assert(load_end(image) == ZZ_OK);
    zzapp_test_set_entry(dynamic_entry);
    start_pending = 1;
    dynamic_started = 0;
    zzapp_worker_hook();
    assert(dynamic_started);

    zzapp_boot_init(); make_image(image);
    put32(image + NOTE_OFF + 28u, 3u);
    put32(image + NOTE_OFF + 40u, 600u);
    assert(load_end(image) == ZZ_ENOMEM);
    assert(zzapp_loader_state() == ZZAPP_ST_IDLE);
    return 0;
}
