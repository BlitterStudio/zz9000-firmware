/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "card_pool.h"
#include "host_card_pool.h"
#include "mw_host.h"
#include "mw_types.h"
#include "mw_texture.h"
int smp_cpu_id(void) { return 0; }
uint32_t smp_local_irq_save(void) { return 0U; }
void smp_local_irq_restore(uint32_t flags) { (void)flags; }
void smp_raw_spin_lock(volatile uint32_t *word) { *word = 1U; }
int smp_raw_spin_try_lock(volatile uint32_t *word)
{
    if (*word)
        return 0;
    *word = 1U;
    return 1;
}
void smp_raw_spin_unlock(volatile uint32_t *word) { *word = 0U; }


static void put_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static uint32_t texture_packet(uint8_t *packet, uint32_t opcode,
                               uint32_t payload_bytes)
{
    memset(packet, 0, 24U + payload_bytes);
    put_be32(packet, MW_MAGIC);
    put_be32(packet + 4, MW_VERSION);
    put_be32(packet + 8, 24U + payload_bytes);
    put_be32(packet + 12, 1U);
    put_be32(packet + 16, opcode);
    put_be32(packet + 20, payload_bytes);
    return 24U + payload_bytes;
}

static uint32_t define_texture(uint8_t *packet, uint32_t name)
{
    uint32_t bytes = texture_packet(packet, MW_TEXTURE_IMAGE_RAW, 56U);

    put_be32(packet + 24, name);
    put_be32(packet + 28, 0U);
    put_be32(packet + 32, 4U);
    put_be32(packet + 36, 256U);
    put_be32(packet + 40, 256U);
    put_be32(packet + 44, 0x1908U);
    put_be32(packet + 48, 0x1401U);
    put_be32(packet + 52, 4U);
    put_be32(packet + 76, 0U);
    return bytes;
}

static uint32_t texture_generate_mips(uint8_t *packet, uint32_t name)
{
    uint32_t bytes = texture_packet(packet, MW_TEXTURE_PARAM, 12U);

    put_be32(packet + 24, name);
    put_be32(packet + 28, 0x8191U);
    put_be32(packet + 32, 1U);
    return bytes;
}

static uint32_t lightmap_subimage(uint8_t *packet, uint32_t name,
                                  uint8_t value)
{
    uint32_t bytes = texture_packet(packet, MW_TEXTURE_SUB_RAW,
                                    60U + 16U * 16U * 4U);
    uint8_t *pixels = packet + 84;

    put_be32(packet + 24, name);
    put_be32(packet + 28, 0U);
    put_be32(packet + 32, 0U);
    put_be32(packet + 36, 0U);
    put_be32(packet + 40, 16U);
    put_be32(packet + 44, 16U);
    put_be32(packet + 48, 0x1908U);
    put_be32(packet + 52, 0x1401U);
    put_be32(packet + 56, 4U);
    put_be32(packet + 80, 16U * 16U * 4U);
    memset(pixels, value, 16U * 16U * 4U);
    return bytes;
}

static void test_texture_arena_growth_and_cap(void)
{
    const uint32_t owner = CARD_POOL_OWNER(CARD_POOL_CLASS_MINIGL, 7U);
    mw_texture_manager manager;
    uint32_t texture;
    uint32_t image;
    uint32_t minigl_fill;

    host_card_pool_reset(512U << 20);
    mw_texmgr_init_owned(&manager, owner);
    assert(manager.arena_chunk_count == 1U);
    assert(manager.arena_bytes == MINIGL_TEXTURE_CPU_ARENA_INITIAL_BYTES);
    for (texture = 1U; texture < 1024U; ++texture) {
        mw_texture *entry = mw_texmgr_get_or_create(&manager, texture);

        assert(entry != NULL);
        if (!mw_texmgr_allocate_level(&manager, entry, 0U, 1024U, 256U))
            break;
    }
    assert(manager.arena_chunk_count ==
           MINIGL_TEXTURE_CPU_ARENA_MAX_BYTES /
           MINIGL_TEXTURE_ARENA_GROW_BYTES);
    assert(manager.arena_bytes == MINIGL_TEXTURE_CPU_ARENA_MAX_BYTES);
    assert(texture > 100U);
    minigl_fill = card_pool_alloc(&card_pool,
                                  CARD_POOL_LIMIT_MINIGL - manager.arena_bytes,
                                  owner);
    assert(minigl_fill != 0U);
    assert(card_pool.class_used[CARD_POOL_CLASS_MINIGL] ==
           CARD_POOL_LIMIT_MINIGL);
    image = card_pool_alloc(&card_pool, CARD_POOL_PAGE_BYTES,
                            CARD_POOL_OWNER(CARD_POOL_CLASS_IMAGE, 7U));
    assert(image != 0U);
    assert(card_pool_free(&card_pool, image,
                          CARD_POOL_OWNER(CARD_POOL_CLASS_IMAGE, 7U)) == 1);
    assert(card_pool_free(&card_pool, minigl_fill, owner) == 1);
    mw_texmgr_destroy(&manager);
    assert(card_pool.class_used[CARD_POOL_CLASS_MINIGL] == 0U);
    assert(card_pool.block_count == 0U);
}

int main(void)
{
    enum { texture_count = 64U, update_count = 2048U };
    const uint32_t owner = CARD_POOL_OWNER(CARD_POOL_CLASS_MINIGL, 1U);
    uint8_t packet[1108];
    uint32_t image, texture;
    mw_ctx *context;

    test_texture_arena_growth_and_cap();
    host_card_pool_reset(128U << 20);
    assert(card_pool.block_count == 0U);
    context = mw_host_create_owned(640U, 480U, owner);
    assert(context != NULL);
    assert(card_pool.block_count <= 4U);

    for (texture = 1U; texture <= texture_count; ++texture) {
        assert(mw_host_submit(context, packet,
                              define_texture(packet, texture)) == MW_STATUS_OK);
        assert(mw_host_submit(context, packet,
                              texture_generate_mips(packet, texture)) == MW_STATUS_OK);
    }
    for (texture = 0U; texture < update_count; ++texture) {
        assert(mw_host_submit(context, packet,
                              lightmap_subimage(packet, texture_count,
                                                (uint8_t)texture)) == MW_STATUS_OK);
    }

    /* Chunked CPU texture residency has at most eight card-pool records. */
    assert(card_pool.block_count <= 8U);
    image = card_pool_alloc(&card_pool, CARD_POOL_PAGE_BYTES,
                            CARD_POOL_OWNER(CARD_POOL_CLASS_IMAGE, 1U));
    assert(image != 0U);
    assert(card_pool_free(&card_pool, image,
                          CARD_POOL_OWNER(CARD_POOL_CLASS_IMAGE, 1U)) == 1);

    mw_host_destroy(context);
    assert(card_pool.block_count == 0U);
    assert(card_pool.class_used[CARD_POOL_CLASS_MINIGL] == 0U);
    assert(card_pool.class_used[CARD_POOL_CLASS_IMAGE] == 0U);
    puts("PASS: test_mw_card_pool");
    return 0;
}
