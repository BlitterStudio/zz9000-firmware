#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "zzapp.h"
#include "zzapp_runtime.h"

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

    zzapp_boot_init();
    make_image(image);
    assert(load_end(image) == ZZ_OK);
    card = zzapp_test_arena_card();
    assert(card != 0u);
    assert(get32(zzapp_test_arena_mem() + LOAD_OFF) == card + LOAD_OFF);
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
    put32(image + PHOFF + 16, (16u << 20) + 1u);
    assert(load_end(image) == ZZ_EINVAL);
    return 0;
}
