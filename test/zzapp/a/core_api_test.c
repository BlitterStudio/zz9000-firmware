/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "sdk_audio_flac.h"
#include "sdk_audio_vorbis.h"
#include "sdk_image_stream.h"
#include "zzapp.h"
#include "zzapp_runtime.h"
int zzapp_host_mem_info(zzapp_mem_info *out);

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
static int stop_during_host_alloc;
static int stop_during_image_begin;
int zzapp_svc_should_stop(void)
{
    return stop_during_host_alloc || stop_during_image_begin;
}

void zzapp_test_host_visible_allocated(void)
{
    zzapp_host_tick_at(1u);
    zzapp_host_tick_at(1u + ZZAPP_STOP_GRACE_US);
}
int zzapp_svc_file_stat(const char *path, zzapp_stat *out)
{
    if (!path || strcmp(path, "RAM:fixture") != 0 || !out)
        return ZZ_ENOENT;
    out->size = sizeof(file_data) - 1u;
    out->mtime = 0u;
    out->mode = 1u;
    return ZZ_OK;
}

int zzapp_svc_file_open(const char *path, uint32_t flags)
{
    return path && strcmp(path, "RAM:fixture") == 0 &&
           flags == ZZ_FILE_READ ? 1 : ZZ_ENOENT;
}

int zzapp_svc_file_read(int fd, void *dst, uint32_t bytes, uint32_t *done)
{
    assert(fd == 1);
    assert(bytes >= sizeof(file_data) - 1u);
    memcpy(dst, file_data, sizeof(file_data) - 1u);
    *done = sizeof(file_data) - 1u;
    return ZZ_OK;
}

int zzapp_svc_file_close(int fd)
{
    assert(fd == 1);
    return ZZ_OK;
}

uint16_t sdk_image_stream_begin(const struct SDKImageStreamBegin *begin,
                                struct SDKImageStreamResult *out)
{
    assert(begin->core1_affine == 1u);
    assert(begin->direct_arm_local == 1u);
    assert(begin->output_format == SDK_SURFACE_FORMAT_BGRA8888);
    image_begin_calls++;
    if (stop_during_image_begin) {
        zzapp_host_tick_at(1u);
        zzapp_host_tick_at(1u + ZZAPP_STOP_GRACE_US);
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

unsigned sdk_compression_reclaim_app_decode(void)
{
    app_decode_reclaim_calls++;
    return 1u;
}

int main(void)
{
    const zzapp_api *api;
    zzapp_mem_info mem;
    zzapp_image_open image;
    zzapp_image_result image_result;
    zzapp_audio_decode_result audio_result;
    const void *mapped;
    uint32_t bytes;
    uint32_t image_session;
    uint32_t audio_session;
    uint8_t source[8] = { 0 };
    uint8_t mp3[834] = { 0 };
    int16_t pcm[8];
    int16_t mp3_pcm[2304];
    void *host_visible;

    zzapp_boot_init();
    assert(zzapp_loader_begin() == ZZ_OK);
    api = zzapp_api_table();

    assert(api->map_file("RAM:fixture", &mapped, &bytes) == ZZ_OK);
    assert(bytes == sizeof(file_data) - 1u);
    assert(memcmp(mapped, file_data, bytes) == 0);
    assert(api->mem_info(&mem) == ZZ_OK);
    assert(mem.host_visible_used == bytes);
    api->unmap_file(mapped);
    assert(api->mem_info(&mem) == ZZ_OK);
    assert(mem.host_visible_used == 0u);
    assert(api->map_file("RAM:fixture", &mapped, &bytes) == ZZ_OK);
    api->unmap_file(mapped);

    host_visible = api->alloc(4096u, 8u, ZZ_ALLOC_HOST_VISIBLE);
    assert(host_visible != NULL);
    assert(api->mem_info(&mem) == ZZ_OK && mem.host_visible_used == 4096u);

    memset(&image, 0, sizeof(image));
    image.codec = ZZ_IMAGE_PNG;
    image.output_format = ZZ_FMT_BGRA8888;
    image.tile = source;
    image.stride = 4u;
    image.rows = 2u;
    image.tile_bytes = sizeof(source);
    assert(api->image_open(&image, &image_session, &image_result) == ZZ_OK);
    assert(api->image_decode(image_session, source, sizeof(source), 0u,
                             &image_result) == ZZ_OK);
    assert(api->image_close(image_session) == ZZ_OK);
    assert(image_begin_calls == 1u && image_feed_calls == 1u &&
           image_close_calls == 1u);
    assert(api->image_open(&image, &image_session, &image_result) == ZZ_OK);

    mp3[0] = 0xffu; mp3[1] = 0xfbu; mp3[2] = 0x90u; mp3[3] = 0x64u;
    mp3[417] = 0xffu; mp3[418] = 0xfbu;
    mp3[419] = 0x90u; mp3[420] = 0x64u;
    assert(api->audio_decode_open(ZZ_AUDIO_MP3, &audio_session) == ZZ_OK);
    assert(api->audio_decode(audio_session, mp3, sizeof(mp3), 0u, mp3_pcm,
                             1152u, &audio_result) == ZZ_OK);
    assert(audio_result.bytes_consumed == 417u);
    assert(audio_result.frames_written == 1152u);
    assert(audio_result.rate == 44100u && audio_result.channels == 2u);
    assert(api->audio_decode_close(audio_session) == ZZ_OK);

    assert(api->audio_decode_open(ZZ_AUDIO_FLAC, &audio_session) == ZZ_OK);
    assert(api->audio_decode(audio_session, source, sizeof(source), 0u, pcm,
                             2u, &audio_result) == ZZ_OK);
    assert(audio_result.bytes_consumed == sizeof(source));
    assert(audio_result.frames_written == 1u);
    assert(audio_result.rate == 44100u && audio_result.channels == 2u);
    assert(api->audio_decode(audio_session, NULL, 0u,
                             ZZ_AUDIO_DECODE_EOF | ZZ_AUDIO_DECODE_DRAIN,
                             pcm, 2u, &audio_result) == ZZ_OK);
    assert(api->audio_decode_close(audio_session) == ZZ_OK);
    assert(api->audio_decode_open(ZZ_AUDIO_VORBIS, &audio_session) == ZZ_OK);

    /* The host allocator must publish its ledger entry before a stop tick
     * can unwind it; host-only seam because ARM entry/FPSCR cannot run here. */
    stop_during_host_alloc = 1;
    assert(api->alloc(64u, 8u, ZZ_ALLOC_HOST_VISIBLE) != NULL);
    assert(api->mem_info(&mem) == ZZ_OK && mem.host_visible_used == 4160u);
    zzapp_test_mark_running();
    zzapp_host_tick_at(1u);
    zzapp_host_tick_at(1u + ZZAPP_STOP_GRACE_US);
    assert(zzapp_runtime_state() == ZZAPP_ST_FAULTED);
    assert(image_close_calls == 1u && image_poisoned_close_calls == 1u);
    assert(image_poison_calls == 0u && image_poison_session_calls == 1u);
    assert(audio_release_calls == 1u && audio_forget_calls == 1u);
    assert(decode_reclaim_calls == 0u && app_decode_reclaim_calls == 1u);
    assert(zzapp_host_mem_info(&mem) == ZZ_OK && mem.host_visible_used == 0u);

    zzapp_loader_release();
    zzapp_boot_init();
    assert(zzapp_loader_begin() == ZZ_OK);
    api = zzapp_api_table();
    zzapp_test_mark_running();
    stop_during_host_alloc = 0;
    stop_during_image_begin = 1;
    image_poison_session_calls = 0u;
    image_poisoned_close_calls = 0u;
    assert(api->image_open(&image, &image_session, &image_result) == ZZ_OK);
    assert(zzapp_runtime_state() == ZZAPP_ST_RUNNING);
    stop_during_image_begin = 0;
    stop_during_host_alloc = 1;
    zzapp_host_tick_at(1u);
    zzapp_host_tick_at(1u + ZZAPP_STOP_GRACE_US);
    assert(zzapp_runtime_state() == ZZAPP_ST_FAULTED);
    assert(image_poison_session_calls == 1u);
    assert(image_poison_calls == 0u);
    return 0;
}
