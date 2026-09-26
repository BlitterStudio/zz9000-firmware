/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Host coverage for the GEM multicast-hash register contract.
 * Build/run: make -C test/ethernet test
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xemacps_hw.h"
#include "ethernet.h"
#include "zz_regs.h"

#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", \
		        __FILE__, __LINE__, #expr); \
		exit(EXIT_FAILURE); \
	} \
} while (0)

#define GEM_BASE 0xE000B000U
#define NWCFG_OTHER 0x00001234U

static u32 regs[256];
static u32 emac_base;
static int read_count;
static int write_count;
static int log_count;
static int restart_count;
static int pause_count;
static int clear_count;
static int resume_count;
static const char *last_restart;

int ethernet_task_state = ETH_TASK_SETUP;

u32 ethernet_emac_base(void)
{
	return emac_base;
}

u32 XEmacPs_ReadReg(u32 BaseAddress, u32 RegOffset)
{
	read_count++;
	CHECK(BaseAddress == GEM_BASE);
	CHECK(RegOffset < sizeof(regs) / sizeof(regs[0]));
	return regs[RegOffset / 4];
}

void XEmacPs_WriteReg(u32 BaseAddress, u32 RegOffset, u32 Data)
{
	write_count++;
	if (BaseAddress != GEM_BASE || RegOffset >= sizeof(regs)) {
		fprintf(stderr, "unexpected GEM write %08x+%02x\n",
		        BaseAddress, RegOffset);
		exit(EXIT_FAILURE);
	}
	regs[RegOffset / 4] = Data;
}

void ethernet_log_status(const char *reason)
{
	(void)reason;
	log_count++;
}

int ethernet_restart_dma(const char *reason)
{
	restart_count++;
	last_restart = reason;
	return 0;
}

int ethernet_pause_rx_irq(void)
{
	pause_count++;
	return 1;
}

void ethernet_clear_host_state(void)
{
	clear_count++;
}

void ethernet_resume_rx_irq(int paused)
{
	resume_count++;
	CHECK(paused == 1);
}

static void gem_reset(void)
{
	memset(regs, 0, sizeof(regs));
	regs[XEMACPS_NWCFG_OFFSET / 4] = NWCFG_OTHER;
	emac_base = GEM_BASE;
	read_count = 0;
	write_count = 0;
	log_count = 0;
	restart_count = 0;
	pause_count = 0;
	clear_count = 0;
	resume_count = 0;
	last_restart = 0;
	ethernet_task_state = ETH_TASK_SETUP;
}

static u32 hashl(void)
{
	return regs[XEMACPS_HASHL_OFFSET / 4];
}

static u32 hashh(void)
{
	return regs[XEMACPS_HASHH_OFFSET / 4];
}

static u32 nwcfg(void)
{
	return regs[XEMACPS_NWCFG_OFFSET / 4];
}

static int gate_on(void)
{
	return (nwcfg() & XEMACPS_NWCFG_MCASTHASHEN_MASK) != 0;
}

static int test_bucket_halves_and_set_preserves_bits(void)
{
	gem_reset();
	regs[XEMACPS_HASHL_OFFSET / 4] = 0x00000002U;
	regs[XEMACPS_HASHH_OFFSET / 4] = 0x00000008U;

	ethernet_set_multicast_hash(ETH_CONFIG_HASH_SET | 0);
	CHECK(hashl() == 0x00000003U);
	CHECK(hashh() == 0x00000008U);
	CHECK(gate_on());
	CHECK((nwcfg() & ~XEMACPS_NWCFG_MCASTHASHEN_MASK) == NWCFG_OTHER);

	ethernet_set_multicast_hash(ETH_CONFIG_HASH_SET | 31);
	CHECK(hashl() == (0x00000003U | (1U << 31)));
	CHECK(hashh() == 0x00000008U);

	ethernet_set_multicast_hash(ETH_CONFIG_HASH_SET | 32);
	CHECK(hashh() == 0x00000009U);
	CHECK(hashl() == (0x00000003U | (1U << 31)));

	ethernet_set_multicast_hash(ETH_CONFIG_HASH_SET | 63);
	CHECK(hashh() == (0x00000009U | (1U << 31)));
	CHECK(hashl() == (0x00000003U | (1U << 31)));
	CHECK(gate_on());
	CHECK((nwcfg() & ~XEMACPS_NWCFG_MCASTHASHEN_MASK) == NWCFG_OTHER);
	return EXIT_SUCCESS;
}

static int test_clear_keeps_gate_until_both_halves_empty(void)
{
	gem_reset();
	ethernet_set_multicast_hash(ETH_CONFIG_HASH_SET | 0);
	ethernet_set_multicast_hash(ETH_CONFIG_HASH_SET | 31);
	ethernet_set_multicast_hash(ETH_CONFIG_HASH_SET | 32);
	ethernet_set_multicast_hash(ETH_CONFIG_HASH_SET | 63);

	ethernet_set_multicast_hash(ETH_CONFIG_HASH_CLEAR | 0);
	CHECK((hashl() & 1U) == 0);
	CHECK((hashl() & (1U << 31)) != 0);
	CHECK(hashh() == ((1U << 0) | (1U << 31)));
	CHECK(gate_on());
	CHECK((nwcfg() & ~XEMACPS_NWCFG_MCASTHASHEN_MASK) == NWCFG_OTHER);

	ethernet_set_multicast_hash(ETH_CONFIG_HASH_CLEAR | 31);
	CHECK(hashl() == 0);
	CHECK(hashh() != 0);
	CHECK(gate_on());

	ethernet_set_multicast_hash(ETH_CONFIG_HASH_CLEAR | 32);
	CHECK(hashh() == (1U << 31));
	CHECK(gate_on());

	ethernet_set_multicast_hash(ETH_CONFIG_HASH_CLEAR | 63);
	CHECK(hashl() == 0);
	CHECK(hashh() == 0);
	CHECK(!gate_on());
	CHECK(nwcfg() == NWCFG_OTHER);
	return EXIT_SUCCESS;
}

static int test_reset_and_amiga_reset_clear_gate(void)
{
	gem_reset();
	ethernet_set_multicast_hash(ETH_CONFIG_HASH_SET | 31);
	ethernet_set_multicast_hash(ETH_CONFIG_HASH_SET | 63);
	CHECK(gate_on());

	ethernet_set_multicast_hash(ETH_CONFIG_HASH_RESET);
	CHECK(hashl() == 0);
	CHECK(hashh() == 0);
	CHECK(!gate_on());
	CHECK(nwcfg() == NWCFG_OTHER);

	ethernet_set_multicast_hash(ETH_CONFIG_HASH_SET | 0);
	ethernet_set_multicast_hash(ETH_CONFIG_HASH_SET | 32);
	ethernet_task_state = ETH_TASK_SETUP;
	ethernet_reset_for_amiga();
	CHECK(hashl() == 0);
	CHECK(hashh() == 0);
	CHECK(!gate_on());
	CHECK(nwcfg() == NWCFG_OTHER);
	CHECK(log_count == 2);
	CHECK(restart_count == 0);
	CHECK(pause_count == 1);
	CHECK(clear_count == 1);
	CHECK(resume_count == 1);

	ethernet_set_multicast_hash(ETH_CONFIG_HASH_SET | 63);
	ethernet_task_state = ETH_TASK_READY;
	log_count = restart_count = pause_count = clear_count = resume_count = 0;
	ethernet_reset_for_amiga();
	CHECK(hashl() == 0);
	CHECK(hashh() == 0);
	CHECK(!gate_on());
	CHECK(restart_count == 1);
	CHECK(last_restart != 0 && strcmp(last_restart, "amiga-reset") == 0);
	CHECK(pause_count == 0);
	CHECK(clear_count == 0);
	CHECK(log_count == 2);
	return EXIT_SUCCESS;
}

static int test_unprogrammed_base_does_not_touch_registers(void)
{
	gem_reset();
	emac_base = 0;
	ethernet_set_multicast_hash(ETH_CONFIG_HASH_SET | 1);
	ethernet_reset_for_amiga();
	CHECK(read_count == 0);
	CHECK(write_count == 0);
	CHECK(hashl() == 0);
	CHECK(hashh() == 0);
	CHECK(nwcfg() == NWCFG_OTHER);
	return EXIT_SUCCESS;
}

static int test_capability_read_preserves_mac_bytes(void)
{
	uint8_t mac[6] = {0x11, 0x22, 0x33, 0x44, 0xab, 0xcd};
	uint8_t before[6];
	u32 word;
	u16 config16;

	memcpy(before, mac, sizeof(mac));
	CHECK(ethernet_get_multicast_config() == ETH_CONFIG_CAP_MULTICAST_HASH);
	CHECK(ETH_CONFIG_CAP_MULTICAST_HASH == 0x0001);
	CHECK(REG_ZZ_ETH_MAC_LO == 0x88);
	CHECK(REG_ZZ_ETH_CONFIG == 0x8A);
	CHECK((REG_ZZ_ETH_CONFIG & ~3u) == REG_ZZ_ETH_MAC_LO);

	word = ethernet_mac_lo_word(mac);
	CHECK(memcmp(mac, before, sizeof(mac)) == 0);
	CHECK(word == 0xabcd0001U);
	CHECK((word >> 16) == 0xabcdU);
	CHECK((word & 0xffffU) == 0x0001U);

	/* Z2 16-bit read of 0x8A is the low half; 0x88 is the two MAC bytes. */
	config16 = ethernet_zorro16(word, REG_ZZ_ETH_CONFIG);
	CHECK(config16 == 0x0001);
	CHECK(ethernet_zorro16(word, REG_ZZ_ETH_MAC_LO) == 0xabcd);
	CHECK(memcmp(mac, before, sizeof(mac)) == 0);
	return EXIT_SUCCESS;
}

int main(void)
{
	if (test_bucket_halves_and_set_preserves_bits() != EXIT_SUCCESS)
		return EXIT_FAILURE;
	if (test_clear_keeps_gate_until_both_halves_empty() != EXIT_SUCCESS)
		return EXIT_FAILURE;
	if (test_reset_and_amiga_reset_clear_gate() != EXIT_SUCCESS)
		return EXIT_FAILURE;
	if (test_unprogrammed_base_does_not_touch_registers() != EXIT_SUCCESS)
		return EXIT_FAILURE;
	if (test_capability_read_preserves_mac_bytes() != EXIT_SUCCESS)
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
}
