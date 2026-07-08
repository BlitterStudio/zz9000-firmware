/*
 * ZZ9000 SD/HDF activity LED
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sd_activity_led.h"

#include "xgpiops.h"
#include "xparameters.h"

#define GPIO_DEVICE_ID XPAR_XGPIOPS_0_DEVICE_ID

static XGpioPs sd_activity_gpio;
static int sd_activity_gpio_ready = 0;
static int sd_activity_led_active = 0;
static XTime sd_activity_led_off_at = 0;

static void sd_activity_led_write(int value)
{
	if (sd_activity_gpio_ready) {
		XGpioPs_WritePin(&sd_activity_gpio, SD_ACTIVITY_LED_MIO_PIN, value);
	}
}

int sd_activity_led_init(void)
{
	XGpioPs_Config *ConfigPtr;

	sd_activity_gpio_ready = 0;
	sd_activity_led_active = 0;
	sd_activity_led_off_at = 0;

	ConfigPtr = XGpioPs_LookupConfig(GPIO_DEVICE_ID);
	if (!ConfigPtr) {
		return -1;
	}

	if (XGpioPs_CfgInitialize(&sd_activity_gpio, ConfigPtr,
				  ConfigPtr->BaseAddr) != 0) {
		return -1;
	}

	sd_activity_gpio_ready = 1;
	XGpioPs_SetDirectionPin(&sd_activity_gpio, SD_ACTIVITY_LED_MIO_PIN, 1);
	XGpioPs_SetOutputEnablePin(&sd_activity_gpio, SD_ACTIVITY_LED_MIO_PIN, 1);
	sd_activity_led_write(0);

	return 0;
}

void sd_activity_led_begin(void)
{
	if (!sd_activity_gpio_ready) {
		return;
	}

	if (!sd_activity_led_active) {
		sd_activity_led_write(1);
		sd_activity_led_active = 1;
	}
}

void sd_activity_led_end(void)
{
	XTime now;

	if (!sd_activity_gpio_ready) {
		return;
	}

	XTime_GetTime(&now);
	sd_activity_led_off_at = now + SD_ACTIVITY_LED_STRETCH_TICKS;
}

void sd_activity_led_poll(void)
{
	XTime now;

	if (!sd_activity_gpio_ready || !sd_activity_led_active) {
		return;
	}

	XTime_GetTime(&now);
	if (now >= sd_activity_led_off_at) {
		sd_activity_led_write(0);
		sd_activity_led_active = 0;
	}
}
