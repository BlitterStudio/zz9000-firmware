/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "sdk_audio_flac.h"
#include "sdk_audio_vorbis.h"
#include "sdk_compression.h"
#include "sdk_image_stream.h"
#include "aar/app.h"
#include "zzapp_svc.h"
#include "zzapp_runtime.h"
void zzapp_api_reclaim_resources(void);
void zzapp_api_poison_resources(void);

static uint64_t shared_used_now(const aar_api *api)
{
    aar_info info;

    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    assert(api->info(&info) == AAR_OK);
    return info.shared_used;
}

static void *shared_block(const aar_api *api, uint64_t bytes)
{
    aar_shared shared;

    memset(&shared, 0, sizeof(shared));
    if (api->shared_alloc(bytes, 0u, 0u, &shared) != AAR_OK)
        return NULL;
    return shared.ptr;
}

static const uint8_t file_data[] = "mapped";
static uint32_t image_begin_calls;
static uint32_t image_feed_calls;
static uint32_t image_close_calls;
static uint32_t image_poisoned_close_calls;
static uint32_t image_poison_calls;
static uint32_t audio_release_calls;
static uint32_t audio_forget_calls;
static uint32_t decode_reclaim_calls;
static uint32_t image_poison_session_calls;
static uint32_t app_decode_reclaim_calls;
static int image_begin_in_progress;
static int stop_during_host_alloc;
static int stop_during_image_begin;
static uint32_t flac_pool_owner[2];
static uint32_t flac_pool_owner_count;
static int tile_writable = 1;
static uint32_t selected_service_slot;
uint32_t zzapp_services_current_slot(void)
{
    return selected_service_slot;
}
int zzapp_svc_check_user(const zzapp_as_t *as, uint32_t address,
                         uint32_t bytes, int write)
{
    (void)as;
    (void)address;
    (void)bytes;
    return write && tile_writable ? AAR_OK : AAR_E_FAULT;
}

void zzapp_test_host_visible_allocated(void)
{
    if (stop_during_host_alloc)
        zzapp_host_latch_stop(0U);
    zzapp_host_tick_at(1u);
    zzapp_host_tick_at(1u + ZZAPP_STOP_GRACE_US);
}
int zzapp_svc_file_stat(const char *path, aar_stat *out)
{
    if (!path || strcmp(path, "RAM:fixture") != 0 || !out)
        return AAR_E_NOENT;
    out->size = sizeof(file_data) - 1u;
    out->mtime = 0u;
    out->mode = 1u;
    return AAR_OK;
}

int zzapp_svc_file_open(const char *path, uint32_t flags)
{
    return path && strcmp(path, "RAM:fixture") == 0 &&
           flags == AAR_FILE_READ ? 1 : AAR_E_NOENT;
}

int zzapp_svc_file_read(int fd, void *dst, uint32_t bytes, uint32_t *done)
{
    assert(fd == 1);
    assert(bytes >= sizeof(file_data) - 1u);
    memcpy(dst, file_data, sizeof(file_data) - 1u);
    *done = sizeof(file_data) - 1u;
    return AAR_OK;
}

int zzapp_svc_file_close(int fd)
{
    assert(fd == 1);
    return AAR_OK;
}

uint16_t sdk_image_stream_begin(const struct SDKImageStreamBegin *begin,
                                struct SDKImageStreamResult *out)
{
    assert(begin->core1_affine == 1u);
    assert(begin->direct_arm_local == 1u);
    assert(begin->output_format == SDK_SURFACE_FORMAT_BGRA8888);
    image_begin_calls++;
    if (stop_during_image_begin) {
        zzapp_host_latch_stop(0U);
        zzapp_host_tick_at(1u);
        zzapp_host_tick_at(1u + ZZAPP_STOP_GRACE_US);
    }
    if (image_begin_in_progress) {
        image_begin_in_progress--;
        return SDK_TASK_IN_PROGRESS;
    }
    memset(out, 0, sizeof(*out));
    out->session = 0x100u + image_begin_calls;
    return SDK_STATUS_OK;
}

uint16_t sdk_image_stream_feed(const struct SDKImageStreamFeed *feed,
                               const uint8_t *src,
                               struct SDKImageStreamResult *out)
{
    assert(feed->session != 0u);
    assert(src != NULL);
    image_feed_calls++;
    memset(out, 0, sizeof(*out));
    out->session = feed->session;
    out->bytes_consumed = feed->src_length;
    return SDK_STATUS_OK;
}

uint16_t sdk_image_stream_close(uint32_t session)
{
    assert(session != 0u);
    image_close_calls++;
    return SDK_STATUS_OK;
}

void sdk_image_stream_poison_core1_sessions(void)
{
    image_poison_calls++;
}

void sdk_image_stream_poison_session(uint32_t session)
{
    assert(session != 0u);
    image_poison_session_calls++;
}

uint16_t sdk_image_stream_release_poisoned(uint32_t session)
{
    assert(session != 0u && image_poison_session_calls);
    image_poisoned_close_calls++;
    return SDK_STATUS_OK;
}

void sdk_flac_init(struct sdk_flac_state *state, uint32_t format)
{
    memset(state, 0, sizeof(*state));
    state->output_format = format;
}

uint16_t sdk_flac_decode(struct sdk_flac_state *state,
                         struct sdk_audio_codec_io *io)
{
    assert(io->input_length != 0u || (io->eof && io->drain));
    if (flac_pool_owner_count < 2u)
        flac_pool_owner[flac_pool_owner_count++] = state->pool_owner;
    state->sample_rate = 44100u;
    state->channels = 2u;
    io->consumed = io->input_length;
    io->produced = 4u;
    io->frames = 1u;
    memset(io->pcm, 0, io->produced);
    return SDK_STATUS_OK;
}

void sdk_flac_release(struct sdk_flac_state *state)
{
    (void)state;
    audio_release_calls++;
}

void sdk_flac_forget(struct sdk_flac_state *state)
{
    (void)state;
    audio_forget_calls++;
}

void sdk_vorbis_init(struct sdk_vorbis_state *state, uint32_t format)
{
    memset(state, 0, sizeof(*state));
    state->output_format = format;
}

uint16_t sdk_vorbis_decode(struct sdk_vorbis_state *state,
                           struct sdk_audio_codec_io *io)
{
    assert(io->input_length != 0u || (io->eof && io->drain));
    state->sample_rate = 48000u;
    state->channels = 2u;
    io->consumed = io->input_length;
    io->produced = 4u;
    io->frames = 1u;
    memset(io->pcm, 0, io->produced);
    return SDK_STATUS_OK;
}

int sdk_vorbis_holds_memory(const struct sdk_vorbis_state *state)
{
    return state->codec != 0;
}

void sdk_vorbis_release(struct sdk_vorbis_state *state)
{
    (void)state;
    audio_release_calls++;
}

void sdk_vorbis_forget(struct sdk_vorbis_state *state)
{
    (void)state;
    audio_forget_calls++;
}
unsigned sdk_compression_reclaim_core1_decode(void)
{
    decode_reclaim_calls++;
    return 1u;
}

unsigned sdk_compression_reclaim_app_decode(uint32_t owner)
{
    (void)owner;
    app_decode_reclaim_calls++;
    return 1u;
}
uint32_t sdk_compression_app_decode_owner(uint32_t slot, uint32_t generation)
{
    return 0x02000000u | (generation << 2) | slot;
}

static uint32_t decompress_owner_seen;
static uint32_t decompress_calls;
static uint16_t fake_decompress(uint32_t algorithm, uint32_t flags,
                                const uint8_t *src, uint32_t src_length,
                                uint8_t *dst, uint32_t dst_capacity,
                                struct SDKDecompressResult *result,
                                uint32_t owner)
{
    (void)algorithm;
    (void)flags;
    (void)src;
    (void)src_length;
    (void)dst;
    (void)dst_capacity;
    memset(result, 0, sizeof(*result));
    decompress_owner_seen = owner;
    decompress_calls++;
    /* Simulate the force-stop mid-call: scratch stays allocated. */
    return SDK_STATUS_IO_ERROR;
}

uint16_t sdk_decompress_buffer(uint32_t algorithm, uint32_t flags,
                              const uint8_t *src, uint32_t src_length,
                              uint8_t *dst, uint32_t dst_capacity,
                              struct SDKDecompressResult *result)
{
    return fake_decompress(algorithm, flags, src, src_length, dst,
                           dst_capacity, result, SDK_DECODE_POOL_OWNER);
}

uint16_t sdk_decompress_buffer_owned(uint32_t algorithm, uint32_t flags,
                                     const uint8_t *src, uint32_t src_length,
                                     uint8_t *dst, uint32_t dst_capacity,
                                     struct SDKDecompressResult *result,
                                     uint32_t scratch_owner)
{
    return fake_decompress(algorithm, flags, src, src_length, dst,
                           dst_capacity, result, scratch_owner);
}

int main(void)
{
    const aar_api *api;
    aar_info info;
    aar_image_params image;
    aar_image_result image_result;
    aar_audio_decode_result audio_result;
    const void *mapped;
    uint64_t bytes;
    uint32_t image_session;
    uint32_t audio_session;
    uint32_t slot_audio_session;
    uint8_t source[8] = { 0 };
    uint8_t mp3[834] = { 0 };
    int16_t pcm[8];
    int16_t mp3_pcm[2304];
    void *host_visible;

    zzapp_boot_init();
    assert(zzapp_loader_begin() == AAR_OK);
    api = zzapp_api_table();

    assert(api->map_file("RAM:fixture", &mapped, &bytes) == AAR_OK);
    assert(bytes == sizeof(file_data) - 1u);
    assert(memcmp(mapped, file_data, bytes) == 0);
    assert(shared_used_now(api) == 4096u);
    api->unmap_file(mapped);
    assert(shared_used_now(api) == 0u);
    assert(api->map_file("RAM:fixture", &mapped, &bytes) == AAR_OK);
    api->unmap_file(mapped);

    host_visible = shared_block(api, 4096u);
    assert(host_visible != NULL);
    assert(shared_used_now(api) == 4096u);

    memset(&image, 0, sizeof(image));
    image.codec = AAR_IMAGE_PNG;
    image.output_format = 99u;
    image.tile = source;
    image.stride = 4u;
    image.rows = 2u;
    image.tile_bytes = sizeof(source);
    assert(api->image_open(&image, &image_session, &image_result) == AAR_E_INVAL);
    assert(api->image_open(&image, &image_session, &image_result) == AAR_E_INVAL);
    assert(api->image_open(&image, &image_session, &image_result) == AAR_E_INVAL);
    assert(api->image_open(&image, &image_session, &image_result) == AAR_E_INVAL);
    assert(zzapp_runtime_kernel()->resources[0].image_sessions == 0u);
    image.output_format = AAR_FMT_BGRA8888;
    assert(api->image_open(&image, &image_session, &image_result) == AAR_OK);
    tile_writable = 0;
    assert(api->image_decode(image_session, source, sizeof(source), 0u,
                             &image_result) == AAR_E_FAULT);
    assert(image_feed_calls == 0u);
    tile_writable = 1;
    assert(api->image_decode(image_session, source, sizeof(source), 0u,
                             &image_result) == AAR_OK);
    zzapp_kernel_set_current_thread(0);
    assert(api->audio_decode_open(AAR_AUDIO_FLAC, &slot_audio_session) == AAR_OK);
    assert(api->audio_decode(slot_audio_session, source, sizeof(source), 0u,
                             pcm, 2u, &audio_result) == AAR_OK);
    selected_service_slot = 1U;
    zzapp_kernel_set_current_thread(-1);
    assert(api->audio_decode_open(AAR_AUDIO_FLAC, &slot_audio_session) == AAR_OK);
    zzapp_api_reclaim_resources();
    assert(audio_release_calls == 1u);
    assert(zzapp_runtime_kernel()->resources[1].audio_sessions == 0u);
    assert(api->audio_decode_open(AAR_AUDIO_FLAC, &slot_audio_session) == AAR_OK);
    zzapp_api_poison_resources();
    assert(zzapp_runtime_kernel()->resources[1].audio_sessions == 0u);
    audio_release_calls = 0u;
    selected_service_slot = 0U;
    zzapp_kernel_set_current_thread(-1);
    assert(api->image_close(image_session) == AAR_OK);
    assert(image_begin_calls == 1u && image_feed_calls == 1u &&
           image_close_calls == 1u);
    image_begin_in_progress = 1;
    assert(api->image_open(&image, &image_session, &image_result) == AAR_OK);
    assert(image_begin_calls == 3u);

    mp3[0] = 0xffu; mp3[1] = 0xfbu; mp3[2] = 0x90u; mp3[3] = 0x64u;
    mp3[417] = 0xffu; mp3[418] = 0xfbu;
    mp3[419] = 0x90u; mp3[420] = 0x64u;
    assert(api->audio_decode_open(AAR_AUDIO_MP3, &audio_session) == AAR_OK);
    assert(api->audio_decode(audio_session, mp3, sizeof(mp3), 0u, mp3_pcm,
                             1152u, &audio_result) == AAR_OK);
    assert(audio_result.bytes_consumed == 417u);
    assert(audio_result.frames_written == 1152u);
    assert(audio_result.rate == 44100u && audio_result.channels == 2u);
    assert(api->audio_decode_close(audio_session) == AAR_OK);

    assert(api->audio_decode_open(AAR_AUDIO_FLAC, &audio_session) == AAR_OK);
    assert(api->audio_decode(audio_session, source, sizeof(source), 0u, pcm,
                             2u, &audio_result) == AAR_OK);
    assert(audio_result.bytes_consumed == sizeof(source));
    assert(audio_result.frames_written == 1u);
    assert(audio_result.rate == 44100u && audio_result.channels == 2u);
    assert(api->audio_decode(audio_session, NULL, 0u,
                             AAR_AUDIO_DECODE_EOF | AAR_AUDIO_DECODE_DRAIN,
                             pcm, 2u, &audio_result) == AAR_OK);
    assert(api->audio_decode_close(audio_session) == AAR_OK);
    assert(api->audio_decode_open(AAR_AUDIO_VORBIS, &audio_session) == AAR_OK);
    /* App decompress scratch carries the app decode owner, so the
     * force-stop poison below reclaims it instead of the shared pool. */
    {
        aar_service_result decompressed;

        assert(api->decompress(AAR_COMPRESS_ZLIB, 0u, source, sizeof(source),
                               mp3_pcm, sizeof(mp3_pcm),
                               &decompressed) == AAR_E_IO);
        assert(decompress_calls == 1u);
        assert(decompress_owner_seen == flac_pool_owner[0]);
    }

    /* The host allocator must publish its ledger entry before a stop tick
     * can unwind it; host-only seam because ARM entry/FPSCR cannot run here. */
    stop_during_host_alloc = 1;
    assert(shared_block(api, 64u) != NULL);
    assert(shared_used_now(api) == 8192u);
    zzapp_test_mark_running();
    zzapp_host_tick_at(1u);
    zzapp_worker_hook();
    zzapp_host_tick_at(1u + ZZAPP_STOP_GRACE_US);
    zzapp_worker_hook();
    assert(zzapp_runtime_state() == ZZAPP_ST_FAULTED);
    assert(image_close_calls == 1u && image_poisoned_close_calls == 1u);
    assert(image_poison_calls == 0u && image_poison_session_calls == 1u);
    assert(audio_release_calls == 1u && audio_forget_calls == 3u);
    assert(decode_reclaim_calls == 0u && app_decode_reclaim_calls == 2u);
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    assert(zzapp_host_info(&info) == AAR_OK && info.shared_used == 0u);

    zzapp_loader_release();
    zzapp_boot_init();
    assert(zzapp_loader_begin() == AAR_OK);
    api = zzapp_api_table();
    zzapp_test_mark_running();
    stop_during_host_alloc = 0;
    stop_during_image_begin = 1;
    image_poison_session_calls = 0u;
    image_poisoned_close_calls = 0u;
    assert(api->image_open(&image, &image_session, &image_result) == AAR_OK);
    assert(zzapp_runtime_state() == ZZAPP_ST_RUNNING);
    stop_during_image_begin = 0;
    stop_during_host_alloc = 1;
    zzapp_host_tick_at(1u);
    zzapp_worker_hook();
    zzapp_host_tick_at(1u + ZZAPP_STOP_GRACE_US);
    zzapp_worker_hook();
    assert(zzapp_runtime_state() == ZZAPP_ST_FAULTED);
    assert(image_poison_session_calls == 1u);
    assert(image_poison_calls == 0u);
    return 0;
}
