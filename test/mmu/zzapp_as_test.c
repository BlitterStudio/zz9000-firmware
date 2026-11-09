/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mmu_page.h"
#include "xil_cache.h"
#include "zzapp_as.h"
#define DDR_WB 0x15de6U
#define DEVICE 0x0c06U
#define FPGA 0x0c02U
#define OCM 0x04c0eU
#define SECTION_AP_MASK (3U << 10)
#define SECTION_NG (1U << 17)
#define PAGE_AP_MASK (3U << 4)
#define PAGE_XN 1U
#define PAGE_NG (1U << 11)
#define PAGE_TYPE 2U
#define PAGE_USER_RW (3U << 4)
#define PAGE_USER_RO (2U << 4)
struct xil_cache_mock_state g_xil_cache_mock;


static uint32_t bsp[4096];
static int checks;
static int failures;

#define CHECK(expr) do { \
	checks++; \
	if (!(expr)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
	} \
} while (0)

static void setup(void)
{
	uint32_t i;

	for (i = 0U; i < 0x400U; i++)
		bsp[i] = (i << 20) | DDR_WB;
	bsp[0x400U] = 0x40000000U | FPGA;
	bsp[0xe00U] = 0xe0000000U | DEVICE;
	bsp[0xfffU] = 0xfff00000U | OCM;
	zzapp_as_init(bsp);
}

static void test_template_is_privileged_global(void)
{
	const uint32_t *template = zzapp_as_template();

	CHECK((template[3] & SECTION_AP_MASK) == (1U << 10));
	CHECK((template[3] & SECTION_NG) == 0U);
	CHECK((template[3] & ~(SECTION_AP_MASK | SECTION_NG)) ==
	      (bsp[3] & ~(SECTION_AP_MASK | SECTION_NG)));
	CHECK((template[0x400U] & SECTION_AP_MASK) == (1U << 10));
	CHECK((template[0x400U] & ~(SECTION_AP_MASK | SECTION_NG)) ==
	      (bsp[0x400U] & ~(SECTION_AP_MASK | SECTION_NG)));
	CHECK((template[0xe00U] & SECTION_AP_MASK) == (1U << 10));
	CHECK((template[0xe00U] & ~(SECTION_AP_MASK | SECTION_NG)) ==
	      (bsp[0xe00U] & ~(SECTION_AP_MASK | SECTION_NG)));
	CHECK((template[0xfffU] & SECTION_AP_MASK) == (1U << 10));
	CHECK((template[0xfffU] & ~(SECTION_AP_MASK | SECTION_NG)) ==
	      (bsp[0xfffU] & ~(SECTION_AP_MASK | SECTION_NG)));
}

static void test_frame_sections_only_fill_window(void)
{
	zzapp_as_t as;
	const uint32_t frames[] = { 0x12000000U, 0x1a000000U, 0x24000000U };
	const uint32_t *l1;

	CHECK(zzapp_as_create(&as, 0U, 1U) == 0);
	CHECK(zzapp_as_map_frames(&as, ZZAPP_AS_WINDOW_BASE, frames, 3U) == 0);
	l1 = zzapp_as_l1(&as);
	CHECK((l1[0xc00U] & 0xfff00000U) == frames[0]);
	CHECK((l1[0xc01U] & 0xfff00000U) == frames[1]);
	CHECK((l1[0xc02U] & 0xfff00000U) == frames[2]);
	CHECK((l1[0xc00U] & SECTION_NG) != 0U);
	CHECK((l1[0xc03U] & 3U) == 0U);
	zzapp_as_destroy(&as);
}
static void test_window_is_one_user_section(void)
{
	zzapp_as_t as;
	const uint32_t *l1;

	CHECK(zzapp_as_create(&as, 0U, 4U) == 0);
	CHECK(zzapp_as_map_window(&as, 0x03400000U) == 0);
	l1 = zzapp_as_l1(&as);
	CHECK((l1[0x034U] & SECTION_AP_MASK) == (3U << 10));
	CHECK((l1[0x034U] & SECTION_NG) != 0U);
	CHECK((l1[0x034U] & 3U) == 2U);
	CHECK(zzapp_as_l2_for(&as, 0x03400000U) == NULL);
	zzapp_as_destroy(&as);
}


static void test_grant_and_ungrant_preserve_neighbours(void)
{
	zzapp_as_t as;
	const uint32_t *l1;
	const uint32_t *l2;

	CHECK(zzapp_as_create(&as, 1U, 1U) == 0);
	CHECK(zzapp_as_grant_user(&as, 0x03405000U, 5U * 4096U) == 0);
	l1 = zzapp_as_l1(&as);
	CHECK((l1[0x034U] & 3U) == 1U);
	l2 = zzapp_as_l2_for(&as, 0x03400000U);
	CHECK(l2 != NULL);
	CHECK((l2[4] & PAGE_AP_MASK) != PAGE_USER_RW);
	CHECK((l2[5] & PAGE_AP_MASK) == PAGE_USER_RW);
	CHECK((l2[9] & PAGE_AP_MASK) == PAGE_USER_RW);
	CHECK((l2[10] & PAGE_AP_MASK) != PAGE_USER_RW);
	CHECK(zzapp_as_ungrant_user(&as, 0x03406000U, 2U * 4096U) == 0);
	CHECK((l2[5] & PAGE_AP_MASK) == PAGE_USER_RW);
	CHECK((l2[6] & PAGE_AP_MASK) != PAGE_USER_RW);
	CHECK((l2[7] & PAGE_AP_MASK) != PAGE_USER_RW);
	CHECK((l2[8] & PAGE_AP_MASK) == PAGE_USER_RW);
	CHECK((l2[9] & PAGE_AP_MASK) == PAGE_USER_RW);
	zzapp_as_destroy(&as);
}

static void test_image_permissions_and_window_bounds(void)
{
	zzapp_as_t as;
	const uint32_t *l2;

	CHECK(zzapp_as_create(&as, 2U, 1U) == 0);
	CHECK(zzapp_as_map_image(&as, ZZAPP_AS_WINDOW_BASE, 0x10000000U,
	                         4096U, ZZAPP_AS_IMAGE_RX) == 0);
	CHECK(zzapp_as_map_image(&as, ZZAPP_AS_WINDOW_BASE + 4096U,
	                         0x10001000U, 4096U, ZZAPP_AS_IMAGE_RO) == 0);
	CHECK(zzapp_as_map_image(&as, ZZAPP_AS_WINDOW_BASE + 8192U,
	                         0x10002000U, 4096U, ZZAPP_AS_IMAGE_RW) == 0);
	l2 = zzapp_as_l2_for(&as, ZZAPP_AS_WINDOW_BASE);
	CHECK((l2[0] & PAGE_AP_MASK) == PAGE_USER_RO);
	CHECK((l2[0] & PAGE_XN) == 0U);
	CHECK((l2[1] & PAGE_AP_MASK) == PAGE_USER_RO);
	CHECK((l2[1] & PAGE_XN) != 0U);
	CHECK((l2[2] & PAGE_AP_MASK) == PAGE_USER_RW);
	CHECK((l2[2] & PAGE_XN) != 0U);
	CHECK((l2[0] & PAGE_NG) != 0U);
	CHECK(zzapp_as_map_image(&as, 0xdffff000U, 0x10003000U,
	                         8192U, ZZAPP_AS_IMAGE_RW) != 0);
	zzapp_as_destroy(&as);
}

static void test_noncacheable_change_propagates(void)
{
	zzapp_as_t first;
	zzapp_as_t second;
	const uint32_t *template;

	CHECK(zzapp_as_create(&first, 0U, 2U) == 0);
	CHECK(zzapp_as_create(&second, 3U, 1U) == 0);
	mmu_page_test_l1[5] = bsp[5];
	CHECK(mmu_page_set_noncacheable(0x005df000U));
	template = zzapp_as_template();
	CHECK(template[5] == zzapp_as_l1(&first)[5]);
	CHECK(template[5] == zzapp_as_l1(&second)[5]);
	CHECK((template[5] & 3U) == 1U);
	zzapp_as_destroy(&first);
	zzapp_as_destroy(&second);
}

/* Xil_SetTlbAttributes rewrites whole BSP sections at run time (audio,
 * Ethernet, MiniGL, the Z3 mailbox). An app holding a grant in that section
 * must see the new memory type on every page while keeping its access. */
static void test_section_attribute_change_reaches_app_pages(void)
{
	zzapp_as_t as;
	const uint32_t *l2;
	const uint32_t noncache = 0x03400000U | 0x11de2U;
	const uint32_t mem_type = (7U << 6) | (1U << 3) | (1U << 2);

	CHECK(zzapp_as_create(&as, 1U, 7U) == 0);
	CHECK(zzapp_as_grant_user(&as, 0x03405000U, 5U * 4096U) == 0);
	zzapp_as_mmu_page_changed(0x034U, noncache, NULL);
	CHECK((zzapp_as_template()[0x034U] & ~(SECTION_AP_MASK | SECTION_NG)) ==
	      (noncache & ~(SECTION_AP_MASK | SECTION_NG)));
	l2 = zzapp_as_l2_for(&as, 0x03400000U);
	CHECK(l2 != NULL);
	CHECK((l2[5] & mem_type) == (1U << 6));
	CHECK((l2[5] & PAGE_AP_MASK) == PAGE_USER_RW);
	CHECK((l2[4] & mem_type) == (1U << 6));
	CHECK((l2[4] & PAGE_AP_MASK) != PAGE_USER_RW);
	zzapp_as_destroy(&as);
}

static void test_l2_exhaustion_is_atomic(void)
{
	zzapp_as_t as;
	uint32_t before[4096];
	uint32_t section;
	int rc = 0;

	CHECK(zzapp_as_create(&as, 0U, 3U) == 0);
	for (section = 0U; section < 0x400U; section++) {
		memcpy(before, zzapp_as_l1(&as), sizeof(before));
		rc = zzapp_as_grant_user(&as, section << 20, 4096U);
		if (rc != 0)
			break;
	}
	CHECK(rc == ZZAPP_AS_ENOMEM);
	CHECK(memcmp(before, zzapp_as_l1(&as), sizeof(before)) == 0);
	zzapp_as_destroy(&as);
}

int main(void)
{
	setup();
	test_template_is_privileged_global();
	test_frame_sections_only_fill_window();
	test_window_is_one_user_section();
	test_grant_and_ungrant_preserve_neighbours();
	test_image_permissions_and_window_bounds();
	test_noncacheable_change_propagates();
	test_section_attribute_change_reaches_app_pages();
	test_l2_exhaustion_is_atomic();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
