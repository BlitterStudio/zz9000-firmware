#include <stdio.h>
#include "mp3.h"
#define MINIMP3_IMPLEMENTATION 1
#include "minimp3_ex.h"

// TODO: package all minimp3 dependencies in this file
// TODO: enforce output format 16 bit signed integer samples, stereo interleaved, 48000Hz (first step: check if minimp3 can do all the necessary conversions by itself; for re-sampling consider something like libsamplerate?)
// TODO: adapt decode_mp3 interface to more specific requirements ("call a decode_mp3() function via a ZZ9000 register giving source and dest (uint32_t) pointers")
// TODO: status register where Amiga can pull result (OK/Error/busy)

int decode_mp3(unsigned char * input_buffer, size_t input_buffer_size, unsigned char * output_buffer, size_t output_buffer_size) {
	mp3dec_ex_t mp3d;
	mp3dec_frame_info_t frame_info;
	memset(&frame_info, 0, sizeof(frame_info));
	// // sets up input_buffer as mp3d->file.buffer
	int ret = mp3dec_ex_open_buf(&mp3d, input_buffer, input_buffer_size, MP3D_DO_NOT_SCAN);
	if (ret) {
		printf("mp3dec_ex_open_buf failed: %d\n", ret);
		exit(ret);
	}
	size_t offset_new_samples = 0;
	int max_samples = UINT_MAX;
	memset(output_buffer, 0, output_buffer_size);
	while (1) {
		// this will point into mp3d->buffer, which is defined on the stack
		// as mp3d_sample_t buffer[MINIMP3_MAX_SAMPLES_PER_FRAME]
		mp3d_sample_t * pcm_buffer = NULL;
		size_t read_samples = mp3dec_ex_read_frame(&mp3d, &pcm_buffer, &frame_info, max_samples);
		if (!read_samples) {
			break;
		}
		if (offset_new_samples + (read_samples * sizeof(mp3d_sample_t)) >= output_buffer_size) {
			break;
		}
		size_t new_frame_size = read_samples * sizeof(mp3d_sample_t);
		memcpy(output_buffer + offset_new_samples, pcm_buffer, new_frame_size);
		offset_new_samples += read_samples * sizeof(mp3d_sample_t);
	}
	return offset_new_samples;
}
