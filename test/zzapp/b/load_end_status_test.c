/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * What LOAD_END reports to the client when the loader refuses an image: a
 * note whose profile this runtime does not run is BAD_REQUEST, a note that
 * requires a group the runtime lacks is UNSUPPORTED (the client tells the
 * two apart by the code), the other refusals keep their usual codes, and the
 * claim ends either way: its control block reads IDLE and the record is the
 * client's to release, after which a new claim loads normally.
 */
#ifndef ZZAPP_HOST_DEFERRED_TEST
#define ZZAPP_HOST_DEFERRED_TEST
#endif
#include "zzapp_services.c"
#include "rt_env.h"

/* Not a descriptor word: the note is left as rt_image wrote it. */
#define NO_WORD (TEST_NOTE_W_RESERVED + 1u)

/* Claims client 0, loads `image` up to LOAD_END and returns LOAD_END's
 * status. */
static uint16_t load_status(const uint8_t image[RT_IMAGE_BYTES])
{
    rt_claim(0U);
    assert(rt_load_image(0U, image) == SDK_STATUS_OK);
    assert(RT_STATE(0U) == AAR_STATE_LOADING);
    return rt_simple(0U, ZZAPP_OP_LOAD_END);
}

static void expect_load(unsigned word, uint32_t value, uint32_t groups,
                        uint16_t status)
{
    uint8_t image[RT_IMAGE_BYTES];
    uint16_t got;
    struct rt_counts start;

    rt_boot(RT_MB(256), 8U);
    start = rt_counts();
    zzapp_test_set_groups(groups, groups != 0U);
    rt_image(image, 1U);
    if (word != NO_WORD)
        test_note_set(image + TEST_NOTE_OFF, word, value);
    got = load_status(image);
    if (got != status) {
        fprintf(stderr, "note word %u = 0x%x groups 0x%x: status %u, wanted %u\n",
                word, value, groups, (unsigned)got, (unsigned)status);
        assert(0 && "LOAD_END status");
    }
    if (status != SDK_STATUS_OK) {
        /* A refusal ends the claim at once: nothing is left loaded and the
         * control block says so; only the shell stays for the client. */
        assert(RT_STATE(0U) == AAR_STATE_IDLE);
        assert(zzapp_loader_state() == ZZAPP_ST_IDLE);
        assert(zzapp_test_arena_card() == 0U);
        assert(rt_simple(0U, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
        rt_settle(4U);
        rt_assert_back(start);
    } else {
        assert(RT_STATE(0U) == AAR_STATE_LOADING);
        assert(rt_simple(0U, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
        rt_settle(4U);
        rt_assert_back(start);
    }
    zzapp_test_set_groups(0U, 0);
}

int main(void)
{
    /* The refusals the client distinguishes by status. */
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

    /* A refused load frees what it held for the next one. */
    {
        uint8_t image[RT_IMAGE_BYTES];
        struct rt_counts start;

        rt_boot(RT_MB(256), 8U);
        start = rt_counts();
        rt_image(image, 1U);
        test_note_set(image + TEST_NOTE_OFF, TEST_NOTE_W_PROFILE, AAR_PROFILE_A64);
        assert(load_status(image) == SDK_STATUS_BAD_REQUEST);
        assert(RT_STATE(0U) == AAR_STATE_IDLE);
        assert(rt_simple(0U, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
        rt_settle(4U);
        rt_assert_back(start);
        rt_image(image, 1U);
        assert(load_status(image) == SDK_STATUS_OK);
        assert(RT_STATE(0U) == AAR_STATE_LOADING);
        assert(rt_simple(0U, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
        rt_settle(4U);
        rt_assert_back(start);
    }
    puts("load_end_status_test: ok");
    return 0;
}
