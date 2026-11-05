/*
 * Stereo square-wave tone. It uses the normal zzapp_audio ring, so the
 * launcher-selected card-fabric and AHI routes need no app-side changes.
 *
 * SPDX-License-Identifier: MIT
 */
#include "zzapp.h"

#define TONE_RATE 22050u
#define TONE_HZ 440u

int zz_main(int argc, char **argv)
{
    zzapp_audio audio;
    uint32_t phase = 0u;

    (void)argc;
    (void)argv;
    if (zz_audio_open(TONE_RATE, 2u, &audio) != ZZ_OK)
        return 20;
    while (!zz_should_stop()) {
        uint32_t read = *audio.read_frame;
        uint32_t write = *audio.write_frame;
        uint32_t used = write - read;
        uint32_t frames;

        if (used > audio.frames)
            break;
        frames = audio.frames - used;
        if (frames > 128u)
            frames = 128u;
        if (frames == 0u) {
            zz_yield();
            continue;
        }
        while (frames--) {
            int16_t value = phase < TONE_RATE / (2u * TONE_HZ) ? 12000 : -12000;
            uint32_t slot = write & (audio.frames - 1u);

            audio.samples[slot * 2u] = value;
            audio.samples[slot * 2u + 1u] = value;
            write++;
            phase++;
            if (phase == TONE_RATE / TONE_HZ)
                phase = 0u;
        }
        *audio.write_frame = write;
        zz_yield();
    }
    (void)zz_audio_close();
    return 0;
}
