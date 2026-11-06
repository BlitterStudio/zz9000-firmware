#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "card_pool.h"
#include "sdk_palette.h"
#include "surface_allocator.h"
#include "memorymap.h"
#include "zzm_runtime.h"

static int failures;
static uint64_t now_us;
static uint32_t cache_clean_calls;
static uint32_t cache_invalidate_calls;
static uint32_t cache_instruction_calls;
static uint32_t app_in_api;
static uint32_t flip_calls;
static uint32_t log_lines;

#define CHECK(expr) do { \
    if (!(expr)) { \
        ++failures; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    } \
} while (0)

static int trap_held_lock;
static int held_lock_taken;
int smp_cpu_id(void) { return 0; }
uint32_t smp_local_irq_save(void) { return 0U; }
void smp_local_irq_restore(uint32_t state) { (void)state; }
void smp_raw_spin_lock(volatile uint32_t *word)
{
    if (trap_held_lock && *word) {
        held_lock_taken = 1;
        return;
    }
    *word = 1U;
}
int smp_raw_spin_try_lock(volatile uint32_t *word)
{
    if (*word)
        return 0;
    *word = 1U;
    return 1;
}
void smp_raw_spin_unlock(volatile uint32_t *word) { *word = 0U; }

void XTime_GetTime(uint64_t *time) { *time = now_us; }
void Xil_DCacheFlushRange(uint32_t address, uint32_t length)
{
    (void)address;
    (void)length;
    ++cache_clean_calls;
}
void Xil_DCacheInvalidateRange(uint32_t address, uint32_t length)
{
    (void)address;
    (void)length;
    ++cache_invalidate_calls;
}
void Xil_ICacheInvalidateRange(uint32_t address, uint32_t length)
{
    (void)address;
    (void)length;
    ++cache_instruction_calls;
}

int zzm_test_printf(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    va_end(ap);
    ++log_lines;
    return 0;
}

void zzapp_api_enter(void) { ++app_in_api; }
void zzapp_api_leave(void) { if (app_in_api) --app_in_api; }
uint32_t zzapp_current_generation(void) { return 7U; }
int zzapp_display_bound(uint32_t *width, uint32_t *height, uint32_t *pitch,
                        uint32_t *format, uint32_t *card_front,
                        uint32_t *card_back)
{
    if (width) *width = 320U;
    if (height) *height = 200U;
    if (pitch) *pitch = 640U;
    if (format) *format = 1U;
    if (card_front) *card_front = 0x200000U;
    if (card_back) *card_back = 0x210000U;
    return 0;
}
int zzapp_display_layout(uint32_t *layout)
{
    if (layout)
        *layout = 5U;
    return 0;
}

int zzapp_svc_display_flip(uint32_t surface_id)
{
    (void)surface_id;
    ++flip_calls;
    return 0;
}
int zzapp_svc_display_flip_async(uint32_t surface_id)
{
    return zzapp_svc_display_flip(surface_id);
}
int zzapp_display_flip_idle(void) { return 1; }
int sdk_aperture_runtime_is_zorro3(void) { return 1; }

int zzm_registry_register_ops(uint32_t module_id, const void *ops, uint32_t size)
{ (void)module_id; (void)ops; (void)size; return 1; }
int zzm_registry_register_service(uint32_t module_id, uint32_t id,
                                  uint32_t version, const void *interface,
                                  uint32_t size)
{ (void)module_id; (void)id; (void)version; (void)interface; (void)size; return 1; }
void zzm_loader_mark_faulted(uint32_t module_id, const char *reason)
{ (void)module_id; (void)reason; }

static void reset_resources(void)
{
    card_pool_init(&card_pool);
    CHECK(card_pool_add_range(&card_pool, 0x10000000U, 0x14000000U, 0) == 0);
    card_pool_open_range(&card_pool, 0U);
    surface_allocator_init(0x08100000U, 0x00100000U);
    sdk_palette_reset();
    now_us = 0U;
    cache_clean_calls = 0U;
    cache_invalidate_calls = 0U;
    cache_instruction_calls = 0U;
    app_in_api = 0U;
    flip_calls = 0U;
    log_lines = 0U;
}

static void test_typed_owner_quota_and_reclaim(void)
{
    const zzm_services_t *services = zzm_runtime_services_v3();
    CHECK(services->abi_version == ZZM_V3_ABI_MAJOR);
    const zzm_reclaim_service_t *reclaim;
    const zzm_memory_service_t *memory;
    const zzm_cache_service_t *cache;
    uint32_t context;
    uint32_t rix;
    uint32_t address;

    reset_resources();
    reclaim = services->get_service(ZZM_SERVICE_RECLAIM, ZZM_SERVICE_RECLAIM_V3);
    memory = services->get_service(ZZM_SERVICE_MEMORY, ZZM_SERVICE_MEMORY_V3);
    cache = services->get_service(ZZM_SERVICE_CACHE, ZZM_SERVICE_CACHE_V3);
    CHECK(reclaim != 0 && memory != 0 && cache != 0);
    CHECK(reclaim->create_owner(4U, ZZM_RUNTIME_OWNER_CLASS_MINIGL_CONTEXT,
                                0U, CARD_POOL_PAGE_BYTES, &context));
    address = memory->alloc_ex(context, ZZM_RUNTIME_OWNER_CLASS_MINIGL_CONTEXT,
                               CARD_POOL_PAGE_BYTES, CARD_POOL_PAGE_BYTES,
                               64U, ZZM_MEMORY_ATTR_NONE);
    CHECK(address != 0U);
    CHECK(memory->alloc_ex(context, ZZM_RUNTIME_OWNER_CLASS_MINIGL_CONTEXT,
                           CARD_POOL_PAGE_BYTES, 1U, 64U,
                           ZZM_MEMORY_ATTR_NONE) == 0U);
    CHECK(cache->dcache_clean_owned(context, address + 32U, 64U));
    CHECK(cache_clean_calls == 1U);
    CHECK(!cache->dcache_invalidate_owned(context, address + CARD_POOL_PAGE_BYTES,
                                          1U));
    CHECK(reclaim->create_owner(4U, ZZM_RUNTIME_OWNER_CLASS_MINIGL_RIX,
                                context, CARD_POOL_PAGE_BYTES, &rix));
    CHECK(memory->alloc_ex(rix, ZZM_RUNTIME_OWNER_CLASS_MINIGL_RIX,
                           CARD_POOL_PAGE_BYTES, 1U, 64U,
                           ZZM_MEMORY_ATTR_NONE) != 0U);
    CHECK(reclaim->force_reclaim(context) >= CARD_POOL_PAGE_BYTES * 2U);
    CHECK(card_pool.block_count == 0U);
}

/* MiniGL invalidates a fence in a context-owned command buffer while its
 * allocation context names another owner of the same module (RasterIX,
 * module or app owner).  That must reach the cache; another module must
 * still be refused. */
static void test_owned_ops_cover_the_whole_module(void)
{
    const zzm_services_t *services = zzm_runtime_services_v3();
    const zzm_reclaim_service_t *reclaim;
    const zzm_memory_service_t *memory;
    const zzm_cache_service_t *cache;
    uint32_t context;
    uint32_t rix;
    uint32_t other;
    uint32_t address;

    reset_resources();
    reclaim = services->get_service(ZZM_SERVICE_RECLAIM, ZZM_SERVICE_RECLAIM_V3);
    memory = services->get_service(ZZM_SERVICE_MEMORY, ZZM_SERVICE_MEMORY_V3);
    cache = services->get_service(ZZM_SERVICE_CACHE, ZZM_SERVICE_CACHE_V3);
    CHECK(reclaim->create_owner(4U, ZZM_RUNTIME_OWNER_CLASS_MINIGL_CONTEXT,
                                0U, CARD_POOL_PAGE_BYTES, &context));
    CHECK(reclaim->create_owner(4U, ZZM_RUNTIME_OWNER_CLASS_MINIGL_RIX,
                                0U, CARD_POOL_PAGE_BYTES, &rix));
    CHECK(reclaim->create_owner(5U, ZZM_RUNTIME_OWNER_CLASS_MINIGL_CONTEXT,
                                0U, CARD_POOL_PAGE_BYTES, &other));
    address = memory->alloc_ex(context, ZZM_RUNTIME_OWNER_CLASS_MINIGL_CONTEXT,
                               CARD_POOL_PAGE_BYTES, 256U, 64U,
                               ZZM_MEMORY_ATTR_NONE);
    CHECK(address != 0U);
    CHECK(cache->dcache_invalidate_owned(rix, address, 2U));
    CHECK(cache->dcache_invalidate_owned(
        (11U << 24) | ZZM_MODULE_OWNER(4U), address, 2U));
    CHECK(cache_invalidate_calls == 2U);
    CHECK(!cache->dcache_invalidate_owned(other, address, 2U));
    CHECK(cache_invalidate_calls == 2U);
}

/*
 * Module malloc goes through memory->alloc.  GLQuake makes thousands of
 * small allocations (texture levels, objects); with one 64 KiB card-pool
 * block per call the card's CARD_POOL_MAX_BLOCKS ran out and every later
 * allocation failed.  Small allocations must share chunks, stay disjoint,
 * and give the chunks back.
 */
static void test_small_allocations_share_chunks(void)
{
    const zzm_services_t *services = zzm_runtime_services_v3();
    const zzm_reclaim_service_t *reclaim;
    const zzm_memory_service_t *memory;
    static uint32_t addresses[2000];
    uint32_t context;
    uint32_t i;
    int ok = 1;

    reset_resources();
    /* The allocator writes its block headers into card memory. */
    CHECK(mmap((void *)0x10000000UL, 0x04000000UL, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) ==
          (void *)0x10000000UL);
    reclaim = services->get_service(ZZM_SERVICE_RECLAIM, ZZM_SERVICE_RECLAIM_V3);
    memory = services->get_service(ZZM_SERVICE_MEMORY, ZZM_SERVICE_MEMORY_V3);
    CHECK(reclaim->create_owner(4U, ZZM_RUNTIME_OWNER_CLASS_MINIGL_CONTEXT,
                                0U, 32U << 20, &context));
    for (i = 0U; i < 2000U; ++i) {
        addresses[i] = memory->alloc(context, ZZM_MEMORY_CLASS_MINIGL_CONTEXT,
                                     32U << 20, 100U + (i % 7U) * 300U, 8U);
        if (!addresses[i]) {
            ok = 0;
            break;
        }
        memset((void *)(uintptr_t)addresses[i], (int)(i & 0xffU),
               100U + (i % 7U) * 300U);
    }
    CHECK(ok);
    CHECK(card_pool.block_count < 16U);
    for (i = 0U; ok && i < 2000U; ++i) {
        const uint8_t *bytes = (const uint8_t *)(uintptr_t)addresses[i];
        uint32_t n;

        CHECK((addresses[i] & 63U) == 0U);
        for (n = 0U; n < 100U + (i % 7U) * 300U; ++n)
            if (bytes[n] != (uint8_t)(i & 0xffU)) {
                CHECK(!"allocations overlap");
                ok = 0;
                break;
            }
    }
    for (i = 0U; ok && i < 2000U; i += 2U)
        CHECK(memory->free(addresses[i], context));
    for (i = 1U; ok && i < 2000U; i += 2U)
        CHECK(memory->free(addresses[i], context));
    CHECK(card_pool.block_count == 0U);
    /* Closing an owner must not leave chunk records that a new owner hits. */
    CHECK(memory->alloc(context, ZZM_MEMORY_CLASS_MINIGL_CONTEXT, 32U << 20,
                        64U, 8U) != 0U);
    CHECK(reclaim->close_owner(context) != 0U);
    CHECK(card_pool.block_count == 0U);
}

static void test_host_window_palette_and_app_guard(void)
{
    const zzm_services_t *services = zzm_runtime_services_v3();
    const zzm_reclaim_service_t *reclaim;
    const zzm_host_window_service_t *window;
    const zzm_palette_service_t *palette;
    const zzm_app_gl_host_service_t *app_host;
    zzm_host_window_t allocation;
    uint32_t owner;
    uint32_t snapshot[256];
    uint32_t generation;

    reset_resources();
    reclaim = services->get_service(ZZM_SERVICE_RECLAIM, ZZM_SERVICE_RECLAIM_V3);
    window = services->get_service(ZZM_SERVICE_HOST_WINDOW, ZZM_SERVICE_HOST_WINDOW_V3);
    palette = services->get_service(ZZM_SERVICE_PALETTE, ZZM_SERVICE_PALETTE_V3);
    app_host = services->get_service(ZZM_SERVICE_APP_GL_HOST,
                                     ZZM_SERVICE_APP_GL_HOST_V3);
    CHECK(reclaim->create_owner(4U, ZZM_RUNTIME_OWNER_CLASS_MINIGL_TRANSPORT,
                                0U, CARD_POOL_PAGE_BYTES, &owner));
    CHECK(window->alloc(owner, 0x91000U, 4096U, &allocation));
    CHECK((allocation.attributes & (ZZM_MEMORY_ATTR_HOST_VISIBLE |
                                    ZZM_MEMORY_ATTR_NONCACHEABLE)) ==
          (ZZM_MEMORY_ATTR_HOST_VISIBLE | ZZM_MEMORY_ATTR_NONCACHEABLE));
    CHECK(allocation.board_offset == allocation.arm_address - ADDR_ADJ);
    CHECK(window->free(owner, allocation.arm_address, allocation.generation));

    sdk_palette_set(3U, 0x00112233U);
    CHECK(palette->snapshot(snapshot, &generation));
    CHECK((generation & 1U) == 0U && snapshot[3] == 0x00112233U);
    CHECK(palette->lookup(3U) == snapshot[3]);

    app_host->api_enter();
    CHECK(app_in_api == 1U);
    app_host->api_leave();
    CHECK(app_in_api == 0U);
    CHECK(app_host->flip_bound(6U) != 0);
    CHECK(app_host->flip_bound(7U) == 0 && flip_calls == 1U);
}

static void test_log_rate_limit(void)
{
    const zzm_services_t *services = zzm_runtime_services_v3();
    const zzm_core_service_t *core;
    uint32_t i;

    reset_resources();
    core = services->get_service(ZZM_SERVICE_CORE, ZZM_SERVICE_CORE_V3);
    for (i = 0U; i < 40U; ++i)
        core->log_limited(4U, 0U, "noisy");
    CHECK(log_lines == 32U);
    now_us = 1000000U;
    core->log_limited(4U, 0U, "next");
    /* the "suppressed 8 logs" summary, then the new line */
    CHECK(log_lines == 34U);
}

void zzm_runtime_test_orphan_heap_lock(void);

static void test_orphaned_heap_lock_does_not_deadlock(void)
{
    reset_resources();
    zzm_runtime_test_orphan_heap_lock();
    zzm_runtime_reset_heap_lock();
    trap_held_lock = 1;
    held_lock_taken = 0;
    (void)zzm_runtime_release_module(1U);
    trap_held_lock = 0;
    CHECK(!held_lock_taken);
}

int main(void)
{
    test_typed_owner_quota_and_reclaim();
    test_small_allocations_share_chunks();
    test_owned_ops_cover_the_whole_module();
    test_host_window_palette_and_app_guard();
    test_log_rate_limit();
    test_orphaned_heap_lock_does_not_deadlock();
    if (failures)
        return 1;
    puts("runtime resource tests passed");
    return 0;
}
