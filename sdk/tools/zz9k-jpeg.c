#include "zz9k-fb-common.h"
#include "zz9k-image-window.h"
#include "zz9k-jpeg-view.h"
#include "zz9k/caps.h"
#include "zz9k/host.h"
#include "zz9k/image.h"
#include "zz9k/shared.h"
#include "zz9k/surface.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZZ9K_JPEG_STAGING_BYTES (32UL * 1024UL)
#define ZZ9K_JPEG_TILE_TARGET_BYTES (256UL * 1024UL)
#define ZZ9K_JPEG_DEFAULT_TILE_ROWS 32U
#define ZZ9K_JPEG_DEFAULT_FRAMEBUFFER_HOLD_TICKS 100U
#define ZZ9K_JPEG_MAX_FRAMEBUFFER_HOLD_TICKS 1000U
#define ZZ9K_JPEG_BACKUP_NONE 0U
#define ZZ9K_JPEG_BACKUP_CPU 1U
#define ZZ9K_JPEG_BACKUP_SURFACE 2U
#define ZZ9K_JPEG_WINDOW_MIN_WIDTH 180U
#define ZZ9K_JPEG_WINDOW_MIN_HEIGHT 120U
#define ZZ9K_JPEG_WINDOW_MARGIN_X 32U
#define ZZ9K_JPEG_WINDOW_MARGIN_Y 48U
#define ZZ9K_JPEG_SCALE_FILTER_AUTO 0xffffffffUL

#if defined(__amigaos__) || defined(__amiga__) || defined(__AMIGA__) || \
    defined(__VBCC__)
#define ZZ9K_JPEG_AMIGA 1
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <exec/types.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <utility/tagitem.h>
#endif

typedef struct ZZ9KJpegInput {
	const uint8_t *bytes;
	uint32_t length;
	const char *path;
	uint32_t width;
	uint32_t height;
	uint32_t tile_rows;
	uint32_t framebuffer_hold_ticks;
	int use_framebuffer;
	int fit_framebuffer;
	int restore_framebuffer;
	int prefer_surface_backup;
	int resize_framebuffer;
	int view_framebuffer;
	uint32_t scale_filter_override;
	int built_in;
} ZZ9KJpegInput;

typedef struct ZZ9KJpegFramebufferBackup {
	ZZ9KFbRect rect;
	ZZ9KSurface surface;
	ZZ9KSurfaceCopyDesc save_copy;
	ZZ9KSurfaceCopyDesc restore_copy;
	uint8_t *bytes;
	uint32_t length;
	uint32_t bpp;
	uint32_t mode;
	int active;
	int surface_allocated;
} ZZ9KJpegFramebufferBackup;

static const uint8_t jpeg_2x2[] = {
	0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01,
	0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43,
	0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0xff, 0xdb, 0x00, 0x43, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0xff, 0xc0, 0x00, 0x11, 0x08, 0x00, 0x02, 0x00, 0x02, 0x03,
	0x01, 0x11, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xff, 0xc4, 0x00,
	0x1f, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
	0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0xff, 0xc4, 0x00, 0xb5, 0x10, 0x00,
	0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00,
	0x00, 0x01, 0x7d, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21,
	0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81,
	0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24,
	0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25,
	0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
	0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56,
	0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a,
	0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86,
	0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
	0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3,
	0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6,
	0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9,
	0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1,
	0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xff, 0xc4, 0x00,
	0x1f, 0x01, 0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
	0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0xff, 0xc4, 0x00, 0xb5, 0x11, 0x00,
	0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00,
	0x01, 0x02, 0x77, 0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31,
	0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08,
	0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15,
	0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18,
	0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39,
	0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55,
	0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
	0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84,
	0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
	0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa,
	0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4,
	0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
	0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
	0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xff, 0xda, 0x00,
	0x0c, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3f, 0x00, 0xfe,
	0xb0, 0x3f, 0xe0, 0x9c, 0x1f, 0x02, 0x3e, 0x07, 0xf8, 0x97, 0xfe, 0x09,
	0xe3, 0xfb, 0x06, 0x78, 0x8b, 0xc4, 0x5f, 0x06, 0xbe, 0x14, 0xeb, 0xfe,
	0x20, 0xd7, 0xff, 0x00, 0x63, 0x0f, 0xd9, 0x73, 0x5a, 0xd7, 0x75, 0xdd,
	0x6b, 0xe1, 0xdf, 0x84, 0x35, 0x5d, 0x67, 0x5a, 0xd6, 0x75, 0x5f, 0x81,
	0xfe, 0x06, 0xbe, 0xd5, 0x35, 0x6d, 0x5b, 0x54, 0xbe, 0xd1, 0xe7, 0xbd,
	0xd4, 0xb5, 0x3d, 0x4a, 0xf6, 0x79, 0xef, 0x2f, 0xef, 0xef, 0x27, 0x9a,
	0xea, 0xf2, 0xea, 0x69, 0x6e, 0x2e, 0x25, 0x92, 0x69, 0x1d, 0xdb, 0xe8,
	0x3e, 0x94, 0x3f, 0x47, 0xcf, 0x00, 0xf2, 0xaf, 0xa4, 0xc7, 0xd2, 0x27,
	0x2b, 0xca, 0xfc, 0x10, 0xf0, 0x83, 0x2d, 0xcb, 0x72, 0xdf, 0x1d, 0x3c,
	0x5b, 0xc0, 0x65, 0xd9, 0x76, 0x03, 0xc3, 0x4e, 0x0c, 0xc1, 0xe0, 0x70,
	0x18, 0x1c, 0x1f, 0x1f, 0xf1, 0x06, 0x1f, 0x09, 0x82, 0xc1, 0x61, 0x30,
	0xf9, 0x2d, 0x3c, 0x3e, 0x17, 0x09, 0x85, 0xc3, 0xd3, 0xa7, 0x43, 0x0f,
	0x86, 0xa1, 0x4e, 0x9d, 0x1a, 0x14, 0x69, 0xc2, 0x95, 0x28, 0x46, 0x11,
	0x8c, 0x57, 0xe3, 0xf8, 0xff, 0x00, 0x05, 0xfc, 0x1d, 0xe3, 0xdc, 0x76,
	0x37, 0x8e, 0xb8, 0xeb, 0xc2, 0x7f, 0x0d, 0x38, 0xd3, 0x8d, 0xb8, 0xd3,
	0x17, 0x89, 0xe2, 0xce, 0x31, 0xe3, 0x1e, 0x2c, 0xe0, 0x4e, 0x16, 0xe2,
	0x3e, 0x2a, 0xe2, 0xce, 0x2a, 0xe2, 0x3a, 0xd3, 0xce, 0x38, 0x87, 0x89,
	0xb8, 0x9b, 0x88, 0x73, 0x8c, 0xab, 0x19, 0x9b, 0xe7, 0xdc, 0x41, 0x9e,
	0xe6, 0xf8, 0xcc, 0x66, 0x69, 0x9c, 0xe7, 0x39, 0xa6, 0x33, 0x15, 0x98,
	0xe6, 0x99, 0x8e, 0x2b, 0x13, 0x8e, 0xc7, 0x62, 0x6b, 0xe2, 0x6b, 0xd5,
	0xab, 0x2f, 0xff, 0xd9,
};

static int dominant_red(const uint8_t *pixel)
{
	return pixel[0] < 64U && pixel[1] < 64U &&
	       pixel[2] > 200U && pixel[3] == 0xff;
}

static int dominant_green(const uint8_t *pixel)
{
	return pixel[0] < 64U && pixel[1] > 200U &&
	       pixel[2] < 64U && pixel[3] == 0xff;
}

static int dominant_blue(const uint8_t *pixel)
{
	return pixel[0] > 200U && pixel[1] < 64U &&
	       pixel[2] < 64U && pixel[3] == 0xff;
}

static int near_white(const uint8_t *pixel)
{
	return pixel[0] > 200U && pixel[1] > 200U &&
	       pixel[2] > 200U && pixel[3] == 0xff;
}

static void volatile_copy_from(uint8_t *dst, volatile const uint8_t *src,
                               uint32_t length)
{
	uint32_t i;

	for (i = 0; i < length; i++) {
		dst[i] = src[i];
	}
}

static int require_cap(uint32_t caps, uint32_t bit)
{
	const char *name;

	if ((caps & bit) != 0U) {
		return 1;
	}

	name = zz9k_capability_name(bit);
	printf("zz9k-jpeg: missing required capability: %s\n",
	       name ? name : "unknown");
	return 0;
}

static uint16_t zz9k_jpeg_read_be16(const uint8_t *bytes)
{
	return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static int zz9k_jpeg_sof_marker(uint8_t marker)
{
	switch (marker) {
	case 0xc0:
	case 0xc1:
	case 0xc2:
	case 0xc3:
	case 0xc5:
	case 0xc6:
	case 0xc7:
	case 0xc9:
	case 0xca:
	case 0xcb:
	case 0xcd:
	case 0xce:
	case 0xcf:
		return 1;
	default:
		return 0;
	}
}

static int zz9k_jpeg_standalone_marker(uint8_t marker)
{
	return marker == 0x01U || marker == 0xd8U || marker == 0xd9U ||
	       (marker >= 0xd0U && marker <= 0xd7U);
}

static int zz9k_jpeg_read_dimensions(const uint8_t *bytes, uint32_t length,
                                      uint32_t *out_width,
                                      uint32_t *out_height)
{
	uint32_t pos;

	if (!bytes || length < 4U || bytes[0] != 0xffU || bytes[1] != 0xd8U)
		return 0;

	pos = 2U;
	while (pos + 1U < length) {
		uint8_t marker;

		if (bytes[pos] != 0xffU)
			return 0;
		while (pos < length && bytes[pos] == 0xffU)
			pos++;
		if (pos >= length)
			return 0;

		marker = bytes[pos++];
		if (zz9k_jpeg_standalone_marker(marker))
			continue;
		if (marker == 0xdaU || marker == 0xd9U)
			return 0;
		if (pos + 2U > length)
			return 0;

		{
			uint16_t segment_length = zz9k_jpeg_read_be16(&bytes[pos]);
			uint32_t segment_start = pos + 2U;
			uint32_t segment_payload;

			if (segment_length < 2U)
				return 0;
			segment_payload = (uint32_t)segment_length - 2U;
			if (segment_start > length ||
			    segment_payload > (length - segment_start)) {
				return 0;
			}

			if (zz9k_jpeg_sof_marker(marker)) {
				uint32_t height;
				uint32_t width;

				if (segment_payload < 6U)
					return 0;
				height = zz9k_jpeg_read_be16(&bytes[segment_start + 1U]);
				width = zz9k_jpeg_read_be16(&bytes[segment_start + 3U]);
				if (width == 0U || height == 0U)
					return 0;
				if (out_width)
					*out_width = width;
				if (out_height)
					*out_height = height;
				return 1;
			}

			pos = segment_start + segment_payload;
		}
	}

	return 0;
}

static int zz9k_jpeg_read_file_exact(FILE *file, uint8_t *dst, uint32_t length)
{
	return fread(dst, 1U, length, file) == length;
}

static int zz9k_jpeg_skip_file_bytes(FILE *file, uint32_t length)
{
	return fseek(file, (long)length, SEEK_CUR) == 0;
}

static int zz9k_jpeg_read_file_dimensions(FILE *file,
                                           uint32_t *out_width,
                                           uint32_t *out_height)
{
	uint8_t bytes[6];

	if (!file || !zz9k_jpeg_read_file_exact(file, bytes, 2U) ||
	    bytes[0] != 0xffU || bytes[1] != 0xd8U) {
		return 0;
	}

	for (;;) {
		uint8_t marker;
		uint16_t segment_length;
		uint32_t payload_length;

		do {
			if (!zz9k_jpeg_read_file_exact(file, bytes, 1U))
				return 0;
		} while (bytes[0] == 0xffU);

		marker = bytes[0];
		if (zz9k_jpeg_standalone_marker(marker))
			continue;
		if (marker == 0xdaU || marker == 0xd9U)
			return 0;
		if (!zz9k_jpeg_read_file_exact(file, bytes, 2U))
			return 0;

		segment_length = zz9k_jpeg_read_be16(bytes);
		if (segment_length < 2U)
			return 0;
		payload_length = (uint32_t)segment_length - 2U;

		if (zz9k_jpeg_sof_marker(marker)) {
			uint32_t width;
			uint32_t height;

			if (payload_length < 6U ||
			    !zz9k_jpeg_read_file_exact(file, bytes, 6U)) {
				return 0;
			}
			height = zz9k_jpeg_read_be16(&bytes[1]);
			width = zz9k_jpeg_read_be16(&bytes[3]);
			if (width == 0U || height == 0U)
				return 0;
			if (out_width)
				*out_width = width;
			if (out_height)
				*out_height = height;
			return 1;
		}

		if (!zz9k_jpeg_skip_file_bytes(file, payload_length))
			return 0;
	}
}

static uint32_t zz9k_jpeg_choose_tile_rows(uint32_t width,
                                           uint32_t output_format,
                                           uint32_t requested_rows)
{
	uint32_t row_bytes;
	uint32_t rows_by_cap;

	if (requested_rows == 0U)
		requested_rows = ZZ9K_JPEG_DEFAULT_TILE_ROWS;
	if (!zz9k_surface_min_pitch(width, output_format, &row_bytes))
		return 0U;

	rows_by_cap = ZZ9K_JPEG_TILE_TARGET_BYTES / row_bytes;
	if (rows_by_cap == 0U)
		rows_by_cap = 1U;
	if (requested_rows > rows_by_cap)
		return rows_by_cap;
	return requested_rows;
}

static int zz9k_jpeg_parse_u32(const char *text, uint32_t *value)
{
	uint32_t result = 0U;

	if (!text || !*text || !value)
		return 0;
	while (*text) {
		if (*text < '0' || *text > '9')
			return 0;
		if (result > ((0xffffffffU - (uint32_t)(*text - '0')) / 10U))
			return 0;
		result = (result * 10U) + (uint32_t)(*text - '0');
		text++;
	}
	if (result == 0U)
		return 0;

	*value = result;
	return 1;
}

static int zz9k_jpeg_parse_hold_ticks(const char *text, uint32_t *value)
{
	uint32_t parsed;

	if (!value)
		return 0;
	if (text && strcmp(text, "0") == 0) {
		*value = 0U;
		return 1;
	}
	if (!zz9k_jpeg_parse_u32(text, &parsed))
		return 0;
	if (parsed > ZZ9K_JPEG_MAX_FRAMEBUFFER_HOLD_TICKS)
		parsed = ZZ9K_JPEG_MAX_FRAMEBUFFER_HOLD_TICKS;
	*value = parsed;
	return 1;
}

static void zz9k_jpeg_usage(void)
{
	printf("usage: zz9k-jpeg [--view|--fb [--fit] [--resize] [--nearest|--bilinear] "
	       "[--hold N] [--keep]] [--tile-rows N] [file.jpg]\n");
	printf("       no file runs the built-in 2x2 smoke vector\n");
	printf("       --view opens a resizable layer-aware viewer window until close\n");
}

static int zz9k_jpeg_load_file(const char *path, ZZ9KJpegInput *input)
{
	FILE *file;
	long file_size;

	if (!path || !input)
		return 0;

	file = fopen(path, "rb");
	if (!file) {
		printf("zz9k-jpeg: failed to open '%s'\n", path);
		return 0;
	}
	if (fseek(file, 0L, SEEK_END) != 0) {
		printf("zz9k-jpeg: failed to seek '%s'\n", path);
		fclose(file);
		return 0;
	}
	file_size = ftell(file);
	if (file_size <= 0L || (unsigned long)file_size > 0xffffffffUL) {
		printf("zz9k-jpeg: unsupported input size for '%s'\n", path);
		fclose(file);
		return 0;
	}
	if (fseek(file, 0L, SEEK_SET) != 0) {
		printf("zz9k-jpeg: failed to rewind '%s'\n", path);
		fclose(file);
		return 0;
	}
	if (!zz9k_jpeg_read_file_dimensions(file, &input->width,
	                                    &input->height)) {
		printf("zz9k-jpeg: could not read JPEG dimensions from '%s'\n", path);
		fclose(file);
		return 0;
	}
	fclose(file);

	input->length = (uint32_t)file_size;
	input->path = path;
	input->built_in = 0;
	return 1;
}

static int zz9k_jpeg_parse_args(int argc, char **argv, ZZ9KJpegInput *input)
{
	int i;

	if (!input)
		return 0;

	memset(input, 0, sizeof(*input));
	input->bytes = jpeg_2x2;
	input->length = (uint32_t)sizeof(jpeg_2x2);
	input->width = 2U;
	input->height = 2U;
	input->tile_rows = ZZ9K_JPEG_DEFAULT_TILE_ROWS;
	input->framebuffer_hold_ticks = ZZ9K_JPEG_DEFAULT_FRAMEBUFFER_HOLD_TICKS;
	input->restore_framebuffer = 1;
	input->prefer_surface_backup = 1;
	input->scale_filter_override = ZZ9K_JPEG_SCALE_FILTER_AUTO;
	input->built_in = 1;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--fb") == 0) {
			input->use_framebuffer = 1;
		} else if (strcmp(argv[i], "--view") == 0) {
			input->use_framebuffer = 1;
			input->resize_framebuffer = 1;
			input->view_framebuffer = 1;
		} else if (strcmp(argv[i], "--fit") == 0) {
			input->fit_framebuffer = 1;
		} else if (strcmp(argv[i], "--resize") == 0) {
			input->resize_framebuffer = 1;
		} else if (strcmp(argv[i], "--nearest") == 0) {
			input->scale_filter_override = ZZ9K_SCALE_NEAREST;
		} else if (strcmp(argv[i], "--bilinear") == 0) {
			input->scale_filter_override = ZZ9K_SCALE_BILINEAR;
		} else if (strcmp(argv[i], "--keep") == 0) {
			input->restore_framebuffer = 0;
		} else if (strcmp(argv[i], "--hold") == 0) {
			if (++i >= argc ||
			    !zz9k_jpeg_parse_hold_ticks(
				    argv[i], &input->framebuffer_hold_ticks)) {
				zz9k_jpeg_usage();
				return 0;
			}
		} else if (strcmp(argv[i], "--tile-rows") == 0) {
			if (++i >= argc ||
			    !zz9k_jpeg_parse_u32(argv[i], &input->tile_rows)) {
				zz9k_jpeg_usage();
				return 0;
			}
		} else if (strcmp(argv[i], "-h") == 0 ||
		           strcmp(argv[i], "--help") == 0) {
			zz9k_jpeg_usage();
			return 0;
		} else if (argv[i][0] == '-') {
			zz9k_jpeg_usage();
			return 0;
		} else if (input->path == 0) {
			if (!zz9k_jpeg_load_file(argv[i], input))
				return 0;
		} else {
			zz9k_jpeg_usage();
			return 0;
		}
	}
	if (input->fit_framebuffer &&
	    (!input->use_framebuffer || input->built_in)) {
		zz9k_jpeg_usage();
		return 0;
	}
	if (input->resize_framebuffer && !input->use_framebuffer) {
		zz9k_jpeg_usage();
		return 0;
	}
	if (input->view_framebuffer && input->built_in) {
		zz9k_jpeg_usage();
		return 0;
	}

	return 1;
}

static uint32_t zz9k_jpeg_staging_bytes(void)
{
	return ZZ9K_JPEG_STAGING_BYTES;
}

static void zz9k_jpeg_delay_ticks(uint32_t ticks)
{
#if ZZ9K_JPEG_AMIGA
	if (ticks != 0U)
		Delay((LONG)ticks);
#else
	volatile uint32_t spin;
	uint32_t i;

	for (i = 0; i < ticks; i++) {
		for (spin = 0; spin < 1000000UL; spin++) {
		}
	}
#endif
}

static uint32_t zz9k_jpeg_muldiv_floor_u32(uint32_t value,
                                           uint32_t multiplier,
                                           uint32_t divisor)
{
	uint64_t product;

	if (divisor == 0U)
		return 0U;
	product = (uint64_t)value * (uint64_t)multiplier;
	return (uint32_t)(product / divisor);
}

static int zz9k_jpeg_fit_size_to_area(uint32_t src_w, uint32_t src_h,
                                       uint32_t area_w, uint32_t area_h,
                                       uint32_t *out_w, uint32_t *out_h)
{
	return zz9k_image_window_fit_size_to_area(
		src_w, src_h, area_w, area_h, out_w, out_h);
}

static int zz9k_jpeg_choose_decode_surface_size(
	const ZZ9KJpegInput *jpeg_input,
	uint32_t max_width,
	uint32_t max_height,
	uint32_t *out_width,
	uint32_t *out_height)
{
	uint32_t width;
	uint32_t height;

	if (!jpeg_input || !out_width || !out_height ||
	    jpeg_input->width == 0U || jpeg_input->height == 0U ||
	    max_width == 0U || max_height == 0U) {
		return 0;
	}

	width = jpeg_input->width;
	height = jpeg_input->height;
	if (width > max_width || height > max_height) {
		if (!zz9k_jpeg_fit_size_to_area(width, height, max_width,
		                                max_height, &width, &height)) {
			return 0;
		}
	}

	*out_width = width;
	*out_height = height;
	return 1;
}

static int zz9k_jpeg_choose_draw_rect_in_area(
	const ZZ9KFbRect *area,
	uint32_t src_width,
	uint32_t src_height,
	ZZ9KFbRect *rect)
{
	return zz9k_image_window_choose_draw_rect_in_area(
		area, src_width, src_height, rect);
}

static int zz9k_jpeg_rect_contains(
	const ZZ9KFbRect *outer,
	const ZZ9KFbRect *inner)
{
	uint64_t outer_right;
	uint64_t outer_bottom;
	uint64_t inner_right;
	uint64_t inner_bottom;

	if (!outer || !inner || outer->w == 0U || outer->h == 0U ||
	    inner->w == 0U || inner->h == 0U) {
		return 0;
	}

	outer_right = (uint64_t)outer->x + (uint64_t)outer->w;
	outer_bottom = (uint64_t)outer->y + (uint64_t)outer->h;
	inner_right = (uint64_t)inner->x + (uint64_t)inner->w;
	inner_bottom = (uint64_t)inner->y + (uint64_t)inner->h;

	return inner->x >= outer->x && inner->y >= outer->y &&
	       inner_right <= outer_right && inner_bottom <= outer_bottom;
}

static int zz9k_jpeg_scale_needs_clip(
	const ZZ9KFbRect *clip_rect,
	const ZZ9KFbRect *draw_rect)
{
	return !zz9k_jpeg_rect_contains(clip_rect, draw_rect);
}

static uint32_t zz9k_jpeg_scale_slice_rows(void)
{
	return zz9k_image_window_scale_slice_rows();
}

static uint32_t zz9k_jpeg_count_scale_slices(const ZZ9KFbRect *clip_rect)
{
	return zz9k_image_window_count_scale_slices(clip_rect);
}

static int zz9k_jpeg_build_framebuffer_scale_desc_for_rect(
	ZZ9KScaleImageDesc *desc,
	uint32_t source_handle,
	uint32_t source_width,
	uint32_t source_height,
	const ZZ9KFbRect *draw_rect,
	uint32_t filter)
{
	return zz9k_image_window_build_scale_desc(
		desc, source_handle, source_width, source_height, draw_rect,
		filter);
}

static int zz9k_jpeg_build_framebuffer_scale_desc(
	ZZ9KScaleImageDesc *desc,
	uint32_t source_handle,
	uint32_t source_width,
	uint32_t source_height,
	const ZZ9KFbRect *target_area,
	uint32_t filter)
{
	ZZ9KFbRect draw_rect;

	if (!desc || source_handle == ZZ9K_INVALID_HANDLE ||
	    source_width == 0U || source_height == 0U ||
	    !zz9k_jpeg_choose_draw_rect_in_area(target_area, source_width,
	                                        source_height, &draw_rect)) {
		return 0;
	}

	return zz9k_jpeg_build_framebuffer_scale_desc_for_rect(
		desc, source_handle, source_width, source_height, &draw_rect,
		filter);
}

static int zz9k_jpeg_build_framebuffer_clipped_scale_desc_for_rect(
	ZZ9KScaleImageClippedDesc *desc,
	uint32_t source_handle,
	uint32_t source_width,
	uint32_t source_height,
	const ZZ9KFbRect *draw_rect,
	const ZZ9KFbRect *clip_rect,
	uint32_t filter)
{
	return zz9k_image_window_build_clipped_scale_desc(
		desc, source_handle, source_width, source_height,
		draw_rect, clip_rect, filter);
}

static int zz9k_jpeg_build_framebuffer_clipped_scale_desc(
	ZZ9KScaleImageClippedDesc *desc,
	uint32_t source_handle,
	uint32_t source_width,
	uint32_t source_height,
	const ZZ9KFbRect *target_area,
	const ZZ9KFbRect *clip_rect,
	uint32_t filter)
{
	ZZ9KFbRect draw_rect;

	if (!desc || !clip_rect || source_handle == ZZ9K_INVALID_HANDLE ||
	    source_width == 0U || source_height == 0U ||
	    !zz9k_jpeg_choose_draw_rect_in_area(target_area, source_width,
	                                        source_height, &draw_rect)) {
		return 0;
	}

	return zz9k_jpeg_build_framebuffer_clipped_scale_desc_for_rect(
		desc, source_handle, source_width, source_height, &draw_rect,
		clip_rect, filter);
}

static int zz9k_jpeg_scale_framebuffer_sliced(
	ZZ9KContext *ctx,
	uint32_t source_handle,
	uint32_t source_width,
	uint32_t source_height,
	const ZZ9KFbRect *draw_rect,
	const ZZ9KFbRect *clip_rect,
	uint32_t filter)
{
	return zz9k_image_window_scale_sliced(
		ctx, source_handle, source_width, source_height,
		draw_rect, clip_rect, filter);
}

static int zz9k_jpeg_choose_window_origin(
	uint32_t screen_width,
	uint32_t screen_height,
	uint32_t window_width,
	uint32_t window_height,
	uint32_t *left,
	uint32_t *top)
{
	return zz9k_image_window_choose_origin(
		screen_width, screen_height, window_width, window_height,
		left, top);
}

static int zz9k_jpeg_choose_window_max_extent(
	uint32_t framebuffer_width,
	uint32_t framebuffer_height,
	uint32_t left,
	uint32_t top,
	uint32_t min_width,
	uint32_t min_height,
	uint32_t *max_width,
	uint32_t *max_height)
{
	return zz9k_image_window_choose_max_extent(
		framebuffer_width, framebuffer_height, left, top,
		min_width, min_height, max_width, max_height);
}

static int zz9k_jpeg_choose_framebuffer_rect(
	const ZZ9KSurface *framebuffer,
	const ZZ9KJpegInput *jpeg_input,
	ZZ9KFbRect *rect)
{
	uint32_t width;
	uint32_t height;
	uint64_t width_limited;

	if (!framebuffer || !jpeg_input || !rect ||
	    framebuffer->width == 0U || framebuffer->height == 0U ||
	    jpeg_input->width == 0U || jpeg_input->height == 0U) {
		return 0;
	}

	width = jpeg_input->width;
	height = jpeg_input->height;
	if (jpeg_input->fit_framebuffer &&
	    (width > framebuffer->width || height > framebuffer->height)) {
		width_limited = (uint64_t)width * (uint64_t)framebuffer->height;
		if (width_limited >
		    ((uint64_t)height * (uint64_t)framebuffer->width)) {
			height = zz9k_jpeg_muldiv_floor_u32(
				height, framebuffer->width, width);
			width = framebuffer->width;
			if (height == 0U)
				height = 1U;
		} else {
			width = zz9k_jpeg_muldiv_floor_u32(
				width, framebuffer->height, height);
			height = framebuffer->height;
			if (width == 0U)
				width = 1U;
		}
	}

	rect->x = 0U;
	rect->y = 0U;
	rect->w = width;
	rect->h = height;
	return zz9k_fb_rect_fits(framebuffer, rect,
	                         zz9k_fb_bytes_per_pixel(framebuffer->format));
}

static int zz9k_jpeg_make_framebuffer_backup_copy_descs(
	const ZZ9KSurface *backup_surface,
	const ZZ9KFbRect *rect,
	ZZ9KSurfaceCopyDesc *save_copy,
	ZZ9KSurfaceCopyDesc *restore_copy)
{
	return zz9k_fb_build_framebuffer_backup_copy_descs(
		backup_surface, rect, save_copy, restore_copy);
}

static void zz9k_jpeg_framebuffer_backup_init(
	ZZ9KJpegFramebufferBackup *backup)
{
	if (backup)
		memset(backup, 0, sizeof(*backup));
}

static int zz9k_jpeg_framebuffer_restore(
	ZZ9KContext *ctx,
	const ZZ9KSurface *framebuffer,
	ZZ9KJpegFramebufferBackup *backup)
{
	int status;

	if (!framebuffer || !backup || !backup->active) {
		return ZZ9K_STATUS_OK;
	}

	if (backup->mode == ZZ9K_JPEG_BACKUP_SURFACE) {
		if (!ctx)
			return ZZ9K_STATUS_INTERNAL_ERROR;
		status = zz9k_copy_surface(ctx, &backup->restore_copy);
		if (status != ZZ9K_STATUS_OK)
			return status;
		backup->active = 0;
		return ZZ9K_STATUS_OK;
	}

	if (backup->mode != ZZ9K_JPEG_BACKUP_CPU || !backup->bytes ||
	    !framebuffer->data) {
		return ZZ9K_STATUS_INTERNAL_ERROR;
	}
	zz9k_fb_copy_rect_to_surface((volatile uint8_t *)framebuffer->data,
	                             backup->bytes, framebuffer->pitch,
	                             &backup->rect, backup->bpp);
	backup->active = 0;
	return ZZ9K_STATUS_OK;
}

static int zz9k_jpeg_framebuffer_restore_visible(
	ZZ9KContext *ctx,
	const ZZ9KSurface *framebuffer,
	const ZZ9KImageWindow *ui,
	ZZ9KJpegFramebufferBackup *backup)
{
	ZZ9KFbRect clips[ZZ9K_IMAGE_WINDOW_MAX_VISIBLE_CLIPS];
	uint32_t clip_count;
	uint32_t i;

	if (!framebuffer || !ui || !backup || !backup->active)
		return ZZ9K_STATUS_OK;
	if (!zz9k_image_window_visible_clips(
		    ui, &backup->rect, clips, ZZ9K_IMAGE_WINDOW_MAX_VISIBLE_CLIPS,
		    &clip_count)) {
		return ZZ9K_STATUS_INTERNAL_ERROR;
	}

	if (backup->mode == ZZ9K_JPEG_BACKUP_SURFACE) {
		if (!ctx)
			return ZZ9K_STATUS_INTERNAL_ERROR;
		for (i = 0U; i < clip_count; i++) {
			ZZ9KSurfaceCopyDesc restore_clip;
			int status;

			if (!zz9k_fb_build_framebuffer_backup_restore_clip_desc(
				    &backup->surface, &backup->rect, &clips[i],
				    &restore_clip)) {
				return ZZ9K_STATUS_BAD_REQUEST;
			}
			status = zz9k_copy_surface(ctx, &restore_clip);
			if (status != ZZ9K_STATUS_OK)
				return status;
		}
		backup->active = 0;
		return ZZ9K_STATUS_OK;
	}

	if (backup->mode != ZZ9K_JPEG_BACKUP_CPU || !backup->bytes ||
	    !framebuffer->data) {
		return ZZ9K_STATUS_INTERNAL_ERROR;
	}
	for (i = 0U; i < clip_count; i++) {
		if (!zz9k_fb_copy_backup_clip_to_surface(
			    (volatile uint8_t *)framebuffer->data, backup->bytes,
			    framebuffer->pitch, &backup->rect, &clips[i],
			    backup->bpp)) {
			return ZZ9K_STATUS_BAD_REQUEST;
		}
	}
	backup->active = 0;
	return ZZ9K_STATUS_OK;
}

static void zz9k_jpeg_framebuffer_backup_free(
	ZZ9KContext *ctx,
	ZZ9KJpegFramebufferBackup *backup)
{
	if (!backup)
		return;
	if (backup->surface_allocated && backup->surface.handle != 0U)
		zz9k_free_surface(ctx, backup->surface.handle);
	free(backup->bytes);
	zz9k_jpeg_framebuffer_backup_init(backup);
}

static int zz9k_jpeg_framebuffer_backup_prepare_cpu(
	const ZZ9KSurface *framebuffer,
	ZZ9KJpegFramebufferBackup *backup)
{
	uint32_t row_bytes;

	if (!framebuffer || !backup)
		return 0;
	if (!framebuffer->data) {
		printf("zz9k-jpeg: framebuffer is not CPU-mapped; use --keep "
		       "to leave the image on screen\n");
		return 0;
	}
	if (backup->rect.w > (0xffffffffU / backup->bpp))
		return 0;
	row_bytes = backup->rect.w * backup->bpp;
	if (backup->rect.h > (0xffffffffU / row_bytes))
		return 0;
	backup->length = row_bytes * backup->rect.h;
	backup->bytes = (uint8_t *)malloc(backup->length);
	if (!backup->bytes) {
		printf("zz9k-jpeg: framebuffer backup allocation failed: "
		       "%lu bytes; use --keep to leave the image on screen\n",
		       (unsigned long)backup->length);
		return 0;
	}
	zz9k_fb_copy_rect_from_surface(backup->bytes,
	                               (const volatile uint8_t *)framebuffer->data,
	                               framebuffer->pitch, &backup->rect,
	                               backup->bpp);
	backup->mode = ZZ9K_JPEG_BACKUP_CPU;
	backup->active = 1;
	return 1;
}

static int zz9k_jpeg_framebuffer_backup_prepare_surface(
	ZZ9KContext *ctx,
	const ZZ9KSurface *framebuffer,
	ZZ9KJpegFramebufferBackup *backup)
{
	uint32_t row_bytes;
	int status;

	if (!ctx || !framebuffer || !backup || backup->bpp == 0U)
		return 0;
	if (backup->rect.w > (0xffffffffU / backup->bpp))
		return 0;
	row_bytes = backup->rect.w * backup->bpp;
	status = zz9k_alloc_surface_ex(ctx, backup->rect.w, backup->rect.h,
	                               framebuffer->format,
	                               ZZ9K_SURFACE_FLAG_ARM_LOCAL,
	                               row_bytes,
	                               &backup->surface);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-jpeg: ARM framebuffer backup surface alloc failed: "
		       "%s (%d), falling back to CPU backup\n",
		       zz9k_status_name(status), status);
		return 0;
	}
	backup->surface_allocated = 1;
	if (!zz9k_jpeg_make_framebuffer_backup_copy_descs(
		    &backup->surface, &backup->rect, &backup->save_copy,
		    &backup->restore_copy)) {
		return 0;
	}
	status = zz9k_copy_surface(ctx, &backup->save_copy);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-jpeg: ARM framebuffer backup copy failed: "
		       "%s (%d), falling back to CPU backup\n",
		       zz9k_status_name(status), status);
		return 0;
	}
	backup->mode = ZZ9K_JPEG_BACKUP_SURFACE;
	backup->active = 1;
	return 1;
}

static int zz9k_jpeg_framebuffer_backup_prepare(
	ZZ9KContext *ctx,
	const ZZ9KSurface *framebuffer,
	const ZZ9KJpegInput *jpeg_input,
	ZZ9KJpegFramebufferBackup *backup)
{
	if (!framebuffer || !jpeg_input || !backup)
		return 0;
	zz9k_jpeg_framebuffer_backup_init(backup);
	backup->bpp = zz9k_fb_bytes_per_pixel(framebuffer->format);
	if (backup->bpp == 0U ||
	    !zz9k_jpeg_choose_framebuffer_rect(framebuffer, jpeg_input,
	                                       &backup->rect)) {
		printf("zz9k-jpeg: could not choose framebuffer restore rectangle\n");
		return 0;
	}
	if (jpeg_input->prefer_surface_backup &&
	    zz9k_jpeg_framebuffer_backup_prepare_surface(ctx, framebuffer,
	                                                 backup)) {
		return 1;
	}
	if (backup->surface_allocated) {
		zz9k_free_surface(ctx, backup->surface.handle);
		memset(&backup->surface, 0, sizeof(backup->surface));
		backup->surface_allocated = 0;
	}
	return zz9k_jpeg_framebuffer_backup_prepare_cpu(framebuffer, backup);
}

static int zz9k_jpeg_framebuffer_backup_prepare_rect(
	ZZ9KContext *ctx,
	const ZZ9KSurface *framebuffer,
	const ZZ9KJpegInput *jpeg_input,
	const ZZ9KFbRect *rect,
	ZZ9KJpegFramebufferBackup *backup)
{
	if (!framebuffer || !jpeg_input || !rect || !backup)
		return 0;
	zz9k_jpeg_framebuffer_backup_init(backup);
	backup->bpp = zz9k_fb_bytes_per_pixel(framebuffer->format);
	if (backup->bpp == 0U ||
	    !zz9k_fb_rect_fits(framebuffer, rect, backup->bpp)) {
		printf("zz9k-jpeg: invalid framebuffer backup rectangle\n");
		return 0;
	}
	backup->rect = *rect;
	if (jpeg_input->prefer_surface_backup &&
	    zz9k_jpeg_framebuffer_backup_prepare_surface(ctx, framebuffer,
	                                                 backup)) {
		return 1;
	}
	if (backup->surface_allocated) {
		zz9k_free_surface(ctx, backup->surface.handle);
		memset(&backup->surface, 0, sizeof(backup->surface));
		backup->surface_allocated = 0;
	}
	return zz9k_jpeg_framebuffer_backup_prepare_cpu(framebuffer, backup);
}

static void print_diag(ZZ9KContext *ctx, const char *label)
{
	ZZ9KDiagInfo diag;
	int status;

	memset(&diag, 0, sizeof(diag));
	status = zz9k_read_diag(ctx, &diag);
	if (status != ZZ9K_STATUS_OK) {
		printf("Allocator diag (%s): %s (%d)\n",
		       label, zz9k_status_name(status), status);
		return;
	}

	printf("Allocator diag (%s):\n", label);
	printf("  Requests done/fail: %lu/%lu\n",
	       (unsigned long)diag.requests_completed,
	       (unsigned long)diag.requests_failed);
	printf("  Last status:        %s (%lu)\n",
	       zz9k_status_name((int)diag.last_status),
	       (unsigned long)diag.last_status);
	printf("  Shared buffers used: %lu\n",
	       (unsigned long)diag.shared_buffers_used);
	printf("  Surfaces used:       %lu\n",
	       (unsigned long)diag.surfaces_used);
	printf("  Invalid alloc slots: %lu\n",
	       (unsigned long)diag.allocator_invalid_slots);
	printf("  Shared heap free:    %lu bytes\n",
	       (unsigned long)diag.shared_heap_free);
	printf("  Largest free block:  %lu bytes\n",
	       (unsigned long)diag.shared_heap_largest_free);
}

typedef ZZ9KImageWindow ZZ9KJpegWindow;

static int zz9k_jpeg_open_window(const ZZ9KSurface *framebuffer,
                                 const ZZ9KJpegInput *jpeg_input,
                                 ZZ9KJpegWindow *ui)
{
	ZZ9KImageWindowConfig config;

	if (!framebuffer || !jpeg_input || !ui)
		return 0;
	zz9k_image_window_config_init(
		&config, "ZZ9000 SDK JPEG", jpeg_input->width,
		jpeg_input->height, jpeg_input->fit_framebuffer,
		jpeg_input->resize_framebuffer, ZZ9K_JPEG_WINDOW_MIN_WIDTH,
		ZZ9K_JPEG_WINDOW_MIN_HEIGHT, ZZ9K_JPEG_WINDOW_MARGIN_X,
		ZZ9K_JPEG_WINDOW_MARGIN_Y);
	return zz9k_image_window_open(framebuffer, &config, ui);
}

static int zz9k_jpeg_poll_window(ZZ9KJpegWindow *ui,
                                 const ZZ9KSurface *framebuffer,
                                 int *changed,
                                 int *closed)
{
	return zz9k_image_window_poll(ui, framebuffer, changed, closed);
}

static void zz9k_jpeg_close_window(ZZ9KJpegWindow *ui)
{
	zz9k_image_window_close(ui);
}

static uint32_t zz9k_jpeg_choose_scale_filter(
	ZZ9KContext *ctx,
	const ZZ9KJpegInput *jpeg_input)
{
	ZZ9KServiceInfo service;

	if (jpeg_input &&
	    jpeg_input->scale_filter_override != ZZ9K_JPEG_SCALE_FILTER_AUTO) {
		return jpeg_input->scale_filter_override;
	}

	memset(&service, 0, sizeof(service));
	if (zz9k_query_service(ctx, ZZ9K_SERVICE_IMAGE, &service) ==
	    ZZ9K_STATUS_OK) {
		if (zz9k_image_scale_filter_supported_by_service(
			    service.flags, ZZ9K_SCALE_BILINEAR)) {
			return ZZ9K_SCALE_BILINEAR;
		}
	}
	return ZZ9K_SCALE_NEAREST;
}

static int zz9k_jpeg_require_clipped_scale_service(ZZ9KContext *ctx)
{
	ZZ9KServiceInfo service;
	int status;

	memset(&service, 0, sizeof(service));
	status = zz9k_query_service(ctx, ZZ9K_SERVICE_IMAGE, &service);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-jpeg: image service query failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		return 0;
	}
	if (!zz9k_image_service_supports_clipped_scale(
		    service.opcode_count, service.flags, ZZ9K_SCALE_NEAREST)) {
		printf("zz9k-jpeg: firmware does not advertise clipped scale\n");
		return 0;
	}
	return 1;
}

static int zz9k_jpeg_restore_and_free_backup(
	ZZ9KContext *ctx,
	const ZZ9KSurface *framebuffer,
	ZZ9KJpegFramebufferBackup *backup)
{
	int status;

	if (!backup)
		return ZZ9K_STATUS_OK;
	status = zz9k_jpeg_framebuffer_restore(ctx, framebuffer, backup);
	if (status != ZZ9K_STATUS_OK)
		return status;
	zz9k_jpeg_framebuffer_backup_free(ctx, backup);
	return ZZ9K_STATUS_OK;
}

static int zz9k_jpeg_restore_visible_and_free_backup(
	ZZ9KContext *ctx,
	const ZZ9KSurface *framebuffer,
	const ZZ9KImageWindow *ui,
	ZZ9KJpegFramebufferBackup *backup)
{
	int status;

	if (!backup)
		return ZZ9K_STATUS_OK;
	status = zz9k_jpeg_framebuffer_restore_visible(
		ctx, framebuffer, ui, backup);
	if (status != ZZ9K_STATUS_OK)
		return status;
	zz9k_jpeg_framebuffer_backup_free(ctx, backup);
	return ZZ9K_STATUS_OK;
}

static int zz9k_jpeg_windowed_restore_enabled(void)
{
	return 1;
}

static int zz9k_jpeg_render_source_to_area(
	ZZ9KContext *ctx,
	const ZZ9KSurface *framebuffer,
	const ZZ9KJpegInput *jpeg_input,
	const ZZ9KSurface *source,
	uint32_t source_width,
	uint32_t source_height,
	const ZZ9KJpegWindow *ui,
	const ZZ9KFbRect *area,
	uint32_t filter,
	ZZ9KJpegFramebufferBackup *backup)
{
	ZZ9KSurfaceFillDesc fill;
	ZZ9KFbRect clips[ZZ9K_IMAGE_WINDOW_MAX_VISIBLE_CLIPS];
	ZZ9KFbRect render_area;
	ZZ9KFbRect draw_rect;
	uint32_t clip_count;
	uint32_t i;
	int status;

	if (!ctx || !framebuffer || !jpeg_input || !source || !ui || !area ||
	    !backup || source_width == 0U || source_height == 0U ||
	    area->w == 0U || area->h == 0U) {
		return ZZ9K_STATUS_BAD_REQUEST;
	}
	render_area = *area;
	if (!zz9k_jpeg_choose_draw_rect_in_area(&render_area, source_width,
	                                        source_height, &draw_rect)) {
		return ZZ9K_STATUS_BAD_REQUEST;
	}

	if (zz9k_jpeg_windowed_restore_enabled()) {
		if (jpeg_input->resize_framebuffer && backup->active) {
			zz9k_jpeg_framebuffer_backup_free(ctx, backup);
		} else {
			status = zz9k_jpeg_restore_visible_and_free_backup(
				ctx, framebuffer, ui, backup);
			if (status != ZZ9K_STATUS_OK)
				return status;
		}
		if (jpeg_input->restore_framebuffer &&
		    !zz9k_jpeg_framebuffer_backup_prepare_rect(ctx, framebuffer,
		                                               jpeg_input, area,
		                                               backup)) {
			return ZZ9K_STATUS_INTERNAL_ERROR;
		}
	}

	if (!zz9k_image_window_visible_clips(
		    ui, area, clips, ZZ9K_IMAGE_WINDOW_MAX_VISIBLE_CLIPS,
		    &clip_count)) {
		return ZZ9K_STATUS_INTERNAL_ERROR;
	}

	for (i = 0U; i < clip_count; i++) {
		if (!zz9k_image_window_build_framebuffer_fill_desc(
			    &fill, &clips[i], zz9k_surface_color_rgb(0U, 0U, 0U),
			    0U)) {
			return ZZ9K_STATUS_BAD_REQUEST;
		}
		status = zz9k_fill_surface(ctx, &fill);
		if (status != ZZ9K_STATUS_OK)
			return status;

		status = zz9k_jpeg_scale_framebuffer_sliced(
			ctx, source->handle, source_width, source_height,
			&draw_rect, &clips[i], filter);
		if (status != ZZ9K_STATUS_OK)
			return status;
	}

	return ZZ9K_STATUS_OK;
}

static int zz9k_jpeg_show_decoded_surface(
	ZZ9KContext *ctx,
	const ZZ9KSurface *framebuffer,
	const ZZ9KJpegInput *jpeg_input,
	const ZZ9KSurface *source,
	uint32_t source_width,
	uint32_t source_height,
	ZZ9KJpegFramebufferBackup *backup)
{
	ZZ9KJpegWindow ui;
	uint32_t filter;
	uint32_t tick;
	int status;
	int rc = 1;

	memset(&ui, 0, sizeof(ui));
	if (!zz9k_jpeg_open_window(framebuffer, jpeg_input, &ui))
		return 0;

	printf("zz9k-jpeg: window area %lu,%lu %lu x %lu\n",
	       (unsigned long)ui.inner.x, (unsigned long)ui.inner.y,
	       (unsigned long)ui.inner.w, (unsigned long)ui.inner.h);
	filter = zz9k_jpeg_choose_scale_filter(ctx, jpeg_input);
	status = zz9k_jpeg_render_source_to_area(ctx, framebuffer, jpeg_input,
	                                         source, source_width,
	                                         source_height, &ui,
	                                         &ui.inner,
	                                         filter, backup);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-jpeg: framebuffer scale failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		goto cleanup;
	}

	if (jpeg_input->view_framebuffer) {
		printf("zz9k-jpeg: showing until close%s, %lu scale slices\n",
		       filter == ZZ9K_SCALE_BILINEAR ? " using bilinear scale" :
		                                       " using nearest scale",
		       (unsigned long)zz9k_jpeg_count_scale_slices(&ui.inner));
	} else {
		printf("zz9k-jpeg: showing for %lu ticks%s, %lu scale slices\n",
		       (unsigned long)jpeg_input->framebuffer_hold_ticks,
		       filter == ZZ9K_SCALE_BILINEAR ? " using bilinear scale" :
		                                       " using nearest scale",
		       (unsigned long)zz9k_jpeg_count_scale_slices(&ui.inner));
	}
	tick = 0U;
	while (!zz9k_image_window_loop_done(jpeg_input->view_framebuffer,
	                                    tick,
	                                    jpeg_input->framebuffer_hold_ticks,
	                                    0)) {
		int changed;
		int closed;

		if (!zz9k_jpeg_poll_window(&ui, framebuffer, &changed, &closed)) {
			printf("zz9k-jpeg: invalid JPEG window after event\n");
			goto cleanup;
		}
		if (zz9k_image_window_loop_done(
			    jpeg_input->view_framebuffer, tick,
			    jpeg_input->framebuffer_hold_ticks, closed)) {
			break;
		}
		if (changed) {
			status = zz9k_jpeg_render_source_to_area(
				ctx, framebuffer, jpeg_input, source,
				source_width, source_height, &ui, &ui.inner,
				filter, backup);
			if (status != ZZ9K_STATUS_OK) {
				printf("zz9k-jpeg: framebuffer rescale failed: "
				       "%s (%d)\n",
				       zz9k_status_name(status), status);
				goto cleanup;
			}
			printf("zz9k-jpeg: resized window area %lu,%lu %lu x %lu\n",
			       (unsigned long)ui.inner.x,
			       (unsigned long)ui.inner.y,
			       (unsigned long)ui.inner.w,
			       (unsigned long)ui.inner.h);
		}
		tick++;
		zz9k_jpeg_delay_ticks(1U);
	}

	if (backup->active) {
		status = zz9k_jpeg_framebuffer_restore_visible(
			ctx, framebuffer, &ui, backup);
		if (status != ZZ9K_STATUS_OK) {
			printf("zz9k-jpeg: framebuffer restore failed: %s (%d)\n",
			       zz9k_status_name(status), status);
			goto cleanup;
		}
		printf("zz9k-jpeg: restored framebuffer window area%s\n",
		       backup->mode == ZZ9K_JPEG_BACKUP_SURFACE ?
			       " using ARM surface copy" : "");
	}
	rc = 0;

cleanup:
	if (backup->active) {
		status = zz9k_jpeg_framebuffer_restore_visible(
			ctx, framebuffer, &ui, backup);
		if (status != ZZ9K_STATUS_OK) {
			printf("zz9k-jpeg: framebuffer restore failed during cleanup: "
			       "%s (%d)\n",
			       zz9k_status_name(status), status);
		}
	}
	zz9k_jpeg_close_window(&ui);
	return rc == 0;
}

static int zz9k_jpeg_run_builtin(ZZ9KContext *ctx,
                                  const ZZ9KJpegInput *jpeg_input)
{
	ZZ9KSharedBuffer input;
	ZZ9KSurface surface;
	ZZ9KSurface framebuffer;
	ZZ9KJpegFramebufferBackup backup;
	ZZ9KImageDecodeDesc desc;
	ZZ9KImageDecodeResult result;
	ZZ9KRect decode_rect;
	volatile uint8_t *pixels;
	uint8_t top_left[4];
	uint8_t top_right[4];
	uint8_t bottom_left[4];
	uint8_t bottom_right[4];
	int input_allocated = 0;
	int surface_allocated = 0;
	int status;
	int rc = 1;
	uint32_t decode_width;
	uint32_t decode_height;
	uint32_t output_format;
	uint32_t output_bpp;
	uint32_t output_pitch;
	uint32_t decode_pitch;
	uint32_t expected_output_bytes;

	memset(&input, 0, sizeof(input));
	memset(&surface, 0, sizeof(surface));
	memset(&framebuffer, 0, sizeof(framebuffer));
	zz9k_jpeg_framebuffer_backup_init(&backup);

	printf("JPEG input bytes:     %lu\n",
	       (unsigned long)jpeg_input->length);
	printf("JPEG dimensions:      %lu x %lu\n",
	       (unsigned long)jpeg_input->width,
	       (unsigned long)jpeg_input->height);
	print_diag(ctx, "before input alloc");
	status = zz9k_alloc_shared(ctx, jpeg_input->length, 16U, 0,
	                          &input);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-jpeg: input alloc failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		print_diag(ctx, "after input alloc failure");
		goto cleanup;
	}
	input_allocated = 1;
	if (!zz9k_shared_copy_to(&input, 0U, jpeg_input->bytes,
	                         jpeg_input->length)) {
		printf("zz9k-jpeg: input copy failed\n");
		goto cleanup;
	}

	output_format = zz9k_surface_native_rtg_format();
	output_bpp = zz9k_surface_bytes_per_pixel(output_format);
	if (output_bpp == 0U || output_bpp > sizeof(top_left)) {
		printf("zz9k-jpeg: unsupported native RTG bytes-per-pixel %lu\n",
		       (unsigned long)output_bpp);
		goto cleanup;
	}
	if (!zz9k_surface_layout(jpeg_input->width, jpeg_input->height,
	                         output_format, &output_pitch,
	                         &expected_output_bytes)) {
		printf("zz9k-jpeg: decoded output is too large\n");
		goto cleanup;
	}
	if (jpeg_input->use_framebuffer) {
		status = zz9k_map_framebuffer_surface(ctx, &framebuffer);
		if (status != ZZ9K_STATUS_OK) {
			printf("zz9k-jpeg: framebuffer map failed: %s (%d)\n",
			       zz9k_status_name(status), status);
			print_diag(ctx, "after framebuffer map failure");
			goto cleanup;
		}
		if (!zz9k_surface_is_native_rtg_format(framebuffer.format)) {
			printf("zz9k-jpeg: framebuffer is not native RTG "
			       "(image %lu x %lu, framebuffer %lu x %lu format=%lu)\n",
			       (unsigned long)jpeg_input->width,
			       (unsigned long)jpeg_input->height,
			       (unsigned long)framebuffer.width,
			       (unsigned long)framebuffer.height,
			       (unsigned long)framebuffer.format);
			goto cleanup;
		}
		if (!zz9k_jpeg_choose_decode_surface_size(
			    jpeg_input, framebuffer.width, framebuffer.height,
			    &decode_width, &decode_height)) {
			printf("zz9k-jpeg: could not choose ARM decode surface size\n");
			goto cleanup;
		}
		if (!zz9k_surface_min_pitch(decode_width, output_format,
		                            &decode_pitch)) {
			printf("zz9k-jpeg: invalid decode surface pitch\n");
			goto cleanup;
		}
		status = zz9k_alloc_surface_ex(ctx, decode_width, decode_height,
		                               output_format,
		                               ZZ9K_SURFACE_FLAG_ARM_LOCAL,
		                               decode_pitch,
		                               &surface);
		if (status != ZZ9K_STATUS_OK) {
			printf("zz9k-jpeg: decode surface alloc failed: %s (%d)\n",
			       zz9k_status_name(status), status);
			goto cleanup;
		}
		surface_allocated = 1;
	} else {
		decode_width = jpeg_input->width;
		decode_height = jpeg_input->height;
		status = zz9k_alloc_surface_ex(ctx, jpeg_input->width,
		                               jpeg_input->height, output_format, 0,
		                               output_pitch, &surface);
		if (status != ZZ9K_STATUS_OK) {
			printf("zz9k-jpeg: surface alloc failed: %s (%d)\n",
			       zz9k_status_name(status), status);
			goto cleanup;
		}
		surface_allocated = 1;
	}

	decode_rect.x = 0U;
	decode_rect.y = 0U;
	decode_rect.w = decode_width;
	decode_rect.h = decode_height;
	if (!zz9k_image_build_decode_desc(&desc, input.handle, 0U,
	                                  jpeg_input->length, surface.handle,
	                                  &decode_rect, output_format, 0U)) {
		printf("zz9k-jpeg: could not build decode descriptor\n");
		goto cleanup;
	}

	memset(&result, 0, sizeof(result));
	status = zz9k_decode_jpeg(ctx, &desc, &result);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-jpeg: decode failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		goto cleanup;
	}
	if (result.width != jpeg_input->width ||
	    result.height != jpeg_input->height ||
	    result.output_format != output_format ||
	    result.bytes_written != expected_output_bytes) {
		printf("zz9k-jpeg: unexpected result %lu x %lu format=%lu bytes=%lu\n",
		       (unsigned long)result.width, (unsigned long)result.height,
		       (unsigned long)result.output_format,
		       (unsigned long)result.bytes_written);
		goto cleanup;
	}

	if (jpeg_input->built_in) {
		if (jpeg_input->use_framebuffer) {
			printf("zz9k-jpeg: known vector decode ok; drawing via "
			       "ARM-side framebuffer scale\n");
			if (!zz9k_jpeg_show_decoded_surface(ctx, &framebuffer,
			                                    jpeg_input, &surface,
			                                    result.width,
			                                    result.height,
			                                    &backup)) {
				goto cleanup;
			}
			rc = 0;
			goto cleanup;
		}
		if (!surface.data && !jpeg_input->restore_framebuffer) {
			printf("zz9k-jpeg: known vector decode ok "
			       "(framebuffer output, no host CPU pointer)\n");
			rc = 0;
			goto cleanup;
		}
		if (!surface.data) {
			printf("zz9k-jpeg: framebuffer output has no host CPU "
			       "pointer\n");
			goto cleanup;
		}
		pixels = (volatile uint8_t *)surface.data;
		volatile_copy_from(top_left, &pixels[(0U * surface.pitch) +
		                                     (0U * output_bpp)], output_bpp);
		volatile_copy_from(top_right, &pixels[(0U * surface.pitch) +
		                                      (1U * output_bpp)], output_bpp);
		volatile_copy_from(bottom_left, &pixels[(1U * surface.pitch) +
		                                        (0U * output_bpp)], output_bpp);
		volatile_copy_from(bottom_right, &pixels[(1U * surface.pitch) +
		                                         (1U * output_bpp)], output_bpp);
		if (!dominant_red(top_left) || !dominant_green(top_right) ||
		    !dominant_blue(bottom_left) || !near_white(bottom_right)) {
			printf("zz9k-jpeg: decoded BGRA pixels did not match vector\n");
			goto cleanup;
		}

		printf("zz9k-jpeg: known vector ok (%lu x %lu, %lu bytes)\n",
		       (unsigned long)result.width, (unsigned long)result.height,
		       (unsigned long)result.bytes_written);
	} else {
		printf("zz9k-jpeg: file decode ok (%lu x %lu, %lu input bytes, "
		       "%lu output bytes%s)\n",
		       (unsigned long)result.width, (unsigned long)result.height,
		       (unsigned long)jpeg_input->length,
		       (unsigned long)result.bytes_written,
		       jpeg_input->use_framebuffer ? ", framebuffer" : "");
	}

	rc = 0;

cleanup:
	if (backup.active) {
		status = zz9k_jpeg_framebuffer_restore(ctx, &framebuffer, &backup);
		if (status != ZZ9K_STATUS_OK) {
			printf("zz9k-jpeg: framebuffer restore failed during cleanup: "
			       "%s (%d)\n",
			       zz9k_status_name(status), status);
			rc = 1;
		}
	}
	zz9k_jpeg_framebuffer_backup_free(ctx, &backup);
	if (surface_allocated) {
		zz9k_free_surface(ctx, surface.handle);
	}
	if (input_allocated) {
		zz9k_free_shared(ctx, input.handle);
	}

	return rc;
}

static int zz9k_jpeg_require_stream_service(ZZ9KContext *ctx,
                                            int use_framebuffer,
                                            int fit_framebuffer)
{
	ZZ9KServiceInfo service;
	uint32_t required_flags;
	int status;

	memset(&service, 0, sizeof(service));
	status = zz9k_query_service(ctx, ZZ9K_SERVICE_IMAGE, &service);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-jpeg: image service query failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		return 0;
	}

	if (!zz9k_image_stream_required_service_flags(
		    ZZ9K_IMAGE_CODEC_JPEG,
		    use_framebuffer ? ZZ9K_IMAGE_OUTPUT_SURFACE :
		                      ZZ9K_IMAGE_OUTPUT_TILE_BUFFER,
		    &required_flags)) {
		printf("zz9k-jpeg: could not build required image service "
		       "flags\n");
		return 0;
	}
	if (service.opcode_count < 7U ||
	    !zz9k_has_service_flags(service.flags, required_flags)) {
		printf("zz9k-jpeg: firmware does not advertise JPEG streaming "
		       "support\n");
		return 0;
	}
	if ((fit_framebuffer || use_framebuffer) &&
	    (service.flags & ZZ9K_SERVICE_FLAG_IMAGE_JPEG_SCALING) == 0U) {
		printf("zz9k-jpeg: firmware does not advertise JPEG scaling "
		       "support\n");
		return 0;
	}

	return 1;
}

static int zz9k_jpeg_read_chunk_to_shared(FILE *file,
                                          ZZ9KSharedBuffer *staging,
                                          uint32_t offset,
                                          uint32_t length)
{
	uint8_t scratch[1024];
	uint32_t copied = 0U;

	if (!file || !staging)
		return 0;
	while (copied < length) {
		uint32_t want = length - copied;
		size_t bytes_read;

		if (want > sizeof(scratch))
			want = sizeof(scratch);
		bytes_read = fread(scratch, 1U, want, file);
		if (bytes_read == 0U)
			return 0;
		if (!zz9k_shared_copy_to(staging, offset + copied, scratch,
		                         (uint32_t)bytes_read))
			return 0;
		copied += (uint32_t)bytes_read;
	}

	return 1;
}

static int zz9k_jpeg_stream_result_made_progress(
	const ZZ9KImageSessionResult *result)
{
	if (!result)
		return 0;

	return result->bytes_consumed != 0U || result->bytes_written != 0U ||
	       result->state == ZZ9K_IMAGE_SESSION_STATE_COMPLETE;
}

static int zz9k_jpeg_stream_no_progress_is_fatal(
	const ZZ9KImageSessionResult *result, uint32_t buffered,
	uint32_t capacity, int eof)
{
	if (zz9k_jpeg_stream_result_made_progress(result))
		return 0;
	if (!eof && buffered < capacity)
		return 0;
	return 1;
}

static int zz9k_jpeg_fill_staging(FILE *file,
                                  const ZZ9KJpegInput *jpeg_input,
                                  ZZ9KSharedBuffer *staging,
                                  uint32_t *file_offset,
                                  uint32_t *buffered)
{
	uint32_t remaining_file;
	uint32_t space;
	uint32_t want;

	if (!file || !jpeg_input || !staging || !file_offset || !buffered)
		return 0;
	if (*file_offset > jpeg_input->length || *buffered > staging->length)
		return 0;

	remaining_file = jpeg_input->length - *file_offset;
	space = staging->length - *buffered;
	if (remaining_file == 0U || space == 0U)
		return 1;

	want = remaining_file < space ? remaining_file : space;
	if (!zz9k_jpeg_read_chunk_to_shared(file, staging, *buffered, want)) {
		printf("zz9k-jpeg: short read from '%s'\n", jpeg_input->path);
		return 0;
	}

	*file_offset += want;
	*buffered += want;
	return 1;
}

static int zz9k_jpeg_feed_stream(ZZ9KContext *ctx, FILE *file,
                                 const ZZ9KJpegInput *jpeg_input,
                                 ZZ9KSharedBuffer *staging,
                                 uint32_t session,
                                 uint32_t *out_tiles,
                                 uint32_t *out_bytes_written,
                                 ZZ9KImageSessionResult *final_result)
{
	uint32_t file_offset = 0U;
	uint32_t buffered = 0U;
	uint32_t tiles = 0U;
	uint32_t bytes_written = 0U;
	uint32_t empty_eof_feeds = 0U;

	memset(final_result, 0, sizeof(*final_result));
	while (final_result->state != ZZ9K_IMAGE_SESSION_STATE_COMPLETE) {
		uint32_t consumed;
		int eof;

		if (!zz9k_jpeg_fill_staging(file, jpeg_input, staging,
		                            &file_offset, &buffered)) {
			return 0;
		}
		eof = file_offset == jpeg_input->length;
		if (buffered == 0U && eof) {
			if (empty_eof_feeds > jpeg_input->height + 8U) {
				printf("zz9k-jpeg: stream exceeded EOF drain limit\n");
				return 0;
			}
			empty_eof_feeds++;
		}

		do {
			ZZ9KImageSessionFeedDesc feed;
			ZZ9KImageSessionResult result;
			int status;

			if (!zz9k_image_build_session_feed_desc(
				    &feed, session, staging->handle, 0U,
				    buffered,
				    eof ? ZZ9K_IMAGE_SESSION_FEED_EOF : 0U)) {
				printf("zz9k-jpeg: could not build stream feed "
				       "descriptor\n");
				return 0;
			}

			memset(&result, 0, sizeof(result));
			status = zz9k_image_session_feed(ctx, &feed, &result);
			if (status != ZZ9K_STATUS_OK) {
				printf("zz9k-jpeg: stream feed failed: %s (%d)\n",
				       zz9k_status_name(status), status);
				return 0;
			}
			if (result.bytes_consumed > buffered) {
				printf("zz9k-jpeg: stream consumed beyond input chunk\n");
				return 0;
			}
			consumed = result.bytes_consumed;
			if (consumed != 0U) {
				buffered -= consumed;
				if (buffered != 0U) {
					if (!zz9k_shared_move(staging, 0U, consumed,
					                      buffered)) {
						printf("zz9k-jpeg: stream input compaction "
						       "failed\n");
						return 0;
					}
				}
			}

			if (result.state == ZZ9K_IMAGE_SESSION_STATE_TILE_READY) {
				if (result.tile_width == 0U ||
				    result.tile_height == 0U) {
					printf("zz9k-jpeg: empty tile returned\n");
					return 0;
				}
				tiles++;
				bytes_written += result.bytes_written;
			} else if (result.state ==
			           ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT ||
			           result.state ==
			           ZZ9K_IMAGE_SESSION_STATE_HEADER_READY) {
				if (zz9k_jpeg_stream_no_progress_is_fatal(
				        &result, buffered, staging->length, eof)) {
					printf("zz9k-jpeg: stream made no input "
					       "progress (state=%lu consumed=%lu "
					       "written=%lu buffered=%lu file=%lu/%lu "
					       "eof=%lu)\n",
					       (unsigned long)result.state,
					       (unsigned long)result.bytes_consumed,
					       (unsigned long)result.bytes_written,
					       (unsigned long)buffered,
					       (unsigned long)file_offset,
					       (unsigned long)jpeg_input->length,
					       (unsigned long)eof);
					return 0;
				}
				/* Read another chunk once the current one is consumed. */
			} else if (result.state ==
			           ZZ9K_IMAGE_SESSION_STATE_COMPLETE) {
				*final_result = result;
				break;
			} else {
				printf("zz9k-jpeg: stream decode returned state %lu\n",
				       (unsigned long)result.state);
				return 0;
			}

			if (buffered == 0U ||
			    (!zz9k_jpeg_stream_result_made_progress(&result) &&
			     !eof))
				break;
		} while (1);
	}

	if (out_tiles)
		*out_tiles = tiles;
	if (out_bytes_written)
		*out_bytes_written = bytes_written;
	return 1;
}

static int zz9k_jpeg_run_streaming(ZZ9KContext *ctx,
                                   const ZZ9KJpegInput *jpeg_input)
{
	ZZ9KSharedBuffer staging;
	ZZ9KSharedBuffer tile;
	ZZ9KSurface framebuffer;
	ZZ9KSurface decoded_surface;
	ZZ9KJpegFramebufferBackup backup;
	ZZ9KImageSessionBeginDesc begin;
	ZZ9KImageSessionResult result;
	FILE *file = 0;
	uint32_t tile_rows = 0U;
	uint32_t tile_bytes = 0U;
	uint32_t tiles = 0U;
	uint32_t tile_output_bytes = 0U;
	uint32_t output_width = 0U;
	uint32_t output_height = 0U;
	uint32_t decode_width = 0U;
	uint32_t decode_height = 0U;
	uint32_t begin_flags = 0U;
	uint32_t expected_output_bytes;
	uint32_t session = 0U;
	uint32_t output_format;
	uint32_t output_pitch;
	uint32_t decode_pitch;
	uint32_t expected_output_pitch;
	int staging_allocated = 0;
	int tile_allocated = 0;
	int decoded_surface_allocated = 0;
	int session_open = 0;
	int status;
	int rc = 1;
	uint32_t staging_bytes;

	memset(&staging, 0, sizeof(staging));
	memset(&tile, 0, sizeof(tile));
	memset(&framebuffer, 0, sizeof(framebuffer));
	memset(&decoded_surface, 0, sizeof(decoded_surface));
	memset(&result, 0, sizeof(result));
	zz9k_jpeg_framebuffer_backup_init(&backup);

	printf("JPEG input bytes:     %lu\n",
	       (unsigned long)jpeg_input->length);
	printf("JPEG dimensions:      %lu x %lu\n",
	       (unsigned long)jpeg_input->width,
	       (unsigned long)jpeg_input->height);
	print_diag(ctx, "before input alloc");

	staging_bytes = zz9k_jpeg_staging_bytes();
	status = zz9k_alloc_shared(ctx, staging_bytes, 16U,
	                          ZZ9K_ALLOC_HOST_WINDOW, &staging);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-jpeg: input alloc failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		print_diag(ctx, "after input alloc failure");
		goto cleanup;
	}
	staging_allocated = 1;

	output_format = zz9k_surface_native_rtg_format();
	if (jpeg_input->use_framebuffer) {
		status = zz9k_map_framebuffer_surface(ctx, &framebuffer);
		if (status != ZZ9K_STATUS_OK) {
			printf("zz9k-jpeg: framebuffer map failed: %s (%d)\n",
			       zz9k_status_name(status), status);
			print_diag(ctx, "after framebuffer map failure");
			goto cleanup;
		}
		if (!zz9k_surface_is_native_rtg_format(framebuffer.format)) {
			printf("zz9k-jpeg: framebuffer is not native RTG "
			       "(framebuffer %lu x %lu format=%lu)\n",
			       (unsigned long)framebuffer.width,
			       (unsigned long)framebuffer.height,
			       (unsigned long)framebuffer.format);
			goto cleanup;
		}
		if (!zz9k_jpeg_choose_decode_surface_size(
			    jpeg_input, framebuffer.width, framebuffer.height,
			    &decode_width, &decode_height)) {
			printf("zz9k-jpeg: could not choose ARM decode surface size\n");
			goto cleanup;
		}
		if (!zz9k_surface_min_pitch(decode_width, output_format,
		                            &decode_pitch)) {
			printf("zz9k-jpeg: invalid decode surface pitch\n");
			goto cleanup;
		}
		status = zz9k_alloc_surface_ex(ctx, decode_width, decode_height,
		                               output_format,
		                               ZZ9K_SURFACE_FLAG_ARM_LOCAL,
		                               decode_pitch,
		                               &decoded_surface);
		if (status != ZZ9K_STATUS_OK) {
			printf("zz9k-jpeg: decode surface alloc failed: %s (%d)\n",
			       zz9k_status_name(status), status);
			goto cleanup;
		}
		decoded_surface_allocated = 1;
		if (decode_width != jpeg_input->width ||
		    decode_height != jpeg_input->height) {
			begin_flags = ZZ9K_IMAGE_DECODE_FLAG_FIT |
				      ZZ9K_IMAGE_DECODE_FLAG_PRESERVE_ASPECT;
		}
		{
			ZZ9KRect output_rect;

			output_rect.x = 0U;
			output_rect.y = 0U;
			output_rect.w = decode_width;
			output_rect.h = decode_height;
			if (!zz9k_image_build_surface_session_begin_desc(
				    &begin, ZZ9K_IMAGE_CODEC_JPEG,
				    decoded_surface.handle, &output_rect,
				    output_format, begin_flags)) {
				printf("zz9k-jpeg: could not build stream begin "
				       "descriptor\n");
				goto cleanup;
			}
		}
	} else {
		tile_rows = zz9k_jpeg_choose_tile_rows(jpeg_input->width,
		                                       output_format,
		                                       jpeg_input->tile_rows);
		if (tile_rows == 0U ||
		    !zz9k_surface_min_pitch(jpeg_input->width, output_format,
		                            &output_pitch) ||
		    tile_rows > (0xffffffffU / output_pitch)) {
			printf("zz9k-jpeg: invalid tile geometry\n");
			goto cleanup;
		}
		tile_bytes = output_pitch * tile_rows;
		status = zz9k_alloc_shared(ctx, tile_bytes, 16U, 0, &tile);
		if (status != ZZ9K_STATUS_OK) {
			printf("zz9k-jpeg: tile alloc failed: %s (%d)\n",
			       zz9k_status_name(status), status);
			goto cleanup;
		}
		tile_allocated = 1;
		if (!zz9k_image_build_tile_session_begin_desc(
			    &begin, ZZ9K_IMAGE_CODEC_JPEG, tile.handle,
			    output_pitch, tile_rows,
			    output_format, 0U)) {
			printf("zz9k-jpeg: could not build stream begin "
			       "descriptor\n");
			goto cleanup;
		}
	}

	status = zz9k_image_session_begin(ctx, &begin, &result);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-jpeg: stream begin failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		goto cleanup;
	}
	session = result.session;
	session_open = 1;
	if (session == 0U ||
	    result.state != ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT) {
		printf("zz9k-jpeg: unexpected stream begin result\n");
		goto cleanup;
	}

	file = fopen(jpeg_input->path, "rb");
	if (!file) {
		printf("zz9k-jpeg: failed to open '%s'\n", jpeg_input->path);
		goto cleanup;
	}
	if (!zz9k_jpeg_feed_stream(ctx, file, jpeg_input, &staging, session,
	                           &tiles, &tile_output_bytes, &result)) {
		goto cleanup;
	}

	if (result.image_width != jpeg_input->width ||
	    result.image_height != jpeg_input->height ||
	    result.output_format != output_format) {
		printf("zz9k-jpeg: unexpected stream result %lu x %lu "
		       "format=%lu\n",
		       (unsigned long)result.image_width,
		       (unsigned long)result.image_height,
		       (unsigned long)result.output_format);
		goto cleanup;
	}
	output_width = jpeg_input->width;
	output_height = jpeg_input->height;
	if (jpeg_input->use_framebuffer) {
		if (result.tile_width == 0U || result.tile_height == 0U ||
		    result.tile_width > decoded_surface.width ||
		    result.tile_height > decoded_surface.height) {
			printf("zz9k-jpeg: unexpected decoded surface output "
			       "%lu x %lu for surface %lu x %lu\n",
			       (unsigned long)result.tile_width,
			       (unsigned long)result.tile_height,
			       (unsigned long)decoded_surface.width,
			       (unsigned long)decoded_surface.height);
			goto cleanup;
		}
		output_width = result.tile_width;
		output_height = result.tile_height;
	}
	if (!zz9k_surface_layout(output_width, output_height, output_format,
	                         &expected_output_pitch,
	                         &expected_output_bytes)) {
		printf("zz9k-jpeg: decoded output is too large\n");
		goto cleanup;
	}
	if (jpeg_input->use_framebuffer) {
		if (result.bytes_written != expected_output_bytes) {
			printf("zz9k-jpeg: unexpected framebuffer bytes=%lu\n",
			       (unsigned long)result.bytes_written);
			goto cleanup;
		}
		printf("zz9k-jpeg: file stream ok (%lu x %lu -> %lu x %lu, "
		       "%lu input bytes, %lu output bytes, ARM surface -> "
		       "framebuffer%s)\n",
		       (unsigned long)result.image_width,
		       (unsigned long)result.image_height,
		       (unsigned long)output_width,
		       (unsigned long)output_height,
		       (unsigned long)jpeg_input->length,
		       (unsigned long)result.bytes_written,
		       (decode_width != jpeg_input->width ||
		        decode_height != jpeg_input->height) ? ", fitted decode" : "");
		if (!zz9k_jpeg_show_decoded_surface(ctx, &framebuffer, jpeg_input,
		                                    &decoded_surface,
		                                    output_width, output_height,
		                                    &backup)) {
			goto cleanup;
		}
	} else {
		if (tile_output_bytes != expected_output_bytes) {
			printf("zz9k-jpeg: unexpected tile bytes=%lu expected=%lu\n",
			       (unsigned long)tile_output_bytes,
			       (unsigned long)expected_output_bytes);
			goto cleanup;
		}
		printf("zz9k-jpeg: file stream ok (%lu x %lu, %lu input "
		       "bytes, %lu output bytes, %lu tiles, %lu tile rows)\n",
		       (unsigned long)result.image_width,
		       (unsigned long)result.image_height,
		       (unsigned long)jpeg_input->length,
		       (unsigned long)tile_output_bytes,
		       (unsigned long)tiles,
		       (unsigned long)tile_rows);
	}

	rc = 0;

cleanup:
	if (backup.active) {
		status = zz9k_jpeg_framebuffer_restore(ctx, &framebuffer, &backup);
		if (status != ZZ9K_STATUS_OK) {
			printf("zz9k-jpeg: framebuffer restore failed during cleanup: "
			       "%s (%d)\n",
			       zz9k_status_name(status), status);
			rc = 1;
		}
	}
	zz9k_jpeg_framebuffer_backup_free(ctx, &backup);
	if (file)
		fclose(file);
	if (session_open) {
		status = zz9k_image_session_close(ctx, session, 0U);
		if (status != ZZ9K_STATUS_OK) {
			printf("zz9k-jpeg: stream close failed: %s (%d)\n",
			       zz9k_status_name(status), status);
			rc = 1;
		}
	}
	if (tile_allocated)
		zz9k_free_shared(ctx, tile.handle);
	if (decoded_surface_allocated)
		zz9k_free_surface(ctx, decoded_surface.handle);
	if (staging_allocated)
		zz9k_free_shared(ctx, staging.handle);
	return rc;
}

int zz9k_jpeg_decode_viewer_image(ZZ9KContext *ctx,
                                  const ZZ9KSurface *framebuffer,
                                  const char *path,
                                  ZZ9KPictureViewerImage *image)
{
	ZZ9KJpegInput input;
	ZZ9KSharedBuffer staging;
	ZZ9KSurface decoded_surface;
	ZZ9KImageSessionBeginDesc begin;
	ZZ9KImageSessionResult result;
	ZZ9KRect output_rect;
	FILE *file = 0;
	uint32_t tiles = 0U;
	uint32_t tile_output_bytes = 0U;
	uint32_t output_width = 0U;
	uint32_t output_height = 0U;
	uint32_t decode_width = 0U;
	uint32_t decode_height = 0U;
	uint32_t begin_flags = 0U;
	uint32_t output_format = 0U;
	uint32_t decode_pitch = 0U;
	uint32_t expected_output_pitch = 0U;
	uint32_t expected_output_bytes = 0U;
	uint32_t session = 0U;
	int staging_allocated = 0;
	int decoded_surface_allocated = 0;
	int session_open = 0;
	int success = 0;
	int status;

	if (!image) {
		printf("zz9k-view: missing JPEG viewer image output\n");
		return 0;
	}
	zz9k_picture_viewer_image_init(image);

	if (!ctx || !framebuffer || !path || path[0] == '\0' ||
	    framebuffer->width == 0U || framebuffer->height == 0U) {
		printf("zz9k-view: invalid JPEG viewer decode request\n");
		return 0;
	}

	memset(&input, 0, sizeof(input));
	memset(&staging, 0, sizeof(staging));
	memset(&decoded_surface, 0, sizeof(decoded_surface));
	memset(&begin, 0, sizeof(begin));
	memset(&result, 0, sizeof(result));

	if (!zz9k_jpeg_load_file(path, &input)) {
		printf("zz9k-view: failed to load JPEG metadata for '%s'\n", path);
		goto cleanup;
	}
	input.use_framebuffer = 1;
	input.resize_framebuffer = 1;
	input.view_framebuffer = 1;
	input.restore_framebuffer = 1;
	input.fit_framebuffer = 1;
	input.prefer_surface_backup = 1;

	if (!zz9k_jpeg_require_stream_service(ctx, 1, 1)) {
		printf("zz9k-view: JPEG stream surface/scaling service is "
		       "not available\n");
		goto cleanup;
	}

	status = zz9k_alloc_shared(ctx, ZZ9K_JPEG_STAGING_BYTES, 16U,
	                          ZZ9K_ALLOC_HOST_WINDOW, &staging);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-view: JPEG staging alloc failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		goto cleanup;
	}
	staging_allocated = 1;

	if (!zz9k_jpeg_choose_decode_surface_size(
		    &input, framebuffer->width, framebuffer->height,
		    &decode_width, &decode_height)) {
		printf("zz9k-view: could not choose JPEG decode surface size\n");
		goto cleanup;
	}

	output_format = zz9k_picture_viewer_decode_format();
	if (!zz9k_surface_min_pitch(decode_width, output_format,
	                            &decode_pitch)) {
		printf("zz9k-view: invalid JPEG decode surface pitch\n");
		goto cleanup;
	}
	status = zz9k_alloc_surface_ex(ctx, decode_width, decode_height,
	                               output_format,
	                               ZZ9K_SURFACE_FLAG_ARM_LOCAL,
	                               decode_pitch, &decoded_surface);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-view: JPEG decode surface alloc failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		goto cleanup;
	}
	decoded_surface_allocated = 1;

	if (decode_width != input.width || decode_height != input.height) {
		begin_flags = ZZ9K_IMAGE_DECODE_FLAG_FIT |
			      ZZ9K_IMAGE_DECODE_FLAG_PRESERVE_ASPECT;
	}
	output_rect.x = 0U;
	output_rect.y = 0U;
	output_rect.w = decode_width;
	output_rect.h = decode_height;
	if (!zz9k_image_build_surface_session_begin_desc(
		    &begin, ZZ9K_IMAGE_CODEC_JPEG, decoded_surface.handle,
		    &output_rect, output_format, begin_flags)) {
		printf("zz9k-view: could not build JPEG stream begin descriptor\n");
		goto cleanup;
	}

	status = zz9k_image_session_begin(ctx, &begin, &result);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-view: JPEG stream begin failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		goto cleanup;
	}
	session = result.session;
	session_open = 1;
	if (session == 0U ||
	    result.state != ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT) {
		printf("zz9k-view: unexpected JPEG stream begin result\n");
		goto cleanup;
	}

	file = fopen(path, "rb");
	if (!file) {
		printf("zz9k-view: failed to open '%s'\n", path);
		goto cleanup;
	}
	if (!zz9k_jpeg_feed_stream(ctx, file, &input, &staging, session,
	                           &tiles, &tile_output_bytes, &result)) {
		printf("zz9k-view: JPEG stream feed failed for '%s'\n", path);
		goto cleanup;
	}

	if (result.image_width != input.width ||
	    result.image_height != input.height ||
	    result.output_format != output_format) {
		printf("zz9k-view: unexpected JPEG stream result %lu x %lu "
		       "format=%lu\n",
		       (unsigned long)result.image_width,
		       (unsigned long)result.image_height,
		       (unsigned long)result.output_format);
		goto cleanup;
	}
	if (result.tile_width == 0U || result.tile_height == 0U ||
	    result.tile_width > decoded_surface.width ||
	    result.tile_height > decoded_surface.height) {
		printf("zz9k-view: unexpected JPEG surface output %lu x %lu "
		       "for surface %lu x %lu\n",
		       (unsigned long)result.tile_width,
		       (unsigned long)result.tile_height,
		       (unsigned long)decoded_surface.width,
		       (unsigned long)decoded_surface.height);
		goto cleanup;
	}
	output_width = result.tile_width;
	output_height = result.tile_height;
	if (!zz9k_surface_layout(output_width, output_height, output_format,
	                         &expected_output_pitch,
	                         &expected_output_bytes)) {
		printf("zz9k-view: JPEG decoded output is too large\n");
		goto cleanup;
	}
	if (result.bytes_written != expected_output_bytes) {
		printf("zz9k-view: unexpected JPEG output bytes=%lu expected=%lu\n",
		       (unsigned long)result.bytes_written,
		       (unsigned long)expected_output_bytes);
		goto cleanup;
	}

	success = 1;

cleanup:
	if (file)
		fclose(file);
	if (session_open) {
		status = zz9k_image_session_close(ctx, session, 0U);
		if (status != ZZ9K_STATUS_OK) {
			printf("zz9k-view: JPEG stream close failed: %s (%d)\n",
			       zz9k_status_name(status), status);
			success = 0;
		}
	}
	if (success) {
		image->codec = ZZ9K_PICTURE_VIEWER_CODEC_JPEG;
		image->path = path;
		image->width = output_width;
		image->height = output_height;
		image->surface = decoded_surface;
		image->surface_allocated = 1;
		decoded_surface_allocated = 0;
	}
	if (decoded_surface_allocated)
		zz9k_free_surface(ctx, decoded_surface.handle);
	if (staging_allocated)
		zz9k_free_shared(ctx, staging.handle);
	if (!success)
		zz9k_picture_viewer_image_init(image);
	(void)tiles;
	(void)tile_output_bytes;
	(void)expected_output_pitch;
	return success;
}

#ifndef ZZ9K_JPEG_NO_MAIN
int main(int argc, char **argv)
{
	ZZ9KContext *ctx = 0;
	ZZ9KCaps caps;
	ZZ9KJpegInput jpeg_input;
	int status;
	int rc = 1;

	if (!zz9k_jpeg_parse_args(argc, argv, &jpeg_input))
		return 1;

	printf("zz9k-jpeg: opening SDK mailbox\n");
	status = zz9k_open(&ctx);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-jpeg: open failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		return 10;
	}

	status = zz9k_query_caps(ctx, &caps);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-jpeg: query caps failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		goto cleanup;
	}
	if (!require_cap(caps.capability_bits, ZZ9K_CAP_SHARED_ALLOC) ||
	    !require_cap(caps.capability_bits, ZZ9K_CAP_IMAGE_DECODE)) {
		goto cleanup;
	}
	if (jpeg_input.built_in &&
	    !jpeg_input.use_framebuffer &&
	    !require_cap(caps.capability_bits, ZZ9K_CAP_SURFACES)) {
		goto cleanup;
	}
	if (jpeg_input.use_framebuffer &&
	    !require_cap(caps.capability_bits, ZZ9K_CAP_FRAMEBUFFER_SURFACE)) {
		goto cleanup;
	}
	if (jpeg_input.use_framebuffer &&
	    (!require_cap(caps.capability_bits, ZZ9K_CAP_SURFACES) ||
	     !require_cap(caps.capability_bits, ZZ9K_CAP_IMAGE_SCALE) ||
	     !require_cap(caps.capability_bits, ZZ9K_CAP_SURFACE_OPS))) {
		goto cleanup;
	}
	if (jpeg_input.use_framebuffer && jpeg_input.restore_framebuffer &&
	    ((caps.capability_bits & ZZ9K_CAP_SURFACES) == 0U ||
	     (caps.capability_bits & ZZ9K_CAP_SURFACE_OPS) == 0U)) {
		jpeg_input.prefer_surface_backup = 0;
	}
	if (jpeg_input.use_framebuffer &&
	    (!require_cap(caps.capability_bits, ZZ9K_CAP_SERVICE_DISCOVERY) ||
	     !zz9k_jpeg_require_clipped_scale_service(ctx))) {
		goto cleanup;
	}
	if (!jpeg_input.built_in &&
	    (!require_cap(caps.capability_bits, ZZ9K_CAP_SERVICE_DISCOVERY) ||
	     !zz9k_jpeg_require_stream_service(ctx,
	                                       jpeg_input.use_framebuffer,
	                                       jpeg_input.fit_framebuffer))) {
		goto cleanup;
	}

	if (jpeg_input.built_in)
		rc = zz9k_jpeg_run_builtin(ctx, &jpeg_input);
	else
		rc = zz9k_jpeg_run_streaming(ctx, &jpeg_input);

cleanup:
	zz9k_close(ctx);
	return rc;
}
#endif
