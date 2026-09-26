/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "xemacps_hw.h"
#include "ethernet.h"

u32 ethernet_emac_base(void);

void ethernet_log_status(const char *reason);
int ethernet_restart_dma(const char *reason);
int ethernet_pause_rx_irq(void);
void ethernet_clear_host_state(void);
void ethernet_resume_rx_irq(int paused);

/* Program one GEM multicast-hash bucket. The SANA-II driver owns group and
 * collision reference counts; firmware only changes the hardware bitmap. */
void ethernet_set_multicast_hash(u16 command)
{
	u32 base = ethernet_emac_base();
	u32 config, hash;
	u16 index = command & ETH_CONFIG_HASH_INDEX;

	if (!base)
		return;

	config = XEmacPs_ReadReg(base, XEMACPS_NWCFG_OFFSET);
	if (command & ETH_CONFIG_HASH_RESET) {
		XEmacPs_WriteReg(base, XEMACPS_HASHL_OFFSET, 0);
		XEmacPs_WriteReg(base, XEMACPS_HASHH_OFFSET, 0);
		config &= ~XEMACPS_NWCFG_MCASTHASHEN_MASK;
	} else if (command & ETH_CONFIG_HASH_SET) {
		u32 offset = index < 32 ? XEMACPS_HASHL_OFFSET : XEMACPS_HASHH_OFFSET;
		u32 bit = 1U << (index & 31);
		hash = XEmacPs_ReadReg(base, offset);
		XEmacPs_WriteReg(base, offset, hash | bit);
		config |= XEMACPS_NWCFG_MCASTHASHEN_MASK;
	} else if (command & ETH_CONFIG_HASH_CLEAR) {
		u32 offset = index < 32 ? XEMACPS_HASHL_OFFSET : XEMACPS_HASHH_OFFSET;
		u32 bit = 1U << (index & 31);
		hash = XEmacPs_ReadReg(base, offset);
		XEmacPs_WriteReg(base, offset, hash & ~bit);
		if (XEmacPs_ReadReg(base, XEMACPS_HASHL_OFFSET) == 0 &&
		    XEmacPs_ReadReg(base, XEMACPS_HASHH_OFFSET) == 0)
			config &= ~XEMACPS_NWCFG_MCASTHASHEN_MASK;
	}
	XEmacPs_WriteReg(base, XEMACPS_NWCFG_OFFSET, config);
}

u16 ethernet_get_multicast_config(void)
{
	return ETH_CONFIG_CAP_MULTICAST_HASH;
}

u32 ethernet_mac_lo_word(const uint8_t mac[6])
{
	return ((u32)mac[4] << 24) | ((u32)mac[5] << 16) |
	       ethernet_get_multicast_config();
}

/* Z2 register reads split a 32-bit aligned word. Offset bit 1 selects the
 * low half, so 0x8A returns the capability and 0x88 returns the MAC bytes. */
u16 ethernet_zorro16(u32 word, u32 zaddr)
{
	if (zaddr & 2)
		return (u16)word;
	return (u16)(word >> 16);
}

void ethernet_reset_for_amiga(void)
{
	ethernet_log_status("amiga-reset-before");
	/* A rebooted SANA-II driver owns no memberships. Close the receive gate
	 * even though the GEM hash registers themselves survive a warm reset. */
	ethernet_set_multicast_hash(ETH_CONFIG_HASH_RESET);

	if (ethernet_task_state == ETH_TASK_READY) {
		ethernet_restart_dma("amiga-reset");
	} else {
		int paused = ethernet_pause_rx_irq();
		ethernet_clear_host_state();
		ethernet_resume_rx_irq(paused);
	}

	ethernet_log_status("amiga-reset-after");
}
