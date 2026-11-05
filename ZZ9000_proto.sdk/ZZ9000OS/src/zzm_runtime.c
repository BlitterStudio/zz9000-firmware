#include <stdio.h>
#include <string.h>
#include "xil_cache.h"
#include "l2_maint.h"
#include "xtime_l.h"
#include "card_pool.h"
#include "memorymap.h"
#include "sdk_palette.h"
#include "surface_allocator.h"
#include "zzapp_runtime.h"
#include "zzapp_services.h"
#include "sdk_aperture_layout.h"
#include "zzm_registry.h"
#include "zzm_loader.h"

extern const void *zzm_minigl_host_get_service(uint32_t id,
                                                uint32_t min_version)
    __attribute__((weak));
#include "zzm_runtime.h"

extern int zzm_minigl_register_ops(uint32_t module_id, const void *ops,
                                   uint32_t ops_size) __attribute__((weak));
#ifdef MINIGL_HOST_TEST
extern int zzm_test_printf(const char *format, ...);
#define ZZM_RUNTIME_LOG zzm_test_printf
#else
#define ZZM_RUNTIME_LOG printf
#endif

__attribute__((weak)) uint32_t zzapp_services_flags(void)
{
    return 0U;
}

__attribute__((weak)) int zzapp_services_stop_requested(void)
{
    return 0;
}

__attribute__((weak)) void zzapp_host_yield_wait(void) {}

#define ZZM_RUNTIME_MAX_OWNERS 32U
#define ZZM_RUNTIME_MAX_WINDOWS 4U
#define ZZM_RUNTIME_MAX_COUNTERS 16U
/* Lines per module per second.  A RasterIX disable is one line plus a
 * five-line dump; at 8 the reason itself was the line that got dropped. */
#define ZZM_RUNTIME_LOG_BURST 32U

typedef struct {
    uint32_t owner;
    uint32_t module_id;
    uint32_t owner_class;
    uint32_t parent_owner;
    uint32_t quota;
    uint32_t refs;
} zzm_runtime_owner_t;

typedef struct {
    uint32_t owner;
    uint32_t address;
    uint32_t bytes;
    uint32_t generation;
} zzm_runtime_window_t;

typedef struct {
    uint32_t module_id;
    uint32_t id;
    const char *name;
    uint64_t value;
} zzm_runtime_counter_t;

typedef struct {
    uint32_t module_id;
    uint64_t window_start;
    uint32_t emitted;
    uint32_t suppressed;
} zzm_runtime_log_state_t;

static zzm_runtime_owner_t runtime_owners[ZZM_RUNTIME_MAX_OWNERS];
static zzm_runtime_window_t runtime_windows[ZZM_RUNTIME_MAX_WINDOWS];
static zzm_runtime_counter_t runtime_counters[ZZM_RUNTIME_MAX_COUNTERS];
static zzm_runtime_log_state_t runtime_logs[ZZM_RUNTIME_MAX_OWNERS];
static uint32_t runtime_owner_generation;
static uint32_t runtime_window_generation;

static uint32_t module_card_owner(uint32_t owner, uint32_t memory_class)
{
    uint32_t instance = owner & 0x00ffffffU;

    switch (memory_class) {
    case ZZM_MEMORY_CLASS_MEDIA:
        return CARD_POOL_OWNER(CARD_POOL_CLASS_MEDIA, instance);
    case ZZM_MEMORY_CLASS_MEDIA_INPUT:
        return CARD_POOL_OWNER(CARD_POOL_CLASS_MEDIA_INPUT, instance);
    case ZZM_MEMORY_CLASS_IMAGE:
        return CARD_POOL_OWNER(CARD_POOL_CLASS_IMAGE, instance);
    case ZZM_MEMORY_CLASS_MINIGL_MODULE:
        return CARD_POOL_OWNER(CARD_POOL_CLASS_MINIGL_MODULE, instance);
    case ZZM_MEMORY_CLASS_MINIGL_TRANSPORT:
        return CARD_POOL_OWNER(CARD_POOL_CLASS_MINIGL_TRANSPORT, instance);
    case ZZM_MEMORY_CLASS_MINIGL_CONTEXT:
        return CARD_POOL_OWNER(CARD_POOL_CLASS_MINIGL_CONTEXT, instance);
    case ZZM_MEMORY_CLASS_MINIGL_APP_CONTEXT:
        return CARD_POOL_OWNER(CARD_POOL_CLASS_MINIGL_APP_CONTEXT, instance);
    case ZZM_MEMORY_CLASS_MINIGL_RIX:
        return CARD_POOL_OWNER(CARD_POOL_CLASS_MINIGL_RIX, instance);
    default:
        return CARD_POOL_OWNER(CARD_POOL_CLASS_FIRMWARE, instance);
    }
}

static uint32_t owner_class_memory_class(uint32_t owner_class)
{
    switch (owner_class) {
    case ZZM_RUNTIME_OWNER_CLASS_MINIGL_MODULE:
        return ZZM_MEMORY_CLASS_MINIGL_MODULE;
    case ZZM_RUNTIME_OWNER_CLASS_MINIGL_TRANSPORT:
        return ZZM_MEMORY_CLASS_MINIGL_TRANSPORT;
    case ZZM_RUNTIME_OWNER_CLASS_MINIGL_CONTEXT:
        return ZZM_MEMORY_CLASS_MINIGL_CONTEXT;
    case ZZM_RUNTIME_OWNER_CLASS_MINIGL_APP_CONTEXT:
        return ZZM_MEMORY_CLASS_MINIGL_APP_CONTEXT;
    case ZZM_RUNTIME_OWNER_CLASS_MINIGL_RIX:
        return ZZM_MEMORY_CLASS_MINIGL_RIX;
    default:
        return 0U;
    }
}

static zzm_runtime_owner_t *find_runtime_owner(uint32_t owner)
{
    uint32_t i;

    for (i = 0U; i < ZZM_RUNTIME_MAX_OWNERS; ++i) {
        if (runtime_owners[i].owner == owner)
            return &runtime_owners[i];
    }
    return 0;
}

static uint32_t minigl_used(void)
{
    uint32_t i;
    uint32_t used = 0U;

    for (i = CARD_POOL_CLASS_MINIGL_MODULE;
         i <= CARD_POOL_CLASS_MINIGL_RIX; ++i)
        used += card_pool.class_used[i];
    return used;
}

static uint32_t module_owner_used(uint32_t owner)
{
    uint32_t used = 0U;
    uint32_t i;
    for (i = 0; i < card_pool.block_count; i++) {
        if (card_pool.blocks[i].owner == owner)
            used += card_pool.blocks[i].size;
    }
    return used;
}

/*
 * Module malloc (libzzm_rt) reaches the card pool through alloc/free.  The
 * pool hands out 64 KiB pages and holds only CARD_POOL_MAX_BLOCKS blocks for
 * the whole card, so one block per malloc let MiniGL's texture levels and
 * small objects exhaust it after a few hundred allocations: every later
 * allocation failed (GLQuake rendered untextured).  Small requests are
 * carved from 1 MiB chunks owned by the same card owner.  Blocks start and
 * end on 64-byte boundaries with a 64-byte header of their own, so no cache
 * line is shared between two allocations or with allocator metadata.
 */
#define ZZM_HEAP_CHUNK_BYTES  0x00100000U
#define ZZM_HEAP_DIRECT_BYTES 0x00020000U
#define ZZM_HEAP_MAX_CHUNKS   128U
#define ZZM_HEAP_GRANULE      64U
#define ZZM_HEAP_NONE         0xffffffffU
#define ZZM_HEAP_USED_MAGIC   0x5a484155U /* 'ZHAU' */
#define ZZM_HEAP_FREE_MAGIC   0x5a484146U /* 'ZHAF' */

typedef struct {
    uint32_t magic;
    uint32_t bytes; /* whole block including this header */
    uint32_t next;  /* free list: offset of the next free block */
    uint32_t pad[13];
} zzm_heap_block_t;

typedef struct {
    uint32_t card_owner;
    uint32_t address;
    uint32_t bytes;
    uint32_t free_head;
    uint32_t used;
} zzm_heap_chunk_t;

static zzm_heap_chunk_t heap_chunks[ZZM_HEAP_MAX_CHUNKS];
static sdk_smp_lock_t heap_lock = SDK_SMP_LOCK_INIT;

static zzm_heap_block_t *heap_block(const zzm_heap_chunk_t *chunk,
                                    uint32_t offset)
{
    return (zzm_heap_block_t *)(uintptr_t)(chunk->address + offset);
}

/* Caller holds heap_lock.  Drops the records of chunks the pool no longer
 * lists for this owner (an owner close or reclaim freed them). */
static void heap_forget_owner_locked(uint32_t card_owner)
{
    uint32_t i;

    for (i = 0U; i < ZZM_HEAP_MAX_CHUNKS; ++i)
        if (heap_chunks[i].card_owner == card_owner)
            memset(&heap_chunks[i], 0, sizeof(heap_chunks[i]));
}

static void heap_forget_owner(uint32_t card_owner)
{
    sdk_smp_lock_acquire(&heap_lock);
    heap_forget_owner_locked(card_owner);
    sdk_smp_lock_release(&heap_lock);
}

static uint32_t heap_take(zzm_heap_chunk_t *chunk, uint32_t need)
{
    uint32_t *link = &chunk->free_head;
    uint32_t offset;

    for (offset = chunk->free_head; offset != ZZM_HEAP_NONE;
         offset = heap_block(chunk, offset)->next) {
        zzm_heap_block_t *block = heap_block(chunk, offset);

        if (block->magic != ZZM_HEAP_FREE_MAGIC)
            return 0U;
        if (block->bytes < need) {
            link = &block->next;
            continue;
        }
        if (block->bytes - need >= 2U * ZZM_HEAP_GRANULE) {
            zzm_heap_block_t *rest = heap_block(chunk, offset + need);

            rest->magic = ZZM_HEAP_FREE_MAGIC;
            rest->bytes = block->bytes - need;
            rest->next = block->next;
            *link = offset + need;
            block->bytes = need;
        } else {
            *link = block->next;
        }
        block->magic = ZZM_HEAP_USED_MAGIC;
        block->next = ZZM_HEAP_NONE;
        chunk->used += block->bytes;
        return chunk->address + offset + (uint32_t)sizeof(*block);
    }
    return 0U;
}

static uint32_t heap_alloc(uint32_t card_owner, uint32_t quota, uint32_t bytes)
{
    uint32_t need = (bytes + (uint32_t)sizeof(zzm_heap_block_t) +
                     ZZM_HEAP_GRANULE - 1U) & ~(ZZM_HEAP_GRANULE - 1U);
    uint32_t address = 0U;
    uint32_t i;

    sdk_smp_lock_acquire(&heap_lock);
    for (i = 0U; i < ZZM_HEAP_MAX_CHUNKS && !address; ++i) {
        zzm_heap_chunk_t *chunk = &heap_chunks[i];

        if (chunk->card_owner != card_owner)
            continue;
        if (!card_pool_block_owned(&card_pool, chunk->address, chunk->bytes,
                                   card_owner)) {
            heap_forget_owner_locked(card_owner);
            break;
        }
        address = heap_take(chunk, need);
    }
    if (!address) {
        uint32_t used = module_owner_used(card_owner);
        uint32_t chunk_bytes = ZZM_HEAP_CHUNK_BYTES;

        /* An owner with a quota below one chunk gets a smaller chunk. */
        if (used > quota)
            chunk_bytes = 0U;
        else if (quota - used < chunk_bytes)
            chunk_bytes = (quota - used) & ~(CARD_POOL_PAGE_BYTES - 1U);
        for (i = 0U; i < ZZM_HEAP_MAX_CHUNKS; ++i)
            if (heap_chunks[i].card_owner == 0U)
                break;
        if (chunk_bytes >= need && i < ZZM_HEAP_MAX_CHUNKS) {
            uint32_t base = card_pool_alloc(&card_pool, chunk_bytes,
                                            card_owner);

            if (base) {
                zzm_heap_chunk_t *chunk = &heap_chunks[i];
                zzm_heap_block_t *block = (zzm_heap_block_t *)(uintptr_t)base;

                block->magic = ZZM_HEAP_FREE_MAGIC;
                block->bytes = chunk_bytes;
                block->next = ZZM_HEAP_NONE;
                chunk->card_owner = card_owner;
                chunk->address = base;
                chunk->bytes = chunk_bytes;
                chunk->free_head = 0U;
                chunk->used = 0U;
                address = heap_take(chunk, need);
            }
        }
    }
    sdk_smp_lock_release(&heap_lock);
    return address;
}

/* 1: freed from a chunk, 0: not a chunk allocation of this owner. */
static int heap_free(uint32_t address, uint32_t owner)
{
    uint32_t i;
    int freed = 0;

    sdk_smp_lock_acquire(&heap_lock);
    for (i = 0U; i < ZZM_HEAP_MAX_CHUNKS; ++i) {
        zzm_heap_chunk_t *chunk = &heap_chunks[i];
        zzm_heap_block_t *block;
        uint32_t offset;
        uint32_t *link;
        uint32_t previous = ZZM_HEAP_NONE;

        if (chunk->card_owner == 0U ||
            (chunk->card_owner & 0x00ffffffU) != (owner & 0x00ffffffU) ||
            address < chunk->address + sizeof(zzm_heap_block_t) ||
            address >= chunk->address + chunk->bytes)
            continue;
        if (!card_pool_block_owned(&card_pool, chunk->address, chunk->bytes,
                                   chunk->card_owner)) {
            heap_forget_owner_locked(chunk->card_owner);
            break;
        }
        offset = address - chunk->address - (uint32_t)sizeof(*block);
        block = heap_block(chunk, offset);
        if ((offset & (ZZM_HEAP_GRANULE - 1U)) != 0U ||
            block->magic != ZZM_HEAP_USED_MAGIC)
            break;
        chunk->used -= block->bytes;
        block->magic = ZZM_HEAP_FREE_MAGIC;
        link = &chunk->free_head;
        while (*link != ZZM_HEAP_NONE && *link < offset) {
            previous = *link;
            link = &heap_block(chunk, previous)->next;
        }
        block->next = *link;
        *link = offset;
        if (block->next != ZZM_HEAP_NONE &&
            offset + block->bytes == block->next) {
            zzm_heap_block_t *next = heap_block(chunk, block->next);

            block->bytes += next->bytes;
            block->next = next->next;
        }
        if (previous != ZZM_HEAP_NONE) {
            zzm_heap_block_t *prior = heap_block(chunk, previous);

            if (previous + prior->bytes == offset) {
                prior->bytes += block->bytes;
                prior->next = block->next;
            }
        }
        if (chunk->used == 0U) {
            (void)card_pool_free(&card_pool, chunk->address, chunk->card_owner);
            memset(chunk, 0, sizeof(*chunk));
        }
        freed = 1;
        break;
    }
    sdk_smp_lock_release(&heap_lock);
    return freed;
}

static uint32_t module_card_alloc(uint32_t owner, uint32_t memory_class,
                                  uint32_t quota, uint32_t bytes,
                                  uint32_t alignment)
{
    uint32_t card_owner = module_card_owner(owner, memory_class);
    (void)alignment; /* libzzm_rt aligns inside its over-allocation. */
    if (!bytes || !quota || bytes > quota)
        return 0U;
    if (bytes < ZZM_HEAP_DIRECT_BYTES)
        return heap_alloc(card_owner, quota, bytes);
    if (module_owner_used(card_owner) > quota - bytes)
        return 0U;
    return card_pool_alloc(&card_pool, bytes, card_owner);
}

static int module_card_free(uint32_t address, uint32_t owner)
{
    if (heap_free(address, owner))
        return 1;
    for (uint32_t i = 0; i < card_pool.block_count; i++) {
        if (card_pool.blocks[i].addr == address &&
            (card_pool.blocks[i].owner & 0x00ffffffU) ==
                (owner & 0x00ffffffU))
            return card_pool_free(&card_pool, address, card_pool.blocks[i].owner);
    }
    return 0;
}

static uint32_t module_card_realloc(uint32_t address, uint32_t owner,
                                    uint32_t bytes, uint32_t alignment)
{
    (void)address;
    (void)owner;
    (void)bytes;
    (void)alignment;
    return 0U; /* libzzm_rt preserves blocks with allocate/copy/free. */
}

static int module_create_owner(uint32_t module_id, uint32_t owner_class,
                               uint32_t parent_owner, uint32_t quota,
                               uint32_t *owner_out)
{
    zzm_runtime_owner_t *parent;
    uint32_t i;
    uint32_t memory_class = owner_class_memory_class(owner_class);
    uint32_t instance;

    if (!owner_out || !memory_class || quota == 0U)
        return 0;
    if (parent_owner != 0U) {
        parent = find_runtime_owner(parent_owner);
        if (!parent || parent->module_id != module_id)
            return 0;
    }
    for (i = 0U; i < ZZM_RUNTIME_MAX_OWNERS; ++i) {
        if (runtime_owners[i].owner == 0U)
            break;
    }
    if (i == ZZM_RUNTIME_MAX_OWNERS)
        return 0;
    instance = ++runtime_owner_generation & 0x00ffffffU;
    if (instance == 0U)
        instance = ++runtime_owner_generation & 0x00ffffffU;
    runtime_owners[i].owner = module_card_owner(instance, memory_class);
    runtime_owners[i].module_id = module_id;
    runtime_owners[i].owner_class = owner_class;
    runtime_owners[i].parent_owner = parent_owner;
    runtime_owners[i].quota = quota;
    runtime_owners[i].refs = 1U;
    *owner_out = runtime_owners[i].owner;
    return 1;
}

static uint32_t module_alloc_ex(uint32_t owner, uint32_t owner_class,
                                uint32_t quota, uint32_t bytes,
                                uint32_t alignment, uint32_t attributes)
{
    zzm_runtime_owner_t *record = find_runtime_owner(owner);
    uint32_t memory_class = owner_class_memory_class(owner_class);
    uint32_t used;

    if (!record || !memory_class || record->owner_class != owner_class ||
        record->quota != quota || attributes != ZZM_MEMORY_ATTR_NONE)
        return 0U;
    used = module_owner_used(owner);
    if (!bytes || bytes > quota || used > quota - bytes)
        return 0U;
    if (minigl_used() > CARD_POOL_LIMIT_MINIGL - bytes)
        return 0U;
    (void)alignment;
    return card_pool_alloc(&card_pool, bytes, owner);
}

static uint32_t module_realloc_ex(uint32_t address, uint32_t owner,
                                  uint32_t owner_class, uint32_t quota,
                                  uint32_t bytes, uint32_t alignment,
                                  uint32_t attributes)
{
    (void)address;
    (void)owner;
    (void)owner_class;
    (void)quota;
    (void)bytes;
    (void)alignment;
    (void)attributes;
    return 0U;
}

static uint32_t module_quota_used(uint32_t owner, uint32_t owner_class)
{
    zzm_runtime_owner_t *record = find_runtime_owner(owner);

    if (!record || record->owner_class != owner_class)
        return 0U;
    return module_owner_used(owner);
}

static uint32_t module_close_owner(uint32_t owner)
{
    uint32_t i;
    uint32_t released = 0U;

    for (i = 0U; i < ZZM_RUNTIME_MAX_OWNERS; ++i) {
        if (runtime_owners[i].parent_owner == owner)
            released += module_close_owner(runtime_owners[i].owner);
    }
    for (i = 0U; i < ZZM_RUNTIME_MAX_WINDOWS; ++i) {
        if (runtime_windows[i].owner == owner) {
            (void)surface_allocator_free(runtime_windows[i].address);
            memset(&runtime_windows[i], 0, sizeof(runtime_windows[i]));
        }
    }
    heap_forget_owner(owner);
    released += card_pool_release_owner(&card_pool, owner);
    for (i = 0U; i < ZZM_RUNTIME_MAX_OWNERS; ++i) {
        if (runtime_owners[i].owner == owner) {
            memset(&runtime_owners[i], 0, sizeof(runtime_owners[i]));
            break;
        }
    }
    return released;
}

static uint32_t module_owner_refcount(uint32_t owner)
{
    zzm_runtime_owner_t *record = find_runtime_owner(owner);
    return record ? record->refs : 0U;
}

#if !defined(MINIGL_HOST_TEST)
static int module_p96_span(uint32_t address, uint32_t length)
{
    return address >= Z3_P96_VRAM_ADDRESS &&
           address < Z3_P96_VRAM_END &&
           length <= Z3_P96_VRAM_END - address;
}
#endif

static void module_dcache_clean(uint32_t address, uint32_t length)
{
#if !defined(MINIGL_HOST_TEST)
    if (module_p96_span(address, length)) {
        Xil_L1DCacheFlushRange(address, length);
        return;
    }
#endif
    Xil_DCacheFlushRange(address, length);
}

static void module_icache_invalidate(uint32_t address, uint32_t length)
{
    Xil_ICacheInvalidateRange(address, length);
}

static int range_owned_exact(uint32_t owner, uint32_t address, uint32_t length)
{
    uint32_t i;

    if (card_pool_range_owned(&card_pool, address, length, owner))
        return 1;
    for (i = 0U; i < ZZM_RUNTIME_MAX_WINDOWS; ++i) {
        uint32_t offset;

        if (runtime_windows[i].owner != owner || address < runtime_windows[i].address)
            continue;
        offset = address - runtime_windows[i].address;
        if (offset <= runtime_windows[i].bytes &&
            length <= runtime_windows[i].bytes - offset)
            return 1;
    }
    return 0;
}

static uint32_t owner_module_id(uint32_t owner)
{
    uint32_t instance = owner & 0x00ffffffU;
    uint32_t i;

    if ((instance & ZZM_MODULE_OWNER_TAG) != 0U)
        return instance & (ZZM_MODULE_OWNER_TAG - 1U);
    for (i = 0U; i < ZZM_RUNTIME_MAX_OWNERS; ++i) {
        if (runtime_owners[i].owner != 0U &&
            (runtime_owners[i].owner & 0x00ffffffU) == instance)
            return runtime_owners[i].module_id;
    }
    return 0U;
}

/*
 * libzzm_rt derives the owner of a cache call from the module's current
 * allocation context, which need not be the owner the buffer was allocated
 * under (MiniGL waits on RasterIX fences in command buffers owned by a
 * context while another context, the module or an app owner is selected).
 * A failed check makes the call a silent no-op, and a skipped fence
 * invalidate reads a stale value until RasterIX is declared hung.  The
 * guard exists to keep a module off memory it does not own, so accept any
 * range owned by the same module.
 */
static int module_range_owned(uint32_t owner, uint32_t address, uint32_t length)
{
    static const uint32_t base_classes[] = {
        ZZM_MEMORY_CLASS_MEDIA,
        ZZM_MEMORY_CLASS_MEDIA_INPUT,
        ZZM_MEMORY_CLASS_IMAGE,
        ZZM_MEMORY_CLASS_GENERAL,
        ZZM_MEMORY_CLASS_MINIGL_MODULE,
    };
    uint32_t module_id;
    uint32_t base;
    uint32_t i;

    if (range_owned_exact(owner, address, length))
        return 1;
    module_id = owner_module_id(owner);
    if (module_id == 0U)
        return 0;
    for (i = 0U; i < ZZM_RUNTIME_MAX_OWNERS; ++i) {
        if (runtime_owners[i].owner != 0U &&
            runtime_owners[i].module_id == module_id &&
            range_owned_exact(runtime_owners[i].owner, address, length))
            return 1;
    }
    base = ZZM_MODULE_OWNER(module_id);
    for (i = 0U; i < sizeof(base_classes) / sizeof(base_classes[0]); ++i) {
        if (range_owned_exact(module_card_owner(base, base_classes[i]),
                              address, length))
            return 1;
    }
    return 0;
}

static int module_dcache_clean_owned(uint32_t owner, uint32_t address,
                                     uint32_t length)
{
    if (!module_range_owned(owner, address, length))
        return 0;
#if !defined(MINIGL_HOST_TEST)
    if (module_p96_span(address, length))
        Xil_L1DCacheFlushRange(address, length);
    else
#endif
        Xil_DCacheFlushRange(address, length);
    return 1;
}

static int module_dcache_invalidate_owned(uint32_t owner, uint32_t address,
                                          uint32_t length)
{
    if (!module_range_owned(owner, address, length))
        return 0;
#if !defined(MINIGL_HOST_TEST)
    if (module_p96_span(address, length))
        Xil_L1DCacheInvalidateRange(address, length);
    else
#endif
        Xil_DCacheInvalidateRange(address, length);
    return 1;
}

static int module_instruction_sync_owned(uint32_t owner, uint32_t address,
                                         uint32_t length)
{
    if (!module_range_owned(owner, address, length))
        return 0;
    Xil_DCacheFlushRange(address, length);
    Xil_ICacheInvalidateRange(address, length);
    return 1;
}

static void module_log(const char *message)
{
    ZZM_RUNTIME_LOG("[ZZM] %s\n", message ? message : "");
}

static void module_fault(const char *reason)
{
    ZZM_RUNTIME_LOG("[ZZM] module fault: %s\n", reason ? reason : "");
}

static void module_fault_by_id(uint32_t module_id, const char *reason)
{
    module_fault(reason);
    zzm_loader_mark_faulted(module_id, reason);
}

static uint64_t module_time_us(void)
{
    XTime now;
    XTime_GetTime(&now);
    return (uint64_t)now / (COUNTS_PER_SECOND / 1000000U);
}

static void module_log_limited(uint32_t module_id, uint32_t level,
                               const char *message)
{
    zzm_runtime_log_state_t *state = 0;
    uint64_t now = module_time_us();
    uint32_t i;

    (void)level;
    for (i = 0U; i < ZZM_RUNTIME_MAX_OWNERS; ++i) {
        if (runtime_logs[i].module_id == module_id ||
            runtime_logs[i].module_id == 0U) {
            state = &runtime_logs[i];
            break;
        }
    }
    if (!state)
        return;
    if (state->module_id == 0U)
        state->module_id = module_id;
    if (now - state->window_start >= 1000000U) {
        if (state->suppressed != 0U)
            ZZM_RUNTIME_LOG("[ZZM] module %lu: suppressed %lu logs\n",
                   (unsigned long)module_id, (unsigned long)state->suppressed);
        state->window_start = now;
        state->emitted = 0U;
        state->suppressed = 0U;
    }
    if (state->emitted >= ZZM_RUNTIME_LOG_BURST) {
        state->suppressed++;
        return;
    }
    state->emitted++;
    module_log(message);
}

static int module_host_window_alloc(uint32_t owner, uint32_t bytes,
                                    uint32_t alignment, zzm_host_window_t *out)
{
    uint32_t i;
    uint32_t address;

    if (!out || !find_runtime_owner(owner) ||
        !sdk_aperture_runtime_is_zorro3() || bytes == 0U)
        return 0;
    for (i = 0U; i < ZZM_RUNTIME_MAX_WINDOWS; ++i) {
        if (runtime_windows[i].owner == 0U)
            break;
    }
    if (i == ZZM_RUNTIME_MAX_WINDOWS)
        return 0;
    address = surface_allocator_alloc_range(bytes, alignment,
                                            SDK_MAILBOX_Z3_ADDRESS,
                                            HOST_VISIBLE_HEAP_END);
    if (address == 0U)
        return 0;
    runtime_windows[i].owner = owner;
    runtime_windows[i].address = address;
    runtime_windows[i].bytes = surface_allocator_block_size(address);
    runtime_windows[i].generation = ++runtime_window_generation;
    out->arm_address = address;
    out->board_offset = address - ADDR_ADJ;
    out->bytes = runtime_windows[i].bytes;
    out->attributes = ZZM_MEMORY_ATTR_HOST_VISIBLE |
                      ZZM_MEMORY_ATTR_NONCACHEABLE;
    out->generation = runtime_windows[i].generation;
#if !defined(MINIGL_HOST_TEST)
    /* The module writes the window non-cacheable, but earlier 68k reads
     * through the ACP may have left PL310 lines for this range; the 68k
     * would keep reading those instead of the new control block. */
    l2_maint_l2_invalidate_range((intptr_t)address, runtime_windows[i].bytes);
#endif
    return 1;
}

static int module_host_window_free(uint32_t owner, uint32_t address,
                                   uint32_t generation)
{
    uint32_t i;

    for (i = 0U; i < ZZM_RUNTIME_MAX_WINDOWS; ++i) {
        if (runtime_windows[i].owner == owner &&
            runtime_windows[i].address == address &&
            runtime_windows[i].generation == generation) {
            if (surface_allocator_free(address) != 0)
                return 0;
            memset(&runtime_windows[i], 0, sizeof(runtime_windows[i]));
            return 1;
        }
    }
    return 0;
}

static uint32_t module_palette_generation(void)
{
    return sdk_palette_generation();
}

static int module_palette_snapshot(uint32_t entries[256], uint32_t *generation)
{
    return sdk_palette_snapshot(entries, generation);
}

static uint32_t module_palette_lookup(uint32_t index)
{
    return sdk_palette_get(index);
}

static int module_app_flip_bound(uint32_t generation)
{
    if (generation != zzapp_current_generation())
        return -1;
    return zzapp_svc_display_flip_async(1U);
}

static int module_app_flip_idle(uint32_t generation)
{
    return generation == zzapp_current_generation() &&
           zzapp_display_flip_idle();
}

static int module_profile_register(uint32_t module_id, const char *name,
                                   uint32_t *counter_out)
{
    uint32_t i;

    if (!counter_out || !name)
        return 0;
    for (i = 0U; i < ZZM_RUNTIME_MAX_COUNTERS; ++i) {
        if (runtime_counters[i].name == 0) {
            runtime_counters[i].module_id = module_id;
            runtime_counters[i].id = i + 1U;
            runtime_counters[i].name = name;
            *counter_out = runtime_counters[i].id;
            return 1;
        }
    }
    return 0;
}

static int module_profile_sample(uint32_t counter, uint64_t *value_out)
{
    if (!value_out || counter == 0U || counter > ZZM_RUNTIME_MAX_COUNTERS ||
        runtime_counters[counter - 1U].name == 0)
        return 0;
    *value_out = runtime_counters[counter - 1U].value;
    return 1;
}

static int module_profile_snapshot(uint32_t module_id, void *out,
                                   uint32_t *bytes_inout)
{
    uint32_t i;
    uint32_t count = 0U;
    uint32_t bytes;

    if (!bytes_inout)
        return 0;
    for (i = 0U; i < ZZM_RUNTIME_MAX_COUNTERS; ++i) {
        if (runtime_counters[i].module_id == module_id)
            ++count;
    }
    bytes = count * sizeof(zzm_runtime_counter_t);
    if (!out || *bytes_inout < bytes) {
        *bytes_inout = bytes;
        return 0;
    }
    count = 0U;
    for (i = 0U; i < ZZM_RUNTIME_MAX_COUNTERS; ++i) {
        if (runtime_counters[i].module_id == module_id)
            ((zzm_runtime_counter_t *)out)[count++] = runtime_counters[i];
    }
    *bytes_inout = bytes;
    return 1;
}

static uint32_t module_release_owner(uint32_t owner)
{
    heap_forget_owner(owner);
    return card_pool_release_owner(&card_pool, owner);
}

uint32_t zzm_runtime_release_module(uint32_t module_id)
{
    static const uint32_t classes[] = {
        ZZM_MEMORY_CLASS_MEDIA,
        ZZM_MEMORY_CLASS_MEDIA_INPUT,
        ZZM_MEMORY_CLASS_IMAGE,
        ZZM_MEMORY_CLASS_GENERAL,
    };
    uint32_t released = 0U;
    uint32_t owner = ZZM_MODULE_OWNER(module_id);
    uint32_t i;

    for (i = 0U; i < ZZM_RUNTIME_MAX_OWNERS; ++i) {
        if (runtime_owners[i].module_id == module_id &&
            runtime_owners[i].parent_owner == 0U)
            released += module_close_owner(runtime_owners[i].owner);
    }
    for (i = 0U; i < sizeof(classes) / sizeof(classes[0]); i++)
        released += module_release_owner(module_card_owner(owner, classes[i]));
    if (released)
        ZZM_RUNTIME_LOG("[ZZM] module %lu reclaimed %lu bytes\n",
               (unsigned long)module_id, (unsigned long)released);
    return released;
}
int zzm_runtime_quota_available(uint32_t memory_class, uint32_t quota)
{
    uint32_t owner = module_card_owner(0U, memory_class);
    uint32_t class_id = CARD_POOL_OWNER_CLASS(owner);
    uint32_t free_bytes = card_pool_range_free_bytes(
        &card_pool, CARD_POOL_RANGE_PERMANENT) +
        card_pool_range_free_bytes(&card_pool, CARD_POOL_RANGE_REVOCABLE);

    if (quota == 0U || class_id >= CARD_POOL_CLASS_COUNT ||
        quota > card_pool.class_limit[class_id] ||
        card_pool.class_used[class_id] >
            card_pool.class_limit[class_id] - quota ||
        quota > free_bytes)
        return 0;
    if (memory_class >= ZZM_MEMORY_CLASS_MINIGL_MODULE &&
        memory_class <= ZZM_MEMORY_CLASS_MINIGL_RIX &&
        (minigl_used() > CARD_POOL_LIMIT_MINIGL ||
         quota > CARD_POOL_LIMIT_MINIGL - minigl_used()))
        return 0;
    return 1;
}
static int module_register_ops(uint32_t module_id, const void *ops,
                               uint32_t ops_size)
{
    if (!zzm_registry_register_ops(module_id, ops, ops_size))
        return 0;
    if (module_id == ZZM_MODULE_MINIGL && zzm_minigl_register_ops &&
        !zzm_minigl_register_ops(module_id, ops, ops_size))
        return 0;
    return 1;
}

static int module_register_service(uint32_t module_id, uint32_t id,
                                   uint32_t version, const void *interface,
                                   uint32_t interface_size)
{
    return zzm_registry_register_service(module_id, id, version, interface,
                                         interface_size);
}

static const zzm_core_service_t core_service = {
    ZZM_SERVICE_CORE_V3, sizeof(core_service), module_log, module_fault,
    module_time_us, module_fault_by_id, module_log_limited
};
static const zzm_memory_service_t memory_service = {
    ZZM_SERVICE_MEMORY_V3, sizeof(memory_service), module_card_alloc,
    module_card_free, module_card_realloc, module_alloc_ex, module_realloc_ex,
    module_quota_used
};
static const zzm_cache_service_t cache_service = {
    ZZM_SERVICE_CACHE_V3, sizeof(cache_service), module_dcache_clean,
    module_icache_invalidate, module_dcache_clean_owned,
    module_dcache_invalidate_owned, module_instruction_sync_owned
};
static const zzm_reclaim_service_t reclaim_service = {
    ZZM_SERVICE_RECLAIM_V3, sizeof(reclaim_service), module_release_owner,
    module_create_owner, module_close_owner, module_close_owner,
    module_owner_refcount
};
static const zzm_host_window_service_t host_window_service = {
    ZZM_SERVICE_HOST_WINDOW_V3, sizeof(host_window_service),
    module_host_window_alloc, module_host_window_free
};
static const zzm_palette_service_t palette_service = {
    ZZM_SERVICE_PALETTE_V3, sizeof(palette_service), module_palette_generation,
    module_palette_snapshot, module_palette_lookup
};
static const zzm_app_gl_host_service_t app_gl_host_service = {
    ZZM_SERVICE_APP_GL_HOST_V3, sizeof(app_gl_host_service), zzapp_api_enter,
    zzapp_api_leave, zzapp_display_bound, module_app_flip_bound,
    module_app_flip_idle, zzapp_current_generation, zzapp_display_layout,
    zzapp_services_flags, zzapp_services_stop_requested, zzapp_host_yield_wait
};
static const zzm_profile_service_t profile_service = {
    ZZM_SERVICE_PROFILE_V3, sizeof(profile_service), module_profile_register,
    module_time_us, module_profile_sample, module_profile_snapshot
};

static int module_service_compatible(uint32_t provided, uint32_t required)
{
    return required == 1U ||
        ZZM_SERVICE_VERSION_COMPATIBLE(provided, required);
}

static const void *module_get_service(uint32_t id, uint32_t min_version)
{
    switch (id) {
    case ZZM_SERVICE_CORE:
        return module_service_compatible(core_service.version, min_version) ?
            &core_service : 0;
    case ZZM_SERVICE_MEMORY:
        return module_service_compatible(memory_service.version, min_version) ?
            &memory_service : 0;
    case ZZM_SERVICE_CACHE:
        return module_service_compatible(cache_service.version, min_version) ?
            &cache_service : 0;
    case ZZM_SERVICE_RECLAIM:
        return module_service_compatible(reclaim_service.version, min_version) ?
            &reclaim_service : 0;
    case ZZM_SERVICE_HOST_WINDOW:
        return module_service_compatible(host_window_service.version, min_version) ?
            &host_window_service : 0;
    case ZZM_SERVICE_PALETTE:
        return module_service_compatible(palette_service.version, min_version) ?
            &palette_service : 0;
    case ZZM_SERVICE_APP_GL_HOST:
        return module_service_compatible(app_gl_host_service.version, min_version) ?
            &app_gl_host_service : 0;
    case ZZM_SERVICE_PROFILE:
        return module_service_compatible(profile_service.version, min_version) ?
            &profile_service : 0;
    default:
        return zzm_minigl_host_get_service ?
            zzm_minigl_host_get_service(id, min_version) : 0;
    }
}

static const zzm_services_t services_v2 = {
    ZZM_MODULE_ABI_VERSION, sizeof(services_v2), module_get_service,
    module_register_ops, module_register_service
};
static const zzm_services_t services_v3 = {
    ZZM_V3_ABI_MAJOR, sizeof(services_v3), module_get_service,
    module_register_ops, module_register_service
};

const zzm_services_t *zzm_runtime_services(void) { return &services_v2; }
const zzm_services_t *zzm_runtime_services_v3(void) { return &services_v3; }

int zzm_runtime_register_ops(uint32_t module_id, const void *ops,
                             uint32_t ops_size)
{
    return module_register_ops(module_id, ops, ops_size);
}

void zzm_runtime_module_fault(uint32_t module_id, const char *reason)
{
    module_fault_by_id(module_id, reason);
}
