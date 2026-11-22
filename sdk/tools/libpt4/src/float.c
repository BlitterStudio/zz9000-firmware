/* libpt4 float output, kept apart so the integer core links without floating point. zlib license, see LICENSE. */
#include "pt4_internal.h"

#define BLOCK 128

size_t pt4_render_f32(pt4_song *s, float *out, size_t frames, unsigned rate)
{
    int32_t buf[2 * BLOCK];
    size_t done = 0, i;
    if (!s || !out) return 0;
    while (done < frames) {
        size_t want = frames - done, got;
        if (want > BLOCK) want = BLOCK;
        got = pt4__render(s, buf, NULL, want, rate);
        for (i = 0; i < 2 * got; i++) {
            float x = (float)buf[i] * (1.0f / 32768.0f);
            if (x > 1.0f) x = 1.0f;
            if (x < -1.0f) x = -1.0f;
            out[2 * done + i] = x;
        }
        done += got;
        if (got < want) break;
    }
    return done;
}

