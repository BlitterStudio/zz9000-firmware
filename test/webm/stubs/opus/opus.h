/*
 * Host-test stand-in for the libopus decoder API used by sdk_video_webm.c.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef STUB_OPUS_H
#define STUB_OPUS_H

#include <stdint.h>

typedef int32_t opus_int32;
typedef int16_t opus_int16;
typedef struct OpusDecoder OpusDecoder;

#define OPUS_OK 0

OpusDecoder *opus_decoder_create(opus_int32 fs, int channels, int *error);
void opus_decoder_destroy(OpusDecoder *st);
int opus_decode(OpusDecoder *st, const unsigned char *data, opus_int32 len,
		opus_int16 *pcm, int frame_size, int decode_fec);
int opus_packet_get_nb_samples(const unsigned char *packet, opus_int32 len,
			       opus_int32 fs);

#endif
