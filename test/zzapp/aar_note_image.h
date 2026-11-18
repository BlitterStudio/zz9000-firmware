/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The AAR note (spec 2.3, note version 1) as the host tests' hand-made ELF
 * images carry it. The ELF header and three program headers end at 0x94. The
 * 56-byte note follows at TEST_NOTE_OFF: a 12-byte header, the owner name
 * "AAR\0", then the 40-byte descriptor of ten little-endian words. It ends at
 * 0xcc; test images keep their relocation table at 0xd0.
 */
#ifndef ZZAPP_TEST_AAR_NOTE_IMAGE_H
#define ZZAPP_TEST_AAR_NOTE_IMAGE_H

#include <stdint.h>
#include <string.h>

#include "aar/app.h"

#define TEST_NOTE_OFF   0x094u
#define TEST_NOTE_BYTES 56u
#define TEST_NOTE_DESC  16u

/* Descriptor word numbers. */
enum {
    TEST_NOTE_W_MAGIC = 0,
    TEST_NOTE_W_VERSION,
    TEST_NOTE_W_PROFILE,
    TEST_NOTE_W_API_MAJOR,
    TEST_NOTE_W_API_MINOR,
    TEST_NOTE_W_STACK_BYTES,
    TEST_NOTE_W_MIN_HEAP_MIB,
    TEST_NOTE_W_GROUPS_REQUIRED,
    TEST_NOTE_W_GROUPS_OPTIONAL,
    TEST_NOTE_W_RESERVED
};

static inline void test_note_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Sets one descriptor word of the note that starts at `note`. */
static inline void test_note_set(uint8_t *note, unsigned word, uint32_t value)
{
    test_note_le32(note + TEST_NOTE_DESC + 4u * word, value);
}

/* A valid note for this runtime: profile A32, API 0.AAR_API_MINOR, default
 * stack, no private heap, no groups. Tests change single words afterwards. */
static inline void test_note_init(uint8_t *note)
{
    memset(note, 0, TEST_NOTE_BYTES);
    test_note_le32(note + 0, 4u);                    /* namesz, "AAR\0" */
    test_note_le32(note + 4, AAR_NOTE_DESC_BYTES);   /* descsz */
    test_note_le32(note + 8, AAR_NOTE_TYPE);
    memcpy(note + 12, "AAR", 4);
    test_note_set(note, TEST_NOTE_W_MAGIC, AAR_NOTE_MAGIC);
    test_note_set(note, TEST_NOTE_W_VERSION, AAR_NOTE_VERSION);
    test_note_set(note, TEST_NOTE_W_PROFILE, AAR_PROFILE_A32);
    test_note_set(note, TEST_NOTE_W_API_MAJOR, AAR_API_MAJOR);
    test_note_set(note, TEST_NOTE_W_API_MINOR, AAR_API_MINOR);
}

#endif
