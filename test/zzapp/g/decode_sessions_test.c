/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Decode sessions are per process record (slice 5b): image sessions sit in
 * the record's decode part, the audio sessions behind it in a block that
 * exists only while one is open. Two records never share a session list or
 * a block, and retiring one gives back its own and nothing else.
 *
 * zzapp_api.c runs here against a real record arena and scheduler; the host
 * runtime, the decode heap and the image stream are test doubles.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aar/app.h"
#include "sdk_compression.h"
#include "sdk_image_stream.h"
#include "xil_cache.h"
#include "zzapp_kernel.h"
#include "zzapp_rec.h"
#include "zzapp_runtime.h"

/* The retire hooks the host calls with the record selected. */
void zzapp_api_reclaim_resources(void);
void zzapp_api_poison_resources(void);

struct xil_cache_mock_state g_xil_cache_mock;
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

#define RECORDS 8U

static zzapp_kernel_t kernel;
static zzapp_proc_t procs[RECORDS];
static uint32_t services_pid;

/* ---- Host runtime doubles ----------------------------------------------- */

void aar_log(const char *msg, uint32_t len) { (void)msg; (void)len; }
zzapp_kernel_t *zzapp_runtime_kernel(void) { return &kernel; }
uint32_t zzapp_services_current_pid(void) { return services_pid; }
zzapp_proc_t *zzapp_proc_slot(uint32_t pid)
{
    return pid < RECORDS ? &procs[pid] : NULL;
}
void zzapp_api_enter(void) {}
void zzapp_api_leave(void) {}
void zzapp_api_touch(void) {}
uint32_t zzapp_current_generation(void) { return 1U; }
uint32_t zzapp_runtime_groups(void) { return 0U; }
void zzapp_host_yield(void) {}
void zzapp_host_yield_wait(void) {}
void zzapp_host_sleep(uint32_t us) { (void)us; }
uint64_t zzapp_host_time_us(void) { return 0U; }
int zzapp_host_stop_requested(void) { return 0; }
int zzapp_host_faulted(void) { return 0; }
void zzapp_host_commit_begin(void) {}
void zzapp_host_commit_end(void) {}
void zzapp_host_exit(int code) { (void)code; }
void *zzapp_host_alloc(uint32_t bytes, uint32_t align)
{
    (void)bytes;
    (void)align;
    return NULL;
}
int zzapp_host_free(void *p) { (void)p; return AAR_E_NOSYS; }
int zzapp_host_shared_alloc(uint64_t bytes, uint32_t align, uint32_t flags,
                            struct aar_shared *out)
{
    (void)bytes; (void)align; (void)flags; (void)out;
    return AAR_E_NOSYS;
}
int zzapp_host_shared_range(const void *p, uint64_t bytes)
{
    (void)p; (void)bytes;
    return AAR_E_NOSYS;
}
int zzapp_host_info(struct aar_info *out) { (void)out; return AAR_E_NOSYS; }
void zzapp_host_heap(void **base, uint32_t *bytes)
{
    *base = NULL;
    *bytes = 0U;
}
int zzapp_proc_heap_grow(uint32_t bytes, void **old_break)
{
    (void)bytes; (void)old_break;
    return AAR_E_NOSYS;
}
int zzapp_proc_heap_shrink(uint32_t bytes, void **new_break)
{
    (void)bytes; (void)new_break;
    return AAR_E_NOSYS;
}
int zzapp_proc_heap_info(uint32_t *committed, uint32_t *limit,
                         uint32_t *pool_free)
{
    (void)committed; (void)limit; (void)pool_free;
    return AAR_E_NOSYS;
}

/* The decode heap: plain blocks, counted, with the owner they were asked
 * for. */
static unsigned heap_allocs;
static unsigned heap_frees;
static uint32_t heap_last_owner;
static void *heap_live[RECORDS * 2U];

void *sdk_decode_heap_alloc_owned(size_t size, uint32_t owner)
{
    void *block = calloc(1U, size);
    unsigned i;

    heap_allocs++;
    heap_last_owner = owner;
    for (i = 0U; i < RECORDS * 2U; i++)
        if (!heap_live[i]) {
            heap_live[i] = block;
            break;
        }
    return block;
}

void sdk_decode_heap_free(void *ptr)
{
    unsigned i;

    for (i = 0U; i < RECORDS * 2U; i++)
        if (heap_live[i] == ptr)
            heap_live[i] = NULL;
    heap_frees++;
    free(ptr);
}

static unsigned decode_reclaims;
static uint32_t decode_reclaim_owner;
unsigned sdk_compression_reclaim_app_decode(uint32_t owner)
{
    decode_reclaims++;
    decode_reclaim_owner = owner;
    return 0U;
}
/* The production mapping: the owner is a function of the record's serial. */
uint32_t sdk_compression_app_decode_owner(uint32_t pid)
{
    const zzapp_rec_t *rec = zzapp_rec_by_pid(pid);

    return rec && rec->hdr.owner_serial ?
        sdk_compression_app_owner_bits(rec->hdr.owner_serial) : 0U;
}

/* The image stream: sessions are just numbers. */
static uint32_t next_image_session;
static unsigned image_closes;
uint16_t sdk_image_stream_begin(const struct SDKImageStreamBegin *b,
                                struct SDKImageStreamResult *r)
{
    (void)b;
    memset(r, 0, sizeof(*r));
    r->session = ++next_image_session;
    return SDK_STATUS_OK;
}
uint16_t sdk_image_stream_close(uint32_t session)
{
    (void)session;
    image_closes++;
    return SDK_STATUS_OK;
}

/* ---- The arena ----------------------------------------------------------- */

static uint8_t arena_mem[2U << 20] __attribute__((aligned(1U << 20)));
static uint32_t bsp[4096];

static void setup(void)
{
    zzapp_rec_layout_t layout;
    uint32_t pid;

    zzapp_rec_layout_for(RECORDS, 4U << 20, &layout);
    CHECK(layout.bytes <= sizeof(arena_mem));
    CHECK(zzapp_rec_arena_init(arena_mem, &layout) == 0);
    for (pid = 0U; pid < 0x400U; pid++)
        bsp[pid] = (pid << 20) | 0x15de6U;
    zzapp_as_init(bsp);
    CHECK(zzapp_kernel_init(&kernel, zzapp_rec_by_pid(0U),
                            ZZAPP_REC_RECORD_BYTES, zzapp_rec_capacity()) == 0);
    memset(procs, 0, sizeof(procs));
    heap_allocs = heap_frees = decode_reclaims = image_closes = 0U;
    memset(heap_live, 0, sizeof(heap_live));
    services_pid = 0U;
    /* Admit every record, as the services layer does at a claim. */
    for (pid = 0U; pid < RECORDS; pid++) {
        uint32_t handle;
        zzapp_rec_t *rec;

        CHECK(zzapp_rec_admit(&handle, &rec) == 0);
        CHECK(rec != NULL && rec->hdr.pid == pid);
    }
}

static unsigned live_blocks(void)
{
    unsigned i;
    unsigned n = 0U;

    for (i = 0U; i < RECORDS * 2U; i++)
        n += heap_live[i] != NULL;
    return n;
}

static void test_audio_sessions_and_their_block_are_per_record(void)
{
    const aar_api *api = zzapp_api_table();
    uint32_t sessions3[4];
    uint32_t session5;
    uint32_t extra;
    unsigned i;

    setup();
    CHECK(api != NULL);
    /* Nothing is allocated until a session opens. */
    CHECK(heap_allocs == 0U);
    services_pid = 3U;
    for (i = 0U; i < 4U; i++)
        CHECK(api->audio_decode_open(AAR_AUDIO_MP3, &sessions3[i]) == AAR_OK);
    CHECK(heap_allocs == 1U);
    CHECK(heap_last_owner == sdk_compression_app_decode_owner(3U));
    CHECK(heap_last_owner != 0U);
    CHECK(sessions3[0] != sessions3[1] && sessions3[2] != sessions3[3]);
    /* The kernel's per-record limit is four sessions. */
    CHECK(api->audio_decode_open(AAR_AUDIO_MP3, &extra) == AAR_E_NOMEM);
    CHECK(procs[3].audio_sessions == 4U);

    /* Record 5 has its own block, its own list, its own ids. */
    services_pid = 5U;
    CHECK(api->audio_decode_open(AAR_AUDIO_MP3, &session5) == AAR_OK);
    CHECK(heap_allocs == 2U);
    CHECK(heap_last_owner == sdk_compression_app_decode_owner(5U));
    CHECK(heap_last_owner != sdk_compression_app_decode_owner(3U));
    CHECK(live_blocks() == 2U);
    /* Ids count per record: record 5's first session is numbered like
     * record 3's first, and record 3's fourth is no session of record 5. */
    CHECK(session5 == sessions3[0]);
    CHECK(api->audio_decode_close(sessions3[3]) == AAR_E_INVAL);
    CHECK(procs[5].audio_sessions == 1U && procs[3].audio_sessions == 4U);

    /* Closing the last session of a record frees that record's block. */
    services_pid = 3U;
    for (i = 0U; i < 3U; i++)
        CHECK(api->audio_decode_close(sessions3[i]) == AAR_OK);
    CHECK(heap_frees == 0U && live_blocks() == 2U);
    CHECK(api->audio_decode_close(sessions3[3]) == AAR_OK);
    CHECK(heap_frees == 1U && live_blocks() == 1U);
    services_pid = 5U;
    CHECK(api->audio_decode_close(session5) == AAR_OK);
    CHECK(heap_frees == 2U && live_blocks() == 0U);
    CHECK(procs[3].audio_sessions == 0U && procs[5].audio_sessions == 0U);

    /* The block comes back with the next open. */
    CHECK(api->audio_decode_open(AAR_AUDIO_MP3, &session5) == AAR_OK);
    CHECK(heap_allocs == 3U);
    zzapp_api_reclaim_resources();
    CHECK(heap_frees == 3U && live_blocks() == 0U);
    CHECK(procs[5].audio_sessions == 0U);
}

static void test_retire_returns_only_the_records_own_sessions(void)
{
    const aar_api *api = zzapp_api_table();
    uint32_t a;
    uint32_t b;

    setup();
    services_pid = 2U;
    CHECK(api->audio_decode_open(AAR_AUDIO_MP3, &a) == AAR_OK);
    services_pid = 6U;
    CHECK(api->audio_decode_open(AAR_AUDIO_MP3, &b) == AAR_OK);
    CHECK(live_blocks() == 2U);

    /* Normal retire of record 2: its block goes, record 6 keeps its own. */
    services_pid = 2U;
    zzapp_api_reclaim_resources();
    CHECK(live_blocks() == 1U && procs[2].audio_sessions == 0U);
    CHECK(procs[6].audio_sessions == 1U);
    services_pid = 6U;
    CHECK(api->audio_decode_close(b) == AAR_OK);
    CHECK(live_blocks() == 0U);

    /* Poison (core 1 died inside a decoder): the sessions are forgotten, the
     * card-pool owner is reclaimed instead of freeing the block, and the
     * record can open again. */
    services_pid = 6U;
    CHECK(api->audio_decode_open(AAR_AUDIO_MP3, &b) == AAR_OK);
    heap_frees = 0U;
    decode_reclaims = 0U;
    zzapp_api_poison_resources();
    CHECK(heap_frees == 0U);
    CHECK(decode_reclaims == 1U);
    CHECK(decode_reclaim_owner == sdk_compression_app_decode_owner(6U));
    CHECK(procs[6].audio_sessions == 0U);
    CHECK(api->audio_decode_close(b) == AAR_E_INVAL);
    CHECK(api->audio_decode_open(AAR_AUDIO_MP3, &b) == AAR_OK);
    CHECK(procs[6].audio_sessions == 1U);
}

static void test_image_sessions_are_per_record(void)
{
    const aar_api *api = zzapp_api_table();
    static uint8_t tile[4096];
    aar_image_params params;
    aar_image_result result;
    uint32_t sessions[4];
    uint32_t other;
    uint32_t extra;
    unsigned i;

    setup();
    memset(&params, 0, sizeof(params));
    params.codec = 1U;
    params.output_format = AAR_FMT_BGRA8888;
    params.stride = 64U;
    params.rows = 4U;
    params.tile_bytes = sizeof(tile);
    params.tile = tile;

    services_pid = 4U;
    for (i = 0U; i < 4U; i++)
        CHECK(api->image_open(&params, &sessions[i], &result) == AAR_OK);
    CHECK(api->image_open(&params, &extra, &result) == AAR_E_NOMEM);
    CHECK(procs[4].image_sessions == 4U);

    services_pid = 1U;
    CHECK(api->image_open(&params, &other, &result) == AAR_OK);
    CHECK(procs[1].image_sessions == 1U);
    /* Record 4's sessions are invisible to record 1. */
    CHECK(api->image_close(sessions[0]) == AAR_E_INVAL);
    CHECK(api->image_close(other) == AAR_OK);

    services_pid = 4U;
    CHECK(api->image_close(sessions[0]) == AAR_OK);
    CHECK(procs[4].image_sessions == 3U);
    zzapp_api_reclaim_resources();
    CHECK(procs[1].image_sessions == 0U);
    CHECK(image_closes == 1U + 1U + 3U);

    /* A call with no record is refused, not served from record 0. */
    services_pid = ZZAPP_PID_NONE;
    CHECK(api->image_open(&params, &extra, &result) == AAR_E_FAULT);
    CHECK(api->audio_decode_open(AAR_AUDIO_MP3, &extra) == AAR_E_FAULT);
    CHECK(procs[0].image_sessions == 0U && procs[0].audio_sessions == 0U);
}

int main(void)
{
    test_audio_sessions_and_their_block_are_per_record();
    test_retire_returns_only_the_records_own_sessions();
    test_image_sessions_are_per_record();
    printf("decode_sessions_test: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
