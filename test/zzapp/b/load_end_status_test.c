/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * What LOAD_END reports to the launcher when the loader refuses an image:
 * a note whose profile this runtime does not run is BAD_REQUEST, a note that
 * requires a group the runtime lacks is UNSUPPORTED (the launcher tells the
 * two apart by the code), the other refusals keep their usual codes, and the
 * slot is idle again either way.
 */
#ifndef ZZAPP_HOST_DEFERRED_TEST
#define ZZAPP_HOST_DEFERRED_TEST
#endif
#include "zzapp_services.c"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "aar/app.h"
#include "zzapp_proto.h"
#include "zzapp_runtime.h"
#include "aar_note_image.h"

#define IMAGE_BYTES 0x200u
#define PHOFF 52u
#define LOAD_OFF 0x100u
#define NOTE_OFF TEST_NOTE_OFF
#define RELA_OFF 0x0d0u
#define SHOFF 0x180u
/* Not a descriptor word: the note is left as make_image wrote it. */
#define NO_WORD (TEST_NOTE_W_RESERVED + 1u)

static uint8_t win_mem[ZZAPP_SLOT_COUNT][ZZAPP_WINDOW_BYTES];
static uint32_t request_id;

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

/* A minimal ELF32 ARM image with a valid note; tests change one word. */
static void make_image(uint8_t image[IMAGE_BYTES])
{
    uint8_t *ph;
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
    put32(ph + 16, TEST_NOTE_BYTES);
    test_note_init(image + NOTE_OFF);

    put32(image + RELA_OFF + 0, LOAD_OFF);
    put32(image + RELA_OFF + 4, 23);      /* R_ARM_RELATIVE, symbol zero */
    put32(image + RELA_OFF + 8, LOAD_OFF);
    sh = image + SHOFF;
    put32(sh + 4, 4);                     /* SHT_RELA */
    put32(sh + 16, RELA_OFF);
    put32(sh + 20, 12);
    put32(sh + 36, 12);
}

static void extended_request(uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES],
                             uint32_t slot, uint32_t generation)
{
    memset(request, 0, ZZAPP_MAILBOX_PAYLOAD_BYTES);
    zzapp_put_be32(request + ZZAPP_SEL_SLOT, slot);
    zzapp_put_be32(request + ZZAPP_SEL_GENERATION, generation);
}

/* Runs a deferred request to completion and returns the status the
 * launcher harvests. */
static uint16_t dispatch(uint16_t opcode, const uint8_t *request,
                         uint16_t request_bytes)
{
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0;
    uint16_t status;
    uint32_t pass;

    status = zzapp_mailbox_dispatch(opcode, request, request_bytes, reply,
                                    &reply_bytes, 1, ++request_id, 0);
    assert(status == SDK_STATUS_QUEUED);
    for (pass = 0U; pass < 16U; pass++) {
        zzapp_worker_hook();
        if (zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL,
                                  &reply_bytes)) {
            zzapp_mailbox_harvest_ack();
            return status;
        }
        assert(opcode == ZZAPP_OP_LOAD_END);
    }
    assert(0 && "deferred request did not complete");
    return SDK_STATUS_IO_ERROR;
}

static uint32_t claim_slot(void)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0;

    extended_request(request, 0U, 0U);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request,
                                  sizeof(request), reply, &reply_bytes,
                                  1, 0U, 0U) == SDK_STATUS_OK);
    assert(reply_bytes == ZZAPP_ALLOC_EXT_REPLY_BYTES);
    assert(zzapp_get_be32(reply + ZZAPP_ALLOC_SLOT) == 0U);
    return zzapp_get_be32(reply + ZZAPP_ALLOC_GENERATION);
}

static uint32_t slot_state(uint32_t slot)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0;

    extended_request(request, slot, 0U);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_LIST, request, sizeof(request),
                                  reply, &reply_bytes, 1, 0U, 0U) ==
           SDK_STATUS_OK);
    assert(reply_bytes == ZZAPP_LIST_REPLY_BYTES);
    return zzapp_get_be32(reply + ZZAPP_LIST_STATE);
}

static void fresh_runtime(void)
{
    uint32_t slot;

    /* boot_init lays the record arena out again, so nothing of the previous
     * case is left to release (there is no arena before the first one). */
    memset(win_mem, 0, sizeof(win_mem));
    request_id = 0U;
    zzapp_boot_init();
    zzapp_services_test_reset();
    for (slot = 0U; slot < ZZAPP_SLOT_COUNT; slot++)
        zzapp_services_test_set_window_slot(slot, win_mem[slot],
                                            sizeof(win_mem[slot]));
    zzapp_test_set_groups(0U, 0);
}

/* Claims slot 0, loads `image` up to LOAD_END and returns LOAD_END's status. */
static uint16_t load_status(const uint8_t image[IMAGE_BYTES])
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint32_t generation = claim_slot();

    extended_request(request, 0U, generation);
    assert(dispatch(ZZAPP_OP_LOAD_BEGIN, request, sizeof(request)) ==
           SDK_STATUS_OK);
    assert(slot_state(0U) == ZZAPP_ST_LOADING);
    memcpy(win_mem[0] + ZZAPP_STAGING_OFF, image, IMAGE_BYTES);
    extended_request(request, 0U, generation);
    zzapp_put_be32(request + ZZAPP_CHUNK_FILE_OFF, 0U);
    zzapp_put_be32(request + ZZAPP_CHUNK_LENGTH, IMAGE_BYTES);
    zzapp_put_be32(request + ZZAPP_CHUNK_STAGING, ZZAPP_STAGING_OFF);
    assert(dispatch(ZZAPP_OP_LOAD_CHUNK, request, sizeof(request)) ==
           SDK_STATUS_OK);
    extended_request(request, 0U, generation);
    return dispatch(ZZAPP_OP_LOAD_END, request, sizeof(request));
}

static void expect_load(unsigned word, uint32_t value, uint32_t groups,
                        uint16_t status)
{
    uint8_t image[IMAGE_BYTES];
    uint16_t got;

    fresh_runtime();
    if (groups != 0U)
        zzapp_test_set_groups(groups, 1);
    make_image(image);
    if (word != NO_WORD)
        test_note_set(image + NOTE_OFF, word, value);
    got = load_status(image);
    if (got != status) {
        fprintf(stderr, "note word %u = 0x%x groups 0x%x: status %u, wanted %u\n",
                word, value, groups, (unsigned)got, (unsigned)status);
        assert(0 && "LOAD_END status");
    }
    /* Accepted or refused, the loader is not left half-loaded: a refusal
     * returns the slot to idle at once. */
    if (status != SDK_STATUS_OK)
        assert(slot_state(0U) == ZZAPP_ST_IDLE);
    zzapp_test_set_groups(0U, 0);
}


int main(void)
{
    /* The refusals the launcher distinguishes by status. */
    expect_load(TEST_NOTE_W_PROFILE, AAR_PROFILE_A64, 0U,
                SDK_STATUS_BAD_REQUEST);
    expect_load(TEST_NOTE_W_PROFILE, 0U, 0U, SDK_STATUS_BAD_REQUEST);
    expect_load(TEST_NOTE_W_GROUPS_REQUIRED, 0x80000000U, 0U,
                SDK_STATUS_UNSUPPORTED);
    expect_load(TEST_NOTE_W_GROUPS_REQUIRED, AAR_GROUP_DISPLAY,
                AAR_GROUP_INPUT | AAR_GROUP_SERVICES, SDK_STATUS_UNSUPPORTED);
    expect_load(TEST_NOTE_W_GROUPS_REQUIRED, AAR_GROUP_AUDIO | AAR_GROUP_GL,
                AAR_GROUP_DISPLAY | AAR_GROUP_AUDIO, SDK_STATUS_UNSUPPORTED);

    /* The other refusals, and what is accepted. */
    expect_load(TEST_NOTE_W_MAGIC, 0x4E524142U, 0U, SDK_STATUS_BAD_REQUEST);
    expect_load(TEST_NOTE_W_VERSION, 2U, 0U, SDK_STATUS_BAD_REQUEST);
    expect_load(TEST_NOTE_W_API_MAJOR, AAR_API_MAJOR + 1U, 0U,
                SDK_STATUS_BAD_REQUEST);
    expect_load(TEST_NOTE_W_API_MINOR, AAR_API_MINOR + 1U, 0U,
                SDK_STATUS_BAD_REQUEST);
    expect_load(TEST_NOTE_W_STACK_BYTES, 16U, 0U, SDK_STATUS_BAD_REQUEST);
    expect_load(TEST_NOTE_W_RESERVED, 1U, 0U, SDK_STATUS_BAD_REQUEST);
    expect_load(TEST_NOTE_W_MIN_HEAP_MIB, 600U, 0U, SDK_STATUS_NO_MEMORY);
    expect_load(NO_WORD, 0U, 0U, SDK_STATUS_OK);
    expect_load(TEST_NOTE_W_GROUPS_REQUIRED, AAR_GROUP_INPUT,
                AAR_GROUP_INPUT | AAR_GROUP_SERVICES, SDK_STATUS_OK);
    expect_load(TEST_NOTE_W_GROUPS_OPTIONAL, 0x80000000U, 0U, SDK_STATUS_OK);

    /* A refused load frees the slot for the next one. */
    {
        uint8_t image[IMAGE_BYTES];

        fresh_runtime();
        make_image(image);
        test_note_set(image + NOTE_OFF, TEST_NOTE_W_PROFILE, AAR_PROFILE_A64);
        assert(load_status(image) == SDK_STATUS_BAD_REQUEST);
        assert(slot_state(0U) == ZZAPP_ST_IDLE);
        make_image(image);
        assert(load_status(image) == SDK_STATUS_OK);
        assert(slot_state(0U) == ZZAPP_ST_LOADING);
    }
    return 0;
}
