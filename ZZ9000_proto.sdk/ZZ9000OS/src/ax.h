#include <stdint.h>

int audio_adau_init(int program_dsp);
void audio_init_i2s();
void isr_audio(void *dummy);
void isr_audio_rx(void *dummy);
void audio_set_interrupt_enabled(int en);
void audio_clear_interrupt();
uint32_t audio_get_interrupt();
uint32_t audio_get_dma_transfer_count();
int audio_swab(uint16_t audio_buf_samples, uint32_t offset, int byteswap);
void audio_set_tx_buffer(uint8_t* addr);
void audio_set_rx_buffer(uint8_t* addr);
void resample_s16(int16_t *input, int16_t *output,
		int in_sample_rate, int out_sample_rate, int output_samples);
void audio_silence();
void audio_debug_timer(int zdata);

void audio_program_adau(u8* program, u32 program_len);
void audio_program_adau_params(u8* params, u32 param_len);
void audio_adau_set_lpf_params(int f0);

// vol range: 0-100. 50 = 0db
void audio_adau_set_mixer_vol(int vol1, int vol2);
