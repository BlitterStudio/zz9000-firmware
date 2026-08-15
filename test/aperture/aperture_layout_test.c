/*
 * Host tests for the aperture-relative Zorro II layout policy.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include "sdk_aperture_layout.h"

static int checks;
static int failures;

#define CHECK(expr) do { \
	checks++; \
	if (!(expr)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
	} \
} while (0)

static uint32_t end_of(const struct sdk_aperture_region *r)
{
	return r->base + r->size;
}

static void check_common(const struct sdk_aperture_layout *layout,
			 uint32_t aperture)
{
	CHECK(layout->aperture_size == aperture);
	CHECK(SDK_APERTURE_PROFILE_GENERATION(layout->profile) == 1U);
	CHECK(sdk_aperture_layout_validate(layout));
	CHECK(layout->framebuffer.base == 0x00010000U);
	CHECK(end_of(&layout->template_scratch) == layout->host_window.base);
	CHECK(end_of(&layout->host_window) == layout->audio.base);
	CHECK(end_of(&layout->audio) == aperture);
	CHECK(layout->template_scratch.size == 0x00010000U);
	CHECK(layout->audio.size == 0x00010000U);
}

static void test_2m(void)
{
	struct sdk_aperture_layout layout;
	CHECK(sdk_aperture_layout_compute(0x00200000U, &layout));
	check_common(&layout, 0x00200000U);
	CHECK(layout.framebuffer.size == 0x001c0000U);
	CHECK(end_of(&layout.framebuffer) == 0x001d0000U);
	CHECK(layout.pip.size == 0U);
	CHECK(layout.template_scratch.base == 0x001d0000U);
	CHECK(layout.host_window.base == 0x001e0000U);
	CHECK(layout.host_window.size == 0x00010000U);
	CHECK(layout.audio.base == 0x001f0000U);
	CHECK(sdk_aperture_layout_info_word(&layout) == 0x5a010502U);
}

static void test_4m(void)
{
	struct sdk_aperture_layout layout;
	CHECK(sdk_aperture_layout_compute(0x00400000U, &layout));
	check_common(&layout, 0x00400000U);
	CHECK(layout.framebuffer.size == 0x00388000U);
	CHECK(layout.pip.base == 0x00398000U);
	CHECK(layout.pip.size == 0x00038000U);
	CHECK(end_of(&layout.pip) == layout.template_scratch.base);
	CHECK(layout.template_scratch.base == 0x003d0000U);
	CHECK(layout.host_window.base == 0x003e0000U);
	CHECK(layout.audio.base == 0x003f0000U);
	CHECK(sdk_aperture_layout_info_word(&layout) == 0x5a010704U);
}

static void test_8m(void)
{
	struct sdk_aperture_layout layout;
	CHECK(sdk_aperture_layout_compute(0x00800000U, &layout));
	check_common(&layout, 0x00800000U);
	CHECK(layout.framebuffer.size == 0x00770000U);
	CHECK(layout.pip.base == 0x00780000U);
	CHECK(layout.pip.size == 0x00040000U);
	CHECK(layout.template_scratch.base == 0x007c0000U);
	CHECK(layout.host_window.base == 0x007d0000U);
	CHECK(layout.host_window.size == 0x00020000U);
	CHECK(layout.audio.base == 0x007f0000U);
	CHECK(sdk_aperture_layout_info_word(&layout) == 0x5a010708U);
}

static void test_invalid_sizes_and_layouts(void)
{
	struct sdk_aperture_layout layout;
	const uint32_t invalid[] = {
		0U, 0x00100000U, 0x00300000U, 0x01000000U, UINT32_MAX
	};
	unsigned int i;

	for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
		CHECK(!sdk_aperture_layout_compute(invalid[i], &layout));
	CHECK(!sdk_aperture_layout_compute(0x00400000U, 0));

	CHECK(sdk_aperture_layout_compute(0x00400000U, &layout));
	layout.host_window.base = UINT32_MAX - 7U;
	layout.host_window.size = 16U;
	CHECK(!sdk_aperture_layout_validate(&layout));

	CHECK(sdk_aperture_layout_compute(0x00400000U, &layout));
	layout.pip.size += 1U;
	CHECK(!sdk_aperture_layout_validate(&layout));

	CHECK(sdk_aperture_layout_compute(0x00400000U, &layout));
	layout.framebuffer.size -= 0x1000U;
	CHECK(!sdk_aperture_layout_validate(&layout));

	CHECK(sdk_aperture_layout_compute(0x00400000U, &layout));
	layout.template_scratch.base = layout.pip.base;
	CHECK(!sdk_aperture_layout_validate(&layout));

	CHECK(sdk_aperture_layout_compute(0x00400000U, &layout));
	layout.audio.base = layout.aperture_size;
	CHECK(!sdk_aperture_layout_validate(&layout));
}

static void test_runtime_ack_gate(void)
{
	const struct sdk_aperture_layout *layout;

	sdk_aperture_runtime_init(0x00200000U, 0);
	layout = sdk_aperture_runtime_layout();
	CHECK(sdk_aperture_runtime_reported_size() == 0x00200000U);
	CHECK(sdk_aperture_runtime_diag_state() ==
	      SDK_APERTURE_DIAG_STATE_UNACKNOWLEDGED);
	CHECK(sdk_aperture_host_window_address() == 0U);
	CHECK(sdk_aperture_framebuffer_size() == 0x001c0000U);
	CHECK(sdk_aperture_gfxdata_address(0x033f0000U) == 0U);
	CHECK(sdk_aperture_runtime_ack());
	CHECK(sdk_aperture_runtime_diag_state() ==
	      SDK_APERTURE_DIAG_STATE_ACTIVE);
	CHECK(sdk_aperture_host_window_address() == 0x003d0000U);
	CHECK(sdk_aperture_host_window_size() == 0x00010000U);
	CHECK(sdk_aperture_framebuffer_size() == layout->framebuffer.size);
	CHECK(sdk_aperture_gfxdata_address(0x033f0000U) == 0x003c0000U);

	sdk_aperture_runtime_init(0x00400000U, 0);
	CHECK(sdk_aperture_runtime_ack());
	CHECK(sdk_aperture_host_window_address() == 0x005d0000U);
	CHECK(sdk_aperture_gfxdata_address(0x033f0000U) == 0x005c0000U);

	sdk_aperture_runtime_init(0x00800000U, 0);
	CHECK(sdk_aperture_runtime_ack());
	CHECK(sdk_aperture_host_window_address() == 0x009c0000U);
	CHECK(sdk_aperture_gfxdata_address(0x033f0000U) == 0x009b0000U);

	sdk_aperture_runtime_init(0U, 1);
	CHECK(!sdk_aperture_runtime_ack());
	CHECK(sdk_aperture_runtime_is_zorro3());
	CHECK(!sdk_aperture_runtime_is_legacy());
	CHECK(sdk_aperture_runtime_diag_state() ==
	      SDK_APERTURE_DIAG_STATE_LEGACY);
	CHECK(sdk_aperture_gfxdata_address(0x033f0000U) == 0x033f0000U);

	sdk_aperture_runtime_init(0U, 0);
	CHECK(!sdk_aperture_runtime_is_zorro3());
	CHECK(sdk_aperture_runtime_is_legacy());
	CHECK(sdk_aperture_runtime_diag_state() ==
	      SDK_APERTURE_DIAG_STATE_LEGACY);

	sdk_aperture_runtime_init(0x00300000U, 0);
	CHECK(!sdk_aperture_runtime_is_zorro3());
	CHECK(!sdk_aperture_runtime_is_legacy());
	CHECK(sdk_aperture_runtime_flags() == 0U);
	CHECK(sdk_aperture_runtime_reported_size() == 0x00300000U);
	CHECK(sdk_aperture_runtime_diag_state() ==
	      SDK_APERTURE_DIAG_STATE_INVALID);
	CHECK(sdk_aperture_host_window_address() == 0U);
	CHECK(!sdk_aperture_runtime_ack());
}

int main(void)
{
	test_2m();
	test_4m();
	test_8m();
	test_invalid_sizes_and_layouts();
	test_runtime_ack_gate();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
