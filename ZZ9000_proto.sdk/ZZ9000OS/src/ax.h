#include <stdint.h>

int audio_adau_init(int program_dsp);
void isr_audio(void *dummy);
void isr_audio_rx(void *dummy);
void audio_set_interrupt_enabled(int en);
void audio_clear_interrupt();
uint32_t audio_get_interrupt();
uint32_t audio_get_dma_transfer_count();
int audio_swab(int audio_buf_samples, uint32_t offset, int byteswap);
void audio_set_tx_buffer(uint8_t* addr);
void audio_set_rx_buffer(uint8_t* addr);
uint32_t resample_s16(int16_t *input, int16_t *output, int inSampleRate, int outSampleRate, uint32_t inputSize);
void audio_silence();
