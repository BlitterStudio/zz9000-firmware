#ifndef __AX_H__
#define __AX_H__

#include <stdint.h>

enum {
	AP_TX_BUF_OFFS_HI,
	AP_TX_BUF_OFFS_LO,
	AP_RX_BUF_OFFS_HI,
	AP_RX_BUF_OFFS_LO,
	AP_DSP_PROG_OFFS_HI,
	AP_DSP_PROG_OFFS_LO,
	AP_DSP_PARAM_OFFS_HI,
	AP_DSP_PARAM_OFFS_LO,
	AP_DSP_UPLOAD,
	AP_DSP_SET_LOWPASS,
	AP_DSP_SET_VOLUMES,
	AP_DSP_SET_PREFACTOR,
	AP_DSP_SET_EQ_BAND1,
	AP_DSP_SET_EQ_BAND2,
	AP_DSP_SET_EQ_BAND3,
	AP_DSP_SET_EQ_BAND4,
	AP_DSP_SET_EQ_BAND5,
	AP_DSP_SET_EQ_BAND6,
	AP_DSP_SET_EQ_BAND7,
	AP_DSP_SET_EQ_BAND8,
	AP_DSP_SET_EQ_BAND9,
	AP_DSP_SET_EQ_BAND10,
	AP_DSP_SET_STEREO_VOLUME,
	ZZ_NUM_AUDIO_PARAMS
};

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

// vol range: 0-255. 127 = 0db
void audio_adau_set_mixer_vol(int vol1, int vol2);

// gain range: 0 = -12dB .. 50 = 0dB .. 100 = 12 dB
void audio_adau_set_eq_gain(int band, int gain);

// pre range: 0 = -12dB .. 50 = 0dB .. 100 = 12 dB
void audio_adau_set_prefactor(int pre);

// vol range: 0 = muted .. 50 = -6dB .. 100 = 0dB
// pan range: 0 = left .. 50 = center .. 100 = right
void audio_adau_set_vol_pan(int vol, int pan);

#endif

