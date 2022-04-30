#include <stdio.h>

#include "platform.h"
#include "xparameters.h"
#include "adau.h"
#include "xiicps.h"
#include "xi2stx.h"
#include "xi2srx.h"
#include "xaudioformatter.h"
#include "mntzorro.h"
#include "interrupt.h"
#include "sleep.h"
#include "stdlib.h"
#include "ax.h"
#include "memorymap.h"
#include "xtime_l.h"

#define IIC2_DEVICE_ID	XPAR_XIICPS_1_DEVICE_ID
#define IIC2_SCLK_RATE	100000
#define ADAU_I2C_ADDR	0x68

XIicPs Iic2;
XI2s_Tx i2s;
XI2s_Rx i2srx;
XAudioFormatter audio_formatter;
XAudioFormatter audio_formatter_rx;

static uint8_t* audio_tx_buffer = (uint8_t*)AUDIO_TX_BUFFER_ADDRESS;
static uint8_t* audio_rx_buffer = (uint8_t*)AUDIO_RX_BUFFER_ADDRESS;

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
			printf("ADAU I2C write40 timeout.\n");
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
	while (XIicPs_BusIsBusy(iic)) {};
	status1 = XIicPs_MasterSendPolled(iic, abuffer, 2, i2c_addr);
	XIicPs_ClearOptions(iic, XIICPS_REP_START_OPTION);
	XIicPs_MasterRecvPolled(iic, buffer, 3, i2c_addr);
	int timeout = 0;
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

void program_adau(u8* program, u32 program_len, u8* params, u32 param_len) {
	for (u32 i = 0; i < program_len; i+=5) {
		int res = adau_write40(0x34, 1024+i/5, &program[i]);
		if (res != 0) printf("[adau_write40] %lx: %d\n", i, res);
	}

	for (u32 i = 0; i < param_len; i+=4) {
		int res = adau_write32(0x34, 0+i/4, &params[i]);
		if (res != 0) printf("[adau_write32] %lx: %d\n", i, res);
	}
}

void audio_init_i2s() {
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
	/*XAudioFormatter_InterruptEnable(&audio_formatter_rx, 1<<13); // IOC
	printf("[adau] RX XAudioFormatter_InterruptEnable\n");*/

	XI2srx_Config* i2srx_config = XI2s_Rx_LookupConfig(XPAR_XI2SRX_0_DEVICE_ID);
	status = XI2s_Rx_CfgInitialize(&i2srx, i2srx_config, i2srx_config->BaseAddress);

	//printf("[adau] I2S_RX cfg status: %d\n", status);

	//printf("[adau] I2S_RX Dwidth: %d\n", i2srx.Config.DWidth);
	//printf("[adau] I2S_RX MaxNumChannels: %d\n", i2srx.Config.MaxNumChannels);

	XI2s_Rx_Enable(&i2srx, 1);
	XAudioFormatterDMAStart(&audio_formatter_rx);


	printf("[adau] XAudioFormatter_InterruptEnable...\n");

	XAudioFormatter_InterruptEnable(&audio_formatter, 1<<13); // IOC

	printf("[adau] XI2s_Tx_Enable...\n");
	XI2s_Tx_Enable(&i2s, 1);

	printf("[adau] XAudioFormatterDMAStart...\n");
	XAudioFormatterDMAStart(&audio_formatter);
	printf("[adau] XAudioFormatterDMAStart done.\n");
}

// returns 1 if adau1701 found, otherwise 0
// set audio_tx_buffer and audio_rx_buffer before!
int audio_adau_init(int program_dsp) {
	XIicPs_Config* i2c_config;
	i2c_config = XIicPs_LookupConfig(IIC2_DEVICE_ID);
	int status = XIicPs_CfgInitialize(&Iic2, i2c_config, i2c_config->BaseAddress);
	printf("[adau] XIicPs_CfgInitialize 2: %d\n", status);
	usleep(10000);
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
	// DSP core control: set ADM, DAM, CR
	status = adau_write16(i, 2076, (1<<4)|(1<<3)|(1<<2));
	if (status == 0) {
		printf("[adau] write DSP core control: %d\n", i);
		printf("\n[adau] ~~~~ ZZ9000AX detected. ~~~~\n\n");
	} else {
		printf("[adau] ZZ9000AX not detected.\n");
		return 0;
	}

	status = adau_read16(i, 2076, rbuf);
	if (status == 0) {
		printf("[adau] read: %d %x %x\n", i, rbuf[0], rbuf[1]);
	}

	// DAC setup: DS = 01
	status = adau_write16(i, 2087, 1);
	printf("[adau] write DAC setup: %d\n", status);

	rbuf[0] = 0;
	rbuf[1] = 0;

	status = adau_read16(i, 2087, rbuf);
	printf("[adau] read from 2087: %02x%02x (status: %d)\n", rbuf[0], rbuf[1], status);

	if (program_dsp) {
		program_adau(Program_Data_IC_1, sizeof(Program_Data_IC_1), Param_Data_IC_1, sizeof(Param_Data_IC_1));
	}

	// TODO: OBP/OLRP
	u16 MS  = 1<<11; // clock master output
	//u16 OBF = (0<<10)|(0<<9);    // bclock = 49.152/16 = mclk/4 = 3.072mhz
	u16 OBF = (1<<10)|(0<<9);    // bclock = 49.152/4 = mclk = 12.288mhz
	u16 OLF = (0<<8)|(0<<7);    // lrclock = 49.152/1024 = word clock = 48khz?!
	u16 MSB = 0;    // msb 1
	u16 OWL = 1<<1; // 16 bit
	status = adau_write16(i, 0x081e, MS|OBF|OLF|MSB|OWL);
	printf("[adau] write serial output control: %d\n", status);

	u32 MP0 = 1<<2; // MP02 digital input 0
	u32 MP1 = 1<<6; //
	u32 MP2 = 1<<10; //
	u32 MP3 = 1<<14; //
	u32 MP4 = 1<<18; // MP42 serial clock in
	u32 MP5 = 1<<22; // MP52 serial clock in

	u32 MP6 = 1<<2; //
	u32 MP7 = 1<<6; //
	u32 MP8 = 1<<10; //
	u32 MP9 = 1<<14; //
	u32 MP10 = 1<<18; // MP102 set (serial clock out)
	u32 MP11 = 1<<22; // MP112 set (serial clock out)
	status = adau_write24(i, 0x0820, MP0|MP1|MP2|MP3|MP4|MP5);
	printf("[adau] write MP control 0x820: %d\n", status);
	status = adau_write24(i, 0x0821, MP6|MP7|MP8|MP9|MP10|MP11);
	printf("[adau] write MP control 0x821: %d\n", status);

	status = adau_read24(i, 0x0820, rbuf);
	printf("[adau] read from 0x820: %02x%02x%02x (status: %d)\n", rbuf[0], rbuf[1], rbuf[2], status);
	status = adau_read24(i, 0x0821, rbuf);
	printf("[adau] read from 0x821: %02x%02x%02x (status: %d)\n", rbuf[0], rbuf[1], rbuf[2], status);

	audio_init_i2s();

	return 1;
}

static int interrupt_enabled_audio = 0;

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

// audio formatter interrupt, triggered whenever a period is completed
void isr_audio(void *dummy) {
	uint32_t val = XAudioFormatter_ReadReg(XPAR_XAUDIOFORMATTER_0_BASEADDR, XAUD_FORMATTER_STS + XAUD_FORMATTER_MM2S_OFFSET);
	val |= (1<<31); // clear irq
	XAudioFormatter_WriteReg(XPAR_XAUDIOFORMATTER_0_BASEADDR,
		XAUD_FORMATTER_STS + XAUD_FORMATTER_MM2S_OFFSET, val);

	if (isra_count++>100) {
		printf("[isra]\n");
		isra_count = 0;
	}

	if (interrupt_enabled_audio) {
		amiga_interrupt_set(AMIGA_INTERRUPT_AUDIO);
	}
}

int israrx_count = 0;

// audio formatter interrupt, triggered whenever a period is completed
void isr_audio_rx(void *dummy) {
	uint32_t val = XAudioFormatter_ReadReg(XPAR_XAUDIOFORMATTER_1_BASEADDR, XAUD_FORMATTER_STS + XAUD_FORMATTER_S2MM_OFFSET);
	val |= (1<<31); // clear irq
	XAudioFormatter_WriteReg(XPAR_XAUDIOFORMATTER_1_BASEADDR,
		XAUD_FORMATTER_STS + XAUD_FORMATTER_S2MM_OFFSET, val);

	if (israrx_count++>1000) {
		printf("[isra_rx]\n");
		israrx_count = 0;
	}
}

uint32_t audio_get_dma_transfer_count() {
	return XAudioFormatterGetDMATransferCount(&audio_formatter);
}

void audio_set_interrupt_enabled(int en) {
	printf("[audio] enable irq: %d\n", en);
	interrupt_enabled_audio = en;

	if (!en) {
		amiga_interrupt_clear(AMIGA_INTERRUPT_AUDIO);
	}

	audio_silence();
}

// offset = offset from audio tx buffer
// returns audio_buffer_collision (1 or 0)
int audio_swab(uint16_t audio_buf_samples, uint32_t offset, int byteswap) {
	int audio_buffer_collision = 0;
	uint16_t* data = (uint16_t*)(audio_tx_buffer + offset);
	int audio_freq = audio_buf_samples * 50;

	//printf("[audio:%d] play: %d +%lu\n", byteswap, audio_freq, offset);

	// byteswap
	if (byteswap) {
		for (int i=0; i < audio_buf_samples * 2; i++) {
			data[i] = __builtin_bswap16(data[i]);
		}
	}

	// FIXME missing filter, wonky address calculation
	// resample if other freq
	if (audio_freq != 48000) {
		resample_s16((int16_t*)(audio_tx_buffer + offset),
				(int16_t*)((uint8_t*)audio_tx_buffer+AUDIO_TX_BUFFER_SIZE*2),
				audio_freq,
				48000,
				AUDIO_BYTES_PER_PERIOD/4);
		memcpy(audio_tx_buffer + offset, (uint8_t*)audio_tx_buffer+AUDIO_TX_BUFFER_SIZE*2, AUDIO_BYTES_PER_PERIOD);
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

double resample_cur = 0;
double resample_psampl = 0;
double resample_psampr = 0;

void resample_s16(int16_t *input, int16_t *output, int in_sample_rate,
		int out_sample_rate, int output_samples) {
	double step_dist = ((double) in_sample_rate / (double) out_sample_rate);
	double cur = resample_cur;
	int in_pos1 = 0, in_pos2 = 0;
	double sample1l = 0, sample2l = 0, sample1r = 0, sample2r = 0;

	int inmax = (int) (step_dist * 960.0) - 1;

	for (uint32_t i = 0; i < output_samples; i++) {
		in_pos1 = ((int) cur) - 1;
		in_pos2 = (int) cur;

		// FIXME hack
		if (in_pos2 > inmax) {
			in_pos2 = inmax;
			in_pos1 = inmax - 1;
		}

		double frac2 = cur - (1 + in_pos1);
		double frac1 = (double) 1.0 - frac2;

		if (in_pos1 == -1) {
			sample1l = frac1 * resample_psampl;
			sample1r = frac1 * resample_psampr;
		} else {
			sample1l = frac1 * (double) input[in_pos1 * 2 + 0];
			sample1r = frac1 * (double) input[in_pos1 * 2 + 1];
		}
		sample2l = frac2 * (double) input[in_pos2 * 2 + 0];
		sample2r = frac2 * (double) input[in_pos2 * 2 + 1];

		output[i * 2 + 0] = (int16_t) (sample1l + sample2l);
		output[i * 2 + 1] = (int16_t) (sample1r + sample2r);

		cur += step_dist;
	}

	resample_cur = cur - (int) cur;
	resample_psampl = (double) input[in_pos2 * 2 + 0];
	resample_psampr = (double) input[in_pos2 * 2 + 1];
}

void reset_resampling() {
	resample_cur = 0;
	resample_psampl = 0;
	resample_psampr = 0;
}

void audio_set_tx_buffer(uint8_t* addr) {
	printf("[audio] set tx buffer: %p\n", addr);
	audio_tx_buffer = addr;
	reset_resampling();
}

void audio_set_rx_buffer(uint8_t* addr) {
	printf("[audio] set rx buffer: %p\n", addr);
	audio_rx_buffer = addr;
}

void audio_silence() {
	memset(audio_tx_buffer, 0, AUDIO_TX_BUFFER_SIZE);
	reset_resampling();
}
