#include <stdio.h>
#include "mp3.h"
#define MINIMP3_IMPLEMENTATION 1
#include "minimp3_ex.h"

// TODO: package all minimp3 dependencies in this file
// TODO: enforce output format 16 bit signed integer samples, stereo interleaved, 48000Hz (first step: check if minimp3 can do all the necessary conversions by itself; for re-sampling consider something like libsamplerate?)
// TODO: adapt decode_mp3 interface to more specific requirements ("call a decode_mp3() function via a ZZ9000 register giving source and dest (uint32_t) pointers")
// TODO: status register where Amiga can pull result (OK/Error/busy)

void * decode_mp3(char * input_buffer, int * buffer_size) {
    mp3dec_ex_t mp3d;
    mp3dec_frame_info_t frame_info;
    memset(&frame_info, 0, sizeof(frame_info));
    int ret = mp3dec_ex_open_buf(&mp3d, input_buffer, *buffer_size, 0);
    if (ret) {
      printf("mp3dec_ex_open_buf failed: %d\n", ret);
      exit(ret);
    }
    mp3d_sample_t * output_buffer = NULL;
    size_t offset_new_samples = 0;
    size_t output_buffer_size = 0;
    int max_samples = UINT_MAX;
    while (1) {
        mp3d_sample_t * frame_buffer = NULL;
        size_t read_samples = mp3dec_ex_read_frame(&mp3d, &frame_buffer, &frame_info, max_samples);
        if (!read_samples) {
            break;
        }
        size_t new_frame_size = read_samples * sizeof(mp3d_sample_t);
        output_buffer_size += new_frame_size;
        output_buffer = realloc(output_buffer, output_buffer_size);
        if (!output_buffer) {
          printf("memory allocation for mp3 decoder output failed");
          exit(1);
        }
        memcpy(output_buffer + offset_new_samples, frame_buffer, new_frame_size);
        offset_new_samples += read_samples;
    }
    *buffer_size = output_buffer_size;
    return output_buffer;
}
