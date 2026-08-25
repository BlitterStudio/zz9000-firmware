#ifndef __AX_H__
#define __AX_H__

#include <stddef.h>
#include <stdint.h>

/*
 * Immutable identity for the production DSP image using the
 * hardware-selected TDM8 slot-0/1 transport. Increment whenever the
 * compiled signal graph changes; this is a bench identity, not the
 * firmware or coordinated release version.
 */
#define ZZ_AUDIO_CAPTURE_CANDIDATE_BUILD_ID 0xa205U
#define ZZ_AUDIO_CODEC_SERIAL_TDM8_SLOT01   0x0c22U
#define ZZ_AUDIO_CODEC_MP_CONTROL           0x444444U
#define ZZ_AUDIO_CODEC_CORE_LOADING         0x0018U
#define ZZ_AUDIO_CODEC_CORE_RUNNING         0x001cU

static inline int audio_adau_readback_matches(const uint8_t *expected,
		const uint8_t *actual, size_t length)
{
	size_t i;

	if (expected == NULL || actual == NULL) {
		return 0;
	}
	for (i = 0; i < length; ++i) {
		if (expected[i] != actual[i]) {
			return 0;
		}
	}
	return 1;
}

/* Incremental setters use 0 while more substeps remain, 1 when the
 * latch write completed, and -1 on I2C failure. Keep the transport
 * status conversion shared with host stubs so tests cannot model the
 * opposite completion semantics. */
static inline int audio_adau_safeload_latch_result(int write_status)
{
	return (write_status == 0) ? 1 : -1;
}

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
void audio_init_i2s(void);
void isr_audio(void *dummy);
void isr_audio_rx(void *dummy);
void audio_set_interrupt_mask(uint16_t mask);
void audio_set_capture_frames(uint16_t frames);
uint16_t audio_get_rx_status(void);
/* Nonzero while a legacy/AHI client drives the audio output (it keeps
 * the per-period Amiga interrupt enabled during playback). */
int audio_legacy_output_active(void);
void audio_clear_interrupt(void);
uint32_t audio_get_interrupt(void);
uint32_t audio_get_dma_transfer_count(void);
uint16_t audio_get_tx_status(void);
void audio_set_codec_present(int present);
int audio_codec_present(void);
int audio_swab(uint16_t audio_buf_samples, uint32_t offset, int byteswap);
void audio_set_tx_buffer(uint8_t *addr);
void audio_set_rx_buffer(uint8_t *addr);
/* TX buffer the formatter DMA was last initialized with (see ax.c). */
uint8_t *audio_get_inited_tx_buffer(void);
void reset_resampling(void);
void audio_silence(void);
void audio_debug_timer(int zdata);

int audio_program_adau(uint8_t *program, uint32_t program_len);
int audio_program_adau_params(uint8_t *params, uint32_t param_len);
int audio_adau_write_parameter(uint16_t address,
		const uint8_t value[4]);
int audio_adau_set_lpf_params(int f0);

/* vol range: 0-255. 127 = 0 dB */
int audio_adau_set_mixer_vol(int vol1, int vol2);

/* gain range: 0 = -12 dB .. 50 = 0 dB .. 100 = 12 dB */
int audio_adau_set_eq_gain(int band, int gain);

/* pre range: 0 = -12 dB .. 50 = 0 dB .. 100 = 12 dB */
int audio_adau_set_prefactor(int pre);

/* vol range: 0 = muted .. 50 = -6 dB .. 100 = 0 dB
 * pan range: 0 = left .. 50 = center .. 100 = right */
int audio_adau_set_vol_pan(int vol, int pan);
int audio_adau_set_vol_pan_side(int side, int vol, int pan);
int audio_adau_set_mixer_leg(int leg, int value);
int audio_adau_eq_substep(int band, int gain, int substep);
int audio_adau_safe_param_substep(uint16_t address,
	const uint8_t value[4], int substep);
int audio_adau_safe_mixer_leg(int leg, int value, int substep);
int audio_adau_safe_vol_pan_side(int side, int vol, int pan, int substep);
int audio_adau_safe_prefactor(int pre, int substep);

#endif
