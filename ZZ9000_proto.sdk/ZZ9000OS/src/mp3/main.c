#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "mp3.h"
// #include "samplerate.h"

int main(int argc, char **argv) {
    // just for testing the decode_mp3 interface

    // read test input file
    char * input_file_name = "test.mp3";
    FILE * input_file = fopen(input_file_name, "r");
    struct stat st;
    fstat(fileno(input_file), &st);
    int buffer_size = st.st_size;
    char * input_buffer = malloc(buffer_size);
    fread(input_buffer, sizeof(char), buffer_size, input_file);
    fclose(input_file);

    // decode mp3 test input
    char * output_buffer = decode_mp3(input_buffer, &buffer_size);
    free(input_buffer);
    if (output_buffer == NULL) {
      exit(1);
    }

    // // decode mp3 test input
    // char * moutput_buffer = decode_mp3(input_buffer, &buffer_size);
    // free(input_buffer);
    // if (moutput_buffer == NULL) {
    //   exit(1);
    // }
    // SRC_DATA srcdata;
    // srcdata.data_in = (float *) moutput_buffer;
    // long n_input_frames = 14400000;
    // srcdata.input_frames = n_input_frames;
    // char * output_buffer = malloc(n_input_frames * 2 * 4);
    // srcdata.data_out = output_buffer;
    // srcdata.output_frames = n_input_frames / 2;
    // srcdata.src_ratio = 441 / 480;
    // int error = src_simple(&srcdata, SRC_LINEAR, 2);
    // printf("DEBUG %s", src_strerror(error));
    // printf("DEBUG %d %d %d", srcdata.input_frames_used, srcdata.output_frames_gen, srcdata.end_of_input);

    // write decoder output to file out.raw
    // (play with something like: ffplay -f s16le -ar 48k -ac 2 out.raw)
    FILE * output_file = fopen("out.raw", "w");
    fwrite(output_buffer, buffer_size, 1, output_file);
    fclose(output_file);
    free(output_buffer);
}
