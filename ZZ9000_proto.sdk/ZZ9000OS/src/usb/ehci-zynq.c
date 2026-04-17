// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2014, Xilinx, Inc
 *
 * USB Low level initialization(Specific to zynq)
 */

#include "usb.h"
#include "ehci-ci.h"
#include "ulpi.h"
#include "ehci.h"
#include <stdio.h>
#include <xusbps_hw.h>
#include <sleep.h>

#define EINVAL 1

// see zynq TRM
#define USB_BASE_ADDR 0xE0002000

int ehci_hcd_init(int index, enum usb_init_type init,
		struct ehci_hccr **hccr, struct ehci_hcor **hcor)
{
	if (index > 0) {
		printf("[ehci_hcd_init] index out of range: %d\n",index);
		return -EINVAL;
	}

	struct usb_ehci *ehci = (struct usb_ehci *)(USB_BASE_ADDR);

	if (hccr && hcor) {
		*hccr = (struct ehci_hccr *)((uint32_t)&ehci->caplength);
		*hcor = (struct ehci_hcor *)((uint32_t)*hccr + HC_LENGTH(ehci_readl(&(*hccr)->cr_capbase)));
	}

	return 0;
}

int ehci_zynq_probe(struct zynq_ehci_priv *priv)
{
	struct ehci_hccr *hccr;
	struct ehci_hcor *hcor;
	struct ulpi_viewport ulpi_vp;
	/* Used for writing the ULPI data address */
	struct ulpi_regs *ulpi = (struct ulpi_regs *)0;
	int ret;

	struct usb_ehci *ehci = (struct usb_ehci *)(USB_BASE_ADDR);
	priv->ehci = ehci;

	XUsbPs_ResetHw(USB_BASE_ADDR);

	hccr = (struct ehci_hccr *)((uint32_t)&priv->ehci->caplength);
	hcor = (struct ehci_hcor *)((uint32_t) hccr + HC_LENGTH(ehci_readl(&hccr->cr_capbase)));

	//printf("[ehci-zynq] hccr: %p hcor: %p\n", hccr, hcor);

	priv->ehcictrl.hccr = hccr;
	priv->ehcictrl.hcor = hcor;

	ulpi_vp.viewport_addr = (u32)&priv->ehci->ulpi_viewpoint;
	ulpi_vp.port_num = 0;

	//printf("[ehci-zynq] viewport_addr: %p\n", &priv->ehci->ulpi_viewpoint);
// lifted from https://elixir.bootlin.com/u-boot/latest/source/drivers/usb/host/ehci-fsl.c#L275
/* Set to Host mode */
setbits_le32(&ehci->usbmode, CM_HOST);

// Enable controller and select ULPI interface in CONTROL register
// bit 10: ULPI_SEL, bit 2: USB_EN
out_be32(&ehci->control, PHY_CLK_SEL_ULPI | USB_EN);

out_be32(&ehci->prictrl, 0x0000000c);
out_be32(&ehci->age_cnt_limit, 0x00000040);

// SICTRL bit 0 (SITP) should be 0 for ULPI
out_be32(&ehci->sictrl, 0);

// Select ULPI interface in PORTSC
clrsetbits_le32(&ehci->portsc, PORT_PTS_MSK, PORT_PTS_ULPI);

usleep(10000); /* delay required for PHY Clk to appear */

in_le32(&ehci->usbmode);

/* ULPI set flags */

ret = ulpi_init(&ulpi_vp);
if (ret) {
	puts("zynq ULPI viewport init failed\n");
	return -1;
}

// dp and dm pulldown = host mode
// extvbusind = vbus indicator input
ulpi_write(&ulpi_vp, &ulpi->otg_ctrl,
	   ULPI_OTG_DP_PULLDOWN | ULPI_OTG_DM_PULLDOWN |
	   ULPI_OTG_EXTVBUSIND);

/*
 * Put the ULPI PHY in FS/LS (FS4LS) composite mode. XCVR_SELECT = 11.
 *
 * Earlier we used ULPI_FC_HIGH_SPEED (XCVR = 00) relying on HS chirp
 * to auto-downgrade. That works for FS devices but wedges LS devices:
 * the HC keeps PR=1 forever because the PHY never completes LS
 * speed negotiation in HS mode, even with PORTSC.PFSC set. FS4LS puts
 * the PHY in FS electrical mode and enables LS preamble support
 * through the integrated TT, which is the supported path for root-hub
 * LS attach on ChipIdea TDI.
 *
 * Tradeoff: HS devices (mass storage, hubs) will enumerate at FS
 * (12 Mbit/s). Acceptable for keyboards/mice; revisit with dynamic
 * XCVR switching if HS mass-storage performance is needed.
 */
ulpi_write(&ulpi_vp, &ulpi->function_ctrl,
	   ULPI_FC_FS4LS | ULPI_FC_OPMODE_NORMAL |
	   ULPI_FC_SUSPENDM);

ulpi_write(&ulpi_vp, &ulpi->iface_ctrl, 0);

/* drive external vbus switch */
ulpi_write(&ulpi_vp, &ulpi->otg_ctrl_set,
	   ULPI_OTG_DRVVBUS | ULPI_OTG_DRVVBUS_EXT);

usleep(10000);


	// FIXME removing this made it work! probably because there is another ehci_reset in there?
	//return ehci_register(&priv->ehcictrl, hccr, hcor, NULL, 0, USB_INIT_HOST);
	return 0;
}
