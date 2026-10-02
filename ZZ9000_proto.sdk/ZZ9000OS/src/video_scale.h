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
	/* Doubled-scan / short-line / tall-frame source (status [12:10]):
	 * a change of line class re-runs the mode selection like a change
	 * of standard does. */
	int source_class_candidate;
	uint8_t stable_count;
};

#define VIDEO_VIDEOCAP_SOURCE_SHORT   1U
#define VIDEO_VIDEOCAP_SOURCE_DOUBLED 2U
#define VIDEO_VIDEOCAP_SOURCE_TALL    4U
/* Row-count class in status [9:8], folded into the same integer above
 * bits 7..0: the woven frame shape bucket that picks the scanout
 * factor.  0 = 15 kHz/not short (old bitstreams read 0 - the x2
 * fallback); 1 = 256/200-visible shapes (DblPAL/DblNTSC no-lace);
 * 2 = 400..589 woven rows (Dbl laced, Euro72, Multiscan progressive);
 * 3 = 590+ (Super72/Multiscan laced: shown x1, clipped to 512 rows). */
#define VIDEO_VIDEOCAP_ROWS_CLASS_SHIFT 8
#define VIDEO_VIDEOCAP_ROWS_CLASS_MASK  3U
#define VIDEO_VIDEOCAP_ROWS_CLASS_1 \
	(1U << VIDEO_VIDEOCAP_ROWS_CLASS_SHIFT)
#define VIDEO_VIDEOCAP_ROWS_CLASS_2 \
	(2U << VIDEO_VIDEOCAP_ROWS_CLASS_SHIFT)
#define VIDEO_VIDEOCAP_ROWS_CLASS_3 \
	(3U << VIDEO_VIDEOCAP_ROWS_CLASS_SHIFT)

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

/* All centered variants share the same canvas/viewport layout and the same
 * base eligibility gate; the fixed pair differs only in output timing
 * (mode 5 vs mode 7), while MATCH additionally tracks the detected source
 * standard and needs the source-sync controller. */
static inline uint32_t video_videocap_output_profile_centered(
		uint32_t output_profile)
{
	return output_profile == ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60 ||
	       output_profile == ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_50 ||
	       output_profile == ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_MATCH;
}

/* Missing any hardware prerequisite uses the same full_60 fallback as
 * the driver profile table; never retain MATCH without source tracking. */
static inline uint32_t video_videocap_source_sync_eligible(
		uint32_t viewport_layout_capable, uint32_t fullrate_capable,
		uint32_t source_sync_capable)
{
	return video_videocap_centered_eligible(viewport_layout_capable,
	                                        fullrate_capable) &&
	       (source_sync_capable != 0U);
}

static inline uint32_t video_videocap_effective_output_profile(
		uint32_t requested, uint32_t viewport_layout_capable,
		uint32_t fullrate_capable, uint32_t source_sync_capable)
{
	if (requested == ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_MATCH) {
		if (video_videocap_source_sync_eligible(viewport_layout_capable,
		                                        fullrate_capable,
		                                        source_sync_capable))
			return requested;
		return ZZ_VIDEOCAP_OUTPUT_FULL_60;
	}

	return video_videocap_output_profile_centered(requested) &&
	       video_videocap_centered_eligible(viewport_layout_capable,
	                                       fullrate_capable) ?
		requested : ZZ_VIDEOCAP_OUTPUT_FULL_60;
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

	if (video_videocap_output_profile_centered(output_profile)) {
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
	state->source_class_candidate = -1;
	state->stable_count = 0;
}

static inline int video_videocap_detection_stable(
		struct video_videocap_detection_state *state, int ntsc,
		int interlace, int shres, int base_mode, int output_profile,
		int source_class)
{
	if (ntsc != state->ntsc_candidate ||
	    interlace != state->interlace_candidate ||
	    shres != state->shres_candidate ||
	    base_mode != state->base_mode_candidate ||
	    output_profile != state->output_profile_candidate ||
	    source_class != state->source_class_candidate) {
		state->ntsc_candidate = ntsc;
		state->interlace_candidate = interlace;
		state->shres_candidate = shres;
		state->base_mode_candidate = base_mode;
		state->output_profile_candidate = output_profile;
		state->source_class_candidate = source_class;
		state->stable_count = 1;
		return 0;
	}

	if (state->stable_count < VIDEO_VIDEOCAP_MODE_STABLE_VBLANKS)
		state->stable_count++;

	return state->stable_count >= VIDEO_VIDEOCAP_MODE_STABLE_VBLANKS;
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

static inline struct video_videocap_runtime_request
video_videocap_sanitize_runtime_mode(uint32_t mode,
		uint32_t viewport_layout_capable, uint32_t fullrate_capable,
		uint32_t source_sync_capable)
{
	struct video_videocap_runtime_request request = {
		0U, ZZVMODE_800x600, ZZ_VIDEOCAP_OUTPUT_FULL_60
	};

	if (mode == ZZVMODE_CENTERED_1080P_MATCH) {
		/* Virtual id: always resolve to a safe physical base mode. */
		request.valid = 1U;
		request.output_profile = video_videocap_effective_output_profile(
			ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_MATCH,
			viewport_layout_capable, fullrate_capable,
			source_sync_capable);
	} else if (mode == ZZVMODE_1920x1080_60 || mode == ZZVMODE_1920x1080_50) {
		request.valid = 1U;
		if (video_videocap_centered_eligible(viewport_layout_capable,
		                                        fullrate_capable)) {
			request.output_profile = mode == ZZVMODE_1920x1080_50 ?
				ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_50 :
				ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60;
		}
	} else if (mode == ZZVMODE_800x600 || mode == ZZVMODE_720x576) {
		request.valid = 1U;
		request.base_mode = (uint8_t)mode;
	}

	return request;
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
		uint32_t interlace, uint32_t source_class)
{
	/* Short-line sources. Filtered capture is unchanged. For full-width
	 * measured doubled-scan input, the formatter repeats each captured
	 * 28 MHz pixel horizontally once (SCALEX=2x); the VDMA remains at its
	 * 32-bit content pitch. 24 kHz Super72 is short but not doubled and
	 * keeps its existing horizontal sampling. Vertical scaling remains
	 * selected by the woven row-count class. */
	if ((source_class & VIDEO_VIDEOCAP_SOURCE_SHORT) != 0U) {
		if (full_width == 0U)
			return 0U;
		uint32_t scale_x =
			(source_class & VIDEO_VIDEOCAP_SOURCE_DOUBLED) != 0U ? 1U : 0U;
		switch ((source_class >> VIDEO_VIDEOCAP_ROWS_CLASS_SHIFT) &
			VIDEO_VIDEOCAP_ROWS_CLASS_MASK) {
		case 1U: return 4U | scale_x;
		case 2U: return 2U | scale_x;
		case 3U: return scale_x;
		default: return 2U | scale_x;
		}
	}
	/* Full-width capture scales by an integer factor on the legacy
	 * power-of-two paths: PAL fills 1024 lines (x4/x2), 15 kHz NTSC
	 * letterboxes 800 lines with the SAME factors so progressive and
	 * laced pictures keep the identical physical height they share on
	 * a real monitor. Short-line NTSC does not take that letterbox.
	 * Filtered capture retains the legacy x2 path. Interlaced input
	 * already supplies twice as many source lines. */
	return full_width ? (interlace ? 2U : 4U)
	                  : (interlace ? 0U : 2U);
}

#define VIDEO_VIDEOCAP_NTSC_PROGRESSIVE_ROWS 200U
/* Fullscan NTSC letterbox: 200 progressive rows at x4 and 400 woven rows
 * at x2 both render 800 lines, centered with 112-line black bars. */
#define VIDEO_VIDEOCAP_NTSC_LETTERBOX_HEIGHT 800U

static inline uint32_t video_videocap_source_rows(uint32_t content_height,
		uint32_t full_width, uint32_t ntsc, uint32_t interlace,
		uint32_t source_class, uint32_t height_override)
{
	uint32_t base;

	/* The NTSC fractional-resampling row count is a 15 kHz property (200
	 * or 400 rows of a 480-row picture); a short-line source is shown
	 * whole, at an integer factor. */
	if (full_width && ntsc &&
			(source_class & VIDEO_VIDEOCAP_SOURCE_SHORT) == 0U)
		base = VIDEO_VIDEOCAP_NTSC_PROGRESSIVE_ROWS << (interlace != 0U);
	else
		base = content_height /
			video_vertical_scale_factor(
				video_videocap_scalemode(full_width, interlace,
					source_class));
	/* Class-3 exotics (590+ rows, shown x1) clip the viewport to the
	 * 512 rows that are always fresh; the VDMA must match or the next
	 * frame starts from the second half of the DMA buffer. */
	if (((source_class >> VIDEO_VIDEOCAP_ROWS_CLASS_SHIFT) &
			VIDEO_VIDEOCAP_ROWS_CLASS_MASK) == 3U &&
			height_override == 0U && base > 512U)
		base = 512U;

	/* A manual capture-height bound can only shrink the window: the
	 * source itself caps the automatic size. */
	if (height_override != 0U && height_override < base)
		return height_override;
	return base;
}

static inline uint32_t video_videocap_scale_control(uint32_t full_width,
		uint32_t ntsc, uint32_t interlace, uint32_t source_class)
{
	/* Fullscan stays on the legacy duplication paths for every standard
	 * and mode: the x2/x4 fetch budgets are identical to PAL's, which the
	 * missing-lines report proved necessary under full-width writeback
	 * contention. The fractional source-row engine remains available in
	 * the formatter but no fullscan mode selects it. */
	(void)ntsc;
	return video_formatter_scale_control(
		video_videocap_scalemode(full_width, interlace, source_class));
}

struct video_videocap_scanout_rect {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
};

/* Fullscan content rectangle inside the active output canvas: PAL fills
 * the 1024-line raster; 15 kHz NTSC letterboxes the same 800 lines for
 * progressive and interlaced alike, centered, so both modes render the
 * picture at one physical size with its aspect ratio intact. Short-line
 * NTSC (Euro72, DblNTSC) is not that window: its row-class factor already
 * shows the source whole, and the 800-line rule would clip it.
 *
 * Manual capture-window overrides (ZZTop calibration, videocap_width /
 * videocap_height) shrink the displayed content the same way: width is
 * in captured words (doubled horizontally by scalemode bit 0, exactly
 * as a doubled-scan source is), height in source rows multiplied by the
 * vertical factor.  The shrunken rectangle is centered inside the
 * content box the profile would otherwise fill; borders render black.
 * Filtered profiles pass that box as canvas_width/canvas_height (the
 * active mode, 800x600 or 720x480/576); fullscan ignores those dimensions. */
static inline struct video_videocap_scanout_rect
video_videocap_fullscan_rect(uint32_t output_profile, uint32_t full_width,
		uint32_t ntsc, uint32_t interlace, uint32_t source_class,
		uint32_t width_override, uint32_t height_override,
		uint32_t source_rows, uint32_t canvas_width,
		uint32_t canvas_height)
{
	struct video_videocap_scanout_rect rect = {
		0U, 0U, VIDEO_VIDEOCAP_CONTENT_WIDTH,
		VIDEO_VIDEOCAP_CONTENT_HEIGHT
	};
	uint32_t scalemode = video_videocap_scalemode(full_width, interlace,
		source_class);
	uint32_t hdiv = (scalemode & 1U) != 0U ? 2U : 1U;

	if (full_width == 0U) {
		rect.width = canvas_width;
		rect.height = canvas_height;
	} else {
		if (video_videocap_output_profile_centered(output_profile)) {
			rect.x = VIDEO_VIDEOCAP_CENTERED_VIEWPORT_X;
			rect.y = VIDEO_VIDEOCAP_CENTERED_VIEWPORT_Y;
		}
		/* 15 kHz fullscan NTSC only. Class 2 short-line NTSC is
		 * vertical x2, so an 800-line viewport would consume 400
		 * rows and clip Euro72 (~427) and DblNTSC (~478). */
		if (full_width != 0U && ntsc != 0U &&
				(source_class & VIDEO_VIDEOCAP_SOURCE_SHORT) == 0U) {
			rect.y += (VIDEO_VIDEOCAP_CONTENT_HEIGHT -
				VIDEO_VIDEOCAP_NTSC_LETTERBOX_HEIGHT) / 2U;
			rect.height = VIDEO_VIDEOCAP_NTSC_LETTERBOX_HEIGHT;
		}
		/* Short-line classes 1/2 fill the canvas at their class
		 * factor. Only the 590+-row exotics (shown x1) clip to the
		 * 512 rows that are always fresh: the ARM cannot read the
		 * exact captured line count. Manual height overrides
		 * replace this. */
		if (((source_class >> VIDEO_VIDEOCAP_ROWS_CLASS_SHIFT) &
				VIDEO_VIDEOCAP_ROWS_CLASS_MASK) == 3U &&
				height_override == 0U) {
			if (rect.height > 512U)
				rect.height = 512U;
		}
	}
	/* The doubled-source scale bit repeats each stored pixel to fill the
	 * content viewport; only an explicit width override shrinks it. */
	if (width_override != 0U && width_override * hdiv < rect.width) {
		uint32_t shown = width_override * hdiv;

		rect.x += (rect.width - shown) / 2U;
		rect.width = shown;
	}
	if (height_override != 0U) {
		uint32_t shown = source_rows *
			video_vertical_scale_factor(scalemode);

		if (shown < rect.height) {
			rect.y += (rect.height - shown) / 2U;
			rect.height = shown;
		}
	}

	return rect;
}

/* Manual capture-window bounds shared by the CFG boot path and the
 * ZZTop live card-feature path.  Width is in captured words and must be
 * 16-aligned (the writeback burst), height in source lines; both count
 * from the crop origin.  Zero means automatic. */
#define VIDEO_VIDEOCAP_WIDTH_MIN  256U
#define VIDEO_VIDEOCAP_WIDTH_MAX  1280U
#define VIDEO_VIDEOCAP_HEIGHT_MIN 100U
#define VIDEO_VIDEOCAP_HEIGHT_MAX 1024U

static inline uint32_t video_videocap_width_valid(uint32_t width)
{
	return width >= VIDEO_VIDEOCAP_WIDTH_MIN &&
	       width <= VIDEO_VIDEOCAP_WIDTH_MAX &&
	       (width & 15U) == 0U;
}

static inline uint32_t video_videocap_height_valid(uint32_t height)
{
	return height >= VIDEO_VIDEOCAP_HEIGHT_MIN &&
	       height <= VIDEO_VIDEOCAP_HEIGHT_MAX;
}

#endif
