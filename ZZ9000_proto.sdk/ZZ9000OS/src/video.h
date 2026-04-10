#ifndef ZZ_VIDEO_H
#define ZZ_VIDEO_H

#include "zz_regs.h"
#include "zz_video_modes.h"

#define MNTVF_OP_UNUSED 12
#define MNTVF_OP_SPRITE_XY 13
#define MNTVF_OP_SPRITE_ADDR 14
#define MNTVF_OP_SPRITE_DATA 15
#define MNTVF_OP_MAX 6
#define MNTVF_OP_HS 7
#define MNTVF_OP_VS 8
#define MNTVF_OP_POLARITY 10
#define MNTVF_OP_SCALE 4
#define MNTVF_OP_DIMENSIONS 2
#define MNTVF_OP_COLORMODE 1
#define MNTVF_OP_REPORT_LINE 17
#define MNTVF_OP_PALETTE_SEL 18
#define MNTVF_OP_PALETTE_HI 19

struct ZZ_VIDEO_STATE {
	uint32_t* framebuffer;

	int video_mode;
	int colormode;
	int scalemode;

	uint32_t vmode_hsize;
	uint32_t vmode_vsize;
	uint32_t vmode_hdiv;
	uint32_t vmode_vdiv;

	int videocap_video_mode;

	int interlace_old;
	int videocap_ntsc_old;
	int videocap_enabled_old;
	uint16_t split_request_pos;
	uint16_t split_pos;
	uint32_t bgbuf_offset;

	uint32_t framebuffer_pan_offset;
	uint32_t framebuffer_pan_width;
	uint8_t scandoubler_mode_adjust;

	int sprite_showing;
	int16_t sprite_x;
	int16_t sprite_x_adj;
	int16_t sprite_x_base;
	int16_t sprite_y;
	int16_t sprite_y_adj;
	int16_t sprite_y_base;
	int16_t sprite_x_offset;
	int16_t sprite_y_offset;
	uint8_t sprite_width;
	uint8_t sprite_height;
	uint32_t sprite_colors[4];

	uint8_t card_feature_enabled[CARD_FEATURE_NUM];
};

struct ZZ_VIDEO_STATE* video_init();
void video_reset();
void isr_video(void *dummy);
void video_mode_init(int mode, int scalemode, int colormode);
void hw_sprite_show(int show);
void update_hw_sprite(uint8_t *data, int double_sprite, int hires_sprite);
void update_hw_sprite_clut(uint8_t *data_, uint8_t *colors, uint16_t w, uint16_t h, uint8_t keycolor);
void update_hw_sprite_pos(void);
void _update_hw_sprite_pos(int16_t x, int16_t y);
void clip_hw_sprite(int16_t offset_x, int16_t offset_y);
void clear_hw_sprite();
struct zz_video_mode* get_custom_video_mode_ptr(int custom_video_mode);

struct ZZ_VIDEO_STATE* video_get_state();

#endif
