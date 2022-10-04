typedef enum {
	DECODE_CLEAR,
	DECODE_INIT,
	DECODE_RUN
} DECODE_COMMAND;

int decode_mp3_init(uint8_t* input_buffer, size_t input_buffer_size);
int decode_mp3_init_fifo(uint8_t* input_buffer, size_t input_buffer_size);
void fifo_clear(void);
void fifo_set_write_index(unsigned short aWriteIndex);
unsigned short fifo_get_read_index(void);
int decode_mp3_samples(void* output_buffer, int max_samples);
int mp3_get_hz();
int mp3_get_channels();
