#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "mp3.h"

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

    // write decoder output to file out.raw
    // (play with something like: ffplay -f s16le -ar 48k -ac 2 out.raw)
    FILE * output_file = fopen("out.raw", "w");
    fwrite(output_buffer, buffer_size, 1, output_file);
    fclose(output_file);
    free(output_buffer);
}
