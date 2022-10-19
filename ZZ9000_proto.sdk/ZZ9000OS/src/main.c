/*
 * MNT ZZ9000 Amiga Graphics and Coprocessor Card Operating System (ZZ9000OS)
 *
 * Copyright (C) 2019, Lukas F. Hartmann <lukas@mntre.com>
 *                     MNT Research GmbH, Berlin
 *                     https://mntre.com
 *
 * More Info: https://mntre.com/zz9000
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * GNU General Public License v3.0 or later
 *
 * https://spdx.org/licenses/GPL-3.0-or-later.html
 *
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <math.h>

#include "platform.h"
#include "xil_printf.h"
#include "xparameters.h"
#include "xil_io.h"
#include "xscugic.h"
#include "xgpiops.h"
#include "sleep.h"
#include "xil_cache.h"
#include "xil_exception.h"
#include "xclk_wiz.h"
#include "xtime_l.h"
#include "xi2stx.h"
#include "xi2srx.h"

// workaround for typo in xilinx C code
void Xil_AssertNonVoid() {}

#include "memorymap.h"
#include "mntzorro.h"
#include "video.h"
#include "hdmi.h"
#include "gfx.h"
#include "ethernet.h"
#include "usb.h"
#include "interrupt.h"
#include "bootrom.h"
#include "core2.h"
#include "adc.h"
#include "ax.h"
#include "mp3/mp3.h"

#include "zz_regs.h"
#include "zz_video_modes.h"

#define REVISION_MAJOR 1
#define REVISION_MINOR 12

#define GPIO_DEVICE_ID	XPAR_XGPIOPS_0_DEVICE_ID

void disable_reset_out() {
	XGpioPs Gpio;
	XGpioPs_Config *ConfigPtr;
	ConfigPtr = XGpioPs_LookupConfig(GPIO_DEVICE_ID);
	XGpioPs_CfgInitialize(&Gpio, ConfigPtr, ConfigPtr->BaseAddr);
	int output_pin = 7;

	XGpioPs_SetDirectionPin(&Gpio, output_pin, 1);
	XGpioPs_SetOutputEnablePin(&Gpio, output_pin, 1);
	XGpioPs_WritePin(&Gpio, output_pin, 0);
	usleep(10000);
	XGpioPs_WritePin(&Gpio, output_pin, 1);
	print("[gpio] ethernet reset done.\r\n");

	// FIXME
	int adau_reset = 11;
	XGpioPs_SetDirectionPin(&Gpio, adau_reset, 1);
	XGpioPs_SetOutputEnablePin(&Gpio, adau_reset, 1);
	XGpioPs_WritePin(&Gpio, adau_reset, 0);
	usleep(10000);
	XGpioPs_WritePin(&Gpio, adau_reset, 1);

	print("[gpio] ADAU reset done.\r\n");
}

u32 blitter_colormode = MNTVA_COLOR_32BIT;
static u32 blitter_dst_offset = 0;
static u32 blitter_src_offset = 0;

struct ZZ_VIDEO_STATE* video_state;

// FIXME
// This is the absolute offset in ZZ9000 RAM for the "framebuffer transfer register",
// which can be replaced by the DMA acceleration functionality entirely, but some
// software still relies on this legacy register.
unsigned int cur_mem_offset = 0x3500000;

static char usb_storage_available = 0;
static uint32_t usb_storage_read_block = 0;
static uint32_t usb_storage_write_block = 0;

// ethernet state
uint16_t ethernet_send_result = 0;
int eth_backlog_nag_counter = 0;
int interrupt_enabled_ethernet = 0;

// usb state
uint16_t usb_status = 0;
uint32_t usb_read_write_num_blocks = 1;
// debug things like individual reads/writes, greatly slowing the system down
uint32_t debug_lowlevel = 0;

// audio state (ZZ9000AX)
static int audio_buffer_collision = 0;
static uint32_t audio_scale = 48000/50;
static uint32_t audio_offset = 0;
static int adau_enabled = 0;
int interrupt_enabled_audio = 0;

// debug test state
static uint32_t zz_debug_test_counter = 0;
static uint32_t zz_debug_test_prev = 0;
static uint32_t zz_debug_test_ms = 0;

void handle_amiga_reset() {
	printf("    _______________   ___   ___   ___  \n");
	printf("   |___  /___  / _ \\ / _ \\ / _ \\ / _ \\ \n");
	printf("      / /   / / (_) | | | | | | | | | |\n");
	printf("     / /   / / \\__, | | | | | | | | | |\n");
	printf("    / /__ / /__  / /| |_| | |_| | |_| |\n");
	printf("   /_____/_____|/_/  \\___/ \\___/ \\___/ \n\n");

	video_reset();

	// stop audio
	audio_set_tx_buffer((uint8_t*)AUDIO_TX_BUFFER_ADDRESS);
	audio_silence();
	audio_set_rx_buffer((uint8_t*)AUDIO_RX_BUFFER_ADDRESS);

	// usb
	usb_storage_available = zz_usb_init();
	usb_status = 0;
	usb_read_write_num_blocks = 1;

	// ethernet
	ethernet_send_result = 0;
	eth_backlog_nag_counter = 0;
	interrupt_enabled_ethernet = 0;
	interrupt_enabled_audio = 0;

	// FIXME document
	cur_mem_offset = 0x3500000;

	// FIXME
	memset((u32 *)Z3_SCRATCH_ADDR, 0, sizeof(struct GFXData));

	// clear audio buffer on reset
	memset((void*)AUDIO_TX_BUFFER_ADDRESS, 0, AUDIO_TX_BUFFER_SIZE);

	// FIXME test content for audio buffer
	/*int16_t* adata = (uint16_t*)(((void*)AUDIO_TX_BUFFER_ADDRESS));
	float f = 1;
	for (int i=0; i<AUDIO_TX_BUFFER_SIZE/2; i++) {
		adata[i] = (sin((float)i/200.0)*65536)*f;
		f-=0.0001;
	}*/

	// reset ADAU
	mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG5, 8 | 0);
	mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG5, 8 | 4);
	mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG5, 0);

	adau_enabled = audio_adau_init(1);

	// clear interrupt holding amiga
	amiga_interrupt_clear(0xffffffff);

	// Used for testing the nonstandard VSync modes without the driver having to enable them.
	//card_feature_enabled[CARD_FEATURE_NONSTANDARD_VSYNC] = 1;
}

int main() {
	init_platform();

	boot_rom_init();

	disable_reset_out();

	video_state = video_init();

	xadc_init();

	interrupt_configure();

	ethernet_init();

	fpga_interrupt_connect(isr_video, isr_audio, isr_audio_rx);

	handle_amiga_reset();

	// ARM app run environment
	arm_app_init();
	volatile struct ZZ9K_ENV* arm_run_env = arm_app_get_run_env();
	uint32_t arm_run_address = 0;

	// graphics temporary registers
	uint16_t rect_x1 = 0;
	uint16_t rect_x2 = 0;
	uint16_t rect_x3 = 0;
	uint16_t rect_y1 = 0;
	uint16_t rect_y2 = 0;
	uint16_t rect_y3 = 0;
	uint16_t blitter_dst_pitch = 0;
	uint32_t rect_rgb = 0;
	uint32_t rect_rgb2 = 0;
	uint32_t blitter_colormode = MNTVA_COLOR_32BIT;
	uint16_t blitter_src_pitch = 0;
	uint16_t blitter_user1 = 0;
	uint16_t blitter_user2 = 0;

	// custom video mode
	int custom_video_mode = ZZVMODE_CUSTOM;
	int custom_vmode_param = VMODE_PARAM_HRES;

	// zorro state
	u32 zstate_raw;
	int need_req_ack = 0;

	// audio parameters (buffer locations)
	uint16_t audio_params[ZZ_NUM_AUDIO_PARAMS];
	int audio_param = 0; // selected parameter
	int audio_request_init = 0;

	// decoder parameters (mp3 etc)
	const int ZZ_NUM_DECODER_PARAMS = 8;
	uint16_t decoder_params[ZZ_NUM_DECODER_PARAMS];
	int decoder_param = 0; // selected parameter
	int decoder_bytes_decoded = 0;

	// idle task counter (used for ethernet negotiation)
	int idle_task_count = 0;

	while (1) {
		u32 zstate = mntzorro_read(MNTZ_BASE_ADDR, MNTZORRO_REG3);
		zstate_raw = zstate;
		u32 writereq = (zstate & (1 << 31));
		u32 readreq = (zstate & (1 << 30));

		if (writereq) {
			u32 zaddr = mntzorro_read(MNTZ_BASE_ADDR, MNTZORRO_REG0);
			u32 zdata = mntzorro_read(MNTZ_BASE_ADDR, MNTZORRO_REG1);

			u32 ds3 = (zstate_raw & (1 << 29));
			u32 ds2 = (zstate_raw & (1 << 28));
			u32 ds1 = (zstate_raw & (1 << 27));
			u32 ds0 = (zstate_raw & (1 << 26));

			if (debug_lowlevel) {
				printf("WRTE: %08lx <- %08lx [%d%d%d%d]\n",zaddr,zdata,!!ds3,!!ds2,!!ds1,!!ds0);
			}

			if (zaddr > 0x10000000) {
				printf("ERRW illegal address %08lx\n", zaddr);
			} else if (zaddr >= MNT_FB_BASE || zaddr >= MNT_REG_BASE + 0x2000) {
				u8* ptr = (u8*)FRAMEBUFFER_ADDRESS;

				if (zaddr >= MNT_FB_BASE) {
					ptr = ptr + zaddr - MNT_FB_BASE;
				} else if (zaddr < MNT_REG_BASE + 0x8000) {
					// NOP (RX frame is here)
				} else if (zaddr < MNT_REG_BASE + 0xa000) {
					// 0x8000 - 0x9fff ETH TX frame (Z2)
					ptr = (u8*)TX_FRAME_ADDRESS + zaddr - MNT_REG_BASE - 0x8000;
				} else if (zaddr < MNT_REG_BASE + 0x10000) {
					// 0xa000 - 0xffff USB block storage (Z2)
					ptr = (u8*)USB_BLOCK_STORAGE_ADDRESS + zaddr - MNT_REG_BASE - 0xa000;
				}

				// FIXME cache this
				u32 z3 = (zstate_raw & (1 << 25));

				if (z3) {
					if (ds3) ptr[0] = zdata >> 24;
					if (ds2) ptr[1] = zdata >> 16;
					if (ds1) ptr[2] = zdata >> 8;
					if (ds0) ptr[3] = zdata;
				} else {
					// swap bytes
					if (ds1) ptr[0] = zdata >> 8;
					if (ds0) ptr[1] = zdata;
				}
			} else if (zaddr >= MNT_REG_BASE && zaddr < MNT_FB_BASE) {
				// register area
				//printf("REGW: %08lx <- %08lx [%d%d%d%d]\n",zaddr,zdata,!!ds3,!!ds2,!!ds1,!!ds0);

				u32 z3 = (zstate_raw & (1 << 25));
				if (z3) {
					// convert 32bit to 16bit addresses
					if (ds3 && ds2) {
						zdata = zdata >> 16;
					} else if (ds1 && ds0) {
						zdata = zdata & 0xffff;
						zaddr += 2;
					} else {
						zaddr = 0; // cancel
					}
				}
				//printf("CONV: %08lx <- %08lx\n",zaddr,zdata);

				switch (zaddr) {
				// Various blitter/video registers
				case REG_ZZ_PAN_HI:
					video_state->framebuffer_pan_offset = zdata << 16;
					break;
				case REG_ZZ_PAN_LO:
					video_state->framebuffer_pan_offset |= zdata;

					// FIXME cursor offset support for p96 split screen
					video_state->sprite_x_offset = rect_x1;
					video_state->sprite_y_offset = rect_y1;

					// FIXME: document/comment this. rect_x1/x2/y1 are used for panning inside of a screen
					// together with blitter_colormode
					// TODO: rework to dedicated registers because this makes it hard to debug

					video_state->framebuffer_pan_width = rect_x2;
					u32 framebuffer_color_format = blitter_colormode;
					video_state->framebuffer_pan_offset += (rect_x1 << blitter_colormode);
					if (video_state->split_pos == 0) {
						video_state->framebuffer_pan_offset += (rect_y1 * (video_state->framebuffer_pan_width << framebuffer_color_format));
					}
					break;

				case REG_ZZ_BLIT_SRC_HI:
					blitter_src_offset = zdata << 16;
					break;
				case REG_ZZ_BLIT_SRC_LO:
					blitter_src_offset |= zdata;
					break;
				case REG_ZZ_BLIT_DST_HI:
					blitter_dst_offset = zdata << 16;
					break;
				case REG_ZZ_BLIT_DST_LO:
					blitter_dst_offset |= zdata;
					break;

				case REG_ZZ_COLORMODE:
					blitter_colormode = zdata;
					// hack
					if (blitter_colormode == MNTVA_COLOR_15BIT) blitter_colormode = MNTVA_COLOR_16BIT565;
					break;
				case REG_ZZ_CONFIG:
					// enable/disable INT6, currently used to signal incoming ethernet packets
					if (zdata & 8) {
						// clear/ack
						if (zdata & 16) {
							amiga_interrupt_clear(AMIGA_INTERRUPT_ETH);
						}
						if (zdata & 32) {
							amiga_interrupt_clear(AMIGA_INTERRUPT_AUDIO);
						}
					} else {
						printf("[enable] eth: %d\n", (int)zdata);
						interrupt_enabled_ethernet = zdata & 1;

						if (!interrupt_enabled_ethernet) {
							amiga_interrupt_clear(AMIGA_INTERRUPT_ETH);
						}
					}
					break;
				case REG_ZZ_MODE: {
					int mode = zdata & 0xff;
					int colormode = (zdata & 0xf00) >> 8;
					int scalemode = (zdata & 0xf000) >> 12;

					video_mode_init(mode, scalemode, colormode);

					// FIXME
					// remember selected video mode
					// video_mode = zdata;
					break;
				}
				case REG_ZZ_VCAP_MODE:
					printf("videocap default mode select: %lx\n", zdata);
					video_state->videocap_video_mode = zdata & 0xff;
					break;
				//case REG_ZZ_SPRITE_X:
				case REG_ZZ_SPRITE_Y:
					if (!video_state->sprite_showing)
						break;

					video_state->sprite_x_base = (int16_t)rect_x1;
					video_state->sprite_y_base = (int16_t)rect_y1;
					update_hw_sprite_pos(video_state->sprite_x_base, video_state->sprite_y_base);

					break;
				case REG_ZZ_SPRITE_BITMAP: {
					if (zdata == 1) { // Hardware sprite enabled
						hw_sprite_show(1);
						break;
					}
					else if (zdata == 2) { // Hardware sprite disabled
						hw_sprite_show(0);
						break;
					}

					uint8_t* bmp_data = (uint8_t*) ((u32) video_state->framebuffer
							+ blitter_src_offset);

					video_state->sprite_x_offset = rect_x1;
					video_state->sprite_y_offset = rect_y1;
					video_state->sprite_width  = rect_x2;
					video_state->sprite_height = rect_y2;

					clear_hw_sprite();
					update_hw_sprite(bmp_data);
					update_hw_sprite_pos();
					break;
				}
				case REG_ZZ_SPRITE_COLORS: {
					video_state->sprite_colors[zdata] = (blitter_user1 << 16) | blitter_user2;
					if (zdata != 0 && video_state->sprite_colors[zdata] == 0xff00ff)
						video_state->sprite_colors[zdata] = 0xfe00fe;
					break;
				}
				case REG_ZZ_SRC_PITCH:
					blitter_src_pitch = zdata;
					break;

				case REG_ZZ_X1:
					rect_x1 = zdata;
					break;
				case REG_ZZ_Y1:
					rect_y1 = zdata;
					break;
				case REG_ZZ_X2:
					rect_x2 = zdata;
					break;
				case REG_ZZ_Y2:
					rect_y2 = zdata;
					break;
				case REG_ZZ_ROW_PITCH:
					blitter_dst_pitch = zdata;
					break;
				case REG_ZZ_X3:
					rect_x3 = zdata;
					break;
				case REG_ZZ_Y3:
					rect_y3 = zdata;
					break;

				case REG_ZZ_USER1:
					blitter_user1 = zdata;
					break;
				case REG_ZZ_USER2:
					blitter_user2 = zdata;
					break;
				case REG_ZZ_USER3:
					// FIXME unused
					break;
				case REG_ZZ_USER4:
					// FIXME unused
					break;

				case REG_ZZ_RGB_HI:
					rect_rgb &= 0xffff0000;
					rect_rgb |= (((zdata & 0xff) << 8) | zdata >> 8);
					break;
				case REG_ZZ_RGB_LO:
					rect_rgb &= 0x0000ffff;
					rect_rgb |= (((zdata & 0xff) << 8) | zdata >> 8) << 16;
					break;
				case REG_ZZ_RGB2_HI:
					rect_rgb2 &= 0xffff0000;
					rect_rgb2 |= (((zdata & 0xff) << 8) | zdata >> 8);
					break;
				case REG_ZZ_RGB2_LO:
					rect_rgb2 &= 0x0000ffff;
					rect_rgb2 |= (((zdata & 0xff) << 8) | zdata >> 8) << 16;
					break;

				// Generic acceleration ops
				case REG_ZZ_ACC_OP: {
					handle_acc_op(zdata);
					break;
				}

				// DMA RTG rendering
				case REG_ZZ_BITTER_DMA_OP: {
					handle_blitter_dma_op(video_state, zdata);
					break;
				}

				// RTG rendering
				case REG_ZZ_FILLRECT:
					set_fb((uint32_t*) ((u32)video_state->framebuffer + blitter_dst_offset),
							blitter_dst_pitch);
					uint8_t mask = zdata;

					if (mask == 0xFF)
						fill_rect_solid(rect_x1, rect_y1, rect_x2, rect_y2,
								rect_rgb, blitter_colormode);
					else
						fill_rect(rect_x1, rect_y1, rect_x2, rect_y2, rect_rgb,
								blitter_colormode, mask);
					break;

				case REG_ZZ_COPYRECT: {
					set_fb((uint32_t*) ((u32)video_state->framebuffer + blitter_dst_offset),
							blitter_dst_pitch);
					mask = (blitter_colormode >> 8);

					switch (zdata) {
					case 1: // Regular BlitRect
						if (mask == 0xFF || (mask != 0xFF && (blitter_colormode & 0x0F)) != MNTVA_COLOR_8BIT)
							copy_rect_nomask(rect_x1, rect_y1, rect_x2, rect_y2, rect_x3,
											rect_y3, blitter_colormode & 0x0F,
											(uint32_t*) ((u32)video_state->framebuffer
													+ blitter_dst_offset),
											blitter_dst_pitch, MINTERM_SRC);
						else
							copy_rect(rect_x1, rect_y1, rect_x2, rect_y2, rect_x3,
									rect_y3, blitter_colormode & 0x0F,
									(uint32_t*) ((u32)video_state->framebuffer
											+ blitter_dst_offset),
									blitter_dst_pitch, mask);
						break;
					case 2: // BlitRectNoMaskComplete
						copy_rect_nomask(rect_x1, rect_y1, rect_x2, rect_y2, rect_x3,
										rect_y3, blitter_colormode & 0x0F,
										(uint32_t*) ((u32)video_state->framebuffer
												+ blitter_src_offset),
										blitter_src_pitch, mask); // Mask in this case is minterm/opcode.
						break;
					}

					break;
				}

				case REG_ZZ_FILLTEMPLATE: {
					uint8_t draw_mode = blitter_colormode >> 8;
					uint8_t* tmpl_data = (uint8_t*) ((u32)video_state->framebuffer
							+ blitter_src_offset);
					set_fb((uint32_t*) ((u32)video_state->framebuffer + blitter_dst_offset),
							blitter_dst_pitch);

					uint8_t bpp = 2 * (blitter_colormode & 0xff);
					if (bpp == 0)
						bpp = 1;
					uint16_t loop_rows = 0;
					mask = zdata;

					if (zdata & 0x8000) {
						// pattern mode
						// TODO yoffset
						loop_rows = zdata & 0xff;
						mask = blitter_user1;
						blitter_src_pitch = 16;
						pattern_fill_rect((blitter_colormode & 0x0F), rect_x1,
								rect_y1, rect_x2, rect_y2, draw_mode, mask,
								rect_rgb, rect_rgb2, rect_x3, rect_y3, tmpl_data,
								blitter_src_pitch, loop_rows);
					}
					else {
						template_fill_rect((blitter_colormode & 0x0F), rect_x1,
								rect_y1, rect_x2, rect_y2, draw_mode, mask,
								rect_rgb, rect_rgb2, rect_x3, rect_y3, tmpl_data,
								blitter_src_pitch);
					}

					break;
				}

				case REG_ZZ_SCRATCH_COPY: { // Copy from scratch area
					// FIXME for what?
					for (int i = 0; i < rect_y1; i++) {
						memcpy	((uint32_t*) ((u32)video_state->framebuffer + video_state->framebuffer_pan_offset + (i * rect_x1)),
								 (uint32_t*) ((u32)Z3_SCRATCH_ADDR + (i * rect_x1)),
								 rect_x1);
					}
					break;
				}

				case REG_ZZ_CVMODE_PARAM: // Custom video mode param
					// FIXME
					custom_vmode_param = zdata;
					break;

				case REG_ZZ_CVMODE_VAL: { // Custom video mode data
					struct zz_video_mode* vm = get_custom_video_mode_ptr(custom_video_mode);
					int *target = &vm->hres;
					switch(custom_vmode_param) {
						case VMODE_PARAM_VRES: target = &vm->vres; break;
						case VMODE_PARAM_HSTART: target = &vm->hstart; break;
						case VMODE_PARAM_HEND: target = &vm->hend; break;
						case VMODE_PARAM_HMAX: target = &vm->hmax; break;
						case VMODE_PARAM_VSTART: target = &vm->vstart; break;
						case VMODE_PARAM_VEND: target = &vm->vend; break;
						case VMODE_PARAM_VMAX: target = &vm->vmax; break;
						case VMODE_PARAM_POLARITY: target = &vm->polarity; break;
						case VMODE_PARAM_MHZ: target = &vm->mhz; break;
						case VMODE_PARAM_PHZ: target = &vm->phz; break;
						case VMODE_PARAM_VHZ: target = &vm->vhz; break;
						case VMODE_PARAM_HDMI: target = &vm->hdmi; break;
						case VMODE_PARAM_MUL: target = &vm->mul; break;
						case VMODE_PARAM_DIV: target = &vm->div; break;
						case VMODE_PARAM_DIV2: target = &vm->div2; break;
						default: break;
					}

					*target = zdata;
					break;
				}

				case REG_ZZ_CVMODE_SEL: // Set custom video mode index
					custom_video_mode = zdata;
					break;

				case REG_ZZ_CVMODE: // Set custom video mode without any questions asked.
					// This assumes that the custom video mode is 640x480 or higher resolution.
					video_mode_init(custom_video_mode, video_state->scalemode, video_state->colormode);
					break;

				case REG_ZZ_SET_FEATURE:
					switch (blitter_user1) {
						case CARD_FEATURE_SECONDARY_PALETTE:
							printf("[feature] SECONDARY_PALETTE: %lu\n",zdata);
							// Enables/disables the secondary palette on screen split with P96 3.10+
							video_state->card_feature_enabled[CARD_FEATURE_SECONDARY_PALETTE] = zdata;
							break;
						case CARD_FEATURE_NONSTANDARD_VSYNC:
							printf("[feature] NONSTANDARD_VSYNC: %lu\n",zdata);
							// Enables/disables the nonstandard refresh rates for scandoubled PAL/NTSC HDMI output modes.
							if (zdata == 2) {
								video_state->scandoubler_mode_adjust = 2;
							} else {
								video_state->scandoubler_mode_adjust = 0;
							}
							video_state->card_feature_enabled[CARD_FEATURE_NONSTANDARD_VSYNC] = zdata;
							break;
						default:
							break;
					}
					break;

				case REG_ZZ_P2C: {
					uint8_t draw_mode = blitter_colormode >> 8;
					uint8_t planes = (zdata & 0xFF00) >> 8;
					uint8_t mask = (zdata & 0xFF);
					uint8_t layer_mask = blitter_user2;
					uint8_t* bmp_data = (uint8_t*) ((u32)video_state->framebuffer
							+ blitter_src_offset);

					set_fb((uint32_t*) ((u32)video_state->framebuffer + blitter_dst_offset),
							blitter_dst_pitch);

					p2c_rect(rect_x1, 0, rect_x2, rect_y2, rect_x3,
							rect_y3, draw_mode, planes, mask,
							layer_mask, blitter_src_pitch, bmp_data);
					break;
				}

				case REG_ZZ_P2D: {
					uint8_t draw_mode = blitter_colormode >> 8;
					uint8_t planes = (zdata & 0xFF00) >> 8;
					uint8_t mask = (zdata & 0xFF);
					uint8_t layer_mask = blitter_user2;
					uint8_t* bmp_data = (uint8_t*) ((u32)video_state->framebuffer
							+ blitter_src_offset);

					set_fb((uint32_t*) ((u32)video_state->framebuffer + blitter_dst_offset),
							blitter_dst_pitch);
					p2d_rect(rect_x1, 0, rect_x2, rect_y2, rect_x3,
							rect_y3, draw_mode, planes, mask, layer_mask, rect_rgb,
							blitter_src_pitch, bmp_data, (blitter_colormode & 0x0F));
					break;
				}

				case REG_ZZ_DRAWLINE: {
					uint8_t draw_mode = blitter_colormode >> 8;
					set_fb((uint32_t*) ((u32)video_state->framebuffer + blitter_dst_offset),
							blitter_dst_pitch);

					// rect_x3 contains the pattern. if all bits are set for both the mask and the pattern,
					// there's no point in passing non-essential data to the pattern/mask aware function.

					if (rect_x3 == 0xFFFF && zdata == 0xFF)
						draw_line_solid(rect_x1, rect_y1, rect_x2, rect_y2,
								blitter_user1, rect_rgb,
								(blitter_colormode & 0x0F));
					else
						draw_line(rect_x1, rect_y1, rect_x2, rect_y2,
								blitter_user1, rect_x3, rect_y3, rect_rgb,
								rect_rgb2, (blitter_colormode & 0x0F), zdata,
								draw_mode);
					break;
				}

				case REG_ZZ_INVERTRECT:
					set_fb((uint32_t*) ((u32)video_state->framebuffer + blitter_dst_offset),
							blitter_dst_pitch);
					invert_rect(rect_x1, rect_y1, rect_x2, rect_y2,
							zdata & 0xFF, blitter_colormode);
					break;

				case REG_ZZ_SET_SPLIT_POS:
					video_state->bgbuf_offset = blitter_src_offset;
					video_state->split_request_pos = zdata;
					break;

				// Ethernet
				case REG_ZZ_ETH_TX:
					ethernet_send_result = ethernet_send_frame(zdata);
					//printf("SEND frame sz: %ld res: %d\n",zdata,ethernet_send_result);
					break;
				case REG_ZZ_ETH_RX: {
					//printf("RECV eth frame sz: %ld\n",zdata);
					int frfb = ethernet_receive_frame();
					mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG4, frfb);
					break;
				}
				case REG_ZZ_ETH_MAC_HI: {
					uint8_t* mac = ethernet_get_mac_address_ptr();
					mac[0] = (zdata & 0xff00) >> 8;
					mac[1] = (zdata & 0x00ff);
					break;
				}
				case REG_ZZ_ETH_MAC_HI2: {
					uint8_t* mac = ethernet_get_mac_address_ptr();
					mac[2] = (zdata & 0xff00) >> 8;
					mac[3] = (zdata & 0x00ff);
					break;
				}
				case REG_ZZ_ETH_MAC_LO: {
					uint8_t* mac = ethernet_get_mac_address_ptr();
					mac[4] = (zdata & 0xff00) >> 8;
					mac[5] = (zdata & 0x00ff);
					ethernet_update_mac_address();
					break;
				}
				case REG_ZZ_USBBLK_TX_HI: {
					usb_storage_write_block = ((u32) zdata) << 16;
					break;
				}
				case REG_ZZ_USBBLK_TX_LO: {
					usb_storage_write_block |= zdata;
					if (usb_storage_available) {
						usb_status = zz_usb_write_blocks(0, usb_storage_write_block, usb_read_write_num_blocks, (void*)USB_BLOCK_STORAGE_ADDRESS);
					} else {
						printf("[USB] TX but no storage available!\n");
					}
					break;
				}
				case REG_ZZ_USBBLK_RX_HI: {
					usb_storage_read_block = ((u32) zdata) << 16;
					break;
				}
				case REG_ZZ_USBBLK_RX_LO: {
					usb_storage_read_block |= zdata;
					if (usb_storage_available) {
						usb_status = zz_usb_read_blocks(0, usb_storage_read_block, usb_read_write_num_blocks, (void*)USB_BLOCK_STORAGE_ADDRESS);
					} else {
						printf("[USB] RX but no storage available!\n");
					}
					break;
				}
				case REG_ZZ_USB_STATUS: {
					//printf("[USB] write to status/blocknum register: %d\n", zdata);
					if (zdata==0) {
						// reset USB
						// FIXME memory leaks?
						//usb_storage_available = zz_usb_init();
					} else {
						// set number of blocks to read/write at once
						usb_read_write_num_blocks = zdata;
					}
					break;
				}
				case REG_ZZ_USB_BUFSEL: {
					// FIXME: obsolete!
					break;
				}
				case REG_ZZ_DEBUG: {
					debug_lowlevel = zdata;
					break;
				}
				case REG_ZZ_DEBUG_TIMER: {
					audio_debug_timer(zdata);
					break;
				}
				case REG_ZZ_PRINT_CHR: {
					printf("%c",(int)(zdata&0xff));
					break;
				}
				case REG_ZZ_PRINT_HEX: {
					// print zdata has hex (follow up by \n via chr!)
					printf("%04x", (unsigned int)(zdata&0xffff));
					break;
				}
				case REG_ZZ_AUDIO_CONFIG: {
					// audio config
					audio_set_interrupt_enabled((int)(zdata & 1));
					break;
				}

				// ARM core 2 execution
				case REG_ZZ_ARM_RUN_HI:
					arm_run_address = ((u32) zdata) << 16;
					break;
				case REG_ZZ_ARM_RUN_LO:
					arm_run_address |= zdata;
					arm_app_run(arm_run_address);
					break;
				case REG_ZZ_ARM_ARGC:
					arm_run_env->argc = zdata;
					break;
				case REG_ZZ_ARM_ARGV0:
					arm_run_env->argv[0] = ((u32) zdata) << 16;
					break;
				case REG_ZZ_ARM_ARGV1:
					arm_run_env->argv[0] |= zdata;
					break;
				case REG_ZZ_ARM_ARGV2:
					arm_run_env->argv[1] = ((u32) zdata) << 16;
					break;
				case REG_ZZ_ARM_ARGV3:
					arm_run_env->argv[1] |= zdata;
					break;
				case REG_ZZ_ARM_ARGV4:
					arm_run_env->argv[2] = ((u32) zdata) << 16;
					break;
				case REG_ZZ_ARM_ARGV5:
					arm_run_env->argv[2] |= zdata;
					break;
				case REG_ZZ_ARM_ARGV6:
					arm_run_env->argv[3] = ((u32) zdata) << 16;
					break;
				case REG_ZZ_ARM_ARGV7:
					arm_run_env->argv[3] |= zdata;
					break;
				case REG_ZZ_ARM_EV_CODE:
					arm_app_input_event(zdata);
					break;
				case REG_ZZ_AUDIO_SWAB:
					{
						int byteswap = 1;
						if (zdata&(1<<15)) byteswap = 0;
						audio_offset = (zdata&0x7fff)<<8; // *256
						audio_buffer_collision = audio_swab(audio_scale, audio_offset, byteswap);

						break;
					}
				case REG_ZZ_AUDIO_SCALE:
					audio_scale = zdata;
					break;
				case REG_ZZ_UNUSED_REG8C:
					// set up a test (set sleep time, and set counter to 0)
					zz_debug_test_ms = zdata;
					zz_debug_test_counter = 0;
					zz_debug_test_prev = 0;
					printf("[zzdebug] test reset, time: %lu\n", zz_debug_test_ms);
					break;

				case REG_ZZ_UNUSED_REG8E:
					// increase counter by one and compare with the number we are sent
					if (zdata > 0 && zz_debug_test_prev != zdata-1) {
						printf("[zzdebug] loss! zdata: %lu prev: %lu counter: %lu\n", zdata, zz_debug_test_prev, zz_debug_test_counter);
					}
					usleep(zz_debug_test_ms*1000);
					zz_debug_test_counter++;
					zz_debug_test_prev = zdata;
					break;

				case REG_ZZ_AUDIO_PARAM:
					printf("[REG_ZZ_AUDIO_PARAM] %lx\n", zdata);

					if (zdata<ZZ_NUM_AUDIO_PARAMS) {
						audio_param = zdata;
					} else {
						audio_param = 0;
					}
					break;
				case REG_ZZ_AUDIO_VAL:
					printf("[REG_ZZ_AUDIO_VAL] %lx\n", zdata);

					audio_params[audio_param] = zdata;
					if (audio_param == AP_TX_BUF_OFFS_LO) {
						uint8_t* addr = (uint8_t*)video_state->framebuffer +
								((audio_params[AP_TX_BUF_OFFS_HI]<<16)|audio_params[AP_TX_BUF_OFFS_LO]);
						if (((uint32_t)addr-(uint32_t)video_state->framebuffer)<0x100000*128) {
							audio_set_tx_buffer(addr);
							audio_request_init = 1;
						} else {
							printf("[audio] illegal tx address: 0x%p\n", addr);
						}
					} else if (audio_param == AP_RX_BUF_OFFS_LO) {
						uint8_t* addr = (uint8_t*)video_state->framebuffer +
								((audio_params[AP_RX_BUF_OFFS_HI]<<16)|audio_params[AP_RX_BUF_OFFS_LO]);
						if (((uint32_t)addr-(uint32_t)video_state->framebuffer)<0x100000*128) {
							audio_set_rx_buffer(addr);
							audio_request_init = 1;
						} else {
							printf("[audio] illegal tx address: 0x%p\n", addr);
						}
					} else if (audio_param == AP_DSP_UPLOAD) {
						uint8_t* program_ptr = (uint8_t*)video_state->framebuffer +
								((audio_params[AP_DSP_PROG_OFFS_HI]<<16)|audio_params[AP_DSP_PROG_OFFS_LO]);
						uint8_t* params_ptr = (uint8_t*)video_state->framebuffer +
								((audio_params[AP_DSP_PARAM_OFFS_HI]<<16)|audio_params[AP_DSP_PARAM_OFFS_LO]);

						if (zdata == 0) {
							printf("[audio] reprogramming from 0x%p and 0x%p\n", program_ptr, params_ptr);
							audio_program_adau(program_ptr, 5120);
							audio_program_adau_params(params_ptr, 4096);
						} else {
							printf("[audio] programming %ld params from 0x%p\n", zdata, params_ptr);
							audio_program_adau_params(params_ptr, zdata);
						}
					} else if (audio_param == AP_DSP_SET_LOWPASS) {
						// set lowpass filter params by cutoff freq (works only if default program is loaded!)
						audio_adau_set_lpf_params(zdata);
					} else if (audio_param == AP_DSP_SET_VOLUMES) {
						audio_adau_set_mixer_vol(zdata&0xff, (zdata>>8)&0xff);
					} else if (audio_param == AP_DSP_SET_PREFACTOR) {
						audio_adau_set_prefactor(zdata);
					} else if ((audio_param >= AP_DSP_SET_EQ_BAND1) && (audio_param <= AP_DSP_SET_EQ_BAND10)) {
						audio_adau_set_eq_gain(audio_param-AP_DSP_SET_EQ_BAND1, zdata);
					} else if (audio_param == AP_DSP_SET_STEREO_VOLUME) {
						audio_adau_set_vol_pan(zdata&0xff, (zdata>>8)&0xff);
					}
					break;
				case REG_ZZ_DECODER_PARAM:
					if (zdata<ZZ_NUM_DECODER_PARAMS) {
						decoder_param = zdata;
					} else {
						decoder_param = 0;
					}
					break;
				case REG_ZZ_DECODER_VAL:
					decoder_params[decoder_param] = zdata;
					break;
				case REG_ZZ_DECODER_FIFO:
					fifo_set_write_index(zdata);
					break;
				case REG_ZZ_DECODE:
					{
						// DECODER PARAMS:
						// 0: input buffer offset hi
						// 1: input buffer offset lo
						// 2: input buffer size hi
						// 3: input buffer size lo
						// 4: output buffer offset hi
						// 5: output buffer offset lo
						// 6: output buffer size hi
						// 7: output buffer size lo

						uint8_t* input_buffer = (uint8_t*)video_state->framebuffer
								+ ((decoder_params[0]<<16)|decoder_params[1]);
						size_t input_buffer_size = (decoder_params[2]<<16)|decoder_params[3];

						uint8_t* output_buffer = (uint8_t*)video_state->framebuffer
								+ ((decoder_params[4]<<16)|decoder_params[5]);
						size_t output_buffer_size = (decoder_params[6]<<16)|decoder_params[7];

						switch(zdata) {
							case DECODE_CLEAR:
								printf("[decode:clear]\n");
								fifo_clear();
							break;
							case DECODE_INIT:
								printf("[decode:mp3:%d] %p (%x) -> %p (%x)\n", (int)zdata, input_buffer, input_buffer_size,
										output_buffer, output_buffer_size);
								decode_mp3_init_fifo(input_buffer, input_buffer_size);
								decoder_bytes_decoded = -1;
							break;
							case DECODE_RUN: {
								int max_samples = output_buffer_size;
								int mp3_freq = mp3_get_hz();
								if (mp3_freq != 48000) {
									uint8_t* temp_buffer = output_buffer + AUDIO_TX_BUFFER_SIZE; // FIXME hack
									max_samples = mp3_get_hz()/50 * 2;
								
									decoder_bytes_decoded = decode_mp3_samples(temp_buffer, max_samples);
								
									// resample
									resample_s16((int16_t*)temp_buffer, (int16_t*)output_buffer,
											mp3_get_hz(), 48000, AUDIO_BYTES_PER_PERIOD / 4);
								
								} else {
									decoder_bytes_decoded = decode_mp3_samples(output_buffer, max_samples);
								}
							}
							break;
						}
						break;
					}
				}
			}

			// ack the write, set bit 31 in register 0
			mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG0, (1 << 31));
			need_req_ack = 1;
		} else if (readreq) {
			uint32_t zaddr = mntzorro_read(MNTZ_BASE_ADDR, MNTZORRO_REG0);

			//printf("READ: %08lx\n",zaddr);
			u32 z3 = (zstate_raw & (1 << 25));

			if (zaddr >= MNT_FB_BASE || zaddr >= MNT_REG_BASE + 0x2000) {
				u8* ptr = (u8*) FRAMEBUFFER_ADDRESS;

				if (zaddr >= MNT_FB_BASE) {
					// read from framebuffer / generic memory
					ptr = ptr + zaddr - MNT_FB_BASE;
				} else if (zaddr < MNT_REG_BASE + 0x6000) {
					// 0x0000-0x1fff: read from ethernet RX frame
					// used by Z2
					ptr = (u8*) (ethernet_current_receive_ptr() + zaddr - (MNT_REG_BASE + 0x2000));
				} else if (zaddr < MNT_REG_BASE + 0x8000) {
					// 0x6000-0x7fff: boot ROM
					// used by Z2
					//printf("READ ROM: %08lx\n",zaddr);
					ptr = (u8*) (BOOT_ROM_ADDRESS + zaddr - (MNT_REG_BASE + 0x6000));
				} else if (zaddr < MNT_REG_BASE + 0xa000) {
					// 0x8000-0x9fff: read from TX frame (unusual)
					// FIXME: remove
					ptr = (u8*) (TX_FRAME_ADDRESS + zaddr - (MNT_REG_BASE + 0x8000));
				} else if (zaddr < MNT_REG_BASE + 0x10000) {
					// 0xa000-0xafff: read from block device (usb storage)
					// used by Z2
					ptr = (u8*) (USB_BLOCK_STORAGE_ADDRESS + zaddr - (MNT_REG_BASE + 0xa000));
				}

				if (z3) {
					u32 b1 = ptr[0] << 24;
					u32 b2 = ptr[1] << 16;
					u32 b3 = ptr[2] << 8;
					u32 b4 = ptr[3];
					mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG1,
							b1 | b2 | b3 | b4);
				} else {
					if (zaddr >= MNT_REG_BASE + 0x6000 && zaddr < MNT_REG_BASE + 0x8000) {
						// autoboot rom
						u16 ubyte = ptr[0] << 8;
						u16 lbyte = ptr[1];
						//printf("READ ROM: [%04x]",ubyte|lbyte);
						mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG1, ubyte | lbyte);
					} else {
						u16 ubyte = ptr[0] << 8;
						u16 lbyte = ptr[1];
						mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG1, ubyte | lbyte);
					}
				}
			} else if (zaddr >= MNT_REG_BASE) {
				// read ARM "register"
				uint32_t data = 0;
				uint32_t zaddr32 = zaddr & 0xffffffc;

				//printf("REGR: %lx (%d)\n", zaddr, zaddr & 2);

				switch (zaddr32) {
					case REG_ZZ_VBLANK_STATUS:
						data = (zstate_raw & (1 << 21));
						break;
					case REG_ZZ_ARM_EV_SERIAL:
						data = arm_app_output_event();
						break;
					case REG_ZZ_ETH_MAC_HI: {
						uint8_t* mac = ethernet_get_mac_address_ptr();
						data = mac[0] << 24 | mac[1] << 16 | mac[2] << 8 | mac[3];
						break;
					}
					case REG_ZZ_ETH_MAC_LO: {
						uint8_t* mac = ethernet_get_mac_address_ptr();
						data = mac[4] << 24 | mac[5] << 16;
						break;
					}
					case REG_ZZ_ETH_TX:
						// FIXME this is probably wrong (doesn't need swapping?)
						data = (ethernet_send_result & 0xff) << 24
								| (ethernet_send_result & 0xff00) << 16;
						break;
					case REG_ZZ_FW_VERSION:
						data = (REVISION_MAJOR << 24 | REVISION_MINOR << 16);
						break;
					case REG_ZZ_USB_STATUS:
						data = usb_status << 16;
						break;
					case REG_ZZ_USB_CAPACITY: {
						if (usb_storage_available) {
							printf("[USB] query capacity: %lx\n",zz_usb_storage_capacity(0));
							data = zz_usb_storage_capacity(0);
						} else {
							printf("[USB] query capacity: no device.\n");
							data = 0;
						}
						break;
					}
					case REG_ZZ_TEMPERATURE: {
						// includes REG_ZZ_VOLTAGE_AUX in lower 16 bits
						data = (((uint32_t)(xadc_get_temperature()*10.0)) << 16) | ((uint32_t)(xadc_get_aux_voltage()*100.0));
						break;
					}
					case REG_ZZ_VOLTAGE_INT: {
						data = ((int16_t)(xadc_get_int_voltage()*100.0)) << 16;
						break;
					}
					case REG_ZZ_CONFIG: {
						data = (amiga_interrupt_get())<<16;
						break;
					}
					case REG_ZZ_AUDIO_SWAB: {
						// misc status bits
						//printf("read 0x70: %d\n", audio_buffer_collision);
						data = (audio_buffer_collision)<<16 | fifo_get_read_index();
						break;
					}
					case REG_ZZ_AUDIO_CONFIG: {
						// is ZZ9000AX present?
						data = (adau_enabled)<<16;
						break;
					}
					case REG_ZZ_DECODER_VAL: {
						// used to determine if MP3 decoding has finished
						data = decoder_bytes_decoded;
						break;
					}
					case REG_ZZ_UNUSED_REG8C: {
						// sleep test for reads
						data = zz_debug_test_counter;
						break;
					}
				}

				if (z3) {
					mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG1, data);
				} else {
					if (zaddr & 2) {
						// lower 16 bit
						mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG1, data);
					} else {
						// upper 16 bit
						mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG1, data >> 16);
					}
				}
			}

			// ack the read, set bit 30 in register 0
			mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG0, (1 << 30));
			need_req_ack = 2;
		} else {
			// there are no read/write requests, we can do other housekeeping
			idle_task_count++;

			if (idle_task_count > 30000000) {
				ethernet_task();
				idle_task_count=0;
			}

			if ((zstate & 0xff) == 0) {
				// RESET
				handle_amiga_reset();
			}

			if (audio_request_init) {
				audio_debug_timer(0);
				audio_init_i2s();
				audio_request_init = 0;
				audio_debug_timer(1);
			}
		}

		// TODO: potential hang, timeout?
		if (need_req_ack) {
			while (1) {
				// 1. fpga needs to respond to flag bit 31 or 30 going high (signals request fulfilled)
				// 2. it does that by clearing the request bit
				// 3. we read register 3 until request bit (31:write, 30:read) goes to 0 again
				//
				u32 zstate = mntzorro_read(MNTZ_BASE_ADDR, MNTZORRO_REG3);
				u32 writereq = (zstate & (1 << 31));
				u32 readreq = (zstate & (1 << 30));
				if (need_req_ack == 1 && !writereq) // no more write request?
					break;
				if (need_req_ack == 2 && !readreq) // no more read request?
					break;
				if ((zstate & 0xff) == 0)
					break; // reset
			}
			mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG0, 0);
			need_req_ack = 0;
		}

		// check for queued up ethernet frames
		int ethernet_backlog = ethernet_get_backlog();
		if (ethernet_backlog > 0 && eth_backlog_nag_counter > 5000) {
			amiga_interrupt_set(AMIGA_INTERRUPT_ETH);
			eth_backlog_nag_counter = 0;
		}

		if (interrupt_enabled_ethernet && ethernet_backlog > 0) {
			eth_backlog_nag_counter++;
		}
	}

	cleanup_platform();
	return 0;
}
