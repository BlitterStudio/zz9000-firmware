/*
 * Host unit tests for the SD/HDF activity LED GPIO helper.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <string.h>

#include "xgpiops.h"
#include "xparameters.h"
#include "sd_activity_led.h"

typedef struct {
	int pin;
	int value;
} gpio_call_t;

static int g_checks = 0;
static int g_fails = 0;
static XTime g_now = 0;
static int g_lookup_enabled = 1;
static XGpioPs_Config g_config = { XPAR_XGPIOPS_0_DEVICE_ID, 0x12340000U };

static gpio_call_t g_direction_calls[8];
static gpio_call_t g_output_enable_calls[8];
static gpio_call_t g_write_calls[16];
static int g_direction_count = 0;
static int g_output_enable_count = 0;
static int g_write_count = 0;
static int g_cfg_init_count = 0;

#define CHECK(cond, msg)                                                \
	do {                                                            \
		g_checks++;                                             \
		if (!(cond)) {                                          \
			g_fails++;                                      \
			printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, \
			       __LINE__);                                \
		}                                                       \
	} while (0)

static void reset_mocks(void)
{
	g_now = 0;
	g_lookup_enabled = 1;
	g_direction_count = 0;
	g_output_enable_count = 0;
	g_write_count = 0;
	g_cfg_init_count = 0;
	memset(g_direction_calls, 0, sizeof(g_direction_calls));
	memset(g_output_enable_calls, 0, sizeof(g_output_enable_calls));
	memset(g_write_calls, 0, sizeof(g_write_calls));
}

static void clear_gpio_history(void)
{
	g_direction_count = 0;
	g_output_enable_count = 0;
	g_write_count = 0;
	memset(g_direction_calls, 0, sizeof(g_direction_calls));
	memset(g_output_enable_calls, 0, sizeof(g_output_enable_calls));
	memset(g_write_calls, 0, sizeof(g_write_calls));
}

void XTime_GetTime(XTime *Time)
{
	*Time = g_now;
}

XGpioPs_Config *XGpioPs_LookupConfig(uint16_t DeviceId)
{
	CHECK(DeviceId == XPAR_XGPIOPS_0_DEVICE_ID,
	      "GPIO lookup uses the PS GPIO device");
	return g_lookup_enabled ? &g_config : NULL;
}

int XGpioPs_CfgInitialize(XGpioPs *InstancePtr, XGpioPs_Config *ConfigPtr,
			  uint32_t EffectiveAddr)
{
	CHECK(ConfigPtr == &g_config, "GPIO init uses lookup config");
	CHECK(EffectiveAddr == g_config.BaseAddr, "GPIO init uses config base");
	InstancePtr->base_addr = EffectiveAddr;
	g_cfg_init_count++;
	return 0;
}

void XGpioPs_SetDirectionPin(XGpioPs *InstancePtr, int Pin, int Direction)
{
	CHECK(InstancePtr->base_addr == g_config.BaseAddr,
	      "direction write uses initialized GPIO instance");
	g_direction_calls[g_direction_count++] = (gpio_call_t){ Pin, Direction };
}

void XGpioPs_SetOutputEnablePin(XGpioPs *InstancePtr, int Pin, int OpEnable)
{
	CHECK(InstancePtr->base_addr == g_config.BaseAddr,
	      "output-enable write uses initialized GPIO instance");
	g_output_enable_calls[g_output_enable_count++] =
		(gpio_call_t){ Pin, OpEnable };
}

void XGpioPs_WritePin(XGpioPs *InstancePtr, int Pin, int Data)
{
	CHECK(InstancePtr->base_addr == g_config.BaseAddr,
	      "pin write uses initialized GPIO instance");
	g_write_calls[g_write_count++] = (gpio_call_t){ Pin, Data };
}

static void test_init_configures_mio10_idle_low(void)
{
	printf("test_init_configures_mio10_idle_low\n");
	reset_mocks();

	CHECK(sd_activity_led_init() == 0, "init succeeds when GPIO is present");

	CHECK(g_cfg_init_count == 1, "GPIO initialized once");
	CHECK(g_direction_count == 1, "MIO10 direction configured once");
	CHECK(g_direction_calls[0].pin == SD_ACTIVITY_LED_MIO_PIN,
	      "direction configured on MIO10");
	CHECK(g_direction_calls[0].value == 1, "MIO10 configured as output");
	CHECK(g_output_enable_count == 1, "MIO10 output enabled once");
	CHECK(g_output_enable_calls[0].pin == SD_ACTIVITY_LED_MIO_PIN,
	      "output enabled on MIO10");
	CHECK(g_output_enable_calls[0].value == 1, "MIO10 output enable asserted");
	CHECK(g_write_count == 1, "init writes one idle level");
	CHECK(g_write_calls[0].pin == SD_ACTIVITY_LED_MIO_PIN,
	      "idle level written to MIO10");
	CHECK(g_write_calls[0].value == 0, "MIO10 idles low");
}

static void test_activity_stretches_led_until_deadline(void)
{
	printf("test_activity_stretches_led_until_deadline\n");
	reset_mocks();
	CHECK(sd_activity_led_init() == 0, "init succeeds");
	clear_gpio_history();

	g_now = 1000;
	sd_activity_led_begin();
	sd_activity_led_end();

	CHECK(g_write_count == 1, "activity begins with one high write");
	CHECK(g_write_calls[0].value == 1, "activity drives MIO10 high");

	g_now = 1000 + SD_ACTIVITY_LED_STRETCH_TICKS - 1;
	sd_activity_led_poll();
	CHECK(g_write_count == 1, "poll before deadline leaves LED on");

	g_now = 1000 + SD_ACTIVITY_LED_STRETCH_TICKS;
	sd_activity_led_poll();
	CHECK(g_write_count == 2, "poll at deadline writes LED off");
	CHECK(g_write_calls[1].value == 0, "deadline drives MIO10 low");

	sd_activity_led_poll();
	CHECK(g_write_count == 2, "idle poll does not repeat low writes");
}

static void test_retrigger_extends_visible_activity(void)
{
	printf("test_retrigger_extends_visible_activity\n");
	reset_mocks();
	CHECK(sd_activity_led_init() == 0, "init succeeds");
	clear_gpio_history();

	g_now = 2000;
	sd_activity_led_begin();
	sd_activity_led_end();
	CHECK(g_write_count == 1, "first activity writes high");

	g_now = 2000 + SD_ACTIVITY_LED_STRETCH_TICKS / 2;
	sd_activity_led_begin();
	sd_activity_led_end();
	CHECK(g_write_count == 1, "retrigger while lit does not rewrite high");

	g_now = 2000 + SD_ACTIVITY_LED_STRETCH_TICKS;
	sd_activity_led_poll();
	CHECK(g_write_count == 1, "first deadline is ignored after retrigger");

	g_now = 2000 + SD_ACTIVITY_LED_STRETCH_TICKS / 2 +
		SD_ACTIVITY_LED_STRETCH_TICKS;
	sd_activity_led_poll();
	CHECK(g_write_count == 2, "extended deadline writes LED off");
	CHECK(g_write_calls[1].value == 0, "retriggered activity ends low");
}

static void test_missing_gpio_config_disables_led(void)
{
	printf("test_missing_gpio_config_disables_led\n");
	reset_mocks();
	g_lookup_enabled = 0;

	CHECK(sd_activity_led_init() != 0, "init fails without GPIO config");
	sd_activity_led_begin();
	sd_activity_led_end();
	g_now += SD_ACTIVITY_LED_STRETCH_TICKS;
	sd_activity_led_poll();

	CHECK(g_cfg_init_count == 0, "missing config skips GPIO init");
	CHECK(g_direction_count == 0, "missing config skips direction writes");
	CHECK(g_output_enable_count == 0,
	      "missing config skips output-enable writes");
	CHECK(g_write_count == 0, "missing config skips pin writes");
}

int main(void)
{
	test_init_configures_mio10_idle_low();
	test_activity_stretches_led_until_deadline();
	test_retrigger_extends_visible_activity();
	test_missing_gpio_config_disables_led();
	printf("\n%d checks, %d failures\n", g_checks, g_fails);
	return g_fails ? 1 : 0;
}
