/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The app table (AAR API 0.5) as zzapp_api.c builds it: header, aar_info and
 * its size-first fill rule, heap_info, shared buffers (granule rounding, the
 * board-relative token, the live-block and limit rules), publish and acquire
 * range checks, the ZZ9000 extension table, and the 64-bit counts that do not
 * fit the firmware's 32-bit services.
 */
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "aar/app.h"
#include "aar/ext_zz9000.h"
#include "memorymap.h"
#include "zzapp_rec.h"
#include "zzapp_xport.h"
#include "zzapp_runtime.h"

/* A shared block's token is its card address minus ADDR_ADJ: the ARM address
 * of the framebuffer window, 0x200000, is board offset 0x10000. */
#define BOARD_ADJUST 0x001f0000u
_Static_assert(ADDR_ADJ == BOARD_ADJUST, "board offset of a card address");

#define MIB (1u << 20)
#define GRANULE 4096u
#define DEFAULT_SHARED_LIMIT (16u * MIB)
#define BLOCK_LIMIT 64u

/* The surface allocator's stand-in: hands out card addresses from a fixed
 * range and records what the firmware asked for. */
static uint32_t card_next = 0x03400000u;
static uint32_t card_serial;
static uint32_t card_allocs;
static uint32_t card_frees;
static uint32_t card_last;
static uint32_t card_last_bytes;
static uint32_t card_last_align;
static uint32_t card_last_freed;

uint32_t zzapp_test_card_alloc(uint32_t bytes, uint32_t align,
                               uint32_t *serial)
{
    uint32_t card;

    if (align < GRANULE)
        align = GRANULE;
    card = (card_next + align - 1u) & ~(align - 1u);
    card_next = card + ((bytes + GRANULE - 1u) & ~(GRANULE - 1u));
    card_allocs++;
    card_last = card;
    card_last_bytes = bytes;
    card_last_align = align;
    *serial = ++card_serial;
    return card;
}

void zzapp_test_card_free(uint32_t card, uint32_t serial)
{
    (void)serial;
    card_frees++;
    card_last_freed = card;
}

static const aar_api *api;

/* The pair of the admitted record: shared_used() reports the app's own
 * blocks beyond it. */
#define PAIR_BYTES ((uint64_t)ZZAPP_XPORT_PAIR_ALLOC)

static uint64_t shared_used(void)
{
    aar_info info;

    memset(&info, 0xa5, sizeof(info));
    info.size = sizeof(info);
    assert(api->info(&info) == AAR_OK);
    assert(info.shared_used >= PAIR_BYTES);
    return info.shared_used - PAIR_BYTES;
}

/* Every case starts on a fresh record arena with one admitted record (the
 * app under test). The previous case's record is released first, once there
 * is one. */
static void reset(void)
{
    static int booted;

    if (booted)
        zzapp_loader_release();
    booted = 1;
    zzapp_boot_init();
    assert(zzapp_test_admit() != ZZAPP_PID_NONE);
    assert(zzapp_loader_begin() == AAR_OK);
    api = zzapp_api_table();
    zzapp_test_set_groups(0u, 0);
}

static void test_header(void)
{
    static const uint32_t groups_cases[] = {
        AAR_GROUP_INPUT | AAR_GROUP_SERVICES,
        AAR_GROUPS_DEFINED,
        AAR_GROUP_SERVICES,
    };
    unsigned i;

    reset();
    assert(api->size == sizeof(aar_api));
    assert(api->version == AAR_API_VERSION);
    assert(api->version == ((0u << 16) | 5u));
    assert(api->platform_id == AAR_PLATFORM_ZZ9000);
    assert(AAR_API_AT_LEAST(api, 5u));
    assert(api->groups == zzapp_runtime_groups());
    assert((api->groups & ~AAR_GROUPS_DEFINED) == 0u);

    /* The table follows the runtime's group mask. */
    for (i = 0u; i < sizeof(groups_cases) / sizeof(groups_cases[0]); i++) {
        aar_info info;

        zzapp_test_set_groups(groups_cases[i], 1);
        api = zzapp_api_table();
        assert(api->groups == groups_cases[i]);
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        assert(api->info(&info) == AAR_OK && info.groups == groups_cases[i]);
    }
    zzapp_test_set_groups(0u, 0);
    api = zzapp_api_table();

    /* Core slots and the optional three are present. */
    assert(api->exit && api->yield && api->should_stop && api->time_us &&
           api->sleep_us && api->info);
    assert(api->heap_range && api->heap_grow && api->heap_shrink &&
           api->heap_info);
    assert(api->shared_alloc && api->shared_free && api->publish &&
           api->acquire);
    assert(api->log && api->file_open && api->file_close && api->file_read &&
           api->file_write && api->file_seek && api->file_stat &&
           api->file_list_open && api->file_list_next &&
           api->file_list_close && api->console_write && api->console_read);
    assert(api->event_poll && api->msg_send && api->msg_poll &&
           api->msg_wait);
    assert(api->ext && api->map_file && api->unmap_file);
    /* Every call of a group the table advertises is present. */
    if (api->groups & AAR_GROUP_DISPLAY)
        assert(api->display_open && api->display_flip &&
               api->display_flip_async && api->display_flip_idle &&
               api->display_refresh && api->display_vsync &&
               api->display_set_palette && api->display_close &&
               api->display_latest_dropped);
    if (api->groups & AAR_GROUP_AUDIO)
        assert(api->audio_open && api->audio_close);
    if (api->groups & AAR_GROUP_GL)
        assert(api->gl_bind && api->gl_swap && api->gl_unbind);
    if (api->groups & AAR_GROUP_SERVICES)
        assert(api->sha256 && api->decompress && api->image_open &&
               api->image_decode && api->image_close &&
               api->audio_decode_open && api->audio_decode &&
               api->audio_decode_close);
}

static void test_info_values(void)
{
    aar_info info;
    unsigned i;

    reset();
    memset(&info, 0xa5, sizeof(info));
    info.size = sizeof(info);
    assert(api->info(&info) == AAR_OK);
    assert(info.size == sizeof(info));
    assert(info.profile == AAR_PROFILE_A32);
    assert(info.platform_id == AAR_PLATFORM_ZZ9000);
    assert(info.flags == (AAR_INFOF_ISOLATED | AAR_INFOF_SHARED_COHERENT));
    assert(info.groups == api->groups);
    assert(info.heap_granule == MIB);
    assert(info.shared_granule == GRANULE);
    assert((info.shared_granule & (info.shared_granule - 1u)) == 0u);
    assert(info.shared_granule >= 64u);
    assert(info.msg_capacity == 63u);
    assert(info.sleep_granule_us == 5000u);
    assert(info.stall_limit_ms == 30000u);
    assert(info.stall_limit_ms >= AAR_STALL_LIMIT_MIN_MS);
    assert(info.stop_grace_ms == 500u);
    assert(info.cpu_features == 0u);
    assert(info.shared_limit == DEFAULT_SHARED_LIMIT);
    /* The claim's control block and queue pair is shared memory the runtime
     * holds for the client, and counts in the limit. */
    assert(info.shared_used == PAIR_BYTES);
    assert(info.max_files == 16u);
    assert(info.max_files >= AAR_MIN_FILES);
    for (i = 0u; i < 5u; i++)
        assert(info.reserved[i] == 0u);
    assert(api->info(NULL) == AAR_E_INVAL);
}

/* The caller sets `size`; at most that many bytes are written, and the
 * fields the caller's structure has room for are the ones filled. */
static void test_info_fill_rule(void)
{
    static const uint32_t short_sizes[] = { 4u, 8u, 12u, 20u, 52u, 56u, 60u,
                                            64u, 80u };
    union {
        aar_info info;
        uint8_t raw[sizeof(aar_info) + 48u];
    } buf;
    aar_info full;
    unsigned i;
    uint32_t j;

    reset();
    memset(&full, 0, sizeof(full));
    full.size = sizeof(full);
    assert(api->info(&full) == AAR_OK);

    for (i = 0u; i < sizeof(short_sizes) / sizeof(short_sizes[0]); i++) {
        uint32_t size = short_sizes[i];

        memset(&buf, 0xa5, sizeof(buf));
        buf.info.size = size;
        assert(api->info(&buf.info) == AAR_OK);
        /* The size field echoes the caller's, the filled prefix matches a
         * full fill, and nothing past `size` moved. */
        assert(buf.info.size == size);
        assert(memcmp(buf.raw + 4, (const uint8_t *)&full + 4, size - 4u) == 0);
        for (j = size; j < sizeof(buf); j++)
            assert(buf.raw[j] == 0xa5u);
    }

    /* Exactly the runtime's size. */
    memset(&buf, 0xa5, sizeof(buf));
    buf.info.size = sizeof(aar_info);
    assert(api->info(&buf.info) == AAR_OK);
    assert(memcmp(&buf.info, &full, sizeof(aar_info)) == 0);
    for (j = sizeof(aar_info); j < sizeof(buf); j++)
        assert(buf.raw[j] == 0xa5u);

    /* A caller that knows more fields than the runtime: the table fills the
     * fields it knows and goes no further. The zero fill of the unknown tail
     * belongs to the dispatch layer (g/svc_test, test_size_first_info_extent):
     * `size` is the app's word, and past sizeof(aar_info) the table has no
     * buffer of its own to bound a write by. */
    memset(&buf, 0xa5, sizeof(buf));
    buf.info.size = sizeof(buf);
    assert(api->info(&buf.info) == AAR_OK);
    assert(buf.info.size == sizeof(buf));
    assert(memcmp(buf.raw + 4, (const uint8_t *)&full + 4,
                  sizeof(aar_info) - 4u) == 0);
    for (j = sizeof(aar_info); j < sizeof(buf); j++)
        assert(buf.raw[j] == 0xa5u);
}

static void test_heap_info(void)
{
    aar_heap_info mem;
    zzapp_proc_t *proc;
    uint64_t committed;

    reset();
    proc = zzapp_proc_slot(0);
    proc->heap_base = 0x00100000u;
    proc->heap_start = 0x00100000u;
    proc->heap_break = 0x00100000u;
    proc->heap_limit = 0x01000000u;
    proc->memory.committed = 2u * MIB;
    proc->memory.limit = 16u * MIB;

    memset(&mem, 0xa5, sizeof(mem));
    assert(api->heap_info(&mem) == AAR_OK);
    assert(mem.committed == 2u * MIB);
    assert(mem.limit == 16u * MIB);
    assert(api->heap_info(NULL) == AAR_E_INVAL);

    assert(api->heap_grow(3u * MIB, NULL) == AAR_OK);
    assert(api->heap_info(&mem) == AAR_OK);
    assert(mem.committed == 5u * MIB && mem.limit == 16u * MIB);
    assert(api->heap_shrink(MIB, NULL) == AAR_OK);
    assert(api->heap_info(&mem) == AAR_OK && mem.committed == 4u * MIB);

    /* Whole granules, never zero; a count past 32 bits is not truncated. */
    committed = mem.committed;
    assert(api->heap_grow(0u, NULL) == AAR_E_INVAL);
    assert(api->heap_grow(MIB + 1u, NULL) == AAR_E_INVAL);
    assert(api->heap_shrink(0u, NULL) == AAR_E_INVAL);
    assert(api->heap_shrink(MIB - 1u, NULL) == AAR_E_INVAL);
    assert(api->heap_grow((uint64_t)1 << 32, NULL) == AAR_E_NOMEM);
    assert(api->heap_grow(((uint64_t)1 << 32) + MIB, NULL) == AAR_E_NOMEM);
    assert(api->heap_grow(UINT64_MAX - (MIB - 1u), NULL) == AAR_E_NOMEM);
    assert(api->heap_grow(((uint64_t)1 << 32) + 1u, NULL) == AAR_E_INVAL);
    assert(api->heap_shrink((uint64_t)1 << 32, NULL) == AAR_E_INVAL);
    assert(api->heap_shrink(((uint64_t)1 << 32) + MIB, NULL) == AAR_E_INVAL);
    assert(api->heap_info(&mem) == AAR_OK && mem.committed == committed);

    proc->memory.committed = 0u;
    proc->heap_break = proc->heap_start = proc->heap_base = 0u;
}

/* One shared block, with the allocator's view of it. */
static uint64_t alloc_block(uint64_t bytes, uint32_t align, aar_shared *out)
{
    memset(out, 0xa5, sizeof(*out));
    assert(api->shared_alloc(bytes, align, 0u, out) == AAR_OK);
    return out->bytes;
}

static void test_shared_alloc_rules(void)
{
    static const struct {
        uint64_t asked;
        uint64_t granted;
    } rounding[] = {
        { 1u, 4096u },     { 64u, 4096u },    { 4095u, 4096u },
        { 4096u, 4096u },  { 4097u, 8192u },  { 8192u, 8192u },
        { 12345u, 16384u }, { MIB, MIB },     { MIB + 1u, MIB + 4096u },
    };
    aar_shared shared;
    uint32_t allocs;
    unsigned i;

    reset();

    /* Refusals leave no trace: not in the ledger, not at the allocator. */
    allocs = card_allocs;
    memset(&shared, 0xa5, sizeof(shared));
    assert(api->shared_alloc(4096u, 0u, 1u, &shared) == AAR_E_INVAL);
    assert(api->shared_alloc(4096u, 0u, 2u, &shared) == AAR_E_INVAL);
    assert(api->shared_alloc(4096u, 0u, 0x80000000u, &shared) == AAR_E_INVAL);
    assert(api->shared_alloc(4096u, 0u, AAR_SHAREDF_COHERENT, &shared) ==
           AAR_E_INVAL);
    assert(api->shared_alloc(0u, 0u, 0u, &shared) == AAR_E_INVAL);
    assert(api->shared_alloc(4096u, 0u, 0u, NULL) == AAR_E_INVAL);
    assert(card_allocs == allocs && shared_used() == 0u);

    for (i = 0u; i < sizeof(rounding) / sizeof(rounding[0]); i++) {
        uint8_t *bytes;

        assert(alloc_block(rounding[i].asked, 0u, &shared) ==
               rounding[i].granted);
        assert(shared.bytes % GRANULE == 0u);
        assert(shared.flags == AAR_SHAREDF_COHERENT);
        assert(shared.ptr != NULL);
        assert(card_last_bytes == rounding[i].granted);
        assert(card_last_align >= GRANULE);
        /* The token is the board-relative offset of the card block. */
        assert(shared.haddr == card_last - BOARD_ADJUST);
        assert(shared_used() == rounding[i].granted);
        /* The whole granted range is the app's. */
        bytes = shared.ptr;
        bytes[0] = 1u;
        bytes[shared.bytes - 1u] = 2u;
        assert(api->shared_free(shared.ptr) == AAR_OK);
        assert(card_last_freed == card_last);
        assert(shared_used() == 0u);
    }

    /* A larger alignment is honoured at the allocator and still rounds. */
    assert(alloc_block(100u, 64u, &shared) == GRANULE);
    assert(card_last_align >= GRANULE);
    assert(api->shared_free(shared.ptr) == AAR_OK);
    assert(alloc_block(100u, 1u << 16, &shared) == GRANULE);
    assert(card_last_align == (1u << 16));
    assert(shared.haddr == card_last - BOARD_ADJUST);
    assert(card_frees != 0u);
    assert(api->shared_free(shared.ptr) == AAR_OK);
}

static void test_shared_tokens(void)
{
    aar_shared a;
    aar_shared b;
    uint32_t card_a;
    uint32_t card_b;

    reset();
    alloc_block(GRANULE, 0u, &a);
    card_a = card_last;
    alloc_block(3u * GRANULE, 0u, &b);
    card_b = card_last;
    assert(a.haddr == card_a - BOARD_ADJUST);
    assert(b.haddr == card_b - BOARD_ADJUST);
    /* Two live blocks: distinct tokens and mappings, and a token does not
     * move while its block lives. */
    assert(a.haddr != b.haddr && a.ptr != b.ptr);
    assert(b.haddr - a.haddr >= GRANULE || a.haddr - b.haddr >= 3u * GRANULE);
    assert(shared_used() == 4u * GRANULE);
    assert(api->shared_free(a.ptr) == AAR_OK);
    assert(shared_used() == 3u * GRANULE);
    assert(api->shared_free(b.ptr) == AAR_OK);
    assert(shared_used() == 0u);
}

static void test_shared_limits(void)
{
    aar_shared blocks[BLOCK_LIMIT];
    aar_shared extra;
    uint32_t allocs;
    unsigned i;

    reset();
    allocs = card_allocs;

    /* Counts beyond 32 bits are never truncated into a small block. */
    memset(&extra, 0xa5, sizeof(extra));
    assert(api->shared_alloc((uint64_t)1 << 32, 0u, 0u, &extra) == AAR_E_NOMEM);
    assert(api->shared_alloc(((uint64_t)1 << 32) + GRANULE, 0u, 0u, &extra) ==
           AAR_E_NOMEM);
    assert(api->shared_alloc(((uint64_t)1 << 33) + 1u, 0u, 0u, &extra) ==
           AAR_E_NOMEM);
    assert(api->shared_alloc(UINT64_MAX, 0u, 0u, &extra) == AAR_E_NOMEM);
    assert(api->shared_alloc(UINT32_MAX, 0u, 0u, &extra) == AAR_E_NOMEM);
    assert(card_allocs == allocs && shared_used() == 0u);

    /* The limit is a cap on granted bytes. */
    /* The pair counts against the limit, so the app has the rest. */
    assert(api->shared_alloc(DEFAULT_SHARED_LIMIT + GRANULE, 0u, 0u, &extra) ==
           AAR_E_NOMEM);
    alloc_block(DEFAULT_SHARED_LIMIT - PAIR_BYTES, 0u, &extra);
    assert(shared_used() == DEFAULT_SHARED_LIMIT - PAIR_BYTES);
    {
        aar_shared over;

        assert(api->shared_alloc(1u, 0u, 0u, &over) == AAR_E_NOMEM);
    }
    assert(api->shared_free(extra.ptr) == AAR_OK);
    assert(shared_used() == 0u);

    /* At least AAR_SHARED_MIN_BLOCKS live blocks. */
    for (i = 0u; i < BLOCK_LIMIT; i++)
        alloc_block(1u, 0u, &blocks[i]);
    assert(BLOCK_LIMIT >= AAR_SHARED_MIN_BLOCKS);
    assert(shared_used() == (uint64_t)BLOCK_LIMIT * GRANULE);
    assert(api->shared_alloc(1u, 0u, 0u, &extra) == AAR_E_NOMEM);
    for (i = 0u; i < BLOCK_LIMIT; i++)
        assert(api->shared_free(blocks[i].ptr) == AAR_OK);
    assert(shared_used() == 0u);
    alloc_block(1u, 0u, &extra);
    assert(api->shared_free(extra.ptr) == AAR_OK);
}

static void test_shared_free_rules(void)
{
    aar_shared shared;
    uint8_t local[8];
    uint32_t frees;

    reset();
    alloc_block(2u * GRANULE, 0u, &shared);
    frees = card_frees;
    assert(api->shared_free(NULL) == AAR_E_INVAL);
    assert(api->shared_free(local) == AAR_E_INVAL);
    /* Only the base of a live block frees it. */
    assert(api->shared_free((uint8_t *)shared.ptr + 1u) == AAR_E_INVAL);
    assert(api->shared_free((uint8_t *)shared.ptr + GRANULE) == AAR_E_INVAL);
    assert(card_frees == frees && shared_used() == 2u * GRANULE);
    assert(api->shared_free(shared.ptr) == AAR_OK);
    assert(card_frees == frees + 1u && shared_used() == 0u);
    /* A second free of the same block is refused. */
    assert(api->shared_free(shared.ptr) == AAR_E_INVAL);
    assert(card_frees == frees + 1u);
}

static void check_range(int (*call)(const void *, uint64_t))
{
    aar_shared shared;
    uint8_t *p;
    uint8_t local[16];
    uint64_t bytes;

    alloc_block(2u * GRANULE + 1u, 0u, &shared);
    bytes = shared.bytes;
    assert(bytes == 3u * GRANULE);
    p = shared.ptr;

    /* Inside the block. */
    assert(call(p, 1u) == AAR_OK);
    assert(call(p, bytes) == AAR_OK);
    assert(call(p + bytes - 1u, 1u) == AAR_OK);
    assert(call(p + GRANULE, GRANULE) == AAR_OK);
    assert(call(p + 5u, bytes - 5u) == AAR_OK);
    /* The granted size, not the asked one, bounds the range. */
    assert(call(p + 2u * GRANULE + 1u, 1u) == AAR_OK);

    /* Not inside it. */
    assert(call(NULL, 1u) == AAR_E_INVAL);
    assert(call(p, 0u) == AAR_E_INVAL);
    assert(call(p, bytes + 1u) == AAR_E_INVAL);
    assert(call(p + bytes, 1u) == AAR_E_INVAL);
    assert(call(p + bytes - 1u, 2u) == AAR_E_INVAL);
    assert(call(p + 1u, bytes) == AAR_E_INVAL);
    assert(call((const void *)((uintptr_t)p - 1u), 1u) == AAR_E_INVAL);
    assert(call(local, sizeof(local)) == AAR_E_INVAL);
    /* Past 32 bits, never wrapped back into the block. */
    assert(call(p, (uint64_t)1 << 32) == AAR_E_INVAL);
    assert(call(p, ((uint64_t)1 << 32) + 1u) == AAR_E_INVAL);
    assert(call(p, ((uint64_t)1 << 32) + bytes) == AAR_E_INVAL);
    assert(call(p, UINT64_MAX) == AAR_E_INVAL);
    assert(call(p + 1u, UINT64_MAX) == AAR_E_INVAL);

    /* Gone with the block. */
    assert(api->shared_free(shared.ptr) == AAR_OK);
    assert(call(p, 1u) == AAR_E_INVAL);
}

static void test_publish_acquire(void)
{
    reset();
    check_range(api->publish);
    check_range(api->acquire);
}

static void test_wide_counts(void)
{
    uint8_t data[4] = { 0 };
    uint8_t digest[32];
    aar_service_result result;

    reset();
    /* The firmware's services take 32-bit byte counts. */
    assert(api->sha256(data, (uint64_t)1 << 32, digest) == AAR_E_INVAL);
    assert(api->sha256(data, ((uint64_t)1 << 32) + 4u, digest) == AAR_E_INVAL);
    assert(api->sha256(data, UINT64_MAX, digest) == AAR_E_INVAL);
    assert(api->sha256(NULL, 4u, digest) == AAR_E_INVAL);
    assert(api->sha256(data, 4u, NULL) == AAR_E_INVAL);
    assert(api->sha256(data, 4u, digest) == AAR_OK);
    assert(api->decompress(AAR_COMPRESS_ZLIB, 0u, data, (uint64_t)1 << 32,
                           digest, sizeof(digest), &result) == AAR_E_INVAL);
    assert(api->decompress(AAR_COMPRESS_ZLIB, 0u, data, sizeof(data), digest,
                           ((uint64_t)1 << 32) + sizeof(digest), &result) ==
           AAR_E_INVAL);
}

static void test_ext_table(void)
{
    const aar_ext_zz9000 *ext;
    const void *table;

    reset();
    table = api->ext(AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_MAIN);
    assert(table != NULL);
    assert(api->ext(AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_MAIN) == table);
    ext = table;
    assert(ext->size == sizeof(aar_ext_zz9000));
    assert(ext->reserved == 0u);
    assert(AAR_EXT_ZZ9000_COVERS(ext, gl_bind_flags));
    assert(ext->gl_bind_flags != NULL);
    assert(AAR_EXT_ZZ9000_COVERS(ext, build_id));
    assert(ext->build_id != NULL && ext->build_id[0] != '\0');
    /* The module revision has no source in the resident firmware. */
    assert(AAR_EXT_ZZ9000_COVERS(ext, gl_build_id));
    assert(ext->gl_build_id == NULL);

    /* Anything else is absent. */
    assert(api->ext(AAR_PLATFORM_ZZ9000, 0u) == NULL);
    assert(api->ext(AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_MAIN + 1u) == NULL);
    assert(api->ext(AAR_PLATFORM_ZZ9000, UINT32_MAX) == NULL);
    assert(api->ext(AAR_PLATFORM_NONE, AAR_EXT_ZZ9000_MAIN) == NULL);
    assert(api->ext(AAR_PLATFORM_ZZ9000 + 1u, AAR_EXT_ZZ9000_MAIN) == NULL);
    assert(api->ext(UINT32_MAX, AAR_EXT_ZZ9000_MAIN) == NULL);
    assert(api->ext(0u, 0u) == NULL);
}

int main(void)
{
    test_header();
    test_info_values();
    test_info_fill_rule();
    test_heap_info();
    test_shared_alloc_rules();
    test_shared_tokens();
    test_shared_limits();
    test_shared_free_rules();
    test_publish_acquire();
    test_wide_counts();
    test_ext_table();
    return 0;
}
