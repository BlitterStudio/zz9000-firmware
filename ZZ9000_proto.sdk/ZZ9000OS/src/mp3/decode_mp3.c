#include <stdio.h>
#include "mp3.h"
#define MINIMP3_IMPLEMENTATION 1
#include "minimp3_ex.h"

static mp3dec_ex_t mp3d;
static mp3dec_frame_info_t frame_info;

int decode_mp3_samples(void* output_buffer, int max_samples) {
	int max_bytes = max_samples * 2;
	int out_offset = 0;
	int total_bytes_decoded = 0;

	//printf("[mp3] out_offset: %d max_bytes: %d\n", out_offset, max_bytes);

	// this will point into mp3d->buffer, which is defined on the stack
	// as mp3d_sample_t buffer[MINIMP3_MAX_SAMPLES_PER_FRAME]
	mp3d_sample_t * pcm_buffer = NULL;

	while (1) {
		size_t read_samples = mp3dec_ex_read_frame(&mp3d, &pcm_buffer, &frame_info, max_samples);
		max_samples -= read_samples;

		int bytes_decoded = read_samples * sizeof(mp3d_sample_t);
		total_bytes_decoded += bytes_decoded;

		//printf("[mp3] decoded: %d bytes\n", bytes_decoded);

		if (bytes_decoded > 0) {
			int bytes_to_copy = bytes_decoded;
			memcpy(output_buffer + out_offset, pcm_buffer, bytes_to_copy);
			out_offset += bytes_decoded;

			if (out_offset >= max_bytes) {
				break;
			}
		} else {
			break;
		}
	}

	return total_bytes_decoded;
}

int decode_mp3_init(uint8_t* input_buffer, size_t input_buffer_size) {
	memset(&frame_info, 0, sizeof(frame_info));

	// sets up input_buffer as mp3d->file.buffer
	int ret = mp3dec_ex_open_buf(&mp3d, input_buffer, input_buffer_size, MP3D_DO_NOT_SCAN);
	if (ret) {
		printf("mp3dec_ex_open_buf failed: %d\n", ret);
	}
	return ret;
}

int mp3_get_hz() {
	return mp3d.info.hz;
}

int mp3_get_channels() {
	return mp3d.info.channels;
}
