/*
 * Resumable MP3 drain probe regression.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <string.h>

#include "mp3/mp3.h"

#define MPEG1_L3_128K_44100_FRAME_BYTES 417

int main(void)
{
	mp3dec_t decoder;
	mp3dec_t before;
	uint8_t frame[MPEG1_L3_128K_44100_FRAME_BYTES + 3];
	int decode_input_size;

	memset(&decoder, 0, sizeof(decoder));
	memset(frame, 0, sizeof(frame));
	/* MPEG-1 Layer III, 128 kbit/s, 44.1 kHz, stereo. */
	frame[0] = 0xffU;
	frame[1] = 0xfbU;
	frame[2] = 0x90U;
	frame[3] = 0x00U;
	before = decoder;

	if (mp3_probe_complete_frame(&decoder, frame,
	                             MPEG1_L3_128K_44100_FRAME_BYTES / 2,
	                             &decode_input_size))
		return 1;
	if (memcmp(&decoder, &before, sizeof(decoder)) != 0)
		return 2;
	if (!mp3_probe_complete_frame(&decoder, frame,
	                              MPEG1_L3_128K_44100_FRAME_BYTES,
	                              &decode_input_size) ||
	    decode_input_size != MPEG1_L3_128K_44100_FRAME_BYTES)
		return 3;
	if (memcmp(&decoder, &before, sizeof(decoder)) != 0)
		return 4;
	frame[MPEG1_L3_128K_44100_FRAME_BYTES] = 0xffU;
	frame[MPEG1_L3_128K_44100_FRAME_BYTES + 1] = 0xfbU;
	frame[MPEG1_L3_128K_44100_FRAME_BYTES + 2] = 0x90U;
	for (decode_input_size = 1; decode_input_size <= 3;
	     decode_input_size++) {
		int span = 0;
		if (!mp3_probe_complete_frame(
		        &decoder, frame,
		        MPEG1_L3_128K_44100_FRAME_BYTES + decode_input_size,
		        &span) ||
		    span != MPEG1_L3_128K_44100_FRAME_BYTES)
			return 4 + decode_input_size;
		if (memcmp(&decoder, &before, sizeof(decoder)) != 0)
			return 8 + decode_input_size;
	}
	return 0;
}
