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
#define PAGE_SMALL_XN 1U
#define PAGE_XN PAGE_SMALL_XN
#define PAGE_LARGE_XN (1U << 15)
#define PAGE_NG (1U << 11)
#define PAGE_SMALL 2U
#define PAGE_LARGE 1U
#define PAGE_TYPE PAGE_SMALL
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
	unsigned before;
	unsigned i;
	int at_alias = 0;

	CHECK(zzapp_as_create(&as, 1U, 7U) == 0);
	CHECK(zzapp_as_alias_map(&as, 0x03405000U, 5U * 4096U, 1, &va) == 0);
	if (&zzapp_as_test_tlb_mva_invalidations)
		zzapp_as_test_tlb_mva_invalidations = 0U;
	before = &zzapp_as_test_tlb_mva_invalidations ?
		zzapp_as_test_tlb_mva_invalidations : 0U;
	zzapp_as_mmu_page_changed(0x034U, noncache, NULL);
	/* The alias table's rewrites retire at the alias VA the app uses,
	 * not at section 0 or some other address. */
	for (i = before; i < (&zzapp_as_test_tlb_mva_invalidations ?
	      zzapp_as_test_tlb_mva_invalidations : 0U) && i < 256U; i++)
		if (zzapp_as_test_tlb_mva_pages[i] == va ||
		    zzapp_as_test_tlb_mva_pages[i] == va + 4U * 4096U)
			at_alias = 1;
	CHECK(at_alias);

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

/* A launcher can raise an app's host-visible limit past 16 MiB, so a single
 * block that large must map as one alias. */
static void test_alias_maps_a_34_mib_block(void)
{
	zzapp_as_t as;
	const uint32_t bytes = 34U << 20;
	uint32_t va;

	CHECK(zzapp_as_create(&as, 0U, 10U) == 0);
	CHECK(zzapp_as_alias_map(&as, 0x03400000U, bytes, 1, &va) == 0);
	CHECK(va == ZZAPP_AS_ALIAS_BASE);
	CHECK(zzapp_as_alias_phys(&as, va) == 0x03400000U);
	CHECK(zzapp_as_alias_phys(&as, va + bytes - 4096U) ==
	      0x03400000U + bytes - 4096U);
	CHECK(zzapp_as_alias_unmap(&as, va, bytes) == 0);
	CHECK(zzapp_as_alias_phys(&as, va) == 0U);
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
static unsigned tlb_mva_count(void)
{
	return &zzapp_as_test_tlb_mva_invalidations ?
		zzapp_as_test_tlb_mva_invalidations : 0U;
}

static unsigned tlb_bp_count(void)
{
	return &zzapp_as_test_bp_invalidations ?
		zzapp_as_test_bp_invalidations : 0U;
}

static void reset_tlb_counters(void)
{
	zzapp_as_test_asid_invalidations = 0U;
	if (&zzapp_as_test_tlb_mva_invalidations)
		zzapp_as_test_tlb_mva_invalidations = 0U;
	if (&zzapp_as_test_bp_invalidations)
		zzapp_as_test_bp_invalidations = 0U;
}

/* A data mapping change retires exactly the translations it rewrote: one
 * MVA invalidation per page, no ASID-wide or whole-TLB broadcast. */
static void test_alias_mapping_change_retires_by_mva(void)
{
	zzapp_as_t as;
	uint32_t alias = 0U;
	unsigned before;
	unsigned i;
	int covered;

	setup();
	CHECK(zzapp_as_create(&as, 0U, 11U) == 0);
	reset_tlb_counters();
	CHECK(zzapp_as_alias_map(&as, 0x03405000U, 5U * 4096U, 1,
				 &alias) == 0);
	CHECK(alias != 0U);
	CHECK(zzapp_as_test_asid_invalidations == 0U);
	before = tlb_mva_count();
	CHECK(before >= 5U);
	covered = 0;
	for (i = 0U; i < before && i < 256U; i++)
		if (zzapp_as_test_tlb_mva_pages[i] == alias)
			covered = 1;
	CHECK(covered);
	/* A data alias never touches the branch predictor. */
	CHECK(tlb_bp_count() == 0U);
	CHECK(zzapp_as_alias_unmap(&as, alias, 5U * 4096U) == 0);
	CHECK(zzapp_as_test_asid_invalidations == 0U);
	CHECK(tlb_mva_count() >= before + 5U);
	zzapp_as_destroy(&as);
}

/* Frame sections retire by one MVA op per 1 MiB section. */
static void test_frame_mapping_change_retires_by_section(void)
{
	zzapp_as_t as;
	uint32_t frames[2] = { 0x14000000U, 0x15000000U };

	setup();
	CHECK(zzapp_as_create(&as, 1U, 12U) == 0);
	reset_tlb_counters();
	CHECK(zzapp_as_map_frames(&as, ZZAPP_AS_WINDOW_BASE, frames, 2U) == 0);
	CHECK(zzapp_as_test_asid_invalidations == 0U);
	CHECK(tlb_mva_count() == 2U);
	CHECK(zzapp_as_unmap_frames(&as, ZZAPP_AS_WINDOW_BASE, 2U) == 0);
	CHECK(tlb_mva_count() == 4U);
	zzapp_as_destroy(&as);
}

/* An executable mapping must also retire branch predictor entries; a
 * data-only mapping must not (ARM ARM VA-reuse rule). */
static void test_executable_mapping_invalidates_predictors(void)
{
	zzapp_as_t as;
	uint32_t va = 0U;
	unsigned bp;

	setup();
	CHECK(zzapp_as_create(&as, 0U, 13U) == 0);
	reset_tlb_counters();
	CHECK(zzapp_as_map_image(&as, ZZAPP_AS_WINDOW_BASE, 0x03400000U,
				 4U * 4096U, ZZAPP_AS_IMAGE_RX) == 0);
	bp = tlb_bp_count();
	CHECK(bp >= 1U);
	CHECK(zzapp_as_map_image(&as, ZZAPP_AS_WINDOW_BASE + 4U * 4096U,
				 0x03404000U, 4U * 4096U,
				 ZZAPP_AS_IMAGE_RW) == 0);
	CHECK(tlb_bp_count() == bp);
	CHECK(zzapp_as_map_image(&as, ZZAPP_AS_WINDOW_BASE, 0x03400000U,
				 4U * 4096U, ZZAPP_AS_IMAGE_RWX) == 0);
	CHECK(tlb_bp_count() > bp);
	CHECK(zzapp_as_alias_map_flags(&as, 0x03408000U, 4096U,
				       ZZAPP_AS_ALIAS_RX, &va) == 0);
	CHECK(va != 0U);
	CHECK(tlb_bp_count() > bp + 1U);
	CHECK(zzapp_as_alias_map(&as, 0x03409000U, 4096U, 1, &va) == 0);
	CHECK(tlb_mva_count() > 0U);
	zzapp_as_destroy(&as);
}

static void test_attribute_change_reaches_private_mapping_forms(void)
{
	zzapp_as_t as;
	const uint32_t table_va = ZZAPP_AS_WINDOW_BASE + 0x00400000U;
	const uint32_t large_va = ZZAPP_AS_WINDOW_BASE + 0x00500000U;
	const uint32_t section_va = ZZAPP_AS_WINDOW_BASE + 0x00600000U;
	const uint32_t duplicate_section_va =
		ZZAPP_AS_WINDOW_BASE + 0x00700000U;
	const uint32_t table_pa = 0x03401000U;
	const uint32_t large_pa = 0x03410000U;
	const uint32_t noncache = 0x03400000U | 0x11de2U;
	const uint32_t frames[] = { 0x03400000U };
	const uint32_t page_memory = (7U << 6) | (1U << 10) |
				     (1U << 3) | (1U << 2);
	const uint32_t section_memory = (7U << 12) | (1U << 16) |
					(1U << 3) | (1U << 2);
	const uint32_t *l1;
	const uint32_t *l2;
	uint32_t i;

	setup();
	CHECK(zzapp_as_create(&as, 2U, 16U) == 0);
	CHECK(zzapp_as_map_image(&as, table_va, table_pa, 4096U,
				 ZZAPP_AS_IMAGE_RW) == 0);
	CHECK(zzapp_as_map_image(&as, large_va, large_pa, 0x10000U,
				 ZZAPP_AS_IMAGE_RX) == 0);
	CHECK(zzapp_as_map_frames(&as, section_va, frames, 1U) == 0);
	CHECK(zzapp_as_map_frames(&as, duplicate_section_va, frames, 1U) == 0);
	zzapp_as_mmu_page_changed(0x034U, noncache, NULL);

	l2 = zzapp_as_l2_for(&as, table_va);
	CHECK((l2[(table_va >> 12) & 0xffU] & 0xfffff000U) == table_pa);
	CHECK((l2[(table_va >> 12) & 0xffU] & page_memory) ==
	      ((1U << 6) | (1U << 10)));
	CHECK((l2[(table_va >> 12) & 0xffU] & PAGE_AP_MASK) == PAGE_USER_RW);
	CHECK((l2[(table_va >> 12) & 0xffU] & PAGE_XN) != 0U);
	CHECK((l2[(table_va >> 12) & 0xffU] & PAGE_NG) != 0U);

	l2 = zzapp_as_l2_for(&as, large_va);
	for (i = 0U; i < 16U; i++) {
		uint32_t mapped = l2[((large_va >> 12) & 0xffU) + i];

		CHECK((mapped & 3U) == PAGE_SMALL);
		CHECK((mapped & 0xfffff000U) == large_pa + i * 4096U);
		CHECK((mapped & page_memory) == ((1U << 6) | (1U << 10)));
		CHECK((mapped & PAGE_AP_MASK) == PAGE_USER_RO);
		CHECK((mapped & PAGE_XN) == 0U);
		CHECK((mapped & PAGE_NG) != 0U);
	}
	l1 = zzapp_as_l1(&as);
	CHECK((l1[section_va >> 20] & 0xfff00000U) == frames[0]);
	CHECK((l1[section_va >> 20] & section_memory) ==
	      (noncache & section_memory));
	CHECK((l1[section_va >> 20] & SECTION_AP_MASK) == PAGE_USER_RW << 6);
	CHECK((l1[section_va >> 20] & (1U << 4)) != 0U);
	CHECK((l1[section_va >> 20] & SECTION_NG) != 0U);
	CHECK((l1[duplicate_section_va >> 20] & 0xfff00000U) == frames[0]);
	CHECK((l1[duplicate_section_va >> 20] & section_memory) ==
	      (noncache & section_memory));
	CHECK((l1[duplicate_section_va >> 20] & SECTION_AP_MASK) ==
	      PAGE_USER_RW << 6);
	CHECK((l1[duplicate_section_va >> 20] & (1U << 4)) != 0U);
	CHECK((l1[duplicate_section_va >> 20] & SECTION_NG) != 0U);
	zzapp_as_destroy(&as);
}

static void test_typical_image_uses_large_descriptors(void)
{
	zzapp_as_t as;
	const uint32_t text_va = ZZAPP_AS_WINDOW_BASE + 0x1000U;
	const uint32_t text_pa = 0x12001000U;
	const uint32_t bss_va = ZZAPP_AS_WINDOW_BASE + 0x00200000U;
	const uint32_t bss_pa = 0x12200000U;
	const uint32_t *l1;
	const uint32_t *l2;

	setup();
	CHECK(zzapp_as_create(&as, 0U, 14U) == 0);
	reset_tlb_counters();
	CHECK(zzapp_as_map_image(&as, text_va, text_pa, 0x32000U,
				 ZZAPP_AS_IMAGE_RX) == 0);
	l2 = zzapp_as_l2_for(&as, text_va);
	CHECK(l2 != NULL);
	CHECK((l2[(text_va >> 12) & 0xffU] & 3U) == PAGE_SMALL);
	CHECK((l2[((text_va + 0xf000U) >> 12) & 0xffU] & 3U) == PAGE_LARGE);
	CHECK((l2[((text_va + 0x1f000U) >> 12) & 0xffU] & 3U) == PAGE_LARGE);
	CHECK((l2[((text_va + 0xf000U) >> 12) & 0xffU] & 0xffff0000U) ==
	      (text_pa + 0xf000U));
	CHECK((l2[((text_va + 0xf000U) >> 12) & 0xffU] & PAGE_AP_MASK) ==
	      PAGE_USER_RO);
	CHECK((l2[((text_va + 0xf000U) >> 12) & 0xffU] & PAGE_LARGE_XN) == 0U);
	CHECK((l2[((text_va + 0xf000U) >> 12) & 0xffU] & PAGE_NG) != 0U);
	CHECK(zzapp_as_map_image(&as, bss_va, bss_pa, 0x100000U,
				 ZZAPP_AS_IMAGE_RW) == 0);
	l1 = zzapp_as_l1(&as);
	CHECK((l1[bss_va >> 20] & 3U) == 2U);
	CHECK((l1[bss_va >> 20] & 0xfff00000U) == bss_pa);
	CHECK((l1[bss_va >> 20] & (7U << 12)) == (0x15de6U & (7U << 12)));
	CHECK((l1[bss_va >> 20] & (1U << 16)) != 0U);
	CHECK((l1[bss_va >> 20] & (3U << 10)) == (3U << 10));
	CHECK((l1[bss_va >> 20] & (1U << 4)) != 0U);
	CHECK((l1[bss_va >> 20] & SECTION_NG) != 0U);
	zzapp_as_destroy(&as);
}

static void test_large_alias_splits_on_attribute_change(void)
{
	zzapp_as_t as;
	uint32_t source[256];
	uint32_t va;
	const uint32_t *l2;
	uint32_t i;
	uint32_t page;
	int saw_break_before_make = 0;

	setup();
	for (i = 0U; i < 256U; i++)
		source[i] = (0x00600000U + i * 4096U) | 0x576U;
	zzapp_as_mmu_page_changed(6U, 1U | (15U << 5), source);
	CHECK(zzapp_as_create(&as, 0U, 15U) == 0);
	CHECK(zzapp_as_alias_map(&as, 0x006d0000U, 0x10000U, 1, &va) == 0);
	l2 = zzapp_as_l2_for(&as, va);
	page = (va >> 12) & 0xffU;
	CHECK((l2[page] & 3U) == PAGE_LARGE);
	reset_tlb_counters();
	memset(&g_xil_cache_mock, 0, sizeof(g_xil_cache_mock));
	g_xil_cache_mock.watch_word = &l2[page];
	source[0xdfU] = 0x006df000U | 0x472U;
	zzapp_as_mmu_page_changed(6U, 1U | (15U << 5), source);
	for (i = 0U; i + 1U < g_xil_cache_mock.count; i++) {
		unsigned mva = g_xil_cache_mock.ops[i].mva_invalidations;

		if (g_xil_cache_mock.ops[i].watch_value == 0U &&
		    (g_xil_cache_mock.ops[i + 1U].watch_value & PAGE_SMALL) != 0U &&
		    g_xil_cache_mock.ops[i + 1U].mva_invalidations > mva) {
			CHECK(zzapp_as_test_tlb_mva_pages[mva] == va);
			CHECK(g_xil_cache_mock.mva_watch_values[mva] == 0U);
			saw_break_before_make = 1;
			break;
		}
	}
	CHECK(saw_break_before_make);
	for (i = 0U; i < 16U; i++) {
		uint32_t mapped = l2[page + i];
		uint32_t expected = source[0xd0U + i];

		CHECK((mapped & PAGE_SMALL) != 0U);
		CHECK((mapped & 0xfffff000U) == (expected & 0xfffff000U));
		CHECK(((mapped >> 6) & 7U) == ((expected >> 6) & 7U));
		CHECK((mapped & ((1U << 3) | (1U << 2) | (1U << 10))) ==
		      (expected & ((1U << 3) | (1U << 2) | (1U << 10))));
		CHECK((mapped & PAGE_AP_MASK) == PAGE_USER_RW);
		CHECK((mapped & PAGE_XN) != 0U);
		CHECK((mapped & PAGE_NG) != 0U);
	}
	g_xil_cache_mock.watch_word = NULL;
	zzapp_as_destroy(&as);
}

int main(void)
{
	setup();
	test_template_is_privileged_global();
	test_frame_sections_only_fill_window();
	test_image_permissions_and_window_bounds();
	test_noncacheable_change_propagates();
	test_section_attribute_change_reaches_alias_pages();
	test_alias_maps_only_in_private_region();
	test_alias_l2_exhaustion_is_atomic();
	test_alias_rejects_larger_than_private_region();
	test_alias_maps_a_34_mib_block();
	test_alias_honours_virtual_alignment();
	test_template_l2_published_after_clean();
	test_destroy_on_core0_keeps_bsp_table();
	test_reused_asid_is_invalidated_before_dispatch();
	test_alias_mapping_change_retires_by_mva();
	test_frame_mapping_change_retires_by_section();
	test_executable_mapping_invalidates_predictors();
	test_attribute_change_reaches_private_mapping_forms();
	test_typical_image_uses_large_descriptors();
	test_large_alias_splits_on_attribute_change();
	test_orphaned_as_lock_does_not_deadlock_destroy();
	test_firmware_text_is_l2_before_launch();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
