#ifndef VIDEO_SCALE_H
#define VIDEO_SCALE_H

#include <stdint.h>

#include "zz_config.h"
#include "zz_video_modes.h"

#define VIDEO_VIDEOCAP_CONTENT_WIDTH 1280U
#define VIDEO_VIDEOCAP_CONTENT_HEIGHT 1024U
#define VIDEO_VIDEOCAP_CENTERED_CANVAS_WIDTH 1920U
#define VIDEO_VIDEOCAP_CENTERED_CANVAS_HEIGHT 1080U
#define VIDEO_VIDEOCAP_CENTERED_VIEWPORT_X 320U
#define VIDEO_VIDEOCAP_CENTERED_VIEWPORT_Y 28U
#define VIDEO_VIDEOCAP_MODE_STABLE_VBLANKS 2U

struct video_videocap_geometry {
	uint32_t canvas_width;
	uint32_t canvas_height;
	uint32_t content_width;
	uint32_t content_height;
	uint32_t viewport_x;
	uint32_t viewport_y;
};

struct video_videocap_detection_state {
	int ntsc_candidate;
	int interlace_candidate;
	int shres_candidate;
	int base_mode_candidate;
	int output_profile_candidate;
	uint8_t stable_count;
};

struct video_videocap_runtime_request {
	uint8_t valid;
	uint8_t base_mode;
	uint8_t output_profile;
};

static inline uint32_t video_videocap_centered_eligible(
		uint32_t viewport_layout_capable, uint32_t fullrate_capable)
{
	return (viewport_layout_capable != 0U) && (fullrate_capable != 0U);
}

static inline uint32_t video_videocap_effective_output_profile(
		uint32_t requested, uint32_t viewport_layout_capable,
		uint32_t fullrate_capable)
{
	return requested == ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60 &&
	       video_videocap_centered_eligible(viewport_layout_capable,
	                                           fullrate_capable) ?
		ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60 :
		ZZ_VIDEOCAP_OUTPUT_FULL_60;
}

static inline struct video_videocap_geometry
video_videocap_output_geometry(uint32_t output_profile)
{
	struct video_videocap_geometry geometry = {
		VIDEO_VIDEOCAP_CONTENT_WIDTH,
		VIDEO_VIDEOCAP_CONTENT_HEIGHT,
		VIDEO_VIDEOCAP_CONTENT_WIDTH,
		VIDEO_VIDEOCAP_CONTENT_HEIGHT,
		0U,
		0U,
	};

	if (output_profile == ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60) {
		geometry.canvas_width = VIDEO_VIDEOCAP_CENTERED_CANVAS_WIDTH;
		geometry.canvas_height = VIDEO_VIDEOCAP_CENTERED_CANVAS_HEIGHT;
		geometry.viewport_x = VIDEO_VIDEOCAP_CENTERED_VIEWPORT_X;
		geometry.viewport_y = VIDEO_VIDEOCAP_CENTERED_VIEWPORT_Y;
	}

	return geometry;
}

static inline void video_videocap_detection_reset(
		struct video_videocap_detection_state *state)
{
	state->ntsc_candidate = -1;
	state->interlace_candidate = -1;
	state->shres_candidate = -1;
	state->base_mode_candidate = -1;
	state->output_profile_candidate = -1;
	state->stable_count = 0;
}

static inline int video_videocap_detection_stable(
		struct video_videocap_detection_state *state, int ntsc,
		int interlace, int shres, int base_mode, int output_profile)
{
	if (ntsc != state->ntsc_candidate ||
	    interlace != state->interlace_candidate ||
	    shres != state->shres_candidate ||
	    base_mode != state->base_mode_candidate ||
	    output_profile != state->output_profile_candidate) {
		state->ntsc_candidate = ntsc;
		state->interlace_candidate = interlace;
		state->shres_candidate = shres;
		state->base_mode_candidate = base_mode;
		state->output_profile_candidate = output_profile;
		state->stable_count = 1;
		return 0;
	}

	if (state->stable_count < VIDEO_VIDEOCAP_MODE_STABLE_VBLANKS)
		state->stable_count++;

	return state->stable_count >= VIDEO_VIDEOCAP_MODE_STABLE_VBLANKS;
}

static inline struct video_videocap_runtime_request
video_videocap_sanitize_runtime_mode(uint32_t mode,
		uint32_t viewport_layout_capable, uint32_t fullrate_capable)
{
	struct video_videocap_runtime_request request = {
		0U, ZZVMODE_800x600, ZZ_VIDEOCAP_OUTPUT_FULL_60
	};

	if (mode == ZZVMODE_1920x1080_60) {
		request.valid = 1U;
		if (video_videocap_centered_eligible(viewport_layout_capable,
		                                        fullrate_capable)) {
			request.output_profile =
				ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60;
		}
	} else if (mode == ZZVMODE_800x600 || mode == ZZVMODE_720x576) {
		request.valid = 1U;
		request.base_mode = (uint8_t)mode;
	}

	return request;
}

static inline uint32_t video_vertical_scale_factor(uint32_t scalemode)
{
	return 1U << ((scalemode >> 1) & 3U);
}

static inline uint32_t video_formatter_scale_control(uint32_t scalemode)
{
	/* OP_SCALE uses [2:1] for the vertical shift and [3] for sprite
	 * doubling. Preserve the historical behavior where an x2 vertical
	 * mode also doubled the RTG hardware sprite; x4 videocap does not. */
	return (scalemode & 7U) | ((scalemode & 2U) << 2);
}

static inline uint32_t video_videocap_full_width(uint32_t requested,
		uint32_t fullrate_capable)
{
	/* A full-width request is only safe when the loaded bitstream has the
	 * full-rate sampler/writeback path. Filtered-only variants otherwise
	 * leave the unused tail of each 1280-pixel row stale. */
	return (requested != 0U) && (fullrate_capable != 0U);
}

static inline uint32_t video_videocap_scalemode(uint32_t full_width,
		uint32_t interlace)
{
	/* Full-width progressive capture needs x4 to fill 1024 lines;
	 * filtered capture retains the legacy x2 path. Interlaced input
	 * already supplies twice as many source lines. */
	return full_width ? (interlace ? 2U : 4U)
	                  : (interlace ? 0U : 2U);
}

#endif
