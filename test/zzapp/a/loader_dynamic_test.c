/*
 * Relocations come from PT_DYNAMIC (DT_REL/DT_RELSZ/DT_RELENT and the RELA
 * triple, R_ARM_RELATIVE only), not from section headers: an image with none
 * loads, a malformed dynamic table is refused whatever the section headers
 * say, and DT_NEEDED is still refused.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "aar/app.h"
#include "aar_note_image.h"
#include "zzapp_runtime.h"

#define IMAGE_BYTES 0x300u
#define PHOFF 52u
#define REL_OFF 0x0d0u
#define RELA_OFF 0x0e0u
#define DYN_OFF 0x100u
#define DATA_OFF 0x200u
#define SHOFF 0x2c0u

#define DT_NULL 0u
#define DT_NEEDED 1u
#define DT_PLTRELSZ 2u
#define DT_RELA 7u
#define DT_RELASZ 8u
#define DT_RELAENT 9u
#define DT_REL 17u
#define DT_RELSZ 18u
#define DT_RELENT 19u

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

/* A hard-float ET_DYN image with no section headers: one PT_LOAD that maps
 * the whole file at vaddr 0 (so the relocation tables have virtual
 * addresses), the AAR note, and a PT_DYNAMIC with room for ten tags. */
static void make_image(uint8_t image[IMAGE_BYTES])
{
    uint8_t *ph = image + PHOFF;

    memset(image, 0, IMAGE_BYTES);
    image[0] = 0x7f; image[1] = 'E'; image[2] = 'L'; image[3] = 'F';
    image[4] = 1; image[5] = 1; image[6] = 1;
    put16(image + 16, 3);                 /* ET_DYN */
    put16(image + 18, 40);                /* EM_ARM */
    put32(image + 24, DATA_OFF);
    put32(image + 28, PHOFF);
    put32(image + 36, 0x05000400);       /* EABI 5, hard-float */
    put16(image + 40, 52);
    put16(image + 42, 32);
    put16(image + 44, 3);
    /* e_shoff, e_shentsize, e_shnum stay 0: stripped. */
    put32(ph + 0, 1);                     /* PT_LOAD */
    put32(ph + 16, DATA_OFF + 16U);
    put32(ph + 20, DATA_OFF + 32U);
    put32(ph + 24, 5);                    /* PF_R | PF_X */
    ph += 32;
    put32(ph + 0, 4);                     /* PT_NOTE */
    put32(ph + 4, TEST_NOTE_OFF);
    put32(ph + 16, TEST_NOTE_BYTES);
    ph += 32;
    put32(ph + 0, 2);                     /* PT_DYNAMIC */
    put32(ph + 4, DYN_OFF);
    put32(ph + 8, DYN_OFF);
    put32(ph + 16, 80);
    put32(ph + 20, 80);
    put32(ph + 24, 4);
    test_note_init(image + TEST_NOTE_OFF);
}

static void dt(uint8_t *image, unsigned i, uint32_t tag, uint32_t value)
{
    put32(image + DYN_OFF + 8U * i, tag);
    put32(image + DYN_OFF + 8U * i + 4U, value);
}

static void rela(uint8_t *image, unsigned i, uint32_t where, uint32_t info,
                 uint32_t addend)
{
    put32(image + RELA_OFF + 12U * i, where);
    put32(image + RELA_OFF + 12U * i + 4U, info);
    put32(image + RELA_OFF + 12U * i + 8U, addend);
}

static void rel(uint8_t *image, unsigned i, uint32_t where, uint32_t info)
{
    put32(image + REL_OFF + 8U * i, where);
    put32(image + REL_OFF + 8U * i + 4U, info);
}

/* DATA_OFF + 0: RELA, addend 0x1000. DATA_OFF + 4: REL, target word 0x2000. */
static void standard_tables(uint8_t *image)
{
    rela(image, 0, DATA_OFF, 23U, 0x1000U);
    rel(image, 0, DATA_OFF + 4U, 23U);
    put32(image + DATA_OFF + 4U, 0x2000U);
    dt(image, 0, DT_RELA, RELA_OFF);
    dt(image, 1, DT_RELASZ, 12U);
    dt(image, 2, DT_RELAENT, 12U);
    dt(image, 3, DT_REL, REL_OFF);
    dt(image, 4, DT_RELSZ, 8U);
    dt(image, 5, DT_RELENT, 8U);
    dt(image, 6, DT_NULL, 0U);
}

static int load(const uint8_t *image)
{
    int rc;

    zzapp_boot_init();
    assert(zzapp_test_admit() == 0U);
    assert(zzapp_loader_begin() == AAR_OK);
    assert(zzapp_loader_chunk(0, image, IMAGE_BYTES) == AAR_OK);
    do {
        rc = zzapp_loader_end();
    } while (rc == ZZAPP_LOADER_IN_PROGRESS);
    return rc;
}

static void expect_refused(const uint8_t *image)
{
    assert(load(image) == AAR_E_INVAL);
    assert(zzapp_loader_state() == ZZAPP_ST_IDLE);
    assert(zzapp_test_arena_mem() == NULL);
}

static void test_tables_from_dynamic_with_no_section_headers(void)
{
    uint8_t image[IMAGE_BYTES];
    uint32_t card;
    const uint8_t *mem;

    make_image(image);
    standard_tables(image);
    assert(load(image) == AAR_OK);
    card = zzapp_test_arena_card();
    mem = zzapp_test_arena_mem();
    /* RELA: addend in the entry. REL: addend is the word at the target. */
    assert(get32(mem + DATA_OFF) == card + 0x1000U);
    assert(get32(mem + DATA_OFF + 4U) == card + 0x2000U);
    zzapp_loader_release();
}

static void test_rela_alone_and_rel_alone(void)
{
    uint8_t image[IMAGE_BYTES];
    uint32_t card;

    make_image(image);
    rela(image, 0, DATA_OFF + 8U, 23U, 0x30U);
    rela(image, 1, DATA_OFF + 12U, 23U, 0x40U);
    dt(image, 0, DT_RELA, RELA_OFF);
    dt(image, 1, DT_RELASZ, 24U);
    dt(image, 2, DT_RELAENT, 12U);
    assert(load(image) == AAR_OK);
    card = zzapp_test_arena_card();
    assert(get32(zzapp_test_arena_mem() + DATA_OFF + 8U) == card + 0x30U);
    assert(get32(zzapp_test_arena_mem() + DATA_OFF + 12U) == card + 0x40U);
    zzapp_loader_release();

    make_image(image);
    rel(image, 0, DATA_OFF, 23U);
    put32(image + DATA_OFF, 0x70U);
    dt(image, 0, DT_REL, REL_OFF);
    dt(image, 1, DT_RELSZ, 8U);
    dt(image, 2, DT_RELENT, 8U);
    assert(load(image) == AAR_OK);
    card = zzapp_test_arena_card();
    assert(get32(zzapp_test_arena_mem() + DATA_OFF) == card + 0x70U);
    zzapp_loader_release();
}

static void test_section_headers_are_not_the_source(void)
{
    uint8_t image[IMAGE_BYTES];
    uint8_t *sh = image + SHOFF;

    /* An SHT_RELA section that names a relocation, but no PT_DYNAMIC: the
     * old section walk would have applied it; the loader now ignores it. */
    make_image(image);
    rela(image, 0, DATA_OFF, 23U, 0x1000U);
    put32(image + DATA_OFF, 0xdeadbeefU);
    put32(image + 32, SHOFF);
    put16(image + 46, 40);
    put16(image + 48, 1);
    put32(sh + 4, 4);                     /* SHT_RELA */
    put32(sh + 16, RELA_OFF);
    put32(sh + 20, 12);
    put32(sh + 36, 12);
    put16(image + 44, 2);                 /* drop the PT_DYNAMIC header */
    assert(load(image) == AAR_OK);
    assert(get32(zzapp_test_arena_mem() + DATA_OFF) == 0xdeadbeefU);
    zzapp_loader_release();
}

static void test_bad_sizes_and_entry_sizes(void)
{
    uint8_t image[IMAGE_BYTES];

    /* DT_RELSZ not a whole number of entries. */
    make_image(image);
    standard_tables(image);
    dt(image, 4, DT_RELSZ, 12U);
    expect_refused(image);
    /* DT_RELASZ not a whole number of entries. */
    make_image(image);
    standard_tables(image);
    dt(image, 1, DT_RELASZ, 8U);
    expect_refused(image);
    /* DT_RELSZ past the file bytes of every segment. */
    make_image(image);
    standard_tables(image);
    dt(image, 4, DT_RELSZ, 0x1000U);
    expect_refused(image);
    /* DT_RELSZ without a DT_REL. */
    make_image(image);
    dt(image, 0, DT_RELSZ, 8U);
    dt(image, 1, DT_RELENT, 8U);
    expect_refused(image);
    /* DT_RELENT that is not 8, and DT_RELAENT that is not 12. */
    make_image(image);
    standard_tables(image);
    dt(image, 5, DT_RELENT, 12U);
    expect_refused(image);
    make_image(image);
    standard_tables(image);
    dt(image, 2, DT_RELAENT, 8U);
    expect_refused(image);
    /* A table with no entry size at all. */
    make_image(image);
    standard_tables(image);
    dt(image, 5, DT_NULL, 0U);
    expect_refused(image);
    /* A table whose address is in no segment. */
    make_image(image);
    standard_tables(image);
    dt(image, 3, DT_REL, 0x9000U);
    expect_refused(image);
}

static void test_dynamic_table_content_is_refused(void)
{
    uint8_t image[IMAGE_BYTES];

    /* DT_NEEDED stays refused: no shared objects. */
    make_image(image);
    standard_tables(image);
    dt(image, 6, DT_NEEDED, 1U);
    dt(image, 7, DT_NULL, 0U);
    expect_refused(image);
    /* A PLT relocation table is not R_ARM_RELATIVE. */
    make_image(image);
    standard_tables(image);
    dt(image, 6, DT_PLTRELSZ, 8U);
    dt(image, 7, DT_NULL, 0U);
    expect_refused(image);
    /* Any type but R_ARM_RELATIVE, and a relocation against a symbol. */
    make_image(image);
    standard_tables(image);
    rela(image, 0, DATA_OFF, 21U, 0U);    /* R_ARM_GLOB_DAT */
    expect_refused(image);
    make_image(image);
    standard_tables(image);
    rel(image, 0, DATA_OFF + 4U, (1U << 8) | 23U);
    expect_refused(image);
    /* A relocation of an unaligned word. */
    make_image(image);
    standard_tables(image);
    rela(image, 0, DATA_OFF + 2U, 23U, 0U);
    expect_refused(image);
    /* Two PT_DYNAMIC headers. */
    make_image(image);
    standard_tables(image);
    memcpy(image + PHOFF + 32U, image + PHOFF + 64U, 32U);
    expect_refused(image);
}

int main(void)
{
    test_tables_from_dynamic_with_no_section_headers();
    test_rela_alone_and_rel_alone();
    test_section_headers_are_not_the_source();
    test_bad_sizes_and_entry_sizes();
    test_dynamic_table_content_is_refused();
    printf("loader_dynamic_test: ok\n");
    return 0;
}
