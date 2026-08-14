/*
 * MPEG Layer II coverage for the streaming decoder used by mpega.library.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <string.h>

#include "mp3/mp3_backend_config.h"

#define MINIMP3_IMPLEMENTATION 1
#include "mp3/minimp3.h"

#define MPEG1_L2_128K_44100_FRAME_BYTES 417

int main(void)
{
	mp3dec_t decoder;
	mp3dec_frame_info_t info;
	mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
	uint8_t frame[MPEG1_L2_128K_44100_FRAME_BYTES];
	int samples;

	memset(frame, 0, sizeof(frame));
	/* MPEG-1 Layer II, 128 kbit/s, 44.1 kHz, stereo. */
	frame[0] = 0xffU;
	frame[1] = 0xfdU;
	frame[2] = 0x80U;
	frame[3] = 0x00U;
	mp3dec_init(&decoder);
	memset(&info, 0, sizeof(info));
	samples = mp3dec_decode_frame(
		&decoder, frame, sizeof(frame), pcm, &info);
	if (samples != 1152)
		return 1;
	if (info.frame_bytes != MPEG1_L2_128K_44100_FRAME_BYTES ||
	    info.layer != 2 || info.hz != 44100 || info.channels != 2)
		return 2;
	return 0;
}
