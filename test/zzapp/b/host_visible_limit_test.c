/*
 * Per-app host-visible limit requested at LOAD_BEGIN: the default, a raised
 * and a lowered request, the refusal of a request the card could never hold,
 * the legacy request forms, and a single block larger than 16 MiB.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZAPP_HOST_DEFERRED_TEST
#define ZZAPP_HOST_DEFERRED_TEST
#endif
#include "zzapp_services.c"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "aar/app.h"
#include "zzapp_proto.h"

void zzapp_boot_init(void);
void zzapp_worker_hook(void);
int zzapp_loader_abort(void);
void zzapp_test_set_host_visible_heap(uint32_t bytes);

#define MIB (1024U * 1024U)

static uint8_t launcher_window[ZZAPP_WINDOW_BYTES];
static uint32_t request_id;

static void extended_request(uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES],
                             uint32_t generation)
{
    memset(request, 0, ZZAPP_MAILBOX_PAYLOAD_BYTES);
    zzapp_put_be32(request + ZZAPP_SEL_SLOT, 0U);
    zzapp_put_be32(request + ZZAPP_SEL_GENERATION, generation);
}

static uint32_t claim_slot(void)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0U;

    zzapp_boot_init();
    zzapp_services_test_reset();
    memset(launcher_window, 0, sizeof(launcher_window));
    zzapp_services_test_set_window_slot(0U, launcher_window,
                                        sizeof(launcher_window));
    zzapp_test_set_host_visible_heap(78U * MIB);
    extended_request(request, 0U);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request,
                                  sizeof(request), reply, &reply_bytes, 1,
                                  ++request_id, 0U) == SDK_STATUS_OK);
    return zzapp_get_be32(reply + ZZAPP_ALLOC_GENERATION);
}

/* Runs one LOAD_BEGIN through the deferred worker and returns its status.
 * A request refused at dispatch returns that status without queueing. */
static uint16_t load_begin(const uint8_t *request, uint16_t request_bytes)
{
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0U;
    uint16_t status;
    uint32_t pass;

    status = zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, request,
                                    request_bytes, reply, &reply_bytes, 1,
                                    ++request_id, 0U);
    if (status != SDK_STATUS_QUEUED)
        return status;
    for (pass = 0U; pass < 16U; pass++) {
        zzapp_worker_hook();
        if (zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL,
                                  &reply_bytes)) {
            zzapp_mailbox_harvest_ack();
            return status;
        }
    }
    assert(0 && "LOAD_BEGIN did not complete");
    return SDK_STATUS_IO_ERROR;
}

static uint64_t shared_used(void)
{
    aar_info info;

    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    assert(zzapp_host_info(&info) == AAR_OK);
    return info.shared_used;
}

static uint64_t host_limit(void)
{
    aar_info info;

    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    assert(zzapp_host_info(&info) == AAR_OK);
    return info.shared_limit;
}

/* The app ends: its loader arena is dropped, then the record is retired, the
 * one path that returns the record and unbinds the window. (A claim used to
 * be free again as soon as the loader state read IDLE; a bound record is now
 * released by retire only, as in the host's abort paths.) */
static void finish(void)
{
    zzapp_rec_t *rec = window_rec(&windows[0]);

    zzapp_host_revoke_visible();
    assert(zzapp_loader_abort() == AAR_OK);
    assert(rec != NULL);
    assert(zzapp_rec_retire(rec->hdr.pid) == 1);
}

static void begin_with(uint32_t host_mib, uint32_t expected_status)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint32_t generation = claim_slot();

    extended_request(request, generation);
    zzapp_put_be32(request + ZZAPP_LOAD_HOST_VISIBLE_MIB, host_mib);
    assert(load_begin(request, sizeof(request)) == expected_status);
}

static void test_default_is_16_mib_without_a_request(void)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint32_t generation;
    void *block;

    /* Extended request with the word left at zero. */
    begin_with(0U, SDK_STATUS_OK);
    assert(host_limit() == 16U * MIB);
    block = zzapp_host_alloc(16U * MIB, 4096U);
    assert(block != NULL);
    assert(zzapp_host_alloc(4096U, 4096U) == NULL);
    assert(zzapp_host_free(block) == AAR_OK);
    assert(zzapp_host_alloc(16U * MIB + 4096U, 4096U) == NULL);
    finish();

    /* The memory cap alone, as launchers sent before this request existed. */
    generation = claim_slot();
    extended_request(request, generation);
    zzapp_put_be32(request + ZZAPP_LOAD_MEM_LIMIT_MIB, 32U);
    assert(load_begin(request, sizeof(request)) == SDK_STATUS_OK);
    assert(zzapp_services_memory_limit() == 32U * MIB);
    assert(host_limit() == 16U * MIB);
    finish();

    /* The legacy form has no payload at all. */
    claim_slot();
    assert(load_begin(NULL, 0U) == SDK_STATUS_OK);
    assert(host_limit() == 16U * MIB);
    finish();
}

static void test_request_raises_the_limit(void)
{
    void *block;

    begin_with(40U, SDK_STATUS_OK);
    assert(host_limit() == 40U * MIB);
    assert(zzapp_services_memory_limit() == 0U);
    /* One block of 34 MiB, larger than the old whole-app limit. */
    block = zzapp_host_alloc(34U * MIB, 4096U);
    assert(block != NULL);
    assert(shared_used() == 34U * MIB);
    assert(host_limit() == 40U * MIB);
    /* The cap counts every block of the app. */
    assert(zzapp_host_alloc(7U * MIB, 4096U) == NULL);
    assert(zzapp_host_alloc(6U * MIB, 4096U) != NULL);
    assert(zzapp_host_free(block) == AAR_OK);
    finish();
}

static void test_request_can_lower_the_limit(void)
{
    begin_with(4U, SDK_STATUS_OK);
    assert(host_limit() == 4U * MIB);
    assert(zzapp_host_alloc(5U * MIB, 4096U) == NULL);
    assert(zzapp_host_alloc(4U * MIB, 4096U) != NULL);
    finish();
}

static void test_request_beyond_the_card_is_refused(void)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint32_t generation;

    /* Far more than the heap and one app's alias window can hold. */
    begin_with(200U, SDK_STATUS_NO_MEMORY);
    assert(host_limit() == 16U * MIB);
    assert(zzapp_services_state() == ZZAPP_ST_CLAIMED);
    zzapp_host_revoke_visible();

    /* The refusal leaves the claim usable: a smaller request then works. */
    generation = zzapp_get_be32(launcher_window + ZZAPP_LIFE_GENERATION_OFF);
    extended_request(request, generation);
    zzapp_put_be32(request + ZZAPP_LOAD_HOST_VISIBLE_MIB, 24U);
    assert(load_begin(request, sizeof(request)) == SDK_STATUS_OK);
    assert(host_limit() == 24U * MIB);
    finish();

    /* The ceiling follows the heap the allocator manages. */
    generation = claim_slot();
    zzapp_test_set_host_visible_heap(20U * MIB);
    extended_request(request, generation);
    zzapp_put_be32(request + ZZAPP_LOAD_HOST_VISIBLE_MIB, 24U);
    assert(load_begin(request, sizeof(request)) == SDK_STATUS_NO_MEMORY);
    zzapp_put_be32(request + ZZAPP_LOAD_HOST_VISIBLE_MIB, 20U);
    assert(load_begin(request, sizeof(request)) == SDK_STATUS_OK);
    assert(host_limit() == 20U * MIB);
    zzapp_test_set_host_visible_heap(78U * MIB);
    finish();
}

static void test_limit_does_not_outlive_the_claim(void)
{
    begin_with(40U, SDK_STATUS_OK);
    assert(host_limit() == 40U * MIB);
    finish();
    /* The next claim of the slot starts from the default again. */
    begin_with(0U, SDK_STATUS_OK);
    assert(host_limit() == 16U * MIB);
    finish();
}

static void test_capability_is_advertised_to_extended_queries(void)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0U;

    claim_slot();
    extended_request(request, 0U);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_QUERY, request, sizeof(request),
                                  reply, &reply_bytes, 1, ++request_id,
                                  0U) == SDK_STATUS_OK);
    assert(zzapp_get_be32(reply + ZZAPP_QUERY_CAPS) & ZZAPP_CAP_HOST_LIMIT);
    assert(ZZAPP_CAP_HOST_LIMIT == 0x100u);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_QUERY, NULL, 0U, reply,
                                  &reply_bytes, 1, ++request_id, 0U) ==
           SDK_STATUS_OK);
    assert(!(zzapp_get_be32(reply + ZZAPP_QUERY_CAPS) & ZZAPP_CAP_HOST_LIMIT));
    zzapp_host_revoke_visible();
}

/* A second LOAD_BEGIN that the slot refuses (the first is still queued)
 * must not change the limits the first one set. */
static void test_rejected_load_begin_keeps_the_first_limits(void)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0U;
    uint16_t status = SDK_STATUS_BUSY;
    uint32_t generation = claim_slot();
    uint32_t pass;

    extended_request(request, generation);
    zzapp_put_be32(request + ZZAPP_LOAD_MEM_LIMIT_MIB, 8U);
    zzapp_put_be32(request + ZZAPP_LOAD_HOST_VISIBLE_MIB, 40U);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, request,
                                  sizeof(request), reply, &reply_bytes, 1,
                                  ++request_id, 0U) == SDK_STATUS_QUEUED);
    zzapp_put_be32(request + ZZAPP_LOAD_MEM_LIMIT_MIB, 2U);
    zzapp_put_be32(request + ZZAPP_LOAD_HOST_VISIBLE_MIB, 4U);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, request,
                                  sizeof(request), reply, &reply_bytes, 1,
                                  ++request_id, 0U) == SDK_STATUS_BUSY);
    assert(zzapp_services_memory_limit() == 8U * MIB);
    assert(host_limit() == 40U * MIB);
    for (pass = 0U; pass < 16U; pass++) {
        zzapp_worker_hook();
        if (zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL,
                                  &reply_bytes)) {
            zzapp_mailbox_harvest_ack();
            break;
        }
    }
    assert(status == SDK_STATUS_OK);
    assert(zzapp_services_memory_limit() == 8U * MIB);
    assert(host_limit() == 40U * MIB);
    finish();
}

/* Card sequence: a launcher's LOAD_BEGIN is refused (HOSTMEM too large), it
 * closes its handle, and the next launcher's ordinary LOAD_BEGIN must still
 * be accepted. */
static void test_load_after_a_refused_claim_was_cancelled(void)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0U;
    uint32_t generation = claim_slot();
    uint32_t pass;

    extended_request(request, generation);
    zzapp_put_be32(request + ZZAPP_LOAD_HOST_VISIBLE_MIB, 63U);
    assert(load_begin(request, sizeof(request)) == SDK_STATUS_NO_MEMORY);

    /* ZZ9KAppClose: a STOP for the claim that never loaded. */
    extended_request(request, generation);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_STOP, request, sizeof(request),
                                  reply, &reply_bytes, 1, ++request_id,
                                  0U) == SDK_STATUS_OK);
    for (pass = 0U; pass < 8U; pass++)
        zzapp_worker_hook();

    /* Next launcher: claim again and load, plain and with a limit. */
    for (pass = 0U; pass < 2U; pass++) {
        extended_request(request, 0U);
        assert(zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request,
                                      sizeof(request), reply, &reply_bytes,
                                      1, ++request_id, 0U) ==
               SDK_STATUS_OK);
        generation = zzapp_get_be32(reply + ZZAPP_ALLOC_GENERATION);
        extended_request(request, generation);
        if (pass == 1U)
            zzapp_put_be32(request + ZZAPP_LOAD_HOST_VISIBLE_MIB, 8U);
        assert(load_begin(request, sizeof(request)) == SDK_STATUS_OK);
        assert(host_limit() == (pass == 1U ? 8U : 16U) * MIB);
        finish();
        extended_request(request, generation);
        assert(zzapp_mailbox_dispatch(ZZAPP_OP_STOP, request,
                                      sizeof(request), reply, &reply_bytes,
                                      1, ++request_id, 0U) == SDK_STATUS_OK);
        for (reply_bytes = 0U; reply_bytes < 8U; reply_bytes++)
            zzapp_worker_hook();
        reply_bytes = 0U;
    }
}

int main(int argc, char **argv)
{
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        { "default", test_default_is_16_mib_without_a_request },
        { "raise", test_request_raises_the_limit },
        { "lower", test_request_can_lower_the_limit },
        { "refuse", test_request_beyond_the_card_is_refused },
        { "reclaim", test_limit_does_not_outlive_the_claim },
        { "cap", test_capability_is_advertised_to_extended_queries },
        { "rejected", test_rejected_load_begin_keeps_the_first_limits },
        { "cancel", test_load_after_a_refused_claim_was_cancelled },
    };
    unsigned i;

    /* One argument names a single case, so a failing case is reported on
     * its own; no argument runs them all. */
    for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (argc < 2 || strcmp(argv[1], cases[i].name) == 0)
            cases[i].run();
    }
    return 0;
}
