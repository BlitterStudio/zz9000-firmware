/*
 * Stereo S16LE ring, 8192 frames, 22050 Hz only. That is the rate the card
 * fabric producer grants. Other rates and channel counts are ZZ_EINVAL.
 *
 * Without --audio-null the SDL callback advances read_frame. With
 * --audio-null, yield consumes written frames on the app thread, which is
 * what the tone test counts. --audio-frames N stops after N consumed frames.
 * --audio-dump records those consumed frames.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include "host_internal.h"

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct host_audio {
    int open;
    int16_t *samples;
    volatile uint32_t *read_frame;
    volatile uint32_t *write_frame;
    void *block;
    FILE *dump;
    uint32_t consumed;
    SDL_AudioDeviceID dev;
};

static struct host_audio audio;

static void consume(uint32_t frames, int dump)
{
    uint32_t i;
    uint32_t read;

    if (!frames || !audio.read_frame)
        return;
    read = *audio.read_frame;
    if (dump && audio.dump) {
        for (i = 0; i < frames; i++) {
            uint32_t slot = (read + i) & (HOST_AUDIO_FRAMES - 1u);
            fwrite(audio.samples + slot * 2u, sizeof(int16_t), 2, audio.dump);
        }
    }
    *audio.read_frame = read + frames;
    audio.consumed += frames;
    if (host_options()->audio_frames &&
        audio.consumed >= host_options()->audio_frames)
        host_request_stop();
}

static void audio_cb(void *userdata, Uint8 *stream, int len)
{
    uint32_t frames;
    uint32_t read;
    uint32_t write;
    uint32_t avail;
    uint32_t i;

    (void)userdata;
    if (len < 0)
        len = 0;
    memset(stream, 0, (size_t)len);
    if (!audio.open || !audio.read_frame || !audio.write_frame)
        return;
    frames = (uint32_t)len / 4u;
    read = *audio.read_frame;
    write = *audio.write_frame;
    avail = write - read;
    if (avail > HOST_AUDIO_FRAMES)
        avail = 0;
    if (avail > frames)
        avail = frames;
    for (i = 0; i < avail; i++) {
        uint32_t slot = (read + i) & (HOST_AUDIO_FRAMES - 1u);
        memcpy(stream + i * 4u, audio.samples + slot * 2u, 4u);
    }
    consume(avail, 0);
}

static int api_audio_open(uint32_t rate, uint32_t channels, zzapp_audio *out)
{
    const struct host_options *opt = host_options();
    size_t bytes;
    SDL_AudioSpec want, have;

    if (!out)
        return ZZ_EINVAL;
    if (channels != 2u || rate != HOST_AUDIO_RATE)
        return ZZ_EINVAL;
    if (audio.open)
        return ZZ_EBUSY;
    bytes = sizeof(uint32_t) * 2u + HOST_AUDIO_FRAMES * 2u * sizeof(int16_t);
    audio.block = calloc(1, bytes);
    if (!audio.block)
        return ZZ_ENOMEM;
    audio.read_frame = (volatile uint32_t *)audio.block;
    audio.write_frame = audio.read_frame + 1;
    audio.samples = (int16_t *)(audio.read_frame + 2);
    if (!opt->audio_null) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            free(audio.block);
            audio.block = 0;
            return ZZ_EIO;
        }
        memset(&want, 0, sizeof want);
        want.freq = (int)HOST_AUDIO_RATE;
        want.format = AUDIO_S16LSB;
        want.channels = 2;
        want.samples = 1024;
        want.callback = audio_cb;
        audio.dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (!audio.dev) {
            free(audio.block);
            audio.block = 0;
            return ZZ_EIO;
        }
        SDL_PauseAudioDevice(audio.dev, 0);
    }
    audio.open = 1;
    memset(out, 0, sizeof *out);
    out->samples = audio.samples;
    out->frames = HOST_AUDIO_FRAMES;
    out->read_frame = audio.read_frame;
    out->write_frame = audio.write_frame;
    out->rate = HOST_AUDIO_RATE;
    out->channels = 2u;
    return ZZ_OK;
}

static int api_audio_close(void)
{
    if (!audio.open)
        return ZZ_EINVAL;
    if (audio.dev) {
        SDL_LockAudioDevice(audio.dev);
        SDL_PauseAudioDevice(audio.dev, 1);
        SDL_UnlockAudioDevice(audio.dev);
        SDL_CloseAudioDevice(audio.dev);
        audio.dev = 0;
    }
    audio.open = 0;
    free(audio.block);
    audio.block = 0;
    audio.samples = 0;
    audio.read_frame = 0;
    audio.write_frame = 0;
    return ZZ_OK;
}

void host_audio_pump(void)
{
    const struct host_options *opt = host_options();
    uint32_t read;
    uint32_t write;
    uint32_t avail;

    if (!audio.open || !opt->audio_null || !audio.read_frame)
        return;
    read = *audio.read_frame;
    write = *audio.write_frame;
    avail = write - read;
    if (avail > HOST_AUDIO_FRAMES)
        return;
    if (opt->audio_frames && audio.consumed < opt->audio_frames) {
        uint32_t room = opt->audio_frames - audio.consumed;
        if (avail > room)
            avail = room;
    } else if (opt->audio_frames) {
        avail = 0;
    }
    consume(avail, 1);
}

uint32_t host_audio_frames_consumed(void)
{
    return audio.consumed;
}

int host_audio_init(void)
{
    const struct host_options *opt = host_options();

    memset(&audio, 0, sizeof audio);
    if (!opt->audio_dump)
        return 0;
    audio.dump = fopen(opt->audio_dump, "wb");
    if (!audio.dump) {
        fprintf(stderr, "zzapp-host: cannot open %s\n", opt->audio_dump);
        return -1;
    }
    return 0;
}

void host_audio_shutdown(void)
{
    if (audio.open)
        api_audio_close();
    if (audio.dump) {
        fclose(audio.dump);
        audio.dump = 0;
    }
}

void host_audio_bind(zzapp_api *api)
{
    api->audio_open = api_audio_open;
    api->audio_close = api_audio_close;
}
