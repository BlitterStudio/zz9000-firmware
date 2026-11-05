/* API 0.2 launcher-window wire contract. */
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "zzapp_proto.h"

static uint8_t window[ZZAPP_WINDOW_BYTES];

static uint32_t ring_space(uint32_t head, uint32_t tail, uint32_t capacity)
{
    assert(head - tail <= capacity);
    return capacity - (head - tail);
}

int main(void)
{
    uint8_t *out = window + ZZAPP_CONSOLE_OUT_OFF;
    uint8_t *in = window + ZZAPP_CONSOLE_IN_OFF;
    uint8_t *h2a = window + ZZAPP_MSG_H2A_OFF;
    uint8_t *a2h = window + ZZAPP_MSG_A2H_OFF;
    uint8_t *audio = window + ZZAPP_AUDIO_OFF;
    uint32_t i;

    assert(ZZAPP_WINDOW_BYTES > 0x60000u);
    assert(ZZAPP_CONSOLE_OUT_OFF + ZZAPP_CONSOLE_OUT_BYTES == ZZAPP_CONSOLE_IN_OFF);
    assert(ZZAPP_CONSOLE_IN_OFF + ZZAPP_CONSOLE_IN_BYTES == ZZAPP_AUDIO_OFF);
    assert(ZZAPP_AUDIO_OFF + ZZAPP_AUDIO_BYTES == ZZAPP_MSG_H2A_OFF);
    assert(ZZAPP_MSG_H2A_OFF + ZZAPP_MSG_BYTES == ZZAPP_MSG_A2H_OFF);
    assert(ZZAPP_MSG_A2H_OFF + ZZAPP_MSG_BYTES == ZZAPP_WINDOW_BYTES);

    zzapp_put_be32(out, ZZAPP_CONSOLE_OUT_CAP);
    zzapp_put_be32(out + 4, 0u);
    assert(ring_space(zzapp_get_be32(out), zzapp_get_be32(out + 4),
                      ZZAPP_CONSOLE_OUT_CAP) == 0u);
    zzapp_put_be32(out + 4, 1u);
    assert(ring_space(zzapp_get_be32(out), zzapp_get_be32(out + 4),
                      ZZAPP_CONSOLE_OUT_CAP) == 1u);

    zzapp_put_be32(in, 3u);
    zzapp_put_be32(in + 4, 0u);
    in[ZZAPP_CONSOLE_HDR] = 'x';
    in[ZZAPP_CONSOLE_HDR + 1u] = 'y';
    in[ZZAPP_CONSOLE_HDR + 2u] = 'z';
    zzapp_put_be32(in + ZZAPP_CONSOLE_EOF_OFF, 1u);
    assert(zzapp_get_be32(in) - zzapp_get_be32(in + 4) == 3u);
    assert(zzapp_get_be32(in + ZZAPP_CONSOLE_EOF_OFF) == 1u);

    zzapp_put_be32(audio + ZZAPP_AUDIO_HEAD_OFF, 8u);
    zzapp_put_be32(audio + ZZAPP_AUDIO_TAIL_OFF, 4u);
    zzapp_put_be32(audio + ZZAPP_AUDIO_RATE_OFF, 44100u);
    zzapp_put_be32(audio + ZZAPP_AUDIO_CHANNELS_OFF, 2u);
    zzapp_put_be32(audio + ZZAPP_AUDIO_FORMAT_OFF, ZZAPP_AUDIO_FORMAT_S16LE);
    zzapp_put_be32(audio + ZZAPP_AUDIO_ROUTE_OFF, ZZAPP_AUDIO_ROUTE_AHI);
    assert(zzapp_get_be32(audio + ZZAPP_AUDIO_HEAD_OFF) -
           zzapp_get_be32(audio + ZZAPP_AUDIO_TAIL_OFF) == 4u);
    assert(zzapp_get_be32(audio + ZZAPP_AUDIO_ROUTE_OFF) == ZZAPP_AUDIO_ROUTE_AHI);

    for (i = 0u; i < ZZAPP_MSG_BYTES_PER; i++)
        h2a[ZZAPP_MSG_HDR + i] = (uint8_t)i;
    zzapp_put_be32(h2a, 1u);
    zzapp_put_be32(h2a + 4, 0u);
    memcpy(a2h + ZZAPP_MSG_HDR, h2a + ZZAPP_MSG_HDR, ZZAPP_MSG_BYTES_PER);
    zzapp_put_be32(a2h, 1u);
    zzapp_put_be32(a2h + 4, 0u);
    assert(memcmp(h2a + ZZAPP_MSG_HDR, a2h + ZZAPP_MSG_HDR,
                  ZZAPP_MSG_BYTES_PER) == 0);
    assert(zzapp_get_be32(a2h) - zzapp_get_be32(a2h + 4) == 1u);
    return 0;
}
