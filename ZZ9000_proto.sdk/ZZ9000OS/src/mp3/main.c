#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "mp3.h"
#define OUTPUT_BUFFER_SIZE 100 * 1024 * 1024

int main(int argc, char **argv) {
    // just for testing the decode_mp3 interface

    // read test input file
    char * input_file_name = "test.mp3";
    FILE * input_file = fopen(input_file_name, "r");
    struct stat st;
    fstat(fileno(input_file), &st);
    size_t input_buffer_size = st.st_size;
    char * input_buffer = malloc(input_buffer_size);
    fread(input_buffer, sizeof(char), input_buffer_size, input_file);
    fclose(input_file);

    // decode mp3 test input
    char * output_buffer = malloc(OUTPUT_BUFFER_SIZE);
    if (!output_buffer) {
      printf("memory allocation for mp3 decoder output failed");
      exit(1);
    }
    int bytes_decoded = decode_mp3(input_buffer, input_buffer_size, output_buffer, OUTPUT_BUFFER_SIZE);
    if (bytes_decoded < 0) {
      exit(bytes_decoded);
    }
    free(input_buffer);

    // write decoder output to file out.raw
    // (play with something like: ffplay -f s16le -ar 48k -ac 2 out.raw)
    FILE * output_file = fopen("out.raw", "w");
    fwrite(output_buffer, bytes_decoded, 1, output_file);
    fclose(output_file);
    free(output_buffer);
}
