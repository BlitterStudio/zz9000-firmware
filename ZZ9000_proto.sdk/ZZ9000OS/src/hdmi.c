#include "xiicps.h"

#define IIC_DEVICE_ID	XPAR_XIICPS_0_DEVICE_ID
#define HDMI_I2C_ADDR 	0x3b
#define IIC_SCLK_RATE	400000
#define I2C_PAUSE 10

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

static u8 sii9022_init[] = {
	0x1e, 0x00,// TPI Device Power State Control Data (R/W)
	0x09, 0x00, //
	0x0a, 0x00,

	0x60, 0x04, 0x3c, 0x01,	// TPI Interrupt Enable (R/W)

	0x1a, 0x10,	// TPI System Control (R/W)

	0x00, 0x4c,	// PixelClock/10000 - LSB          u16:6
	0x01, 0x1d,	// PixelClock/10000 - MSB
	0x02, 0x70,	// Frequency in HZ - LSB
	0x03, 0x17,	// Vertical Frequency in HZ - MSB
	0x04, 0x70,	// Total Pixels per line - LSB
	0x05, 0x06,	// Total Pixels per line - MSB
	0x06, 0xEE,	// Total Lines - LSB
	0x07, 0x02,	// Total Lines - MSB
	0x08, 0x70, // pixel repeat rate?
	0x1a, 0x00, // CTRL_DATA - bit 1 causes 2 purple extra columns on DVI monitors (probably HDMI mode)
};

void hdmi_set_video_mode(u16 htotal, u16 vtotal, u32 pixelclock_hz, u16 vhz, u8 hdmi) {
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
	u8* sii_mode = sii9022_init + 12;

	sii_mode[2 * 0 + 1] = pixelclock_hz / 10000;
	sii_mode[2 * 1 + 1] = (pixelclock_hz / 10000) >> 8;
	sii_mode[2 * 2 + 1] = vhz * 100;
	sii_mode[2 * 3 + 1] = (vhz * 100) >> 8;
	sii_mode[2 * 4 + 1] = htotal;
	sii_mode[2 * 5 + 1] = htotal >> 8;
	sii_mode[2 * 6 + 1] = vtotal;
	sii_mode[2 * 7 + 1] = vtotal >> 8;
	sii_mode[2 * 9 + 1] = hdmi;
}

void hdmi_ctrl_init(struct zz_video_mode *mode) {
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
	//status = hdmi_ctrl_read_byte(0x1d,buffer);
	//printf("[%d] TPI revision 2: 0x%x\n",status,buffer[1]);
	//status = hdmi_ctrl_read_byte(0x30,buffer);
	//printf("[%d] HDCP revision: 0x%x\n",status,buffer[1]);
	//status = hdmi_ctrl_read_byte(0x3d,buffer);
	//printf("[%d] hotplug: 0x%x\n", status, buffer[1]);

	//hdmi_set_video_mode(mode->hmax, mode->vmax, mode->phz, mode->vhz, mode->hdmi);

	for (int i = 0; i < sizeof(sii9022_init); i += 2) {
		status = hdmi_ctrl_write_byte(sii9022_init[i], sii9022_init[i + 1]);
		usleep(1);
	}
}

