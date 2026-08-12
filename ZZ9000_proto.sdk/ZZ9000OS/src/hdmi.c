#include "xiicps.h"
#include "hdmi.h"
#include <stdio.h>
#include <sleep.h>

#define IIC_DEVICE_ID	XPAR_XIICPS_0_DEVICE_ID
#define HDMI_I2C_ADDR 	0x3b
#define IIC_SCLK_RATE	400000
#define I2C_PAUSE 10
#define SII9022_SYS_CTRL 0x1a
#define SII9022_SYS_CTRL_PWR_DWN 0x10

// I2C controller instances
XIicPs Iic;

int i2c_write_byte(XIicPs* iic, u8 i2c_addr, u8 addr, u8 value) {
	u8 buffer[2];
	buffer[0] = addr;
	buffer[1] = value;
	int status;

	while (XIicPs_BusIsBusy(iic)) {};
	status = XIicPs_MasterSendPolled(iic, buffer, 2, i2c_addr);
	while (XIicPs_BusIsBusy(iic)) {};
	usleep(I2C_PAUSE);

	status = XIicPs_MasterSendPolled(iic, buffer, 1, i2c_addr);
	while (XIicPs_BusIsBusy(iic)) {};
	usleep(I2C_PAUSE);
	buffer[1] = 0xff;
	status = XIicPs_MasterRecvPolled(iic, buffer + 1, 1, i2c_addr);

	if (buffer[1] != value) {
		printf("[i2c:%x] new value of 0x%x: 0x%x (should be 0x%x)\n", i2c_addr, addr,
				buffer[1], value);
	}

	return status;
}

int i2c_read_byte(XIicPs* iic, u8 i2c_addr, u8 addr, u8* buffer) {
	buffer[0] = addr;
	buffer[1] = 0xff;
	while (XIicPs_BusIsBusy(iic)) {};
	int status = XIicPs_MasterSendPolled(iic, buffer, 1, i2c_addr);
	while (XIicPs_BusIsBusy(iic)) {};
	usleep(I2C_PAUSE);
	status = XIicPs_MasterRecvPolled(iic, buffer + 1, 1, i2c_addr);

	return status;
}

int hdmi_ctrl_write_byte(u8 addr, u8 value) {
	return i2c_write_byte(&Iic, HDMI_I2C_ADDR, addr, value);
}

int hdmi_ctrl_read_byte(u8 addr, u8* buffer) {
	return i2c_read_byte(&Iic, HDMI_I2C_ADDR, addr, buffer);
}

struct sii9022_reg {
	u8 addr;
	u8 value;
};

static const struct sii9022_reg sii9022_setup[] = {
	{ 0x1e, 0x00 }, /* TPI Device Power State Control Data */
	{ 0x09, 0x00 },
	{ 0x0a, 0x00 },
	{ 0x60, 0x04 }, /* TPI Interrupt Enable */
	{ 0x3c, 0x01 }
};

enum sii9022_mode_index {
	SII_MODE_PIXEL_CLOCK_LSB,
	SII_MODE_PIXEL_CLOCK_MSB,
	SII_MODE_REFRESH_LSB,
	SII_MODE_REFRESH_MSB,
	SII_MODE_HTOTAL_LSB,
	SII_MODE_HTOTAL_MSB,
	SII_MODE_VTOTAL_LSB,
	SII_MODE_VTOTAL_MSB,
	SII_MODE_PIXEL_REPEAT,
	SII_MODE_REG_COUNT
};

static struct sii9022_reg sii9022_mode[SII_MODE_REG_COUNT] = {
	{ 0x00, 0x4c },
	{ 0x01, 0x1d },
	{ 0x02, 0x70 },
	{ 0x03, 0x17 },
	{ 0x04, 0x70 },
	{ 0x05, 0x06 },
	{ 0x06, 0xee },
	{ 0x07, 0x02 },
	{ 0x08, 0x70 }
};

/* Bit 1 causes two purple columns on DVI monitors, so keep DVI/HDMI
 * selection explicit instead of hiding it in a packed register table. */
static u8 sii9022_output_ctrl;

static void hdmi_ctrl_write_regs(const struct sii9022_reg *regs, int count) {
	int i;

	for (i = 0; i < count; i++) {
		hdmi_ctrl_write_byte(regs[i].addr, regs[i].value);
		usleep(1);
	}
}

static void hdmi_set_video_mode(u16 htotal, u16 vtotal, u32 pixelclock_hz,
		u16 vhz, u8 hdmi) {
	/*
	 * SII9022 registers
	 *
	 0x00, 0x4c,	// PixelClock/10000 - LSB
	 0x01, 0x1d,	// PixelClock/10000 - MSB
	 0x02, 0x70,	// Frequency in HZ - LSB
	 0x03, 0x17,	// Vertical Frequency in HZ - MSB
	 0x04, 0x70,	// Total Pixels per line - LSB
	 0x05, 0x06,	// Total Pixels per line - MSB
	 0x06, 0xEE,	// Total Lines - LSB
	 0x07, 0x02,	// Total Lines - MSB
	 0x08, 0x70, // pixel repeat rate?
	 0x1a, 0x00, // 0: DVI, 1: HDMI
	 */

	// see also https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/bridge/sii902x.c#L358
	sii9022_mode[SII_MODE_PIXEL_CLOCK_LSB].value = pixelclock_hz / 10000;
	sii9022_mode[SII_MODE_PIXEL_CLOCK_MSB].value = (pixelclock_hz / 10000) >> 8;
	sii9022_mode[SII_MODE_REFRESH_LSB].value = vhz * 100;
	sii9022_mode[SII_MODE_REFRESH_MSB].value = (vhz * 100) >> 8;
	sii9022_mode[SII_MODE_HTOTAL_LSB].value = htotal;
	sii9022_mode[SII_MODE_HTOTAL_MSB].value = htotal >> 8;
	sii9022_mode[SII_MODE_VTOTAL_LSB].value = vtotal;
	sii9022_mode[SII_MODE_VTOTAL_MSB].value = vtotal >> 8;
	sii9022_output_ctrl = hdmi;
}

static int hdmi_initialized = 0;

static void hdmi_ctrl_initialize(void) {
	XIicPs_Config *config;
	config = XIicPs_LookupConfig(IIC_DEVICE_ID);
	int status = XIicPs_CfgInitialize(&Iic, config, config->BaseAddress);
	//printf("XIicPs_CfgInitialize: %d\n", status);
	usleep(10000);
	//printf("XIicPs is ready: %lx\n", Iic.IsReady);

	status = XIicPs_SelfTest(&Iic);
	//printf("XIicPs_SelfTest: %x\n", status);

	status = XIicPs_SetSClk(&Iic, IIC_SCLK_RATE);
	//printf("XIicPs_SetSClk: %x\n", status);

	usleep(2500);

	// reset
	status = hdmi_ctrl_write_byte(0xc7, 0);

	u8 buffer[2];
	status = hdmi_ctrl_read_byte(0x1b, buffer);
	//printf("[%d] TPI device id: 0x%x\n", status, buffer[1]);
	status = hdmi_ctrl_read_byte(0x1c, buffer);
	//printf("[%d] TPI revision 1: 0x%x\n",status,buffer[1]);

	(void)status;
	hdmi_initialized = 1;
}

void hdmi_ctrl_prepare_mode(struct zz_video_mode *mode) {
	if (!hdmi_initialized) {
		hdmi_ctrl_initialize();
		hdmi_ctrl_write_regs(sii9022_setup,
			sizeof(sii9022_setup) / sizeof(sii9022_setup[0]));
	}

	/* A real TMDS clock loss gives displays an unambiguous retraining
	 * event. Merely changing the live input clock can leave some receivers
	 * latched in "out of range" until their input is toggled. */
	hdmi_ctrl_write_byte(SII9022_SYS_CTRL, SII9022_SYS_CTRL_PWR_DWN);
	hdmi_set_video_mode(mode->hmax, mode->vmax, mode->phz, mode->vhz,
		mode->hdmi);
	hdmi_ctrl_write_regs(sii9022_mode, SII_MODE_REG_COUNT);
}

void hdmi_ctrl_enable_output(void) {
	if (!hdmi_initialized)
		return;

	/* hdmi_set_video_mode() prepared the DVI/HDMI selection while leaving
	 * the power-down bit clear. */
	hdmi_ctrl_write_byte(SII9022_SYS_CTRL, sii9022_output_ctrl);
}

