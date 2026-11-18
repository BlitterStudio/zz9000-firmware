/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The MEDIA owner of an app's decoders comes from the admission serial of its
 * process record (slice 5b). The owner must be distinct for every record that
 * is alive at once, however many there are, and must never land in the
 * video-session, module or shared-heap ranges of the MEDIA class; releasing
 * one owner must give back that app's blocks and nobody else's.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "card_pool.h"
#include "host_card_pool.h"
#include "sdk_compression.h"
#include "zzapp_rec.h"

int smp_cpu_id(void) { return 0; }
uint32_t smp_local_irq_save(void) { return 0U; }
void smp_local_irq_restore(uint32_t state) { (void)state; }
void smp_raw_spin_lock(volatile uint32_t *word) { *word = 1U; }
int smp_raw_spin_try_lock(volatile uint32_t *word)
{
    if (*word)
        return 0;
    *word = 1U;
    return 1;
}
void smp_raw_spin_unlock(volatile uint32_t *word) { *word = 0U; }

static int checks;
static int failures;

#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { \
        failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    } \
} while (0)

#define MODULE_TAG 0x00800000U

/* Whether `owner` is a MEDIA app-decoder owner and nothing else. */
static int is_app_owner(uint32_t owner)
{
    uint32_t instance = owner & 0x00ffffffU;

    return CARD_POOL_OWNER_CLASS(owner) == CARD_POOL_CLASS_MEDIA &&
           instance >= SDK_MEDIA_APP_INSTANCE_BASE &&
           instance < SDK_MEDIA_APP_INSTANCE_LIMIT &&
           (instance & MODULE_TAG) == 0U &&
           instance >= SDK_MEDIA_VIDEO_INSTANCE_LIMIT &&
           owner != SDK_DECODE_POOL_OWNER;
}

static int all_distinct(uint32_t first_serial, uint32_t count)
{
    static uint32_t owners[ZZAPP_REC_MAX_APPS];
    uint32_t i;
    uint32_t j;

    for (i = 0U; i < count; i++)
        owners[i] = sdk_compression_app_owner_bits(first_serial + i);
    for (i = 0U; i < count; i++)
        for (j = i + 1U; j < count; j++)
            if (owners[i] == owners[j])
                return 0;
    return 1;
}

/* The full handle table: 1024 records, 1024 serials, 1024 owners. */
static void test_owners_are_distinct_for_1024_serials(void)
{
    uint32_t serial;
    uint32_t bad = 0U;

    CHECK(all_distinct(1U, ZZAPP_REC_MAX_APPS));
    for (serial = 1U; serial <= ZZAPP_REC_MAX_APPS; serial++)
        bad += !is_app_owner(sdk_compression_app_owner_bits(serial));
    CHECK(bad == 0U);
    /* Windows that straddle the app range's size, where the serial wraps
     * round the instance space, and the end of the 24-bit serial space. */
    CHECK(all_distinct(SDK_MEDIA_APP_INSTANCE_LIMIT -
                       SDK_MEDIA_APP_INSTANCE_BASE - 512U, ZZAPP_REC_MAX_APPS));
    CHECK(all_distinct(0x00fffc00U, 1023U));
    CHECK(all_distinct(0x00000001U + 0x007fff00U, ZZAPP_REC_MAX_APPS));
}

/* No serial may reach the video sessions below the app range, or the module
 * and shared-heap owners above it. */
static void test_no_serial_leaves_the_app_range(void)
{
    uint32_t serial;
    uint32_t bad = 0U;

    for (serial = 1U; serial <= 0x00ffffffU; serial++)
        bad += !is_app_owner(sdk_compression_app_owner_bits(serial));
    CHECK(bad == 0U);
    CHECK(sdk_compression_app_owner_bits(0x00ffffffU) != SDK_DECODE_POOL_OWNER);
}

static uint32_t blocks_of(uint32_t owner)
{
    uint32_t i;
    uint32_t n = 0U;

    for (i = 0U; i < card_pool.block_count; i++)
        n += card_pool.blocks[i].owner == owner;
    return n;
}

/* Releasing one app's owner returns that app's blocks and only those; the
 * video sessions and the shared decode heap keep theirs. */
static void test_reclaim_by_owner_leaves_every_other_owner(void)
{
    const uint32_t video = CARD_POOL_OWNER(CARD_POOL_CLASS_MEDIA, 4U);
    uint32_t serial;

    host_card_pool_reset(128U << 20);
    CHECK(card_pool_alloc(&card_pool, 0x10000U, video) != 0U);
    CHECK(card_pool_alloc(&card_pool, 0x10000U, SDK_DECODE_POOL_OWNER) != 0U);
    for (serial = 1U; serial <= 200U; serial++) {
        uint32_t owner = sdk_compression_app_owner_bits(serial);

        CHECK(card_pool_alloc(&card_pool, 0x10000U, owner) != 0U);
        CHECK(card_pool_alloc(&card_pool, 0x10000U, owner) != 0U);
    }
    CHECK(card_pool.block_count == 2U + 400U);

    CHECK(card_pool_release_owner(&card_pool,
                                  sdk_compression_app_owner_bits(77U)) != 0U);
    CHECK(card_pool.block_count == 2U + 398U);
    CHECK(blocks_of(sdk_compression_app_owner_bits(77U)) == 0U);
    CHECK(blocks_of(sdk_compression_app_owner_bits(76U)) == 2U);
    CHECK(blocks_of(sdk_compression_app_owner_bits(78U)) == 2U);
    CHECK(blocks_of(video) == 1U);
    CHECK(blocks_of(SDK_DECODE_POOL_OWNER) == 1U);
    CHECK(card_pool_release_owner(&card_pool,
                                  sdk_compression_app_owner_bits(77U)) == 0U);

    for (serial = 1U; serial <= 200U; serial++)
        (void)card_pool_release_owner(&card_pool,
                                      sdk_compression_app_owner_bits(serial));
    CHECK(card_pool.block_count == 2U);
    CHECK(blocks_of(video) == 1U);
    CHECK(blocks_of(SDK_DECODE_POOL_OWNER) == 1U);
}

int main(void)
{
    test_owners_are_distinct_for_1024_serials();
    test_no_serial_leaves_the_app_range();
    test_reclaim_by_owner_leaves_every_other_owner();
    printf("decode_owner_test: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
