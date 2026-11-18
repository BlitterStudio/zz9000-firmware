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

#define TEST_MAX_APPS 320U
#define TEST_MAX_L2   1200U
/* Static, so table addresses stay below 4 GiB (descriptors are 32 bits). */
static uint8_t pool_mem[ZZAPP_AS_POOL_BYTES(TEST_MAX_APPS, TEST_MAX_L2)]
	__attribute__((aligned(0x4000)));

static void setup_pool(uint32_t apps, uint32_t l2_total)
{
	uint32_t i;

	CHECK(zzapp_as_pool_init(pool_mem, sizeof(pool_mem), apps,
				 l2_total) == 0);
	for (i = 0U; i < 0x400U; i++)
		bsp[i] = (i << 20) | DDR_WB;
	bsp[0x400U] = 0x40000000U | FPGA;
	bsp[0xe00U] = 0xe0000000U | DEVICE;
	bsp[0xfffU] = 0xfff00000U | OCM;
	zzapp_as_init(bsp);
}

/* The pool the fixed slab used to be: four address spaces, 128 tables. */
static void setup(void)
{
	setup_pool(4U, 128U);
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

	CHECK(zzapp_as_create(&as, 0U) == 0);
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

	CHECK(zzapp_as_create(&as, 2U) == 0);
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

	CHECK(zzapp_as_create(&first, 0U) == 0);
	CHECK(zzapp_as_create(&second, 3U) == 0);
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

	CHECK(zzapp_as_create(&as, 1U) == 0);
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

	CHECK(zzapp_as_create(&as, 0U) == 0);
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

	CHECK(zzapp_as_create(&as, 0U) == 0);
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

	CHECK(zzapp_as_create(&as, 0U) == 0);
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

	CHECK(zzapp_as_create(&as, 0U) == 0);
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

/* Three spaces and the template take every table of a 128-table pool; the
 * fourth space's mapping is refused and leaves its L1 untouched. */
static void test_alias_l2_exhaustion_is_atomic(void)
{
	zzapp_as_t first;
	zzapp_as_t second;
	zzapp_as_t third;
	zzapp_as_t fourth;
	uint32_t pages[256];
	uint32_t before[4096];
	uint32_t va;
	uint32_t bytes = ZZAPP_AS_ALIAS_END - ZZAPP_AS_ALIAS_BASE;
	unsigned i;

	setup();
	for (i = 0U; i < 256U; i++)
		pages[i] = (5U << 20) | (i << 12) | 0x576U;
	zzapp_as_mmu_page_changed(5U, 1U | (15U << 5), pages);
	CHECK(zzapp_as_create(&first, 0U) == 0);
	CHECK(zzapp_as_create(&second, 1U) == 0);
	CHECK(zzapp_as_create(&third, 2U) == 0);
	CHECK(zzapp_as_create(&fourth, 3U) == 0);
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

	CHECK(zzapp_as_create(&as, 0U) == 0);
	before = zzapp_as_test_kernel_switches;
	test_cpu = 0;
	zzapp_as_destroy(&as);
	CHECK(zzapp_as_test_kernel_switches == before);

	CHECK(zzapp_as_create(&as, 0U) == 0);
	test_cpu = 1;
	zzapp_as_destroy(&as);
	CHECK(zzapp_as_test_kernel_switches == before + 1U);
	test_cpu = 0;
}

static void test_orphaned_as_lock_does_not_deadlock_destroy(void)
{
	zzapp_as_t as;

	CHECK(zzapp_as_create(&as, 1U) == 0);
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
	CHECK(zzapp_as_create(&first, 0U) == 0);
	zzapp_as_switch(&first);
	zzapp_as_test_asid_invalidations = 0U;
	zzapp_as_destroy(&first);
	CHECK(zzapp_as_test_asid_invalidations == 1U);
	CHECK(zzapp_as_create(&second, 0U) == 0);
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
	CHECK(zzapp_as_create(&as, 0U) == 0);
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
	CHECK(zzapp_as_create(&as, 1U) == 0);
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
	CHECK(zzapp_as_create(&as, 0U) == 0);
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
	CHECK(zzapp_as_create(&as, 2U) == 0);
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
	CHECK(zzapp_as_create(&as, 0U) == 0);
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
	CHECK(zzapp_as_create(&as, 0U) == 0);
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

/* ---- Table pool, reserves, ASIDs, alias extents (slice 5b) -------------- */

static zzapp_as_t big[300];

static void stats_get(zzapp_as_stats_t *out)
{
	zzapp_as_stats(out);
}

static int stats_equal_except_refused(const zzapp_as_stats_t *a,
				      const zzapp_as_stats_t *b)
{
	zzapp_as_stats_t x = *a;
	zzapp_as_stats_t y = *b;

	x.tables_refused = 0U;
	y.tables_refused = 0U;
	return memcmp(&x, &y, sizeof(x)) == 0;
}

static void test_pool_init_validation(void)
{
	static uint8_t small[ZZAPP_AS_POOL_BYTES(2U, 8U) + ZZAPP_AS_L1_BYTES]
		__attribute__((aligned(0x4000)));
	const uint32_t need = ZZAPP_AS_POOL_BYTES(2U, 8U);

	CHECK(zzapp_as_pool_bytes(2U, 8U) == need);
	CHECK(zzapp_as_pool_init(small, need, 2U, 8U) == 0);
	/* Big enough, but not aligned: refused. */
	CHECK(zzapp_as_pool_init(small + 4096U, need, 2U, 8U) != 0);
	CHECK(zzapp_as_pool_init(small, need - 1U, 2U, 8U) != 0);
	CHECK(zzapp_as_pool_init(small, need, 0U, 8U) != 0);
	CHECK(zzapp_as_pool_init(small, need, 2U, 0U) != 0);
	CHECK(zzapp_as_pool_init(small, need, ZZAPP_AS_MAX_APPS + 1U, 8U) != 0);
	CHECK(zzapp_as_pool_init(NULL, need, 2U, 8U) != 0);
	CHECK(zzapp_as_pool_l1(0U) == (uint32_t *)(small +
						    ZZAPP_AS_L1_BYTES));
	CHECK(zzapp_as_pool_l1(1U) == (uint32_t *)(small +
						    2U * ZZAPP_AS_L1_BYTES));
	CHECK(zzapp_as_pool_l1(2U) == NULL);
	/* A pid outside the pool is refused. */
	setup_pool(4U, 128U);
	CHECK(zzapp_as_create(&big[0], 4U) != 0);
	CHECK(zzapp_as_create(&big[0], 0xffffffffU) != 0);
}

/* 255 ASIDs with distinct owners, then steals, re-acquires and frees across
 * 300 records. Every ASID generation only grows and no two live owners ever
 * share an ASID. */
static void test_asid_300_run(void)
{
	uint32_t last_gen[ZZAPP_AS_ASID_MAX + 1U];
	uint8_t used[ZZAPP_AS_ASID_MAX + 1U];
	uint32_t i;
	unsigned before;
	unsigned expected_inval;
	uint32_t victim_asid;
	uint32_t old_gen;
	uint32_t running;
	int a;
	int valid;
	zzapp_as_stats_t st;

	setup_pool(300U, 64U);
	for (i = 0U; i < 300U; i++)
		CHECK(zzapp_as_create(&big[i], i) == 0);
	memset(last_gen, 0, sizeof(last_gen));
	memset(used, 0, sizeof(used));
	before = zzapp_as_test_asid_invalidations;

	/* 255 distinct ASIDs for 255 distinct owners, no flush needed. */
	for (i = 0U; i < ZZAPP_AS_ASID_MAX; i++) {
		CHECK(!zzapp_as_asid_valid(&big[i]));
		a = zzapp_as_acquire_asid(&big[i]);
		CHECK(a >= 1 && a <= (int)ZZAPP_AS_ASID_MAX);
		if (a >= 1 && a <= (int)ZZAPP_AS_ASID_MAX) {
			CHECK(!used[a]);
			used[a] = 1U;
			CHECK(big[i].asid == (uint32_t)a);
			CHECK(big[i].asid_gen > last_gen[a] || last_gen[a] == 0U);
			last_gen[a] = big[i].asid_gen;
		}
		CHECK(zzapp_as_asid_valid(&big[i]));
	}
	CHECK(zzapp_as_test_asid_invalidations == before);
	stats_get(&st);
	CHECK(st.asid_assigned == ZZAPP_AS_ASID_MAX && st.asid_steals == 0U);

	/* Dispatching a record makes it the most recently used. */
	CHECK(zzapp_as_acquire_asid(&big[0]) == (int)big[0].asid);
	CHECK(zzapp_as_test_asid_invalidations == before);

	/* The 256th owner steals the least recently dispatched ASID (record 1,
	 * since record 0 was just dispatched) with exactly one flush of it. */
	victim_asid = big[1].asid;
	old_gen = big[1].asid_gen;
	a = zzapp_as_acquire_asid(&big[255]);
	CHECK((uint32_t)a == victim_asid);
	CHECK(zzapp_as_test_asid_invalidations == before + 1U);
	CHECK(zzapp_as_test_last_invalidated_asid == victim_asid);
	CHECK(!zzapp_as_asid_valid(&big[1]));
	CHECK(zzapp_as_asid_valid(&big[255]));
	CHECK(big[255].asid_gen > old_gen);
	last_gen[victim_asid] = big[255].asid_gen;
	stats_get(&st);
	CHECK(st.asid_assigned == ZZAPP_AS_ASID_MAX && st.asid_steals == 1U);

	/* The victim comes back: it steals the next least recent (record 2) and
	 * its generation is higher than the one it held before. */
	old_gen = big[1].asid_gen;
	victim_asid = big[2].asid;
	a = zzapp_as_acquire_asid(&big[1]);
	CHECK((uint32_t)a == victim_asid);
	CHECK(big[1].asid_gen > old_gen);
	CHECK(zzapp_as_test_asid_invalidations == before + 2U);
	CHECK(zzapp_as_test_last_invalidated_asid == victim_asid);
	CHECK(!zzapp_as_asid_valid(&big[2]));
	last_gen[victim_asid] = big[1].asid_gen;

	/* The running record is never the victim, even as the oldest. */
	for (i = 0U; i < 300U; i++)
		if (zzapp_as_asid_valid(&big[i]))
			break;
	running = i;
	zzapp_as_switch(&big[running]);
	for (i = 0U; i < 300U; i++)
		if (i != running && zzapp_as_asid_valid(&big[i]))
			CHECK(zzapp_as_acquire_asid(&big[i]) > 0);
	/* big[running] is now the least recently dispatched owner. */
	victim_asid = big[running].asid;
	for (i = 0U; i < 300U; i++)
		if (!zzapp_as_asid_valid(&big[i]))
			break;
	a = zzapp_as_acquire_asid(&big[i]);
	CHECK(a > 0 && (uint32_t)a != victim_asid);
	if (a > 0)
		last_gen[a] = big[i].asid_gen;
	CHECK(zzapp_as_asid_valid(&big[running]));
	CHECK(big[running].asid == victim_asid);
	zzapp_as_switch_kernel();

	/* Free every record: one flush per valid owner, none for the rest. */
	before = zzapp_as_test_asid_invalidations;
	expected_inval = 0U;
	for (i = 0U; i < 300U; i++)
		if (zzapp_as_asid_valid(&big[i]))
			expected_inval++;
	CHECK(expected_inval == ZZAPP_AS_ASID_MAX);
	for (i = 0U; i < 300U; i++)
		zzapp_as_release_asid(&big[i]);
	CHECK(zzapp_as_test_asid_invalidations == before + expected_inval);
	stats_get(&st);
	CHECK(st.asid_assigned == 0U);
	for (i = 0U; i < 300U; i++)
		CHECK(!zzapp_as_asid_valid(&big[i]) && big[i].asid == 0U);

	/* Re-acquire all 300: 255 from the free stack, 45 steals. Generations
	 * of an ASID only ever increase and live owners never share one. */
	for (i = 0U; i < 300U; i++) {
		a = zzapp_as_acquire_asid(&big[i]);
		CHECK(a >= 1 && a <= (int)ZZAPP_AS_ASID_MAX);
		if (a >= 1 && a <= (int)ZZAPP_AS_ASID_MAX) {
			CHECK(big[i].asid_gen > last_gen[a]);
			last_gen[a] = big[i].asid_gen;
		}
	}
	memset(used, 0, sizeof(used));
	valid = 0;
	for (i = 0U; i < 300U; i++) {
		if (!zzapp_as_asid_valid(&big[i]))
			continue;
		valid++;
		CHECK(!used[big[i].asid]);
		used[big[i].asid] = 1U;
	}
	CHECK(valid == (int)ZZAPP_AS_ASID_MAX);
	for (i = 0U; i < 300U; i++)
		zzapp_as_destroy(&big[i]);
	stats_get(&st);
	CHECK(st.asid_assigned == 0U);
}

static void test_lazy_asid_and_switch(void)
{
	zzapp_as_t a;
	zzapp_as_t b;

	setup();
	CHECK(zzapp_as_create(&a, 0U) == 0);
	CHECK(zzapp_as_create(&b, 1U) == 0);
	CHECK(a.asid == 0U && !zzapp_as_asid_valid(&a));
	zzapp_as_switch_kernel();
	CHECK(zzapp_as_test_current_asid() == 0U);
	zzapp_as_switch(&a);
	CHECK(zzapp_as_asid_valid(&a) && a.asid != 0U);
	CHECK(zzapp_as_test_current_asid() == a.asid);
	zzapp_as_switch(&b);
	CHECK(b.asid != 0U && b.asid != a.asid);
	CHECK(zzapp_as_test_current_asid() == b.asid);
	zzapp_as_switch_kernel();
	CHECK(zzapp_as_test_current_asid() == 0U);
	/* Whole-TLB recovery frees every ASID and invalidates the old ones. */
	{
		unsigned all = zzapp_as_test_tlb_all_invalidations;
		zzapp_as_stats_t st;

		zzapp_as_reset_asids();
		CHECK(zzapp_as_test_tlb_all_invalidations == all + 1U);
		CHECK(!zzapp_as_asid_valid(&a) && !zzapp_as_asid_valid(&b));
		stats_get(&st);
		CHECK(st.asid_assigned == 0U);
		zzapp_as_switch(&a);
		CHECK(zzapp_as_asid_valid(&a));
	}
	zzapp_as_destroy(&a);
	zzapp_as_destroy(&b);
}

/* Reserve admission, draws from the reserve then the shared remainder, a
 * refusal that leaves the space intact, and full return on destroy. */
static void test_l2_reserve_and_exhaustion(void)
{
	zzapp_as_t a;
	zzapp_as_t b;
	zzapp_as_stats_t s0;
	zzapp_as_stats_t s1;
	zzapp_as_stats_t s2;
	uint32_t before[4096];
	uint32_t va;

	setup_pool(8U, 40U);
	stats_get(&s0);
	CHECK(s0.l2_total == 40U && s0.l2_free == 40U && s0.l2_reserved == 0U);
	CHECK(s0.l2_shared_free == 40U);
	CHECK(zzapp_as_reserve_l2(0U) == 0);
	CHECK(zzapp_as_reserve_l2(1U) == 0);
	stats_get(&s1);
	CHECK(s1.l2_reserved == 32U && s1.l2_reserved_unused == 32U);
	CHECK(s1.l2_free == 40U && s1.l2_shared_free == 8U);
	/* The third claim does not fit and changes nothing. */
	CHECK(zzapp_as_reserve_l2(2U) == ZZAPP_AS_ETABLES);
	stats_get(&s2);
	CHECK(memcmp(&s1, &s2, sizeof(s1)) == 0);
	CHECK(zzapp_as_reserve_l2(0U) == -1);
	CHECK(zzapp_as_reserve_l2(8U) == -1);

	CHECK(zzapp_as_create(&a, 0U) == 0);
	CHECK(zzapp_as_create(&b, 1U) == 0);
	/* 20 tables: 16 from a's reserve, 4 from the shared remainder. */
	CHECK(zzapp_as_alias_map(&a, 0x03400000U, 20U << 20, 1, &va) == 0);
	stats_get(&s2);
	CHECK(s2.l2_free == 20U && s2.l2_reserved_unused == 16U);
	CHECK(s2.l2_shared_free == 4U);
	/* b still gets its whole reserve although a drained the pool. */
	CHECK(zzapp_as_alias_map(&b, 0x03400000U, 16U << 20, 1, &va) == 0);
	stats_get(&s2);
	CHECK(s2.l2_free == 4U && s2.l2_reserved_unused == 0U);
	CHECK(s2.l2_shared_free == 4U);
	/* Beyond the reserve: the shared remainder, then a refusal. */
	memcpy(before, zzapp_as_l1(&a), sizeof(before));
	CHECK(zzapp_as_alias_map(&a, 0x03400000U, 5U << 20, 1, &va) ==
	      ZZAPP_AS_ENOMEM);
	CHECK(memcmp(before, zzapp_as_l1(&a), sizeof(before)) == 0);
	{
		unsigned extents = a.alias_count;

		stats_get(&s1);
		CHECK(s1.tables_refused > s2.tables_refused);
		CHECK(s1.l2_free == 4U);
		CHECK(extents == 1U);
	}
	CHECK(zzapp_as_alias_map(&a, 0x03400000U, 4U << 20, 1, &va) == 0);
	stats_get(&s2);
	CHECK(s2.l2_free == 0U && s2.l2_shared_free == 0U);
	CHECK(zzapp_as_alias_map(&a, 0x03400000U, 4096U, 1, &va) ==
	      ZZAPP_AS_ENOMEM);
	/* Destroying returns every table; the reserve stays until released. */
	zzapp_as_destroy(&a);
	zzapp_as_destroy(&b);
	stats_get(&s2);
	CHECK(s2.l2_free == 40U && s2.l2_reserved == 32U &&
	      s2.l2_reserved_unused == 32U);
	CHECK(zzapp_as_release_l2(0U) == 0);
	CHECK(zzapp_as_release_l2(1U) == 0);
	CHECK(zzapp_as_release_l2(1U) == 0);
	stats_get(&s2);
	CHECK(stats_equal_except_refused(&s0, &s2));
}

static void test_release_l2_refuses_live_space(void)
{
	zzapp_as_t a;
	zzapp_as_stats_t s0;
	zzapp_as_stats_t s1;
	uint32_t va;

	setup_pool(4U, 64U);
	stats_get(&s0);
	CHECK(zzapp_as_reserve_l2(0U) == 0);
	CHECK(zzapp_as_create(&a, 0U) == 0);
	CHECK(zzapp_as_alias_map(&a, 0x03400000U, 3U << 20, 1, &va) == 0);
	CHECK(zzapp_as_release_l2(0U) == -1);
	stats_get(&s1);
	CHECK(s1.l2_free == 61U && s1.l2_reserved == 16U);
	zzapp_as_destroy(&a);
	CHECK(zzapp_as_release_l2(0U) == 0);
	stats_get(&s1);
	CHECK(stats_equal_except_refused(&s0, &s1));
	/* A second create of a live pid is refused. */
	CHECK(zzapp_as_create(&a, 0U) == 0);
	CHECK(zzapp_as_create(&big[0], 0U) != 0);
	zzapp_as_destroy(&a);
	stats_get(&s1);
	CHECK(stats_equal_except_refused(&s0, &s1));
}

/* The pool's bookkeeping is rebuilt from the owner marks. */
static void test_reconcile_rebuilds_pool(void)
{
	zzapp_as_t a;
	zzapp_as_t b;
	zzapp_as_stats_t before;
	zzapp_as_stats_t scrambled;
	zzapp_as_stats_t after;
	uint32_t va;

	setup_pool(8U, 64U);
	CHECK(zzapp_as_reserve_l2(0U) == 0);
	CHECK(zzapp_as_reserve_l2(1U) == 0);
	CHECK(zzapp_as_create(&a, 0U) == 0);
	CHECK(zzapp_as_create(&b, 1U) == 0);
	CHECK(zzapp_as_alias_map(&a, 0x03400000U, 5U << 20, 1, &va) == 0);
	CHECK(zzapp_as_alias_map(&b, 0x03400000U, 20U << 20, 1, &va) == 0);
	stats_get(&before);
	zzapp_as_test_scramble_pool();
	stats_get(&scrambled);
	CHECK(memcmp(&before, &scrambled, sizeof(before)) != 0);
	zzapp_as_reconcile();
	stats_get(&after);
	CHECK(memcmp(&before, &after, sizeof(before)) == 0);
	/* The rebuilt lists free exactly what each owner holds. */
	zzapp_as_destroy(&a);
	stats_get(&after);
	CHECK(after.l2_free == before.l2_free + 5U);
	zzapp_as_destroy(&b);
	stats_get(&after);
	CHECK(after.l2_free == 64U);
	CHECK(zzapp_as_release_l2(0U) == 0);
	CHECK(zzapp_as_release_l2(1U) == 0);
	stats_get(&after);
	CHECK(after.l2_reserved == 0U && after.l2_reserved_unused == 0U &&
	      after.l2_free == 64U);
}

static unsigned fault_calls;
static uint32_t fault_pid;

static void on_table_fault(uint32_t pid)
{
	fault_calls++;
	fault_pid = pid;
}

/* The BSP-change hook cannot report a missing table to anyone: it marks the
 * record faulted, tells the host once, and leaves the private mapping as it
 * was. */
static void test_hook_without_tables_faults_the_record(void)
{
	zzapp_as_t as;
	zzapp_as_stats_t st;
	uint32_t pages[256];
	uint32_t frame = 0x12000000U;
	uint32_t l1_before;
	unsigned i;

	setup_pool(2U, 1U);
	zzapp_as_set_table_fault_hook(on_table_fault);
	fault_calls = 0U;
	CHECK(zzapp_as_create(&as, 1U) == 0);
	CHECK(zzapp_as_map_frames(&as, ZZAPP_AS_WINDOW_BASE, &frame, 1U) == 0);
	l1_before = zzapp_as_l1(&as)[0xc00U];
	for (i = 0U; i < 256U; i++)
		pages[i] = (0x120U << 20) | (i << 12) | 0x576U;
	/* The template takes the only table; the record has none to take. */
	zzapp_as_mmu_page_changed(0x120U, 1U | (15U << 5), pages);
	CHECK(zzapp_as_table_faulted(1U));
	CHECK(!zzapp_as_table_faulted(0U));
	CHECK(fault_calls == 1U && fault_pid == 1U);
	CHECK(zzapp_as_l1(&as)[0xc00U] == l1_before);
	stats_get(&st);
	CHECK(st.tables_refused >= 1U);
	/* Told once, not on every later change. */
	zzapp_as_mmu_page_changed(0x120U, 1U | (15U << 5), pages);
	CHECK(fault_calls == 1U);
	zzapp_as_destroy(&as);
	CHECK(!zzapp_as_table_faulted(1U));

	/* With a table to spare the same change reaches the record. */
	setup_pool(2U, 3U);
	fault_calls = 0U;
	CHECK(zzapp_as_create(&as, 1U) == 0);
	CHECK(zzapp_as_map_frames(&as, ZZAPP_AS_WINDOW_BASE, &frame, 1U) == 0);
	zzapp_as_mmu_page_changed(0x120U, 1U | (15U << 5), pages);
	CHECK(!zzapp_as_table_faulted(1U) && fault_calls == 0U);
	CHECK(zzapp_as_l2_for(&as, ZZAPP_AS_WINDOW_BASE) != NULL);
	zzapp_as_destroy(&as);
	zzapp_as_set_table_fault_hook(NULL);
}

/* 64 live spaces: the hook scans the 512 private sections of each and walks
 * each owned L2 list. */
static void test_hook_with_64_live_spaces(void)
{
	const uint32_t noncache = (0x120U << 20) | 0x11de2U;
	const uint32_t mask = (7U << 12) | (1U << 3) | (1U << 2) | (1U << 16);
	const uint32_t mem_type = (7U << 6) | (1U << 3) | (1U << 2);
	uint32_t alias[64];
	uint32_t frame;
	uint32_t i;

	setup_pool(64U, 300U);
	for (i = 0U; i < 64U; i++) {
		uint32_t pid = 63U - i;

		CHECK(zzapp_as_create(&big[pid], pid) == 0);
		frame = (0x120U + (pid & 1U)) << 20;
		CHECK(zzapp_as_map_frames(&big[pid], ZZAPP_AS_WINDOW_BASE +
					  (pid & 3U) * 0x00100000U, &frame,
					  1U) == 0);
		CHECK(zzapp_as_alias_map(&big[pid], 0x03405000U, 4096U, 1,
					 &alias[pid]) == 0);
	}
	/* Section 0x120 changes memory type; section 0x034 too (aliases). */
	zzapp_as_mmu_page_changed(0x120U, noncache, NULL);
	zzapp_as_mmu_page_changed(0x034U, 0x03400000U | 0x11de2U, NULL);
	for (i = 0U; i < 64U; i++) {
		const uint32_t *l1 = zzapp_as_l1(&big[i]);
		uint32_t window = ZZAPP_AS_WINDOW_BASE + (i & 3U) * 0x00100000U;
		uint32_t d = l1[window >> 20];
		const uint32_t *l2 = zzapp_as_l2_for(&big[i], alias[i]);

		if (i & 1U) {
			/* Maps section 0x121: untouched. */
			CHECK((d & 0xfff00000U) == 0x12100000U);
			CHECK((d & mask) == (DDR_WB & mask));
		} else {
			CHECK((d & 0xfff00000U) == 0x12000000U);
			CHECK((d & mask) == (noncache & mask));
		}
		CHECK(l2 != NULL);
		CHECK(l2 && (l2[(alias[i] >> 12) & 0xffU] & mem_type) ==
		      (1U << 6));
	}
	for (i = 0U; i < 64U; i++)
		zzapp_as_destroy(&big[i]);
}

/* A private section that the BSP replaces by a page table becomes an owned
 * L2 table in every record that maps it. */
static void test_hook_converts_private_sections_to_tables(void)
{
	uint32_t pages[256];
	uint32_t frame = 0x12000000U;
	uint32_t i;
	const uint32_t *l2;

	setup_pool(4U, 64U);
	for (i = 0U; i < 256U; i++)
		pages[i] = (0x120U << 20) | (i << 12) | 0x576U;
	CHECK(zzapp_as_create(&big[0], 0U) == 0);
	CHECK(zzapp_as_create(&big[1], 1U) == 0);
	CHECK(zzapp_as_map_frames(&big[0], ZZAPP_AS_WINDOW_BASE, &frame, 1U) == 0);
	CHECK(zzapp_as_map_frames(&big[1], ZZAPP_AS_WINDOW_BASE + 0x00200000U,
				  &frame, 1U) == 0);
	zzapp_as_mmu_page_changed(0x120U, 1U | (15U << 5), pages);
	l2 = zzapp_as_l2_for(&big[0], ZZAPP_AS_WINDOW_BASE);
	CHECK(l2 != NULL);
	CHECK(l2 && (l2[7] & 0xfffff000U) == 0x12007000U);
	CHECK(l2 && (l2[7] & PAGE_NG) != 0U && (l2[7] & PAGE_AP_MASK) == PAGE_USER_RW);
	l2 = zzapp_as_l2_for(&big[1], ZZAPP_AS_WINDOW_BASE + 0x00200000U);
	CHECK(l2 != NULL);
	CHECK(l2 && (l2[9] & 0xfffff000U) == 0x12009000U);
	zzapp_as_destroy(&big[0]);
	zzapp_as_destroy(&big[1]);
}

/* Alias placement is first fit by extent: a freed gap is reused, a gap too
 * small is skipped, an unmap must name the extent exactly, and a space holds
 * a bounded number of extents. */
static void test_alias_extents(void)
{
	zzapp_as_t as;
	uint32_t a;
	uint32_t b;
	uint32_t c;
	uint32_t d;
	uint32_t e;
	uint32_t i;
	uint32_t va;
	uint32_t last = 0U;

	setup();
	CHECK(zzapp_as_create(&as, 0U) == 0);
	CHECK(zzapp_as_alias_map(&as, 0x03400000U, 4096U, 1, &a) == 0);
	CHECK(zzapp_as_alias_map(&as, 0x03401000U, 4096U, 1, &b) == 0);
	CHECK(zzapp_as_alias_map(&as, 0x03402000U, 4096U, 1, &c) == 0);
	CHECK(a == ZZAPP_AS_ALIAS_BASE && b == a + 4096U && c == b + 4096U);
	CHECK(as.alias_count == 3U);
	CHECK(zzapp_as_alias_unmap(&as, b, 4096U) == 0);
	CHECK(as.alias_count == 2U);
	CHECK(zzapp_as_alias_phys(&as, b) == 0U);
	/* The hole fits one page, not two. */
	CHECK(zzapp_as_alias_map(&as, 0x03403000U, 8192U, 1, &e) == 0);
	CHECK(e == c + 4096U);
	CHECK(zzapp_as_alias_map(&as, 0x03405000U, 4096U, 1, &d) == 0);
	CHECK(d == b);
	CHECK(zzapp_as_alias_phys(&as, d) == 0x03405000U);
	for (i = 0U; i + 1U < as.alias_count; i++)
		CHECK(as.alias[i].first + as.alias[i].pages <=
		      as.alias[i + 1U].first);
	/* An unmap names an extent exactly: start and length. */
	CHECK(zzapp_as_alias_unmap(&as, e + 4096U, 4096U) != 0);
	CHECK(zzapp_as_alias_unmap(&as, e, 4096U) != 0);
	CHECK(zzapp_as_alias_unmap(&as, e, 3U * 4096U) != 0);
	CHECK(zzapp_as_alias_phys(&as, e + 4096U) == 0x03404000U);
	CHECK(zzapp_as_alias_unmap(&as, e, 8192U) == 0);
	CHECK(zzapp_as_alias_unmap(&as, e, 8192U) != 0);
	CHECK(zzapp_as_alias_unmap(&as, a, 4096U) == 0);
	CHECK(zzapp_as_alias_unmap(&as, c, 4096U) == 0);
	CHECK(zzapp_as_alias_unmap(&as, d, 4096U) == 0);
	CHECK(as.alias_count == 0U);
	/* The extent table is bounded. */
	for (i = 0U; i < ZZAPP_AS_ALIAS_EXTENTS; i++) {
		CHECK(zzapp_as_alias_map(&as, 0x03400000U + i * 4096U, 4096U,
					 1, &va) == 0);
		CHECK(va > last || i == 0U);
		last = va;
	}
	CHECK(zzapp_as_alias_map(&as, 0x03500000U, 4096U, 1, &va) ==
	      ZZAPP_AS_ENOMEM);
	CHECK(zzapp_as_alias_unmap(&as, ZZAPP_AS_ALIAS_BASE + 5U * 4096U,
				   4096U) == 0);
	CHECK(zzapp_as_alias_map(&as, 0x03500000U, 4096U, 1, &va) == 0);
	CHECK(va == ZZAPP_AS_ALIAS_BASE + 5U * 4096U);
	zzapp_as_destroy(&as);
	CHECK(as.alias_count == 0U);
	/* A new space of the same pid starts with no extents. */
	CHECK(zzapp_as_create(&as, 0U) == 0);
	CHECK(as.alias_count == 0U);
	CHECK(zzapp_as_alias_map(&as, 0x03400000U, 4096U, 1, &va) == 0);
	CHECK(va == ZZAPP_AS_ALIAS_BASE);
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
	test_pool_init_validation();
	test_asid_300_run();
	test_lazy_asid_and_switch();
	test_l2_reserve_and_exhaustion();
	test_release_l2_refuses_live_space();
	test_reconcile_rebuilds_pool();
	test_hook_without_tables_faults_the_record();
	test_hook_with_64_live_spaces();
	test_hook_converts_private_sections_to_tables();
	test_alias_extents();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
