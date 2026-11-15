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
#define SECTION_XN (1U << 4)
#define SECTION_NG (1U << 17)
#define PAGE_AP_MASK (3U << 4)
#define PAGE_SMALL_XN 1U
#define PAGE_XN PAGE_SMALL_XN
#define PAGE_LARGE_XN (1U << 15)
#define PAGE_NG (1U << 11)
#define PAGE_SMALL 2U
#define PAGE_LARGE 1U
#define PAGE_USER_RW (3U << 4)
#define PAGE_USER_RO (2U << 4)
struct xil_cache_mock_state g_xil_cache_mock;
static int test_cpu;
int smp_cpu_id(void) { return test_cpu; }
uint32_t smp_local_irq_save(void) { return 0u; }
extern unsigned zzapp_as_test_asid_invalidations;

void smp_local_irq_restore(uint32_t state) { (void)state; }
void smp_raw_spin_lock(volatile uint32_t *word) { *word = 1u; }
int smp_raw_spin_try_lock(volatile uint32_t *word)
{
    if (*word)
        return 0;
    *word = 1u;
    return 1;
}
void smp_raw_spin_unlock(volatile uint32_t *word) { *word = 0u; }


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
	CHECK((l1[0xc00U] & SECTION_XN) != 0U);
	CHECK((l1[0xc03U] & 3U) == 0U);
	CHECK(zzapp_as_unmap_frames(&as, ZZAPP_AS_WINDOW_BASE + 0x00100000U,
	                            1U) == 0);
	CHECK((l1[0xc01U] & 3U) == 0U);
	CHECK((l1[0xc00U] & 3U) != 0U);
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
	/* A page shared by code and data: user-writable and executable. */
	CHECK(zzapp_as_map_image(&as, ZZAPP_AS_WINDOW_BASE + 12288U,
	                         0x10003000U, 4096U, ZZAPP_AS_IMAGE_RWX) == 0);
	CHECK((l2[3] & PAGE_AP_MASK) == PAGE_USER_RW);
	CHECK((l2[3] & PAGE_XN) == 0U);
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
 * Ethernet, MiniGL, the Z3 mailbox). An alias of that physical range must
 * retain its user access while taking the changed memory attributes. */
static void test_section_attribute_change_reaches_alias_pages(void)
{
	zzapp_as_t as;
	const uint32_t *l2;
	const uint32_t noncache = 0x03400000U | 0x11de2U;
	const uint32_t mem_type = (7U << 6) | (1U << 3) | (1U << 2);
	uint32_t va;

	CHECK(zzapp_as_create(&as, 1U, 7U) == 0);
	CHECK(zzapp_as_alias_map(&as, 0x03405000U, 5U * 4096U, 1, &va) == 0);
	zzapp_as_mmu_page_changed(0x034U, noncache, NULL);
	CHECK((zzapp_as_template()[0x034U] & ~(SECTION_AP_MASK | SECTION_NG)) ==
	      (noncache & ~(SECTION_AP_MASK | SECTION_NG)));
	l2 = zzapp_as_l2_for(&as, va);
	CHECK(l2 != NULL);
	CHECK((l2[(va >> 12) & 0xffU] & mem_type) == (1U << 6));
	CHECK((l2[(va >> 12) & 0xffU] & PAGE_AP_MASK) == PAGE_USER_RW);
	CHECK((l2[((va >> 12) + 4U) & 0xffU] & mem_type) == (1U << 6));
	CHECK((l2[((va >> 12) + 4U) & 0xffU] & PAGE_AP_MASK) == PAGE_USER_RW);
	zzapp_as_destroy(&as);
}


static void test_alias_maps_only_in_private_region(void)
{
	zzapp_as_t as;
	const uint32_t *identity_l2;
	const uint32_t *alias_l2;
	uint32_t rw_va;
	uint32_t ro_va;
	uint32_t rx_va;

	CHECK(zzapp_as_create(&as, 0U, 5U) == 0);
	CHECK(zzapp_as_alias_map(&as, 0x03405000U, 5U * 4096U, 1,
	                         &rw_va) == 0);
	CHECK(rw_va >= ZZAPP_AS_ALIAS_BASE && rw_va < ZZAPP_AS_ALIAS_END);
	identity_l2 = zzapp_as_l2_for(&as, 0x03400000U);
	CHECK(identity_l2 == NULL);
	alias_l2 = zzapp_as_l2_for(&as, rw_va);
	CHECK(alias_l2 != NULL);
	CHECK((alias_l2[(rw_va >> 12) & 0xffU] & 0xfffff000U) ==
	      0x03405000U);
	CHECK((alias_l2[(rw_va >> 12) & 0xffU] & PAGE_AP_MASK) == PAGE_USER_RW);
	CHECK((alias_l2[(rw_va >> 12) & 0xffU] & PAGE_XN) != 0U);
	CHECK((alias_l2[(rw_va >> 12) & 0xffU] & PAGE_NG) != 0U);
	CHECK(zzapp_as_alias_phys(&as, rw_va + 0x234U) == 0x03405234U);
	CHECK(zzapp_as_alias_map(&as, 0x03410000U, 4096U, 0, &ro_va) == 0);
	alias_l2 = zzapp_as_l2_for(&as, ro_va);
	CHECK((alias_l2[(ro_va >> 12) & 0xffU] & PAGE_AP_MASK) == PAGE_USER_RO);
	CHECK(zzapp_as_alias_map_flags(&as, 0x03411000U, 4096U,
				       ZZAPP_AS_ALIAS_RX, &rx_va) == 0);
	alias_l2 = zzapp_as_l2_for(&as, rx_va);
	CHECK((alias_l2[(rx_va >> 12) & 0xffU] & PAGE_AP_MASK) == PAGE_USER_RO);
	CHECK((alias_l2[(rx_va >> 12) & 0xffU] & PAGE_XN) == 0U);
	CHECK(zzapp_as_alias_unmap(&as, rw_va, 4U * 4096U) != 0);
	CHECK(zzapp_as_alias_phys(&as, rw_va) == 0x03405000U);
	CHECK(zzapp_as_alias_unmap(&as, rw_va, 5U * 4096U) == 0);
	CHECK(zzapp_as_alias_phys(&as, rw_va) == 0U);
	CHECK(zzapp_as_alias_unmap(&as, ro_va, 4096U) == 0);
	CHECK(zzapp_as_alias_unmap(&as, rx_va, 4096U) == 0);
	CHECK(zzapp_as_alias_map(&as, 0x03410001U, 4096U, 1, &rw_va) != 0);
	CHECK(zzapp_as_alias_map(&as, 0x03410000U, 4095U, 1, &rw_va) != 0);
	zzapp_as_destroy(&as);
	CHECK(zzapp_as_alias_phys(&as, ZZAPP_AS_ALIAS_BASE) == 0U);
}

static void test_alias_rejects_larger_than_private_region(void)
{
	zzapp_as_t as;
	uint32_t va;

	CHECK(zzapp_as_create(&as, 0U, 8U) == 0);
	CHECK(zzapp_as_alias_map(&as, 0x03400000U,
		ZZAPP_AS_ALIAS_END - ZZAPP_AS_ALIAS_BASE + 4096U, 1, &va) ==
	      ZZAPP_AS_ENOMEM);
	zzapp_as_destroy(&as);
}

static void test_alias_honours_virtual_alignment(void)
{
	zzapp_as_t as;
	uint32_t first;
	uint32_t aligned;

	CHECK(zzapp_as_create(&as, 0U, 9U) == 0);
	CHECK(zzapp_as_alias_map(&as, 0x03400000U, 4096U, 1, &first) == 0);
	CHECK(zzapp_as_alias_map_aligned(&as, 0x03401000U, 4096U, 1,
		65536U, &aligned) == 0);
	CHECK((aligned & (65536U - 1U)) == 0U);
	CHECK(zzapp_as_alias_map_aligned(&as, 0x03402000U, 4096U, 1,
		8192U, &aligned) == 0);
	CHECK((aligned & (8192U - 1U)) == 0U);
	CHECK(zzapp_as_alias_map_aligned(&as, 0x03403000U, 4096U, 1,
		2048U, &aligned) != 0);
	zzapp_as_destroy(&as);
}

static void test_alias_l2_exhaustion_is_atomic(void)
{
	zzapp_as_t first;
	zzapp_as_t second;
	zzapp_as_t third;
	zzapp_as_t fourth;
	uint32_t before[4096];
	uint32_t va;
	uint32_t bytes = ZZAPP_AS_ALIAS_END - ZZAPP_AS_ALIAS_BASE;

	CHECK(zzapp_as_create(&first, 0U, 3U) == 0);
	CHECK(zzapp_as_create(&second, 1U, 3U) == 0);
	CHECK(zzapp_as_create(&third, 2U, 3U) == 0);
	CHECK(zzapp_as_create(&fourth, 3U, 3U) == 0);
	CHECK(zzapp_as_alias_map(&first, 0U, bytes, 1, &va) == 0);
	CHECK(zzapp_as_alias_map(&second, 0U, bytes, 1, &va) == 0);
	CHECK(zzapp_as_alias_map(&third, 0U, 3U << 20, 1, &va) == 0);
	memcpy(before, zzapp_as_l1(&fourth), sizeof(before));
	CHECK(zzapp_as_alias_map(&fourth, 0U, 4096U, 1, &va) ==
	      ZZAPP_AS_ENOMEM);
	CHECK(memcmp(before, zzapp_as_l1(&fourth), sizeof(before)) == 0);
	zzapp_as_destroy(&first);
	zzapp_as_destroy(&second);
	zzapp_as_destroy(&third);
	zzapp_as_destroy(&fourth);
}
typedef struct {
	uint32_t physical;
	uint32_t memory;
	uint8_t type;
	uint8_t user_read;
	uint8_t user_write;
	uint8_t privileged_read;
	uint8_t privileged_write;
	uint8_t xn;
	uint8_t ng;
} app_translation_t;

static void decode_permissions(uint32_t ap, uint32_t apx,
			       app_translation_t *translation)
{
	switch (ap | (apx << 2)) {
	case 1U:
		translation->privileged_read = 1U;
		translation->privileged_write = 1U;
		break;
	case 2U:
		translation->privileged_read = 1U;
		translation->privileged_write = 1U;
		translation->user_read = 1U;
		break;
	case 3U:
		translation->privileged_read = 1U;
		translation->privileged_write = 1U;
		translation->user_read = 1U;
		translation->user_write = 1U;
		break;
	case 5U:
		translation->privileged_read = 1U;
		break;
	case 6U:
		translation->privileged_read = 1U;
		translation->user_read = 1U;
		break;
	case 7U:
		translation->privileged_read = 1U;
		translation->user_read = 1U;
		break;
	}
}

static int walk_app(const zzapp_as_t *as, uint32_t va,
		    app_translation_t *translation)
{
	const uint32_t *l1;
	const uint32_t *l2;
	uint32_t descriptor;
	uint32_t type;
	uint32_t tex;
	uint32_t c;
	uint32_t b;
	uint32_t s;

	if (!as || !as->active || !as->l1)
		return 0;
	memset(translation, 0, sizeof(*translation));
	l1 = as->l1;
	descriptor = l1[va >> 20];
	type = descriptor & 3U;
	if (type == 2U) {
		translation->physical = (descriptor & 0xfff00000U) |
			(va & 0xfffffU);
		translation->type = (uint8_t)type;
		translation->xn = (descriptor >> 4) & 1U;
		translation->ng = (descriptor >> 17) & 1U;
		tex = (descriptor >> 12) & 7U;
		c = (descriptor >> 3) & 1U;
		b = (descriptor >> 2) & 1U;
		s = (descriptor >> 16) & 1U;
		decode_permissions((descriptor >> 10) & 3U,
				   (descriptor >> 15) & 1U, translation);
	} else if (type == 1U) {
		l2 = (const uint32_t *)(uintptr_t)(descriptor & 0xfffffc00U);
		if (!l2)
			return 0;
		descriptor = l2[(va >> 12) & 0xffU];
		type = descriptor & 3U;
		if (type == PAGE_LARGE) {
			translation->physical = (descriptor & 0xffff0000U) |
				(va & 0xffffU);
			translation->xn = (descriptor >> 15) & 1U;
			tex = (descriptor >> 12) & 7U;
		} else if ((type & 2U) != 0U) {
			translation->physical = (descriptor & 0xfffff000U) |
				(va & 0xfffU);
			translation->xn = descriptor & 1U;
			tex = (descriptor >> 6) & 7U;
		} else {
			return 0;
		}
		translation->type = (uint8_t)type;
		translation->ng = (descriptor >> 11) & 1U;
		c = (descriptor >> 3) & 1U;
		b = (descriptor >> 2) & 1U;
		s = (descriptor >> 10) & 1U;
		decode_permissions((descriptor >> 4) & 3U,
				   (descriptor >> 9) & 1U, translation);
	} else {
		return 0;
	}
	translation->memory = (tex << 3) | (s << 2) | (c << 1) | b;
	return 1;
}

static void check_translation(const app_translation_t *translation,
			      uint32_t physical, uint32_t memory,
			      int writable, int executable)
{
	CHECK(translation->physical == physical);
	CHECK(translation->memory == memory);
	CHECK(translation->privileged_read);
	CHECK(translation->privileged_write);
	CHECK(translation->user_read);
	CHECK(translation->user_write == writable);
	CHECK(translation->xn == !executable);
	CHECK(translation->ng);
}

static void test_largest_app_descriptors_preserve_translation(void)
{
	zzapp_as_t as;
	const uint32_t section_va = ZZAPP_AS_WINDOW_BASE + 0x01000000U;
	const uint32_t section_pa = 0x11000000U;
	const uint32_t text_va = ZZAPP_AS_WINDOW_BASE + 0x02001000U;
	const uint32_t text_pa = 0x12001000U;
	const uint32_t text_bytes = 0x32000U;
	const uint32_t ddr_memory = (5U << 3) | (1U << 2) | 1U;
	const uint32_t noncache_memory = (1U << 3) | (1U << 2);
	uint32_t changed_pages[256];
	uint32_t alias_va;
	uint32_t offset;
	uint32_t large_descriptors = 0U;
	app_translation_t translation;

	CHECK(zzapp_as_create(&as, 0U, 11U) == 0);
	CHECK(zzapp_as_map_image(&as, section_va, section_pa, 0x100000U,
	                         ZZAPP_AS_IMAGE_RW) == 0);
	for (offset = 0U; offset < 0x100000U; offset += 4096U) {
		CHECK(walk_app(&as, section_va + offset, &translation));
		CHECK(translation.type == 2U);
		check_translation(&translation, section_pa + offset, ddr_memory,
				  1, 0);
	}
	CHECK(zzapp_as_map_image(&as, text_va, text_pa, text_bytes,
	                         ZZAPP_AS_IMAGE_RX) == 0);
	for (offset = 0U; offset < text_bytes; offset += 4096U) {
		CHECK(walk_app(&as, text_va + offset, &translation));
		check_translation(&translation, text_pa + offset, ddr_memory, 0, 1);
		if (translation.type == PAGE_LARGE &&
		    ((text_va + offset) & 0xffffU) == 0U)
			large_descriptors++;
	}
	CHECK(large_descriptors == 2U);
	for (offset = 0U; offset < sizeof(changed_pages) /
	     sizeof(changed_pages[0]); offset++)
		changed_pages[offset] = (section_pa + offset * 4096U) | 0x576U;
	changed_pages[5] = (section_pa + 5U * 4096U) | 0x472U;
	zzapp_as_mmu_page_changed(section_pa >> 20, 1U | (15U << 5),
				  changed_pages);
	for (offset = 0U; offset < 0x100000U; offset += 4096U) {
		CHECK(walk_app(&as, section_va + offset, &translation));
		CHECK((translation.type & 2U) != 0U);
		check_translation(&translation, section_pa + offset,
				  offset == 5U * 4096U ? noncache_memory :
				  ddr_memory, 1, 0);
	}
	CHECK(zzapp_as_alias_map(&as, 0x03400000U, 0x10000U, 1,
	                         &alias_va) == 0);
	for (offset = 0U; offset < 0x10000U; offset += 4096U) {
		CHECK(walk_app(&as, alias_va + offset, &translation));
		CHECK(translation.type == PAGE_LARGE);
		check_translation(&translation, 0x03400000U + offset, ddr_memory,
				  1, 0);
	}
	CHECK(zzapp_as_alias_unmap(&as, alias_va, 0x10000U) == 0);
	for (offset = 0U; offset < 0x10000U; offset += 4096U)
		CHECK(zzapp_as_alias_phys(&as, alias_va + offset) == 0U);
	zzapp_as_destroy(&as);
	CHECK(zzapp_as_l1(&as) == NULL);
}

static void test_mixed_alias_stays_small(void)
{
	zzapp_as_t as;
	uint32_t source[256];
	uint32_t alias_va;
	uint32_t page;
	app_translation_t translation;
	const uint32_t ddr_memory = (5U << 3) | (1U << 2) | 1U;
	const uint32_t noncache_memory = (1U << 3) | (1U << 2);

	setup();
	for (page = 0U; page < 256U; page++)
		source[page] = (0x00500000U + page * 4096U) | 0x576U;
	source[0xdfU] = 0x005df000U | 0x472U;
	zzapp_as_mmu_page_changed(5U, 1U | (15U << 5), source);
	CHECK(zzapp_as_create(&as, 0U, 12U) == 0);
	CHECK(zzapp_as_alias_map(&as, 0x005d0000U, 0x10000U, 1,
	                         &alias_va) == 0);
	for (page = 0U; page < 16U; page++) {
		CHECK(walk_app(&as, alias_va + page * 4096U, &translation));
		CHECK((translation.type & 2U) != 0U);
		check_translation(&translation, 0x005d0000U + page * 4096U,
				  page == 15U ? noncache_memory : ddr_memory,
				  1, 0);
	}
	zzapp_as_destroy(&as);
}
static void test_noncache_large_page_splits_without_losing_tex(void)
{
	zzapp_as_t as;
	uint32_t source[256];
	uint32_t alias_va;
	uint32_t page;
	app_translation_t translation;
	const uint32_t ddr_memory = (5U << 3) | (1U << 2) | 1U;
	const uint32_t noncache_memory = (1U << 3) | (1U << 2);

	setup();
	for (page = 0U; page < 256U; page++)
		source[page] = (0x00600000U + page * 4096U) | 0x576U;
	for (page = 0xd0U; page <= 0xdfU; page++)
		source[page] = (0x00600000U + page * 4096U) | 0x472U;
	zzapp_as_mmu_page_changed(6U, 1U | (15U << 5), source);
	CHECK(zzapp_as_create(&as, 0U, 13U) == 0);
	CHECK(zzapp_as_alias_map(&as, 0x006d0000U, 0x10000U, 1,
	                         &alias_va) == 0);
	for (page = 0U; page < 16U; page++) {
		CHECK(walk_app(&as, alias_va + page * 4096U, &translation));
		CHECK(translation.type == PAGE_LARGE);
		check_translation(&translation, 0x006d0000U + page * 4096U,
				  noncache_memory, 1, 0);
	}
	source[0xdfU] = 0x006df000U | 0x576U;
	zzapp_as_mmu_page_changed(6U, 1U | (15U << 5), source);
	for (page = 0U; page < 16U; page++) {
		CHECK(walk_app(&as, alias_va + page * 4096U, &translation));
		CHECK((translation.type & 2U) != 0U);
		check_translation(&translation, 0x006d0000U + page * 4096U,
				  page == 15U ? ddr_memory : noncache_memory,
				  1, 0);
	}
	zzapp_as_destroy(&as);
}

static void test_app_section_split_breaks_before_publishing(void)
{
	zzapp_as_t as;
	uint32_t source[256];
	const uint32_t va = ZZAPP_AS_WINDOW_BASE + 0x04000000U;
	const uint32_t pa = 0x14000000U;
	uintptr_t app_table_base;
	uint32_t page;
	unsigned op;
	int table_clean = 0;
	int broken = 0;
	int published = 0;

	setup();
	CHECK(zzapp_as_create(&as, 0U, 14U) == 0);
	CHECK(zzapp_as_map_image(&as, va, pa, 0x100000U,
	                         ZZAPP_AS_IMAGE_RW) == 0);
	for (page = 0U; page < 256U; page++)
		source[page] = (pa + page * 4096U) | 0x576U;
	source[3U] = (pa + 3U * 4096U) | 0x472U;
	g_xil_cache_mock.watch_word = &as.l1[va >> 20];
	g_xil_cache_mock.count = 0U;
	zzapp_as_test_asid_invalidations = 0U;
	zzapp_as_mmu_page_changed(pa >> 20, 1U | (15U << 5), source);
	app_table_base = (uintptr_t)(as.l1[va >> 20] & 0xfffffc00U);
	CHECK(app_table_base != 0U);
	for (op = 0U; op < g_xil_cache_mock.count; op++) {
		const struct xil_cache_mock_flush *flush =
			&g_xil_cache_mock.ops[op];

		if (flush->len == 1024U && flush->adr == app_table_base &&
		    !broken)
			table_clean = 1;
		if (flush->len == sizeof(uint32_t) && flush->watch_value == 0U) {
			CHECK(table_clean);
			CHECK(flush->asid_invalidations == 0U);
			broken = 1;
		}
		if (broken && !published && flush->len == sizeof(uint32_t) &&
		    (flush->watch_value & 3U) == 1U) {
			CHECK(flush->asid_invalidations == 1U);
			published = 1;
		}
	}
	CHECK(broken);
	CHECK(published);
	CHECK(table_clean);
	CHECK(zzapp_as_test_asid_invalidations == 3U);
	g_xil_cache_mock.watch_word = NULL;
	zzapp_as_destroy(&as);
}



static void test_template_l2_published_after_clean(void)
{
	uint32_t pages[256];
	uint32_t section = 5U;
	uint32_t old;
	unsigned i;
	int saw_l2_clean = 0;
	const uint32_t *published;

	setup();
	old = zzapp_as_template()[section];
	for (i = 0U; i < 256U; i++)
		pages[i] = (section << 20) | (i << 12) | 0x576U;
	g_xil_cache_mock.watch_word = &zzapp_as_template()[section];
	g_xil_cache_mock.count = 0U;
	zzapp_as_mmu_page_changed(section, 1U | (15U << 5), pages);
	for (i = 0U; i < g_xil_cache_mock.count; i++) {
		const struct xil_cache_mock_flush *op = &g_xil_cache_mock.ops[i];

		if (op->len != 1024U)
			continue;
		saw_l2_clean = 1;
		CHECK(op->watch_value == old);
		CHECK((op->watch_value & 3U) != 1U);
	}
	CHECK(saw_l2_clean);
	published = zzapp_as_template();
	CHECK((published[section] & 3U) == 1U);
	CHECK(((const uint32_t *)(uintptr_t)(published[section] & 0xfffffc00U))[0] != 0U);
	CHECK(((const uint32_t *)(uintptr_t)(published[section] & 0xfffffc00U))[255] != 0U);
	g_xil_cache_mock.watch_word = NULL;
}

static void test_destroy_on_core0_keeps_bsp_table(void)
{
	zzapp_as_t as;
	unsigned before;

	CHECK(zzapp_as_create(&as, 0U, 9U) == 0);
	before = zzapp_as_test_kernel_switches;
	test_cpu = 0;
	zzapp_as_destroy(&as);
	CHECK(zzapp_as_test_kernel_switches == before);

	CHECK(zzapp_as_create(&as, 0U, 10U) == 0);
	test_cpu = 1;
	zzapp_as_destroy(&as);
	CHECK(zzapp_as_test_kernel_switches == before + 1U);
	test_cpu = 0;
}

static void test_orphaned_as_lock_does_not_deadlock_destroy(void)
{
	zzapp_as_t as;

	CHECK(zzapp_as_create(&as, 1U, 4U) == 0);
	zzapp_as_test_orphan_lock();
	CHECK(zzapp_as_test_lock_held());
	zzapp_as_reset_lock();
	CHECK(!zzapp_as_test_lock_held());
	zzapp_as_destroy(&as);
	CHECK(!zzapp_as_test_lock_held());
}

static void test_firmware_text_is_l2_before_launch(void)
{
	const uint32_t *l2;
	uint32_t pages[256];
	uint32_t before;
	unsigned i;
	int saw_break = 0;

	zzapp_as_set_firmware_text(0x00500000U, 0x00700000U);
	setup();
	CHECK((zzapp_as_template()[5] & 3U) == 1U);
	CHECK((zzapp_as_template()[6] & 3U) == 1U);
	l2 = (const uint32_t *)(uintptr_t)(zzapp_as_template()[5] & 0xfffffc00U);
	CHECK(l2[0] != 0U);
	CHECK(l2[255] != 0U);
	before = zzapp_as_template()[5];
	for (i = 0U; i < 256U; i++)
		pages[i] = (5U << 20) | (i << 12) | 0x756U;
	g_xil_cache_mock.watch_word = &zzapp_as_template()[5];
	g_xil_cache_mock.count = 0U;
	zzapp_as_mmu_page_changed(5U, before, pages);
	for (i = 0U; i < g_xil_cache_mock.count; i++) {
		if (g_xil_cache_mock.ops[i].watch_value == 0U)
			saw_break = 1;
		CHECK((g_xil_cache_mock.ops[i].watch_value & 3U) == 1U);
	}
	CHECK(!saw_break);
	CHECK(zzapp_as_template()[5] == before);
	g_xil_cache_mock.watch_word = NULL;
	zzapp_as_set_firmware_text(0U, 0U);
}
static void test_reused_asid_is_invalidated_before_dispatch(void)
{
	zzapp_as_t first;
	zzapp_as_t second;

	setup();
	CHECK(zzapp_as_create(&first, 0U, 1U) == 0);
	zzapp_as_switch(&first);
	zzapp_as_test_asid_invalidations = 0U;
	zzapp_as_destroy(&first);
	CHECK(zzapp_as_test_asid_invalidations == 1U);
	CHECK(zzapp_as_create(&second, 0U, 2U) == 0);
	CHECK(zzapp_as_test_asid_invalidations == 1U);
	zzapp_as_switch(&second);
	CHECK(zzapp_as_test_asid_invalidations == 1U);
	zzapp_as_destroy(&second);
}


int main(void)
{
	setup();
	test_template_is_privileged_global();
	test_frame_sections_only_fill_window();
	test_image_permissions_and_window_bounds();
	test_largest_app_descriptors_preserve_translation();
	test_mixed_alias_stays_small();
	test_noncacheable_change_propagates();
	test_section_attribute_change_reaches_alias_pages();
	test_alias_maps_only_in_private_region();
	test_alias_l2_exhaustion_is_atomic();
	test_alias_rejects_larger_than_private_region();
	test_alias_honours_virtual_alignment();
	test_noncache_large_page_splits_without_losing_tex();
	test_app_section_split_breaks_before_publishing();
	test_template_l2_published_after_clean();
	test_destroy_on_core0_keeps_bsp_table();
	test_orphaned_as_lock_does_not_deadlock_destroy();
	test_firmware_text_is_l2_before_launch();
	test_reused_asid_is_invalidated_before_dispatch();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
