int decode_mp3_init(uint8_t* input_buffer, size_t input_buffer_size);
int decode_mp3_samples(void* output_buffer, int max_samples);
int mp3_get_hz();
int mp3_get_channels();
