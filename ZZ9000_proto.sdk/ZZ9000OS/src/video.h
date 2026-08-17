#ifndef ZZ_VIDEO_H
#define ZZ_VIDEO_H

#include <stdint.h>

#include "zz_regs.h"
#include "zz_video_modes.h"
#include "video_scale.h"

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
/* OP_DIMENSIONS bit 15 marks a larger output canvas whose capture pitch is
 * published later by OP_VIEWPORT_SIZE_COMMIT. Width itself occupies [11:0]. */
#define MNTVF_DIMENSIONS_VIEWPORT_CONTAINER_FLAG (1U << 15)
#define MNTVF_OP_COLORMODE 1
#define MNTVF_OP_REPORT_LINE 17
#define MNTVF_OP_PALETTE_SEL 18
#define MNTVF_OP_PALETTE_HI 19
/* Videocap sampler control, snooped by mntzorro.v (the video formatter
 * declares op 16 as ignored). Live-capable bitstreams route this packed
 * command through the shared acknowledged, frame-boundary RTL engine;
 * older bitstreams ignore it and expose no live-control capability.
 *   [1:0]   sample mode: 0=average pair, 1=even sample, 2=odd sample
 *   [2]     full-width (28 MHz) capture
 *   [15:4]  horizontal crop origin, in 28 MHz samples
 *   [27:16] vertical crop origin, in captured lines
 *   [28]    horizontal crop is automatic
 *   [29]    vertical crop is automatic
 *   [31:30] reserved, zero
 */
#define MNTVF_OP_VIDEOCAP 16
#define VIDEOCAP_FULL_WIDTH_DEFAULT 1
#define VIDEOCAP_CROP_H_COMPAT 188U
#define VIDEOCAP_CROP_V_COMPAT 26U
#define VIDEOCAP_CROP_H_AUTO_FLAG (1U << 28)
#define VIDEOCAP_CROP_V_AUTO_FLAG (1U << 29)

static inline uint32_t videocap_control_pack(uint32_t sample,
		uint32_t full_width, uint32_t crop_h, uint32_t crop_v,
		uint32_t crop_h_present, uint32_t crop_v_present)
{
	uint32_t packed_crop_h = crop_h_present ?
		(crop_h & 0x0fffU) : VIDEOCAP_CROP_H_COMPAT;
	uint32_t packed_crop_v = crop_v_present ?
		(crop_v & 0x0fffU) : VIDEOCAP_CROP_V_COMPAT;
	uint32_t data = (packed_crop_v << 16) | (packed_crop_h << 4) |
		((full_width & 1U) << 2) | (sample & 3U);

	if (!crop_h_present)
		data |= VIDEOCAP_CROP_H_AUTO_FLAG;
	if (!crop_v_present)
		data |= VIDEOCAP_CROP_V_AUTO_FLAG;

	return data;
}
// decoded by mntzorro.v (snooped off the op stream, like OP_VIDEOCAP),
// not by the video formatter: data[1:0] = scanline mode, data[2] = parity
#define MNTVF_OP_SCANLINES 20
#define MNTVF_OP_DPMS 21
#define MNTVF_OP_OVERLAY_CTRL 22
#define MNTVF_OP_OVERLAY_POS 23
#define MNTVF_OP_OVERLAY_SIZE 24
#define MNTVF_OP_OVERLAY_KEY 25
#define MNTVF_OP_OVERLAY_SOURCE_SIZE 26
#define MNTVF_OP_OVERLAY_FRAME 27
#define MNTVF_OP_VIEWPORT_POS 28
#define MNTVF_OP_VIEWPORT_SIZE_COMMIT 29

enum zz_dpms_level {
	ZZ_DPMS_ON,
	ZZ_DPMS_STANDBY,
	ZZ_DPMS_SUSPEND,
	ZZ_DPMS_OFF,
};

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
	int videocap_video_mode_applied;
	int videocap_output_profile_requested;
	int videocap_output_profile_applied;

	int interlace_old;
	int videocap_ntsc_old;
	int videocap_shres_old;
	int videocap_enabled_old;
	struct video_videocap_detection_state videocap_detection;
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
void video_set_dpms(uint8_t level);
int video_set_videocap_video_mode(uint32_t mode);
int video_set_videocap_vsync(uint32_t setting);
uint32_t video_firmware_capabilities(void);
void hw_sprite_show(int show);
void update_hw_sprite(uint8_t *data, int double_sprite, int hires_sprite);
void update_hw_sprite_clut(uint8_t *data_, uint8_t *colors, uint16_t w, uint16_t h, uint8_t keycolor);
void update_hw_sprite_pos(void);
void _update_hw_sprite_pos(int16_t x, int16_t y);
void clip_hw_sprite(int16_t offset_x, int16_t offset_y);
void clear_hw_sprite();
struct zz_video_mode* get_custom_video_mode_ptr(int custom_video_mode);

struct ZZ_VIDEO_STATE* video_get_state();
void video_formatter_write(uint32_t data, uint16_t op);

#endif
