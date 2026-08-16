/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stddef.h>
#include <string.h>
#include "sdk_aperture_layout.h"

#define KIB(x) ((uint32_t)(x) * 1024U)

static struct sdk_aperture_layout runtime_layout;
static uint32_t runtime_flags;
static uint32_t runtime_reported_size;
static int runtime_is_zorro3;
static int runtime_is_legacy;

static int region_end(const struct sdk_aperture_region *region,
		      uint32_t *end)
{
	if (!region || !end || region->base > UINT32_MAX - region->size)
		return 0;
	*end = region->base + region->size;
	return 1;
}

static int region_inside(const struct sdk_aperture_region *region,
			 uint32_t aperture_size)
{
	uint32_t end;

	return region_end(region, &end) && end <= aperture_size;
}

static int regions_ordered(const struct sdk_aperture_region *left,
			   const struct sdk_aperture_region *right)
{
	uint32_t left_end;

	return region_end(left, &left_end) && left_end <= right->base;
}

static int build_canonical_layout(uint32_t aperture_size,
				  struct sdk_aperture_layout *layout)
{
	uint32_t top;
	uint32_t pip_size;
	uint32_t host_size;
	uint32_t flags = SDK_APERTURE_FLAG_VALID |
		SDK_APERTURE_FLAG_HOST_WINDOW;

	if (!layout)
		return 0;
	memset(layout, 0, sizeof(*layout));

	switch (aperture_size) {
	case SDK_APERTURE_BYTES_2M:
		pip_size = 0U;
		host_size = KIB(64);
		break;
	case SDK_APERTURE_BYTES_4M:
		pip_size = KIB(224);
		host_size = KIB(64);
		flags |= SDK_APERTURE_FLAG_PIP_POOL;
		break;
	case SDK_APERTURE_BYTES_8M:
		pip_size = KIB(256);
		host_size = KIB(128);
		flags |= SDK_APERTURE_FLAG_PIP_POOL;
		break;
	default:
		return 0;
	}

	layout->profile = SDK_APERTURE_PROFILE(
		SDK_APERTURE_LAYOUT_GENERATION, flags);
	layout->aperture_size = aperture_size;
	layout->audio.size = KIB(64);
	layout->audio.base = aperture_size - layout->audio.size;
	layout->host_window.size = host_size;
	layout->host_window.base = layout->audio.base - host_size;
	layout->template_scratch.size = KIB(64);
	layout->template_scratch.base =
		layout->host_window.base - layout->template_scratch.size;
	layout->pip.size = pip_size;
	layout->pip.base = layout->template_scratch.base - pip_size;

	/* The 2 MB profile deliberately preserves the old driver boundary:
	 * framebuffer size was BoardSize - 0x40000 from board offset 0x10000,
	 * ending at BoardSize - 0x30000.  Its PIP-sized gap is empty, leaving
	 * the old visible VRAM unchanged while naming the spare 64 KB as the
	 * aperture-relative host heap. */
	top = pip_size != 0U ? layout->pip.base :
		(aperture_size - 0x00030000U);
	layout->framebuffer.base = SDK_APERTURE_FRAMEBUFFER_BASE;
	layout->framebuffer.size = top - layout->framebuffer.base;

	return 1;
}

int sdk_aperture_layout_compute(uint32_t aperture_size,
				struct sdk_aperture_layout *layout)
{
	if (!build_canonical_layout(aperture_size, layout))
		return 0;
	if (!sdk_aperture_layout_validate(layout)) {
		memset(layout, 0, sizeof(*layout));
		return 0;
	}
	return 1;
}

int sdk_aperture_layout_validate(const struct sdk_aperture_layout *layout)
{
	struct sdk_aperture_layout expected;
	uint32_t flags;
	uint32_t generation;
	uint32_t expected_flags;

	if (!layout ||
	    !build_canonical_layout(layout->aperture_size, &expected))
		return 0;
	generation = SDK_APERTURE_PROFILE_GENERATION(layout->profile);
	flags = SDK_APERTURE_PROFILE_FLAGS(layout->profile);
	expected_flags = SDK_APERTURE_PROFILE_FLAGS(expected.profile);
	if (generation != SDK_APERTURE_LAYOUT_GENERATION ||
	    (flags != expected_flags &&
	     flags != (expected_flags | SDK_APERTURE_FLAG_ACKED)))
		return 0;
	expected.profile = layout->profile;
	if (memcmp(layout, &expected, sizeof(expected)) != 0)
		return 0;
	if (!region_inside(&layout->framebuffer, layout->aperture_size) ||
	    !region_inside(&layout->pip, layout->aperture_size) ||
	    !region_inside(&layout->template_scratch, layout->aperture_size) ||
	    !region_inside(&layout->host_window, layout->aperture_size) ||
	    !region_inside(&layout->audio, layout->aperture_size))
		return 0;
	if (layout->pip.size != 0U) {
		if (!regions_ordered(&layout->framebuffer, &layout->pip) ||
		    !regions_ordered(&layout->pip, &layout->template_scratch))
			return 0;
	} else if (!regions_ordered(&layout->framebuffer,
				    &layout->template_scratch)) {
		return 0;
	}
	return regions_ordered(&layout->template_scratch,
			       &layout->host_window) &&
	       regions_ordered(&layout->host_window, &layout->audio);
}

uint32_t sdk_aperture_layout_info_word(const struct sdk_aperture_layout *layout)
{
	uint32_t flags = SDK_APERTURE_INFO_FLAG_VALID |
		SDK_APERTURE_INFO_FLAG_HOST_WINDOW;

	if (!sdk_aperture_layout_validate(layout))
		return 0U;
	if (layout->pip.size != 0U)
		flags |= SDK_APERTURE_INFO_FLAG_PIP_POOL;
	return SDK_APERTURE_INFO_WORD(layout->aperture_size >> 20, flags);
}

void sdk_aperture_runtime_init(uint32_t fpga_aperture_size, int is_zorro3)
{
	memset(&runtime_layout, 0, sizeof(runtime_layout));
	runtime_flags = 0U;
	runtime_reported_size = fpga_aperture_size;
	runtime_is_zorro3 = is_zorro3 != 0;
	runtime_is_legacy = !runtime_is_zorro3 && fpga_aperture_size == 0U;
	if (!runtime_is_zorro3 &&
	    sdk_aperture_layout_compute(fpga_aperture_size, &runtime_layout))
		runtime_flags = SDK_APERTURE_PROFILE_FLAGS(runtime_layout.profile);
}

int sdk_aperture_runtime_ack(void)
{
	if (runtime_is_zorro3 ||
	    (runtime_flags & SDK_APERTURE_FLAG_VALID) == 0U ||
	    !sdk_aperture_layout_validate(&runtime_layout))
		return 0;
	runtime_flags |= SDK_APERTURE_FLAG_ACKED;
	runtime_layout.profile = SDK_APERTURE_PROFILE(
		SDK_APERTURE_LAYOUT_GENERATION, runtime_flags);
	return 1;
}

const struct sdk_aperture_layout *sdk_aperture_runtime_layout(void)
{
	return &runtime_layout;
}

uint32_t sdk_aperture_runtime_flags(void)
{
	return runtime_flags;
}

int sdk_aperture_runtime_is_zorro3(void)
{
	return runtime_is_zorro3;
}

int sdk_aperture_runtime_is_legacy(void)
{
	return runtime_is_legacy;
}

uint32_t sdk_aperture_runtime_reported_size(void)
{
	return runtime_reported_size;
}

uint32_t sdk_aperture_runtime_diag_state(void)
{
	if (runtime_is_zorro3 || runtime_is_legacy)
		return SDK_APERTURE_DIAG_STATE_LEGACY;
	if ((runtime_flags & SDK_APERTURE_FLAG_ACKED) != 0U)
		return SDK_APERTURE_DIAG_STATE_ACTIVE;
	if ((runtime_flags & SDK_APERTURE_FLAG_VALID) != 0U)
		return SDK_APERTURE_DIAG_STATE_UNACKNOWLEDGED;
	return SDK_APERTURE_DIAG_STATE_INVALID;
}

uint32_t sdk_aperture_host_window_address(void)
{
	if ((runtime_flags & (SDK_APERTURE_FLAG_VALID |
	                      SDK_APERTURE_FLAG_ACKED |
	                      SDK_APERTURE_FLAG_HOST_WINDOW)) !=
	    (SDK_APERTURE_FLAG_VALID | SDK_APERTURE_FLAG_ACKED |
	     SDK_APERTURE_FLAG_HOST_WINDOW))
		return 0U;
	return runtime_layout.host_window.base +
		SDK_APERTURE_ARM_ADDRESS_ADJUSTMENT;
}

uint32_t sdk_aperture_host_window_size(void)
{
	return sdk_aperture_host_window_address() != 0U ?
		runtime_layout.host_window.size : 0U;
}

uint32_t sdk_aperture_framebuffer_size(void)
{
	if ((runtime_flags & SDK_APERTURE_FLAG_VALID) == 0U)
		return 0U;
	if ((runtime_flags & SDK_APERTURE_FLAG_ACKED) != 0U)
		return runtime_layout.framebuffer.size;
	/* Until a matching driver acknowledges the new contract, retain the
	 * legacy driver's BoardSize - 0x40000 MemorySize calculation. */
	return runtime_layout.aperture_size - 0x00040000U;
}

uint32_t sdk_aperture_gfxdata_address(uint32_t z3_scratch_address)
{
	if ((runtime_flags & (SDK_APERTURE_FLAG_VALID |
	                      SDK_APERTURE_FLAG_ACKED)) ==
	    (SDK_APERTURE_FLAG_VALID | SDK_APERTURE_FLAG_ACKED))
		return runtime_layout.template_scratch.base +
			SDK_APERTURE_ARM_ADDRESS_ADJUSTMENT;
	return runtime_is_zorro3 ? z3_scratch_address : 0U;
}
