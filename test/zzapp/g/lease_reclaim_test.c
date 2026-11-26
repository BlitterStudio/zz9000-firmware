/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A core-0 audio ring (codec lease) decoder survives the core-1 reset reclaim.
 * The production backend runs libFLAC and Tremor over the real card pool; the
 * production sdk_compression_reclaim_core1_decode() releases the core-1 decode
 * owners while the lease is mid-stream, once with an SDK FLAC or Vorbis stream
 * on core 1 holding blocks. Only the halt of core 1 is a host substitute (the
 * SDK stream state is forgotten the way sdk_mailbox_poison_core1_audio_streams
 * does). The lease keeps decoding to the same PCM, and closing it leaves no
 * block behind. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_fabric_codec.h"
#include "card_pool.h"
#include "flac_fixtures.h"
#include "host_card_pool.h"
#include "sdk_audio_flac.h"
#include "sdk_audio_vorbis.h"
#include "sdk_compression.h"
#include "sdk_mailbox.h"
#include "zzapp_rec.h"

static int g_cpu;
static int checks;
static int failures;

#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { \
        failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    } \
} while (0)

int smp_cpu_id(void) { return g_cpu; }
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

/* No app records, nothing in the core-1 decode tracker. */
uint32_t zzapp_rec_capacity(void) { return 0U; }
zzapp_rec_t *zzapp_rec_by_pid(uint32_t pid) { (void)pid; return NULL; }
zzapp_rec_state_t zzapp_rec_state(uint32_t pid) { (void)pid; return ZZAPP_REC_FREE; }
void zz9k_lzh_reclaim(void) {}
void sdk_decode_flush_table(void) {}
void sdk_decode_invalidate_table(void) {}

#define PCM_MAX (4U * 1024U * 1024U)
#define FIRST_BYTES 3000U

static uint8_t g_pcm[PCM_MAX];
static uint8_t g_ref[PCM_MAX];
static uint32_t g_pcm_len;
static uint8_t g_vorbis[64U * 1024U];
static uint32_t g_vorbis_len;
static uint8_t g_hold_pcm[256U * 1024U];

static uint32_t pool_used(void) { return host_card_pool_used(); }

static struct sdk_flac_state g_hold_flac;
static struct sdk_vorbis_state g_hold_vorbis;

/* An SDK stream on core 1 holds decoder memory in the shared decode heap. */
static void sdk_stream_hold(uint32_t codec, const uint8_t *data)
{
    struct sdk_audio_codec_io io;
    const int saved = g_cpu;

    memset(&io, 0, sizeof(io));
    io.input = data;
    io.input_length = FIRST_BYTES;
    io.pcm = g_hold_pcm;
    io.pcm_capacity = sizeof(g_hold_pcm);
    io.pcm_free = sizeof(g_hold_pcm);
    io.pcm_budget = sizeof(g_hold_pcm);
    g_cpu = 1;
    if (codec == SDK_AUDIO_CODEC_FLAC) {
        sdk_flac_init(&g_hold_flac, SDK_AUDIO_SAMPLE_FORMAT_S16LE);
        (void)sdk_flac_decode(&g_hold_flac, &io);
    } else {
        sdk_vorbis_init(&g_hold_vorbis, SDK_AUDIO_SAMPLE_FORMAT_S16LE);
        (void)sdk_vorbis_decode(&g_hold_vorbis, &io);
    }
    g_cpu = saved;
}

/* sdk_mailbox_poison_core1_audio_streams: the reclaim already freed the
 * blocks, so the stream drops its pointers. */
static void sdk_stream_forget(uint32_t codec)
{
    if (codec == SDK_AUDIO_CODEC_FLAC)
        sdk_flac_forget(&g_hold_flac);
    else
        sdk_vorbis_forget(&g_hold_vorbis);
}

/* Decode one file through the lease backend. With `reclaim` the core-1
 * reclaim runs once, mid-stream, with an SDK stream holding blocks. */
static void run_lease(uint32_t codec, const uint8_t *data, uint32_t len,
                      int reclaim, uint8_t *out, uint32_t *out_len)
{
    const struct fabric_codec_ops *ops = &audio_fabric_stream_codec_ops;
    uint64_t cursor = 0U;
    uint32_t steps = 0U;
    int reclaimed = 0;
    struct fabric_codec_view v;

    g_cpu = 0;
    host_card_pool_reset(64U << 20);
    *out_len = 0U;
    ops->begin(1U, codec, 0U);
    for (;;) {
        struct fabric_codec_input in = { data, cursor, len, (uint32_t)(len - cursor), 1 };

        v = ops->step(1U, &in);
        cursor += v.copied;
        steps++;
        while (v.frames != 0U) {
            uint32_t n = v.frames < 4096U ? v.frames : 4096U;

            ops->read_stereo(1U, (int16_t *)(out + *out_len), n);
            *out_len += n * 4U;
            v.frames -= n;
        }
        if (reclaim && !reclaimed && steps >= 2U && !v.complete) {
            const uint32_t lease_used = pool_used();
            unsigned freed;

            CHECK(lease_used != 0U);
            sdk_stream_hold(codec, data);
            CHECK(pool_used() > lease_used);
            g_cpu = 0;
            freed = sdk_compression_reclaim_core1_decode();
            CHECK(freed != 0U);
            CHECK(pool_used() == lease_used);
            sdk_stream_forget(codec);
            reclaimed = 1;
        }
        if (v.complete || steps > 100000U)
            break;
    }
    CHECK(v.error == SDK_AUDIO_RING_DECODE_ERR_NONE);
    CHECK(!reclaim || reclaimed);
    CHECK(pool_used() != 0U);
    ops->end(1U);
    CHECK(pool_used() == 0U);
    CHECK(card_pool.ignored_frees == 0U);
}

static void test_codec(uint32_t codec, const uint8_t *data, uint32_t len)
{
    uint32_t ref_len;

    run_lease(codec, data, len, 0, g_ref, &ref_len);
    CHECK(ref_len != 0U);
    run_lease(codec, data, len, 1, g_pcm, &g_pcm_len);
    CHECK(g_pcm_len == ref_len);
    CHECK(memcmp(g_pcm, g_ref, ref_len < g_pcm_len ? ref_len : g_pcm_len) == 0);
}

/* Allocation failure paths release what was taken. */
static void test_allocation_failure(void)
{
    const struct fabric_codec_ops *ops = &audio_fabric_stream_codec_ops;
    struct fabric_codec_view v = {0};
    uint64_t cursor = 0U;
    uint32_t i;

    g_cpu = 0;
    host_card_pool_reset(256U << 10);        /* too small for the FIFO */
    ops->begin(1U, SDK_AUDIO_CODEC_FLAC, 0U);
    {
        struct fabric_codec_input in = { fx_stereo16_flac, 0U, sizeof(fx_stereo16_flac),
                                         sizeof(fx_stereo16_flac), 1 };

        v = ops->step(1U, &in);
    }
    CHECK(v.complete && v.error == SDK_AUDIO_RING_DECODE_ERR_NO_MEMORY);
    ops->end(1U);
    CHECK(pool_used() == 0U);

    /* Room for the FIFO only: libFLAC's own allocation fails mid-stream. */
    host_card_pool_reset(832U << 10);
    ops->begin(1U, SDK_AUDIO_CODEC_FLAC, 0U);
    for (i = 0U; i < 100U; i++) {
        struct fabric_codec_input in = { fx_stereo16_flac, cursor, sizeof(fx_stereo16_flac),
                                         (uint32_t)(sizeof(fx_stereo16_flac) - cursor), 1 };

        v = ops->step(1U, &in);
        cursor += v.copied;
        if (v.complete)
            break;
    }
    CHECK(v.complete && v.error == SDK_AUDIO_RING_DECODE_ERR_NO_MEMORY);
    ops->end(1U);
    CHECK(pool_used() == 0U);
}

int main(void)
{
    FILE *f = fopen(VORBIS_FIXTURE, "rb");

    if (!f) {
        perror(VORBIS_FIXTURE);
        return 2;
    }
    g_vorbis_len = (uint32_t)fread(g_vorbis, 1U, sizeof(g_vorbis), f);
    fclose(f);
    CHECK(g_vorbis_len > FIRST_BYTES && g_vorbis_len < sizeof(g_vorbis));

    test_codec(SDK_AUDIO_CODEC_FLAC, fx_stereo16_flac, sizeof(fx_stereo16_flac));
    test_codec(SDK_AUDIO_CODEC_VORBIS, g_vorbis, g_vorbis_len);
    test_allocation_failure();
    printf("lease_reclaim_test: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
