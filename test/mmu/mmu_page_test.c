/*
 * Host tests for mmu_page.c: splitting one cached DDR section so a single
 * 4 KiB page is Normal non-cacheable (the Zorro II SDK mailbox page).
 *
 * Descriptors are decoded with the ARMv7 short-descriptor field positions,
 * independently of the encodings mmu_page.c writes, and compared with the
 * section attributes the BSP uses (translation_table.S, xil_mmu.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mmu_page.h"
#include "xil_cache.h"

struct xil_cache_mock_state g_xil_cache_mock;

static int checks;
static int failures;

#define CHECK(expr) do { \
	checks++; \
	if (!(expr)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
	} \
} while (0)

/* translation_table.S DDR sections; xil_mmu.h NORM_NONCACHE/STRONG_ORDERED. */
#define BSP_DDR_WB      0x15de6U
#define BSP_NONCACHE    0x11de2U
#define BSP_STRONG      0x00c02U

struct mem_attr {
	uint32_t tex, c, b, ap, ap2, s, ng, xn;
};

static struct mem_attr section_attr(uint32_t d)
{
	struct mem_attr a;

	a.b = (d >> 2) & 1U;
	a.c = (d >> 3) & 1U;
	a.xn = (d >> 4) & 1U;
	a.ap = (d >> 10) & 3U;
	a.tex = (d >> 12) & 7U;
	a.ap2 = (d >> 15) & 1U;
	a.s = (d >> 16) & 1U;
	a.ng = (d >> 17) & 1U;
	return a;
}

static struct mem_attr small_page_attr(uint32_t d)
{
	struct mem_attr a;

	a.xn = d & 1U;
	a.b = (d >> 2) & 1U;
	a.c = (d >> 3) & 1U;
	a.ap = (d >> 4) & 3U;
	a.tex = (d >> 6) & 7U;
	a.ap2 = (d >> 9) & 1U;
	a.s = (d >> 10) & 1U;
	a.ng = (d >> 11) & 1U;
	return a;
}

static int same_attr(struct mem_attr x, struct mem_attr y)
{
	return memcmp(&x, &y, sizeof(x)) == 0;
}

static void set_section(uint32_t section, uint32_t attr)
{
	mmu_page_test_l1[section] = (section << 20) | attr;
}

static void test_refusals_change_nothing(void)
{
	set_section(0x005U, BSP_DDR_WB);
	g_xil_cache_mock.count = 0U;
	CHECK(!mmu_page_set_noncacheable(0x005df800U));   /* unaligned */
	mmu_page_test_l1[0x7c0U] = 0U;                     /* fault entry */
	CHECK(!mmu_page_set_noncacheable(0x7c000000U));
	mmu_page_test_l1[0x006U] = 0x00500000U | BSP_DDR_WB; /* not identity */
	CHECK(!mmu_page_set_noncacheable(0x006df000U));
	CHECK(mmu_page_test_l1[0x006U] == (0x00500000U | BSP_DDR_WB));
	CHECK(mmu_page_test_l1[0x005U] == (0x00500000U | BSP_DDR_WB));
	CHECK(g_xil_cache_mock.count == 0U);
}

static void test_uncached_sections_are_accepted_as_is(void)
{
	set_section(0x3fcU, BSP_NONCACHE);
	set_section(0x3feU, BSP_STRONG);
	g_xil_cache_mock.count = 0U;
	CHECK(mmu_page_set_noncacheable(0x3fc28000U));
	CHECK(mmu_page_set_noncacheable(0x3fe43000U));
	CHECK(mmu_page_test_l1[0x3fcU] == (0x3fc00000U | BSP_NONCACHE));
	CHECK(mmu_page_test_l1[0x3feU] == (0x3fe00000U | BSP_STRONG));
	CHECK(g_xil_cache_mock.count == 0U);
}

static void check_split_section(uint32_t section, const uint32_t *nc_pages,
				unsigned nc_count)
{
	const uint32_t *table = mmu_page_test_page_table();
	struct mem_attr cached = section_attr(BSP_DDR_WB);
	struct mem_attr uncached = section_attr(BSP_NONCACHE);
	uint32_t l1 = mmu_page_test_l1[section];
	uint32_t i;

	CHECK((l1 & 3U) == 1U);                 /* page-table descriptor */
	CHECK(((l1 >> 5) & 0xfU) ==
	      ((BSP_DDR_WB >> 5) & 0xfU));     /* same domain */
	for (i = 0U; i < 256U; i++) {
		uint32_t d = table[i];
		int nc = 0;
		unsigned k;

		for (k = 0U; k < nc_count; k++)
			if (nc_pages[k] == i)
				nc = 1;
		CHECK((d & 2U) == 2U);          /* small page */
		CHECK((d & 0xfffff000U) == ((section << 20) | (i << 12)));
		CHECK(same_attr(small_page_attr(d), nc ? uncached : cached));
	}
}

static void test_split_one_page(void)
{
	static const uint32_t nc[] = { 0xdfU };

	set_section(0x005U, BSP_DDR_WB);
	g_xil_cache_mock.count = 0U;
	CHECK(mmu_page_set_noncacheable(0x005df000U));
	check_split_section(0x005U, nc, 1U);

	/* The page is cleaned and invalidated while its section entry still
	 * maps it cacheable, and again once the page table is live. */
	CHECK(g_xil_cache_mock.count >= 2U);
	CHECK(g_xil_cache_mock.ops[0].adr == 0x005df000U);
	CHECK(g_xil_cache_mock.ops[0].len == 0x1000U);
	CHECK(g_xil_cache_mock.ops[0].l1_entry ==
	      (0x00500000U | BSP_DDR_WB));
	CHECK(g_xil_cache_mock.ops[g_xil_cache_mock.count - 1U].adr ==
	      0x005df000U);
	CHECK(g_xil_cache_mock.ops[g_xil_cache_mock.count - 1U].len ==
	      0x1000U);
	CHECK(g_xil_cache_mock.ops[g_xil_cache_mock.count - 1U].l1_entry ==
	      mmu_page_test_l1[0x005U]);
}

static void test_repeat_and_second_page(void)
{
	static const uint32_t nc[] = { 0xdeU, 0xdfU };
	uint32_t l1 = mmu_page_test_l1[0x005U];

	g_xil_cache_mock.count = 0U;
	CHECK(mmu_page_set_noncacheable(0x005df000U));
	CHECK(g_xil_cache_mock.count == 0U);    /* already done: no-op */

	CHECK(mmu_page_set_noncacheable(0x005de000U));
	CHECK(mmu_page_test_l1[0x005U] == l1);
	check_split_section(0x005U, nc, 2U);
}

static void test_second_cached_section_refused(void)
{
	static const uint32_t nc[] = { 0xdeU, 0xdfU };

	set_section(0x009U, BSP_DDR_WB);
	CHECK(!mmu_page_set_noncacheable(0x009df000U));
	CHECK(mmu_page_test_l1[0x009U] == (0x00900000U | BSP_DDR_WB));
	check_split_section(0x005U, nc, 2U);
}

int main(void)
{
	test_refusals_change_nothing();
	test_uncached_sections_are_accepted_as_is();
	test_split_one_page();
	test_repeat_and_second_page();
	test_second_cached_section_refused();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
