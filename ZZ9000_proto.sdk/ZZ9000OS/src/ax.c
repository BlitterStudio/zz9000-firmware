#include <stdio.h>
#include <string.h>

#include "platform.h"
#include "xparameters.h"
#include "adau.h"
#include "adau_PARAM.h"
#include "xiicps.h"
#include "xi2stx.h"
#include "xi2srx.h"
#include "xaudioformatter.h"
#include "xil_cache.h"
#include "mntzorro.h"
#include "interrupt.h"
#include "sleep.h"
#include "stdlib.h"
#include "ax.h"
#include "audio_capture.h"
#include "audio_convert.h"
#include "audio_playback_rate.h"
#include "audio_dsp_gain.h"
#include "memorymap.h"
#include "xtime_l.h"
#include "math.h"
#include "ax.h"

#define IIC2_DEVICE_ID	XPAR_XIICPS_1_DEVICE_ID
#define IIC2_SCLK_RATE	100000
#define ADAU_I2C_ADDR	0x68
#define ADAU_PROGRAM_RAM_BASE	1024
#define ADAU_PROGRAM_WORD_BYTES	5
#define ADAU_PROGRAM_WRITE_RETRIES	3
#define ADAU_PARAMETER_RAM_BASE	0
#define ADAU_PARAMETER_WORD_BYTES	4
#define ADAU_PARAMETER_WRITE_RETRIES	3
#define ADAU_CONTROL_WRITE_RETRIES	3

void adau_to_5_23(double param_dec, uint8_t *param_hex);
double flt_omega(double fs, double f0);
double flt_alpha(double fs, double f0);

XIicPs Iic2;
XI2s_Tx i2s;
XI2s_Rx i2srx;
XAudioFormatter audio_formatter;
XAudioFormatter audio_formatter_rx;

static uint8_t* audio_tx_buffer = (uint8_t*)AUDIO_TX_BUFFER_ADDRESS;
static uint8_t* audio_inited_tx_buffer = NULL;
static uint8_t* volatile audio_rx_buffer = (uint8_t*)AUDIO_RX_BUFFER_ADDRESS;
static volatile uint16_t audio_interrupt_mask = 0;
static volatile uint16_t audio_capture_frames = ZZ_AUDIO_CAPTURE_INPUT_FRAMES;
static volatile uint16_t audio_rx_status = ZZ_AUDIO_RX_STATUS_CAPABLE;
static volatile uint16_t audio_tx_status =
	ZZ_AUDIO_TX_STATUS_CAPABLE |
	((AUDIO_NUM_PERIODS - 1U) << ZZ_AUDIO_TX_STATUS_PERIOD_SHIFT);
static volatile uint8_t audio_codec_is_present = 0;
static volatile uint8_t audio_capture_ready = 0;
static volatile uint8_t audio_rx_last_completed_period =
    AUDIO_NUM_PERIODS - 1U;

int adau_write16(u8 i2c_addr, u16 addr, u16 value) {
	XIicPs* iic = &Iic2;
	int status;
	u8 buffer[4];
	buffer[0] = addr>>8;
	buffer[1] = addr&0xff;
	buffer[2] = value>>8;
	buffer[3] = value&0xff;

	int timeout = 0;
	while (XIicPs_BusIsBusy(iic)) {
		usleep(1);
		timeout++;
		if (timeout>10000) {
			printf("ADAU I2C write16 timeout.\n");
			return -1;
		}
	}
	status = XIicPs_MasterSendPolled(iic, buffer, 4, i2c_addr);

	return status;
}

int adau_write24(u8 i2c_addr, u16 addr, u32 value) {
	XIicPs* iic = &Iic2;
	int status;
	u8 buffer[5];
	buffer[0] = addr>>8;
	buffer[1] = addr&0xff;
	buffer[2] = (value>>16)&0xff;
	buffer[3] = (value>>8)&0xff;
	buffer[4] = value&0xff;

	int timeout = 0;
	while (XIicPs_BusIsBusy(iic)) {
		usleep(1);
		timeout++;
		if (timeout>10000) {
			printf("ADAU I2C write24 timeout.\n");
			return -1;
		}
	}
	status = XIicPs_MasterSendPolled(iic, buffer, 5, i2c_addr);

	return status;
}

// for storing 40 bit program words
int adau_write40(u8 i2c_addr, u16 addr, u8* data) {
	XIicPs* iic = &Iic2;
	int status;
	u8 buffer[7];
	buffer[0] = addr>>8;
	buffer[1] = addr&0xff;
	buffer[2] = data[0];
	buffer[3] = data[1];
	buffer[4] = data[2];
	buffer[5] = data[3];
	buffer[6] = data[4];

	int timeout = 0;
	while (XIicPs_BusIsBusy(iic)) {
		usleep(1);
		timeout++;
		if (timeout>10000) {
			printf("ADAU I2C write40 timeout.\n");
			return -1;
		}
	}

	status = XIicPs_MasterSendPolled(iic, buffer, 2+5, i2c_addr);
	return status;
}

// for storing 32 bit parameter words
int adau_write32(u8 i2c_addr, u16 addr, u8* data) {
	XIicPs* iic = &Iic2;
	int status;
	u8 buffer[6];
	buffer[0] = addr>>8;
	buffer[1] = addr&0xff;
	buffer[2] = data[0];
	buffer[3] = data[1];
	buffer[4] = data[2];
	buffer[5] = data[3];

	int timeout = 0;
	while (XIicPs_BusIsBusy(iic)) {
		usleep(1);
		timeout++;
		if (timeout>10000) {
			printf("ADAU I2C write32 timeout.\n");
			return -1;
		}
	}

	status = XIicPs_MasterSendPolled(iic, buffer, 2+4, i2c_addr);
	return status;
}

int adau_read16(u8 i2c_addr, u16 addr, u8* buffer) {
	XIicPs* iic = &Iic2;
	int status1;
	u8 abuffer[2];
	abuffer[0] = addr>>8;
	abuffer[1] = addr&0xff;

	XIicPs_SetOptions(iic, XIICPS_REP_START_OPTION);

	int timeout = 0;
	while (XIicPs_BusIsBusy(iic)) {
		usleep(1);
		timeout++;
		if (timeout>10000) {
			printf("ADAU I2C read16a timeout.\n");
			return -1;
		}
	}
	status1 = XIicPs_MasterSendPolled(iic, abuffer, 2, i2c_addr);
	XIicPs_ClearOptions(iic, XIICPS_REP_START_OPTION);
	XIicPs_MasterRecvPolled(iic, buffer, 2, i2c_addr);
	timeout = 0;
	while (XIicPs_BusIsBusy(iic)) {
		usleep(1);
		timeout++;
		if (timeout>10000) {
			printf("ADAU I2C read16b timeout.\n");
			return -1;
		}
	}

	return status1;
}

int adau_read24(u8 i2c_addr, u16 addr, u8* buffer) {
	XIicPs* iic = &Iic2;
	int status1;
	u8 abuffer[2];
	abuffer[0] = addr>>8;
	abuffer[1] = addr&0xff;

	XIicPs_SetOptions(iic, XIICPS_REP_START_OPTION);
	int timeout = 0;
	while (XIicPs_BusIsBusy(iic)) {
		usleep(1);
		timeout++;
		if (timeout>10000) {
			printf("ADAU I2C read24a timeout.\n");
			return -1;
		}
	}
	status1 = XIicPs_MasterSendPolled(iic, abuffer, 2, i2c_addr);
	XIicPs_ClearOptions(iic, XIICPS_REP_START_OPTION);
	XIicPs_MasterRecvPolled(iic, buffer, 3, i2c_addr);
	timeout = 0;
	while (XIicPs_BusIsBusy(iic)) {
		usleep(1);
		timeout++;
		if (timeout>10000) {
			printf("ADAU I2C read24 timeout.\n");
			return -1;
		}
	}

	return status1;
}

// for verifying 32 bit parameter words
static int adau_read32(u8 i2c_addr, u16 addr, u8 *buffer) {
	XIicPs* iic = &Iic2;
	int status;
	u8 abuffer[2];
	abuffer[0] = addr>>8;
	abuffer[1] = addr&0xff;

	XIicPs_SetOptions(iic, XIICPS_REP_START_OPTION);

	int timeout = 0;
	while (XIicPs_BusIsBusy(iic)) {
		usleep(1);
		timeout++;
		if (timeout>10000) {
			XIicPs_ClearOptions(iic, XIICPS_REP_START_OPTION);
			printf("ADAU I2C read32a timeout.\n");
			return -1;
		}
	}

	status = XIicPs_MasterSendPolled(iic, abuffer, 2, i2c_addr);
	if (status != 0) {
		XIicPs_ClearOptions(iic, XIICPS_REP_START_OPTION);
		return status;
	}

	XIicPs_ClearOptions(iic, XIICPS_REP_START_OPTION);
	status = XIicPs_MasterRecvPolled(iic, buffer,
			ADAU_PARAMETER_WORD_BYTES, i2c_addr);
	if (status != 0) {
		return status;
	}

	timeout = 0;
	while (XIicPs_BusIsBusy(iic)) {
		usleep(1);
		timeout++;
		if (timeout>10000) {
			printf("ADAU I2C read32b timeout.\n");
			return -1;
		}
	}

	return 0;
}

// for verifying 40 bit program words
static int adau_read40(u8 i2c_addr, u16 addr, u8* buffer) {
	XIicPs* iic = &Iic2;
	int status;
	u8 abuffer[2];
	abuffer[0] = addr>>8;
	abuffer[1] = addr&0xff;

	XIicPs_SetOptions(iic, XIICPS_REP_START_OPTION);

	int timeout = 0;
	while (XIicPs_BusIsBusy(iic)) {
		usleep(1);
		timeout++;
		if (timeout>10000) {
			XIicPs_ClearOptions(iic, XIICPS_REP_START_OPTION);
			printf("ADAU I2C read40a timeout.\n");
			return -1;
		}
	}

	status = XIicPs_MasterSendPolled(iic, abuffer, 2, i2c_addr);
	if (status != 0) {
		XIicPs_ClearOptions(iic, XIICPS_REP_START_OPTION);
		return status;
	}

	XIicPs_ClearOptions(iic, XIICPS_REP_START_OPTION);
	status = XIicPs_MasterRecvPolled(iic, buffer,
			ADAU_PROGRAM_WORD_BYTES, i2c_addr);
	if (status != 0) {
		return status;
	}

	timeout = 0;
	while (XIicPs_BusIsBusy(iic)) {
		usleep(1);
		timeout++;
		if (timeout>10000) {
			printf("ADAU I2C read40b timeout.\n");
			return -1;
		}
	}

	return 0;
}


int audio_adau_write_parameter(uint16_t address,
		const uint8_t value[ADAU_PARAMETER_WORD_BYTES])
{
	uint8_t wire_value[ADAU_PARAMETER_WORD_BYTES];
	uint8_t readback[ADAU_PARAMETER_WORD_BYTES] = { 0 };
	int attempt;
	int res = -1;

	if (value == NULL) {
		return -1;
	}
	memcpy(wire_value, value, sizeof(wire_value));
	/*
	 * Parameter RAM is 28 bits. SigmaStudio's C exports sign-extend
	 * negative values into the unused high nibble, while the ADAU1701
	 * control-port format requires that nibble to be zero padded.
	 */
	wire_value[0] &= 0x0fU;
	for (attempt = 0;
			attempt < ADAU_PARAMETER_WRITE_RETRIES; ++attempt) {
		res = adau_write32(0x34, address, wire_value);
		if (res == 0) {
			res = adau_read32(0x34, address, readback);
		}
		if (res == 0 &&
				audio_adau_readback_matches(wire_value, readback,
						ADAU_PARAMETER_WORD_BYTES)) {
			return 0;
		}
		res = -1;
	}

	printf("[adau] parameter verify failed at 0x%03x: "
			"%02x%02x%02x%02x != %02x%02x%02x%02x\n",
			address,
			wire_value[0], wire_value[1],
			wire_value[2], wire_value[3],
			readback[0], readback[1], readback[2], readback[3]);
	return -1;
}

int audio_program_adau_params(uint8_t *params, uint32_t param_len) {
	if ((param_len % ADAU_PARAMETER_WORD_BYTES) != 0) {
		printf("[adau] invalid parameter length: %lu\n",
				(unsigned long)param_len);
		return -1;
	}

	for (uint32_t i = 0; i < param_len;
			i += ADAU_PARAMETER_WORD_BYTES) {
		uint16_t addr = ADAU_PARAMETER_RAM_BASE +
				i/ADAU_PARAMETER_WORD_BYTES;

		if (audio_adau_write_parameter(addr, &params[i]) != 0) {
			return -1;
		}
	}

	printf("[adau] verified %lu parameter words\n",
			(unsigned long)(param_len/ADAU_PARAMETER_WORD_BYTES));
	return 0;
}

static int audio_program_adau_word(const uint8_t *program,
		uint32_t offset)
{
	uint8_t readback[ADAU_PROGRAM_WORD_BYTES] = { 0 };
	uint16_t addr = ADAU_PROGRAM_RAM_BASE +
			offset / ADAU_PROGRAM_WORD_BYTES;
	int attempt;
	int res = -1;

	for (attempt = 0; attempt < ADAU_PROGRAM_WRITE_RETRIES; ++attempt) {
		res = adau_write40(0x34, addr, (uint8_t *)&program[offset]);
		if (res == 0) {
			res = adau_read40(0x34, addr, readback);
		}
		if (res == 0 &&
				audio_adau_readback_matches(&program[offset],
						readback,
						ADAU_PROGRAM_WORD_BYTES)) {
			return 0;
		}
		res = -1;
	}

	printf("[adau] program verify failed at 0x%03x: "
			"%02x%02x%02x%02x%02x != "
			"%02x%02x%02x%02x%02x\n",
			addr,
			program[offset], program[offset + 1U],
			program[offset + 2U], program[offset + 3U],
			program[offset + 4U],
			readback[0], readback[1], readback[2],
			readback[3], readback[4]);
	return -1;
}

int audio_program_adau(uint8_t *program, uint32_t program_len) {
	uint32_t offset;

	if ((program_len % ADAU_PROGRAM_WORD_BYTES) != 0) {
		printf("[adau] invalid program length: %lu\n",
				(unsigned long)program_len);
		return -1;
	}

	for (offset = 0U; offset < program_len;
			offset += ADAU_PROGRAM_WORD_BYTES) {
		if (audio_program_adau_word(program, offset) != 0) {
			return -1;
		}
	}
	printf("[adau] verified %lu program words\n",
			(unsigned long)(program_len/ADAU_PROGRAM_WORD_BYTES));
	return 0;
}

void audio_init_i2s() {
	/* Buffer setters run before this deferred reinitialization. Do not let
	 * either formatter's old IOC state publish through a new CPU pointer. */
	audio_capture_ready = 0;
	__asm__ __volatile__("dsb" ::: "memory");

	XI2stx_Config* i2s_config = XI2s_Tx_LookupConfig(XPAR_XI2STX_0_DEVICE_ID);
	int status = XI2s_Tx_CfgInitialize(&i2s, i2s_config, i2s_config->BaseAddress);

	printf("[adau] I2S_TX cfg status: %d\n", status);
	printf("[adau] I2S Dwidth: %d\n", i2s.Config.DWidth);
	printf("[adau] I2S MaxNumChannels: %d\n", i2s.Config.MaxNumChannels);

	XI2s_Tx_JustifyEnable(&i2s, 0);

	XAudioFormatter_Config* af_config = XAudioFormatter_LookupConfig(XPAR_XAUDIOFORMATTER_0_DEVICE_ID);
	audio_formatter.BaseAddress = af_config->BaseAddress;

	status = XAudioFormatter_CfgInitialize(&audio_formatter, af_config);

	//printf("[adau] AudioFormatter cfg status: %d\n", status);

	// reset the goddamn register
	XAudioFormatter_WriteReg(audio_formatter.BaseAddress,
			XAUD_FORMATTER_CTRL + XAUD_FORMATTER_MM2S_OFFSET, 0);

	XAudioFormatterHwParams af_params;
	af_params.buf_addr = (u32)audio_tx_buffer;
	af_params.bits_per_sample = BIT_DEPTH_16;
	af_params.periods = AUDIO_NUM_PERIODS; // 1 second = 192000 bytes
	af_params.active_ch = 2;
	// must be multiple of 32*channels = 64
	af_params.bytes_per_period = AUDIO_BYTES_PER_PERIOD;

	XAudioFormatterSetFsMultiplier(&audio_formatter, 48000*256, 48000); // mclk = 256 * Fs // this doesn't really seem to change anything?!
	XAudioFormatterSetHwParams(&audio_formatter, &af_params);
	XAudioFormatter_InterruptDisable(&audio_formatter, 1<<14); // timeout
	XAudioFormatter_InterruptDisable(&audio_formatter, 1<<13); // IOC

	// set up i2s receiver

	XAudioFormatter_Config* af_config_rx = XAudioFormatter_LookupConfig(XPAR_XAUDIOFORMATTER_1_DEVICE_ID);
	audio_formatter_rx.BaseAddress = af_config_rx->BaseAddress;

	status = XAudioFormatter_CfgInitialize(&audio_formatter_rx, af_config_rx);
	//printf("[adau] AudioFormatter RX cfg status: %d\n", status);

	XAudioFormatter_WriteReg(audio_formatter_rx.BaseAddress,
			XAUD_FORMATTER_CTRL + XAUD_FORMATTER_S2MM_OFFSET, 0);

	XAudioFormatterHwParams afrx_params;
	afrx_params.buf_addr = (u32)audio_rx_buffer;
	afrx_params.bits_per_sample = BIT_DEPTH_16;
	afrx_params.periods = AUDIO_NUM_PERIODS; // 1 second = 192000 bytes
	afrx_params.active_ch = 2;
	// must be multiple of 32*channels = 64
	afrx_params.bytes_per_period = AUDIO_BYTES_PER_PERIOD;

	XAudioFormatterSetFsMultiplier(&audio_formatter_rx, 48000*256, 48000);
	XAudioFormatterSetHwParams(&audio_formatter_rx, &afrx_params);

	XAudioFormatter_InterruptDisable(&audio_formatter_rx, 1<<14); // timeout
	XAudioFormatter_InterruptDisable(&audio_formatter_rx, 1<<13); // IOC

	XI2srx_Config* i2srx_config = XI2s_Rx_LookupConfig(XPAR_XI2SRX_0_DEVICE_ID);
	status = XI2s_Rx_CfgInitialize(&i2srx, i2srx_config, i2srx_config->BaseAddress);

	//printf("[adau] I2S_RX cfg status: %d\n", status);

	//printf("[adau] I2S_RX Dwidth: %d\n", i2srx.Config.DWidth);
	//printf("[adau] I2S_RX MaxNumChannels: %d\n", i2srx.Config.MaxNumChannels);

	XI2s_Rx_Enable(&i2srx, 1);
	audio_rx_last_completed_period = AUDIO_NUM_PERIODS - 1U;
	XAudioFormatter_WriteReg(audio_formatter_rx.BaseAddress,
		XAUD_FORMATTER_STS + XAUD_FORMATTER_S2MM_OFFSET, 1U<<31);
	XAudioFormatter_InterruptEnable(&audio_formatter_rx, 1<<13); // IOC
	XAudioFormatterDMAStart(&audio_formatter_rx);

	printf("[adau] XAudioFormatter_InterruptEnable...\n");

	XAudioFormatter_InterruptEnable(&audio_formatter, 1<<13); // IOC

	printf("[adau] XI2s_Tx_Enable...\n");
	XI2s_Tx_Enable(&i2s, 1);

	printf("[adau] XAudioFormatterDMAStart...\n");
	XAudioFormatterDMAStart(&audio_formatter);
	printf("[adau] XAudioFormatterDMAStart done.\n");

	audio_inited_tx_buffer = audio_tx_buffer;
	/* Preserve audio_rx_status across retargets so a driver that sampled the
	 * sequence before this deferred reinit observes exactly the next period. */
	__asm__ __volatile__("dsb" ::: "memory");
	audio_capture_ready = 1;
}

static int audio_adau_write16_verified(uint16_t address, uint16_t value,
		uint16_t *readback)
{
	uint8_t bytes[2] = { 0U, 0U };
	int status;

	status = adau_write16(0x34, address, value);
	if (status == 0)
		status = adau_read16(0x34, address, bytes);
	if (readback != NULL)
		*readback = (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
	if (status != 0 || bytes[0] != (uint8_t)(value >> 8) ||
			bytes[1] != (uint8_t)value)
		return -1;
	return 0;
}

static int audio_adau_write24_verified(uint16_t address, uint32_t value,
		uint32_t *readback)
{
	uint8_t bytes[3] = { 0U, 0U, 0U };
	uint32_t actual = 0U;
	int attempt;
	int status = -1;

	for (attempt = 0; attempt < ADAU_CONTROL_WRITE_RETRIES; ++attempt) {
		status = adau_write24(0x34, address, value);
		if (status == 0)
			status = adau_read24(0x34, address, bytes);
		actual = ((uint32_t)bytes[0] << 16) |
				((uint32_t)bytes[1] << 8) | bytes[2];
		if (status == 0 && actual == (value & 0x00ffffffU)) {
			if (readback != NULL)
				*readback = actual;
			return 0;
		}
	}

	if (readback != NULL)
		*readback = actual;
	return -1;
}

static void audio_adau_lpf_coefficients(int f0, double coefficients[5])
{
	const double fs = 48000.0;
	double omega = flt_omega(fs, (double)f0);
	double alpha = flt_alpha(fs, (double)f0);
	double a0 = 1.0 + alpha;
	double a1 = -2.0 * cos(omega);
	double a2 = 1.0 - alpha;
	double b0 = (1.0 - cos(omega)) / 2.0;
	double b1 = 1.0 - cos(omega);
	double b2 = b0;

	a1 /= a0;
	a2 /= a0;
	b0 /= a0;
	b1 /= a0;
	b2 /= a0;

	coefficients[0] = b0;
	coefficients[1] = b1;
	coefficients[2] = b2;
	coefficients[3] = -a1;
	coefficients[4] = -a2;
}

// The TX buffer address the audio formatter DMA was last INITIALIZED
// with. audio_set_tx_buffer() only moves the CPU-side pointer; the DMA
// keeps reading the buffer captured at the last audio_init_i2s(). An
// AHI session repoints the DMA at its own buffer (AP_TX_BUF_OFFS +
// re-init) and closing AHI does not restore it, so a consumer that
// needs the default ring must compare against this and re-init.
uint8_t* audio_get_inited_tx_buffer() {
	return audio_inited_tx_buffer;
}

// returns 1 if adau1701 found, otherwise 0
// set audio_tx_buffer and audio_rx_buffer before!
int audio_adau_init(int program_dsp) {
	XIicPs_Config* i2c_config;
	i2c_config = XIicPs_LookupConfig(IIC2_DEVICE_ID);
	int status = XIicPs_CfgInitialize(&Iic2, i2c_config, i2c_config->BaseAddress);
	printf("[adau] XIicPs_CfgInitialize 2: %d\n", status);

	/*
	 * main() releases the ADAU1701 reset immediately before calling us.
	 * At a 12.288 MHz MCLK the datasheet gives approximately 21 ms for
	 * PLL startup plus the internal boot-ROM copy, and forbids control-port
	 * accesses during that interval.  The former 10 ms delay could let the
	 * first program-RAM words race the device's own initialization.
	 */
	usleep(25000);
	printf("[adau] XIicPs 2 is ready: %lx\n", Iic2.IsReady);
	status = XIicPs_SelfTest(&Iic2);
	printf("[adau] XIicPs_SelfTest: %x\n", status);

	if (status != 0) {
		printf("[adau] I2C instance 2 self test failed.");
		return 0;
	}

	status = XIicPs_SetSClk(&Iic2, IIC2_SCLK_RATE);
	printf("[adau] XIicPs_SetSClk: %x\n", status);

	u8 rbuf[5];
	u8 i = 0x34;

	//usleep(10000);
	/*
	 * Hold the DSP data path clear while its program and parameters are
	 * replaced.  This matches SigmaStudio's generated download sequence:
	 * ADM|DAM stay enabled, CR remains low until loading is complete.
	 */
	status = adau_write16(i, 2076,
			ZZ_AUDIO_CODEC_CORE_LOADING);
	if (status == 0) {
		printf("[adau] hold DSP core for loading: %d\n", i);
		printf("\n[adau] ~~~~ ZZ9000AX detected. ~~~~\n\n");
	} else {
		printf("[adau] ZZ9000AX not detected.\n");
		return 0;
	}

	status = adau_read16(i, 2076, rbuf);
	if (status != 0 ||
			rbuf[0] !=
				(uint8_t)(ZZ_AUDIO_CODEC_CORE_LOADING >> 8) ||
			rbuf[1] !=
				(uint8_t)ZZ_AUDIO_CODEC_CORE_LOADING) {
		printf("[adau] DSP core loading-state verify failed: "
				"%02x%02x != %04x (status: %d)\n",
				rbuf[0], rbuf[1],
				ZZ_AUDIO_CODEC_CORE_LOADING, status);
		return 0;
	}
	printf("[adau] verified DSP core loading state: %02x%02x\n",
			rbuf[0], rbuf[1]);

	// DAC setup: DS = 01
	status = adau_write16(i, 2087, 1);
	printf("[adau] write DAC setup: %d\n", status);

	rbuf[0] = 0;
	rbuf[1] = 0;

	status = adau_read16(i, 2087, rbuf);
	printf("[adau] read from 2087: %02x%02x (status: %d)\n", rbuf[0], rbuf[1], status);

	/*
	 * Production capture uses TDM8 with ADC left/right in slots 0/1.
	 * The master-mode divider remains at 256*Fs, so the existing playback
	 * formatter continues to receive its codec-synchronous 12.288 MHz
	 * master clock while the FPGA normalizes those two TDM slots to I2S.
	 */
	u16 serial_output_control =
			ZZ_AUDIO_CODEC_SERIAL_TDM8_SLOT01;
	status = adau_write16(i, 0x081e, serial_output_control);
	printf("[adau] write serial output control: %d\n", status);
	if (status != 0) {
		return 0;
	}

	rbuf[0] = 0;
	rbuf[1] = 0;
	status = adau_read16(i, 0x081e, rbuf);
	if (status != 0 ||
			rbuf[0] != (u8)(serial_output_control >> 8) ||
			rbuf[1] != (u8)serial_output_control) {
		printf("[adau] serial output control verify failed: "
				"%02x%02x != %04x (status: %d)\n",
				rbuf[0], rbuf[1], serial_output_control, status);
		return 0;
	}
	printf("[adau] verified serial output control: %02x%02x\n",
			rbuf[0], rbuf[1]);

	uint32_t mp_readback = 0U;
	status = audio_adau_write24_verified(
			0x0820, ZZ_AUDIO_CODEC_MP_CONTROL, &mp_readback);
	if (status != 0) {
		printf("[adau] MP control 0x820 verify failed: "
				"%06lx != %06x\n",
				(unsigned long)mp_readback,
				ZZ_AUDIO_CODEC_MP_CONTROL);
		return 0;
	}
	printf("[adau] verified MP control 0x820: %06lx\n",
			(unsigned long)mp_readback);

	mp_readback = 0U;
	status = audio_adau_write24_verified(
			0x0821, ZZ_AUDIO_CODEC_MP_CONTROL, &mp_readback);
	if (status != 0) {
		printf("[adau] MP control 0x821 verify failed: "
				"%06lx != %06x\n",
				(unsigned long)mp_readback,
				ZZ_AUDIO_CODEC_MP_CONTROL);
		return 0;
	}
	printf("[adau] verified MP control 0x821: %06lx\n",
			(unsigned long)mp_readback);

	if (program_dsp) {
		status = audio_program_adau(
				Program_Data_Normal_ADC_IC_1,
				sizeof(Program_Data_Normal_ADC_IC_1));
		if (status == 0) {
			status = audio_program_adau_params(
					Param_Data_Normal_ADC_IC_1,
					sizeof(Param_Data_Normal_ADC_IC_1));
		}
		if (status == 0)
			status = audio_adau_set_lpf_params(23900);
		if (status == 0)
			status = audio_adau_set_mixer_vol(128, 64);
		if (status != 0) {
			printf("[adau] verified normal DSP load failed; "
					"capture remains unavailable.\n");
			return 0;
		}
	}

	uint16_t core_readback = 0U;
	status = audio_adau_write16_verified(2076,
			ZZ_AUDIO_CODEC_CORE_RUNNING, &core_readback);
	if (status != 0) {
		printf("[adau] DSP core release failed: %04x != %04x\n",
				core_readback, ZZ_AUDIO_CODEC_CORE_RUNNING);
		return 0;
	}

	printf("[adau] capture build %04x: TDM8 slots 0/1 active\n",
			ZZ_AUDIO_CAPTURE_CANDIDATE_BUILD_ID);
	audio_init_i2s();
	return 1;
}

XTime debug_time_start = 0;

void audio_debug_timer(int zdata) {
	if (zdata == 0) {
		XTime_GetTime(&debug_time_start);
	} else {
		XTime debug_time_stop;
		XTime_GetTime(&debug_time_stop);
		printf("%x;%09.2f us\n", (uint8_t)zdata,
				1.0 * (debug_time_stop-debug_time_start) / (COUNTS_PER_SECOND/1000000));
		XTime_GetTime(&debug_time_start);
	}
}

int isra_count = 0;

// TX-fill half of the SDK playback pump (sdk_mailbox.c); ISR-safe.
extern void sdk_mailbox_audio_playback_pump_isr(void);

// audio formatter interrupt, triggered whenever a period is completed
void isr_audio(void *dummy) {
	uint32_t transfer_count;
	uint8_t completed_period;
	uint16_t sequence;
	uint32_t val = XAudioFormatter_ReadReg(XPAR_XAUDIOFORMATTER_0_BASEADDR, XAUD_FORMATTER_STS + XAUD_FORMATTER_MM2S_OFFSET);
	val |= (1<<31); // clear irq
	XAudioFormatter_WriteReg(XPAR_XAUDIOFORMATTER_0_BASEADDR,
		XAUD_FORMATTER_STS + XAUD_FORMATTER_MM2S_OFFSET, val);

	/*
	 * XFER_COUNT points into the period currently being read. Publish the
	 * period immediately behind it so an Amiga-side producer never has to
	 * guess which ring slot is safe to refill at startup or after latency.
	 */
	transfer_count = XAudioFormatterGetDMATransferCount(&audio_formatter);
	completed_period = zz_audio_capture_completed_period(
	    transfer_count, AUDIO_BYTES_PER_PERIOD);
	sequence = (audio_tx_status + 1U) &
	    ZZ_AUDIO_TX_STATUS_SEQUENCE_MASK;
	audio_tx_status = zz_audio_tx_status_pack(completed_period, sequence);

	if (isra_count++>100) {
		isra_count = 0;
	}

	// Keep the TX ring filled from the bound audio-stream session on a
	// guaranteed 20 ms cadence: main-loop passes stretched by RTG or
	// network load previously let the DMA overrun the fill frontier
	// and glitch MP3 playback. No-op when no session is bound.
	sdk_mailbox_audio_playback_pump_isr();

	if (audio_interrupt_mask & ZZ_AUDIO_CONFIG_PLAY) {
		amiga_interrupt_set(AMIGA_INTERRUPT_AUDIO);
	}
}

int israrx_count = 0;

// audio formatter interrupt, triggered whenever a period is completed
void isr_audio_rx(void *dummy) {
	uint32_t transfer_count;
	uint8_t newest_period;
	uint8_t completed_count;
	uint8_t index;
	uint32_t val = XAudioFormatter_ReadReg(XPAR_XAUDIOFORMATTER_1_BASEADDR, XAUD_FORMATTER_STS + XAUD_FORMATTER_S2MM_OFFSET);
	val |= (1<<31); // clear irq
	XAudioFormatter_WriteReg(XPAR_XAUDIOFORMATTER_1_BASEADDR,
		XAUD_FORMATTER_STS + XAUD_FORMATTER_S2MM_OFFSET, val);

	/* The CPU-side pointer may already name a pending ring while the
	 * formatter is still being retargeted. Only acknowledge in that state. */
	if (!audio_capture_ready)
		return;

	/* The transfer counter points into the period currently being written.
	 * Derive the newest complete period from it instead of assuming that
	 * every edge reached the CPU separately. */
	transfer_count = XAudioFormatterGetDMATransferCount(&audio_formatter_rx);
	newest_period = zz_audio_capture_completed_period(transfer_count,
	                                                 AUDIO_BYTES_PER_PERIOD);
	completed_count = zz_audio_capture_period_distance(
	    newest_period, audio_rx_last_completed_period);
	/* IOC means at least one period completed. Equal cursors mean the CPU
	 * was delayed for at least one full ring. Keep only seven periods: the
	 * eighth slot is already the formatter's active write target. */
	if (completed_count == 0U)
		completed_count = ZZ_AUDIO_CAPTURE_RESIDENT_PERIODS;

	if (israrx_count++>1000) {
		israrx_count = 0;
	}

	if (zz_audio_capture_can_publish(audio_interrupt_mask,
	                                 audio_capture_ready)) {
		uint8_t first_period =
		    (newest_period + AUDIO_NUM_PERIODS - completed_count + 1U) %
		    AUDIO_NUM_PERIODS;

		for (index = 0U; index < completed_count; index++) {
			uint8_t completed_period =
			    (first_period + index) % AUDIO_NUM_PERIODS;
			uint8_t *period = audio_rx_buffer +
			    completed_period * AUDIO_BYTES_PER_PERIOD;
			uint16_t sequence =
			    (audio_rx_status + 1U) &
			    ZZ_AUDIO_RX_STATUS_SEQUENCE_MASK;
			uint16_t output_frames;

			/* The S2MM port is non-coherent, while Amiga Zorro accesses use
			 * the ACP. Pull the completed DMA period into the ARM coherency
			 * domain, then publish the converted bytes back to DDR/L2. */
			Xil_DCacheInvalidateRange((INTPTR)period,
			                          AUDIO_BYTES_PER_PERIOD);
			output_frames = zz_audio_capture_convert(
			    period, audio_capture_frames);
			Xil_DCacheFlushRange((INTPTR)period, output_frames * 4U);
			__asm__ __volatile__("dsb" ::: "memory");

			audio_rx_status = zz_audio_rx_status_pack(
			    completed_period, sequence);
		}
		amiga_interrupt_set(AMIGA_INTERRUPT_AUDIO);
	}

	audio_rx_last_completed_period = newest_period;
}

uint32_t audio_get_dma_transfer_count() {
	return XAudioFormatterGetDMATransferCount(&audio_formatter);
}

uint16_t audio_get_tx_status(void) {
	return audio_tx_status;
}

void audio_set_codec_present(int present) {
	audio_codec_is_present = present ? 1U : 0U;
}

int audio_codec_present(void) {
	return audio_codec_is_present != 0U;
}

void audio_set_interrupt_mask(uint16_t mask) {
	uint16_t old_mask = audio_interrupt_mask;

	mask &= ZZ_AUDIO_CONFIG_MASK;
	printf("[audio] irq mask: %u\n", mask);

	/* Reset BEFORE publishing the RECORD bit: isr_audio_rx can fire
	 * between these statements, and a period converted in that window
	 * would filter through the previous session's history and publish
	 * it as the new recording's first period (KTD5). */
	if ((mask & ZZ_AUDIO_CONFIG_RECORD) &&
	    !(old_mask & ZZ_AUDIO_CONFIG_RECORD))
		zz_audio_capture_reset();

	audio_interrupt_mask = mask;

	if (!mask) {
		amiga_interrupt_clear(AMIGA_INTERRUPT_AUDIO);
	}

	if ((old_mask ^ mask) & ZZ_AUDIO_CONFIG_PLAY)
		audio_silence();
}

void audio_set_capture_frames(uint16_t frames) {
	if (frames == 0U || frames > ZZ_AUDIO_CAPTURE_INPUT_FRAMES)
		frames = ZZ_AUDIO_CAPTURE_INPUT_FRAMES;

	/* The AHI driver writes this register on every playback period;
	 * same-value writes must not reset the capture converter or a
	 * full-duplex recording never reaches steady state (KTD5). */
	if (frames != audio_capture_frames) {
		/* Reset first, then publish the new count, so the RX ISR
		 * cannot convert one period at the new count against the
		 * old ratio and phase. */
		zz_audio_capture_reset();
		audio_capture_frames = frames;
	}
}

uint16_t audio_get_rx_status(void) {
	return audio_rx_status;
}

// Whether a legacy/AHI client currently drives the audio output: those
// clients enable the per-period Amiga interrupt (REG_ZZ_AUDIO_CONFIG=1)
// for the duration of playback. The SDK playback binding must not
// steal the formatter while this is set.
int audio_legacy_output_active() {
	return (audio_interrupt_mask & ZZ_AUDIO_CONFIG_PLAY) != 0;
}


/* Private converter instance for the legacy/AHI per-period path. The
 * rate is re-derived on every audio_swab call; a change re-initializes
 * the instance exactly once (KTD5), and reset_resampling() clears both
 * on silence/buffer reassignment. */
static struct zz_audio_convert audio_playback_convert;
static uint32_t audio_playback_last_rate;
static int16_t audio_playback_scratch[AUDIO_BYTES_PER_PERIOD / 2];
// offset = offset from audio tx buffer
// returns audio_buffer_collision (1 or 0)
int audio_swab(uint16_t audio_buf_samples, uint32_t offset, int byteswap) {
	int audio_buffer_collision = 0;
	uint16_t* data = (uint16_t*)(audio_tx_buffer + offset);
	uint32_t audio_freq = zz_audio_playback_rate(audio_buf_samples);

	//printf("[audio:%d] play: %d +%lu\n", byteswap, audio_freq, offset);

	// byteswap
	if (byteswap) {
		for (int i=0; i < audio_buf_samples * 2; i++) {
			data[i] = __builtin_bswap16(data[i]);
		}
	}

	// Qualified conversion through the shared fixed-point kernel.
	// 48 kHz stays a byte-identical bypass.
	if (audio_freq != 48000U && audio_buf_samples != 0U) {
		if (audio_freq != audio_playback_last_rate) {
			audio_playback_last_rate = audio_freq;
			zz_audio_convert_init(&audio_playback_convert,
			                      audio_freq, 48000U);
		}
		if (audio_playback_convert.ratio == NULL) {
			/* Off-table rate (not one of the six advertised):
			 * no honest conversion exists, so emit a silent
			 * period rather than wrong-speed audio with a
			 * stale tail. */
			memset(audio_tx_buffer + offset, 0,
			       AUDIO_BYTES_PER_PERIOD);
		} else {
			/* The FIR is backward-looking (output m reads
			 * inputs at or below its base), so it must not
			 * convert in place: stage the source frames in a
			 * scratch copy first. */
			memcpy(audio_playback_scratch,
			       audio_tx_buffer + offset,
			       (size_t)audio_buf_samples * 4U);
			zz_audio_convert_stream(&audio_playback_convert,
				audio_playback_scratch,
				(int16_t *)(audio_tx_buffer + offset),
				audio_buf_samples,
				AUDIO_BYTES_PER_PERIOD / 4);
		}
	} else if (audio_freq == 48000U) {
		/* 48 kHz is a byte-identical bypass, but a converted-rate
		 * session that passes through it must not resume with
		 * history from before the detour: record the transition
		 * and retire the converter so returning to a converted
		 * rate re-initializes it (44.1 -> 48 -> 44.1 kHz). */
		if (audio_playback_last_rate != 48000U) {
			audio_playback_last_rate = 48000U;
			zz_audio_convert_reset(&audio_playback_convert);
		}
	} else {
		/* A zero frame count is a client error path; silence the
		 * period instead of converting stale bytes. */
		memset(audio_tx_buffer + offset, 0,
		       AUDIO_BYTES_PER_PERIOD);
	}

	u32 txcount = audio_get_dma_transfer_count();

	// is the distance of reader (audio dma) and writer (amiga) in the ring buffer too small?
	// then signal this condition so amiga can adjust
	if (abs(txcount-offset) < AUDIO_BYTES_PER_PERIOD) {
		audio_buffer_collision = 1;
		//printf("[aswap] ring collision %d\n", abs(txcount-offset));
	} else {
		audio_buffer_collision = 0;
	}

	if (audio_buffer_collision) {
		printf("[aswap] d-a: %ld\n",txcount-offset);
	}

	return audio_buffer_collision;
}

void reset_resampling() {
	audio_playback_last_rate = 0U;
	zz_audio_convert_reset(&audio_playback_convert);
}

void audio_set_tx_buffer(uint8_t* addr) {
	printf("[audio] set tx buffer: %p\n", addr);
	audio_tx_buffer = addr;
	reset_resampling();
}

void audio_set_rx_buffer(uint8_t* addr) {
	printf("[audio] set rx buffer: %p\n", addr);
	/* The formatter keeps writing its active ring until audio_init_i2s().
	 * Block the ISR before exposing the pending CPU-side pointer. */
	audio_capture_ready = 0;
	__asm__ __volatile__("dsb" ::: "memory");
	audio_rx_buffer = addr;
	zz_audio_capture_reset();
}
void audio_silence() {
	memset(audio_tx_buffer, 0, AUDIO_TX_BUFFER_SIZE);
	// TX buffers live in plain cacheable DDR and the formatter DMA does
	// not snoop; push the silence to DRAM so it takes effect this
	// period rather than on eventual eviction.
	Xil_DCacheFlushRange((INTPTR)audio_tx_buffer, AUDIO_TX_BUFFER_SIZE);
	reset_resampling();
}

// sources:
// https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html
// https://wiki.analog.com/resources/tools-software/sigmastudio/usingsigmastudio/systemimplementation
// https://ez.analog.com/dsp/sigmadsp/f/q-a/104470/nth-order-filter-coefficient-calculations
// https://wiki.analog.com/resources/tools-software/sigmastudio/toolbox/filters/general2ndorder
// https://ez.analog.com/dsp/sigmadsp/f/q-a/65510/parameters-with-adau1701

void adau_to_5_23(double param_dec, uint8_t* param_hex) {
	long param223;
	long param227;

	// multiply decimal number by 2^23
	param223 = param_dec * (1 << 23);

	// convert to positive binary
	param227 = param223 + (1 << 27);

	param_hex[3] = (uint8_t) param227;
	param_hex[2] = (uint8_t) (param227 >> 8);
	param_hex[1] = (uint8_t) (param227 >> 16);
	param_hex[0] = (uint8_t) (param227 >> 24);

	// invert sign bit to get correct sign
	param_hex[0] = param_hex[0] ^ 0x08;
}

double flt_omega(double fs, double f0) {
	return 2.0 * M_PI * (f0 / fs);
}

double flt_alpha(double fs, double f0) {
	double omega = flt_omega(fs, f0);
	double Q = 1.0 / sqrt(2.0);
	return sin(omega) / (2.0 * Q);
}

int audio_adau_set_lpf_params(int f0) {
	double coefficients[5];
	double b0;
	double b1;
	double b2;
	double a1;
	double a2;
	uint8_t buf[4];

	printf("[lpf] f0: %d\n", f0);
	audio_adau_lpf_coefficients(f0, coefficients);
	b0 = coefficients[0];
	b1 = coefficients[1];
	b2 = coefficients[2];
	a1 = coefficients[3];
	a2 = coefficients[4];

	adau_to_5_23(b0, buf);
	if (audio_adau_write_parameter(
			MOD_GENFILTER1_ALG0_STAGE0_B0_ADDR, buf) != 0) {
		return -1;
	}
	printf("[lpf] b0: %f\t%02x %02x %02x %02x\n", b0, buf[0], buf[1], buf[2], buf[3]);
	adau_to_5_23(b1, buf);
	if (audio_adau_write_parameter(
			MOD_GENFILTER1_ALG0_STAGE0_B1_ADDR, buf) != 0) {
		return -1;
	}
	printf("[lpf] b1: %f\t%02x %02x %02x %02x\n", b1, buf[0], buf[1], buf[2], buf[3]);
	adau_to_5_23(b2, buf);
	if (audio_adau_write_parameter(
			MOD_GENFILTER1_ALG0_STAGE0_B2_ADDR, buf) != 0) {
		return -1;
	}
	printf("[lpf] b2: %f\t%02x %02x %02x %02x\n", b2, buf[0], buf[1], buf[2], buf[3]);
	adau_to_5_23(a1, buf);
	if (audio_adau_write_parameter(
			MOD_GENFILTER1_ALG0_STAGE0_A1_ADDR, buf) != 0) {
		return -1;
	}
	printf("[lpf] a1: %f\t%02x %02x %02x %02x\n", a1, buf[0], buf[1], buf[2], buf[3]);
	adau_to_5_23(a2, buf);
	if (audio_adau_write_parameter(
			MOD_GENFILTER1_ALG0_STAGE0_A2_ADDR, buf) != 0) {
		return -1;
	}
	printf("[lpf] a2: %f\t%02x %02x %02x %02x\n\n", a2, buf[0], buf[1], buf[2], buf[3]);
	return 0;
}

// vol range: 0-255. 127 = 0db
// vol1: paula
// vol2: i2s
int audio_adau_set_mixer_vol(int vol1, int vol2) {
	double v1 = ((double)vol1)/127.0;
	double v2 = ((double)vol2)/127.0;

	printf("[vol] v1: %f v2: %f\n", v1, v2);

	uint8_t buf[4];
	adau_to_5_23(v1, buf);
	if (audio_adau_write_parameter(
			MOD_STMIXER1_ALG0_STAGE0_VOLUME_ADDR, buf) != 0) {
		return -1;
	}
	adau_to_5_23(v2, buf);
	if (audio_adau_write_parameter(
			MOD_STMIXER1_ALG0_STAGE1_VOLUME_ADDR, buf) != 0) {
		return -1;
	}
	return 0;
}

int audio_adau_set_prefactor(int pre) {
	double p = audio_adau_prefactor_gain(pre);

	uint8_t buf[4];
	adau_to_5_23(p, buf);
	if (audio_adau_write_parameter(
			MOD_PREFACTOR_ALG0_GAIN1940ALGNS3_ADDR, buf) != 0 ||
			audio_adau_write_parameter(
			MOD_PREFACTOR_ALG1_GAIN1940ALGNS4_ADDR, buf) != 0) {
		return -1;
	}
	return 0;
}

int audio_adau_set_vol_pan(int vol, int pan) {
	LONG VolL, VolR;
	double vl, vr;

	VolL = vol;
	if(pan > 50) VolL -= 2*(pan-50);
	VolR = vol;
	if(pan < 50) VolR -= 2*(50-pan);

	if(VolL > 100) VolL = 100;
	if(VolR > 100) VolR = 100;
	if(VolL <   0) VolL =   0;
	if(VolR <   0) VolR =   0;

	vl = .01f * (double)VolL;
	vr = .01f * (double)VolR;

	uint8_t buf[4];
	adau_to_5_23(vl, buf);
	if (audio_adau_write_parameter(
			MOD_VOLUME_ALG0_GAIN1940ALGNS1_ADDR, buf) != 0) {
		return -1;
	}
	adau_to_5_23(vr, buf);
	if (audio_adau_write_parameter(
			MOD_VOLUME_ALG1_GAIN1940ALGNS2_ADDR, buf) != 0) {
		return -1;
	}
	return 0;
}

double eq_omega(double fs, double f0) {
	return 2.0 * M_PI * (f0 / fs);
}

double eq_alpha(double fs, double f0) {
	double omega = eq_omega(fs, f0);
	double Q = 1.2247449;
	return sin(omega) / (2.0 * Q);
}

// gain range: 0 = -12dB .. 50 = 0dB .. 100 = 12 dB
int audio_adau_set_eq_gain(int band, int gain) {
	if(band < 0 || band > 9) {
		return -1;
	}
	// These are the classic 
	static const double BandFreqs[10] = {
		31.25, 62.5, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0
	};
	double dBBoost = ((float)gain-50.0f)*12.0/50.0;
	double gainLinear = 1.0;
	double A= pow(10.0, dBBoost / 40.0);
	double fs = 48000.0f;
	double f0 = BandFreqs[band];

	double omega = eq_omega(fs, f0);
	double alpha = eq_alpha(fs, f0);
	
	double a0 = 1.0 + alpha/A;
	double a1 = -2.0 * cos(omega);
	double a2 = 1.0 - alpha/A;
	double b0 = (1 + alpha*A) * gainLinear;
	double b1 = -(2.0 * cos(omega)) * gainLinear;
	double b2 = (1.0 - alpha*A) * gainLinear;

	a1 /= a0;
	a2 /= a0;
	b0 /= a0;
	b1 /= a0;
	b2 /= a0;

	a1 = -a1;
	a2 = -a2;	

	printf("[equ] band: %d dB: %.1lf\n", band, dBBoost);

	// https://ez.analog.com/dsp/sigmadsp/w/documents/5182/implementing-safeload-writes-on-the-adau1701
	uint8_t buf[5];
	uint8_t expected[5][ADAU_PARAMETER_WORD_BYTES];
	uint8_t readback[ADAU_PARAMETER_WORD_BYTES];
	const uint16_t addresses[5] = {
		MOD_EQUALIZER_ALG0_STAGE0_B0_ADDR + band*5,
		MOD_EQUALIZER_ALG0_STAGE0_B1_ADDR + band*5,
		MOD_EQUALIZER_ALG0_STAGE0_B2_ADDR + band*5,
		MOD_EQUALIZER_ALG0_STAGE0_A0_ADDR + band*5,
		MOD_EQUALIZER_ALG0_STAGE0_A1_ADDR + band*5
	};
	const double coefficients[5] = { b0, b1, b2, a1, a2 };
	int index;

	buf[0] = 0;

	for (index = 0; index < 5; ++index) {
		adau_to_5_23(coefficients[index], expected[index]);
		memcpy(&buf[1], expected[index], ADAU_PARAMETER_WORD_BYTES);
		if (adau_write40(0x34, 0x0810 + index, buf) != 0 ||
				adau_write16(0x34, 0x0815 + index,
						addresses[index]) != 0) {
			printf("[equ] safeload staging failed at index %d\n",
					index);
			return -1;
		}
	}

	// Initiate safeload transfer bit, address 0x081C
	if (adau_write16(0x34, 0x081C, 0x003C) != 0) {
		printf("[equ] safeload transfer trigger failed\n");
		return -1;
	}

	usleep(25);

	for (index = 0; index < 5; ++index) {
		if (adau_read32(0x34, addresses[index], readback) != 0 ||
				!audio_adau_readback_matches(expected[index],
						readback,
						ADAU_PARAMETER_WORD_BYTES)) {
			printf("[equ] safeload verify failed at parameter "
					"0x%03x\n", addresses[index]);
			return -1;
		}
	}
	return 0;
}
