// SPDX-License-Identifier: GPL-2.0+
/*-
 * Copyright (c) 2007-2008, Juniper Networks, Inc.
 *
 * ZZ9000 modifications (integrated TT for LS/FS, async-ring timeout
 * recovery, NAK-on-idle-interrupt behaviour, silenced hot-path
 * printfs, int-queue leak fix, dev->act_len on int-completion, etc.)
 *
 * Copyright (C) 2026 Dimitris Panokostas <midwan@gmail.com>
 */

#include <errno.h>
#include <asm/byteorder.h>
#include "usb.h"
#include "io.h"
#include <malloc.h>
#include <stdio.h>
//#include <watchdog.h>
#include <xil_cache.h>
#include <string.h>
#include <sleep.h>
#include <xtime_l.h>

#include "ehci.h"
#include "memalign.h"
#include "ehci_lifecycle.h"
#include "ehci_periodic.h"
#include "usb_proxy.h"


void flush_dcache_range(unsigned long start, unsigned long stop) {
	Xil_DCacheFlushRange(start, stop-start);
}

void invalidate_dcache_range(unsigned long start, unsigned long stop) {
	Xil_DCacheInvalidateRange(start, stop-start);
}

uint32_t virt_to_phys(void* addr) {
	return (uint32_t)addr;
}

static uint64_t ehci_now_ticks(void)
{
	XTime now;

	XTime_GetTime(&now);
	return (uint64_t)now;
}

static uint64_t ehci_ticks_for_us(uint32_t usec)
{
	return ((uint64_t)usec * (uint64_t)COUNTS_PER_SECOND + 999999ULL) /
	       1000000ULL;
}

void udelay(int us)
{
	usleep(us);
}

void mdelay(int ms)
{
	usleep(1000 * ms);
}

unsigned long get_timer(unsigned long base)
{
	uint64_t ticks = ehci_now_ticks();
	uint64_t seconds = ticks / (uint64_t)COUNTS_PER_SECOND;
	uint64_t remainder = ticks % (uint64_t)COUNTS_PER_SECOND;
	uint32_t now_ms = (uint32_t)(seconds * 1000ULL +
		(remainder * 1000ULL) / (uint64_t)COUNTS_PER_SECOND);

	return (uint32_t)(now_ms - (uint32_t)base);
}

#ifndef CONFIG_USB_MAX_CONTROLLER_COUNT
#define CONFIG_USB_MAX_CONTROLLER_COUNT 1
#endif

/*
 * EHCI spec page 20 says that the HC may take up to 16 uFrames (= 4ms) to halt.
 * Let's time out after 8 to have a little safety margin on top of that.
 */
#define HCHALT_TIMEOUT (8 * 1000)

static struct ehci_ctrl ehcic[CONFIG_USB_MAX_CONTROLLER_COUNT];

struct ehci_async_allocation {
	struct QH *qh;
	struct qTD *qtd;
	struct ehci_async_allocation *next;
};

static struct ehci_async_allocation *quarantined_async;
static int ehci_recovery_required;
struct int_queue;
static struct int_queue *active_interrupt_queues;

static void ehci_release_async(struct ehci_async_allocation *allocation)
{
	if (!allocation)
		return;
	free(allocation->qtd);
	free(allocation->qh);
	free(allocation);
}

static void ehci_quarantine_async(struct ehci_async_allocation *allocation)
{
	allocation->next = quarantined_async;
	quarantined_async = allocation;
	ehci_recovery_required = 1;
}

static void ehci_reclaim_async_after_reset(void)
{
	while (quarantined_async) {
		struct ehci_async_allocation *allocation = quarantined_async;

		quarantined_async = allocation->next;
		ehci_release_async(allocation);
	}
}

static struct descriptor {
	struct usb_hub_descriptor hub;
	struct usb_device_descriptor device;
	struct usb_linux_config_descriptor config;
	struct usb_linux_interface_descriptor interface;
	struct usb_endpoint_descriptor endpoint;
}  __attribute__ ((packed)) descriptor = {
	{
		0x8,		/* bDescLength */
		0x29,		/* bDescriptorType: hub descriptor */
		2,		/* bNrPorts -- runtime modified */
		0,		/* wHubCharacteristics */
		10,		/* bPwrOn2PwrGood */
		0,		/* bHubCntrCurrent */
		{		/* Device removable */
		}		/* at most 7 ports! XXX */
	},
	{
		0x12,		/* bLength */
		1,		/* bDescriptorType: UDESC_DEVICE */
		cpu_to_le16(0x0200), /* bcdUSB: v2.0 */
		9,		/* bDeviceClass: UDCLASS_HUB */
		0,		/* bDeviceSubClass: UDSUBCLASS_HUB */
		1,		/* bDeviceProtocol: UDPROTO_HSHUBSTT */
		64,		/* bMaxPacketSize: 64 bytes */
		0x0000,		/* idVendor */
		0x0000,		/* idProduct */
		cpu_to_le16(0x0100), /* bcdDevice */
		1,		/* iManufacturer */
		2,		/* iProduct */
		0,		/* iSerialNumber */
		1		/* bNumConfigurations: 1 */
	},
	{
		0x9,
		2,		/* bDescriptorType: UDESC_CONFIG */
		cpu_to_le16(0x19),
		1,		/* bNumInterface */
		1,		/* bConfigurationValue */
		0,		/* iConfiguration */
		0x40,		/* bmAttributes: UC_SELF_POWER */
		0		/* bMaxPower */
	},
	{
		0x9,		/* bLength */
		4,		/* bDescriptorType: UDESC_INTERFACE */
		0,		/* bInterfaceNumber */
		0,		/* bAlternateSetting */
		1,		/* bNumEndpoints */
		9,		/* bInterfaceClass: UICLASS_HUB */
		0,		/* bInterfaceSubClass: UISUBCLASS_HUB */
		0,		/* bInterfaceProtocol: UIPROTO_HSHUBSTT */
		0		/* iInterface */
	},
	{
		0x7,		/* bLength */
		5,		/* bDescriptorType: UDESC_ENDPOINT */
		0x81,		/* bEndpointAddress:
				 * UE_DIR_IN | EHCI_INTR_ENDPT
				 */
		3,		/* bmAttributes: UE_INTERRUPT */
		8,		/* wMaxPacketSize */
		255		/* bInterval */
	},
};

#if defined(CONFIG_EHCI_IS_TDI)
#define ehci_is_TDI()	(1)
#else
#define ehci_is_TDI()	(0)
#endif

static struct ehci_ctrl *ehci_get_ctrl(struct usb_device *udev)
{
	return udev->controller;
}

static int ehci_get_port_speed(struct ehci_ctrl *ctrl, uint32_t reg)
{
	return PORTSC_PSPD(reg);
}

static void ehci_set_usbmode(struct ehci_ctrl *ctrl)
{
	uint32_t tmp;
	uint32_t *reg_ptr;

	reg_ptr = (uint32_t *)((u8 *)&ctrl->hcor->or_usbcmd + USBMODE);
	tmp = ehci_readl(reg_ptr);
	tmp |= USBMODE_CM_HC;
#if defined(CONFIG_EHCI_MMIO_BIG_ENDIAN)
	tmp |= USBMODE_BE;
#else
	tmp &= ~USBMODE_BE;
#endif
	ehci_writel(reg_ptr, tmp);
}

static void ehci_powerup_fixup(struct ehci_ctrl *ctrl, uint32_t *status_reg,
			       uint32_t *reg)
{
	mdelay(50); // FIXME
}

static uint32_t *ehci_get_portsc_register(struct ehci_ctrl *ctrl, int port)
{
	int max_ports = HCS_N_PORTS(ehci_readl(&ctrl->hccr->cr_hcsparams));

	if (port < 0 || port >= max_ports) {
		return NULL;
	}

	return (uint32_t *)&ctrl->hcor->or_portsc[port];
}

static int handshake(uint32_t *ptr, uint32_t mask, uint32_t done, int usec)
{
	uint64_t start = ehci_now_ticks();
	uint64_t budget;
	uint32_t result;

	if (usec <= 0)
		return -1;
	budget = ehci_ticks_for_us((uint32_t)usec);
	for (;;) {
		result = ehci_readl(ptr);
		if (result == ~(uint32_t)0)
			return -1;
		if ((result & mask) == done)
			return 0;
		if ((uint64_t)(ehci_now_ticks() - start) >= budget)
			return -1;
		udelay(5);
	}
}

static int ehci_reset(struct ehci_ctrl *ctrl)
{
	uint32_t cmd;
	int ret = 0;

	cmd = ehci_readl(&ctrl->hcor->or_usbcmd);
	cmd = (cmd & ~CMD_RUN) | CMD_RESET;
	ehci_writel(&ctrl->hcor->or_usbcmd, cmd);
	ret = handshake((uint32_t *)&ctrl->hcor->or_usbcmd,
			CMD_RESET, 0, 250 * 1000);
	if (ret < 0) {
		goto out;
	}

	if (ehci_is_TDI())
		ctrl->ops.set_usb_mode(ctrl);

#ifdef CONFIG_USB_EHCI_TXFIFO_THRESH
	cmd = ehci_readl(&ctrl->hcor->or_txfilltuning);
	cmd &= ~TXFIFO_THRESH_MASK;
	cmd |= TXFIFO_THRESH(CONFIG_USB_EHCI_TXFIFO_THRESH);
	ehci_writel(&ctrl->hcor->or_txfilltuning, cmd);
#endif
out:
	return ret;
}

static int ehci_shutdown(struct ehci_ctrl *ctrl)
{
	int i, ret = 0;
	uint32_t cmd, reg;
	int max_ports = HCS_N_PORTS(ehci_readl(&ctrl->hccr->cr_hcsparams));

	cmd = ehci_readl(&ctrl->hcor->or_usbcmd);
	/* If not run, directly return */
	if (!(cmd & CMD_RUN))
		return 0;
	cmd &= ~(CMD_PSE | CMD_ASE);
	ehci_writel(&ctrl->hcor->or_usbcmd, cmd);
	ret = handshake(&ctrl->hcor->or_usbsts, STS_ASS | STS_PSS, 0,
		100 * 1000);

	if (!ret) {
		for (i = 0; i < max_ports; i++) {
			reg = ehci_readl(&ctrl->hcor->or_portsc[i]);
			reg |= EHCI_PS_SUSP;
			ehci_writel(&ctrl->hcor->or_portsc[i], reg);
		}

		cmd &= ~CMD_RUN;
		ehci_writel(&ctrl->hcor->or_usbcmd, cmd);
		ret = handshake(&ctrl->hcor->or_usbsts, STS_HALT, STS_HALT,
			HCHALT_TIMEOUT);
	}


	return ret;
}

static int ehci_td_buffer(struct qTD *td, void *buf, size_t sz)
{
	uint32_t delta, next;
	unsigned long addr = (unsigned long)buf;
	int idx;


	flush_dcache_range(addr, ALIGN(addr + sz, ARCH_DMA_MINALIGN));

	idx = 0;
	while (idx < QT_BUFFER_CNT) {
		td->qt_buffer[idx] = cpu_to_hc32(virt_to_phys((void *)addr));
		td->qt_buffer_hi[idx] = 0;
		next = (addr + EHCI_PAGE_SIZE) & ~(EHCI_PAGE_SIZE - 1);
		delta = next - addr;
		if (delta >= sz)
			break;
		sz -= delta;
		addr = next;
		idx++;
	}

	if (idx == QT_BUFFER_CNT) {
		return -1;
	}

	return 0;
}

static inline u8 ehci_encode_speed(enum usb_device_speed speed)
{
	#define QH_HIGH_SPEED	2
	#define QH_FULL_SPEED	0
	#define QH_LOW_SPEED	1
	if (speed == USB_SPEED_HIGH)
		return QH_HIGH_SPEED;
	if (speed == USB_SPEED_LOW)
		return QH_LOW_SPEED;
	return QH_FULL_SPEED;
}

static void ehci_update_endpt2_dev_n_port(struct usb_device *udev,
					  struct QH *qh)
{
	uint8_t portnr = 0;
	uint8_t hubaddr = 0;

	if (udev->speed != USB_SPEED_LOW && udev->speed != USB_SPEED_FULL)
		return;


	/*
	 * A FS/LS device directly attached to the root port has the root device
	 * as its immediate parent. Synthetic proxy devices behind an external
	 * hub have a hub parent even while they still use address zero during
	 * enumeration; address zero must therefore not bypass the external TT.
	 */
	if (!udev->parent ||
	    (udev->parent && udev->parent->parent == NULL)) {
		hubaddr = 0;
		portnr = udev->portnr ? udev->portnr : 1;
	} else {
		usb_find_usb2_hub_address_port(udev, &hubaddr, &portnr);
	}

	qh->qh_endpt2 |= cpu_to_hc32(QH_ENDPT2_PORTNUM(portnr) |
				     QH_ENDPT2_HUBADDR(hubaddr));
}

static int ehci_stop_async_schedule(struct ehci_ctrl *ctrl)
{
	uint32_t cmd = ehci_readl(&ctrl->hcor->or_usbcmd);

	cmd &= ~CMD_ASE;
	ehci_writel(&ctrl->hcor->or_usbcmd, cmd);
	return handshake((uint32_t *)&ctrl->hcor->or_usbsts,
			 STS_ASS, 0, 100 * 1000);
}

static int ehci_unlink_async_qh(struct ehci_ctrl *ctrl)
{
	uint32_t cmd;

	ctrl->qh_list.qh_link = cpu_to_hc32(
		virt_to_phys(&ctrl->qh_list) | QH_LINK_TYPE_QH);
	flush_dcache_range((unsigned long)&ctrl->qh_list,
		ALIGN_END_ADDR(struct QH, &ctrl->qh_list, 1));

	ehci_writel(&ctrl->hcor->or_usbsts, STS_IAA);
	cmd = ehci_readl(&ctrl->hcor->or_usbcmd);
	ehci_writel(&ctrl->hcor->or_usbcmd, cmd | CMD_IAAD);
	if (handshake((uint32_t *)&ctrl->hcor->or_usbsts,
		      STS_IAA, STS_IAA, 10 * 1000) < 0)
		return -ETIMEDOUT;
	ehci_writel(&ctrl->hcor->or_usbsts, STS_IAA);
	return 0;
}

static int
ehci_submit_async_internal(struct usb_device *dev, unsigned long pipe,
		   void *buffer, int length, struct devrequest *req,
		   unsigned long timeout_ms, int idle_bulk_poll)
{
	struct ehci_async_allocation *allocation;
	struct QH *qh;
	struct qTD *qtd;
	int qtd_count = 0;
	int qtd_counter = 0;
	volatile struct qTD *vtd;
	unsigned long ts;
	uint32_t *tdp;
	uint32_t endpt, maxpacket, token, usbsts;
	uint32_t c, toggle;
	uint32_t cmd;
	int async_linked = 0;
	int descriptors_reclaimable = 0;
	unsigned long timeout;
	int ret = 0;
	int idle_bulk_in_timeout = 0;
	struct ehci_ctrl *ctrl = ehci_get_ctrl(dev);
	if (ehci_recovery_required) {
		dev->status = USB_ST_CRC_ERR;
		dev->act_len = 0;
		return -EAGAIN;
	}

	//printf("dev=%p, pipe=%lx, buffer=%p, length=%d, req=%p\n", dev, pipe,
	//      buffer, length, req);
	/*if (req != NULL)
		printf("req=%u (%#x), type=%u (%#x), value=%u (%#x), index=%u\n",
		      req->request, req->request,
		      req->requesttype, req->requesttype,
		      le16_to_cpu(req->value), le16_to_cpu(req->value),
		      le16_to_cpu(req->index));*/

#define PKT_ALIGN	512
	/*
	 * The USB transfer is split into qTD transfers. Eeach qTD transfer is
	 * described by a transfer descriptor (the qTD). The qTDs form a linked
	 * list with a queue head (QH).
	 *
	 * Each qTD transfer starts with a new USB packet, i.e. a packet cannot
	 * have its beginning in a qTD transfer and its end in the following
	 * one, so the qTD transfer lengths have to be chosen accordingly.
	 *
	 * Each qTD transfer uses up to QT_BUFFER_CNT data buffers, mapped to
	 * single pages. The first data buffer can start at any offset within a
	 * page (not considering the cache-line alignment issues), while the
	 * following buffers must be page-aligned. There is no alignment
	 * constraint on the size of a qTD transfer.
	 */
	if (req != NULL)
		/* 1 qTD will be needed for SETUP, and 1 for ACK. */
		qtd_count += 1 + 1;
	if (length > 0 || req == NULL) {
		/*
		 * Determine the qTD transfer size that will be used for the
		 * data payload (not considering the first qTD transfer, which
		 * may be longer or shorter, and the final one, which may be
		 * shorter).
		 *
		 * In order to keep each packet within a qTD transfer, the qTD
		 * transfer size is aligned to PKT_ALIGN, which is a multiple of
		 * wMaxPacketSize (except in some cases for interrupt transfers,
		 * see comment in submit_int_msg()).
		 *
		 * By default, i.e. if the input buffer is aligned to PKT_ALIGN,
		 * QT_BUFFER_CNT full pages will be used.
		 */
		int xfr_sz = QT_BUFFER_CNT;
		/*
		 * However, if the input buffer is not aligned to PKT_ALIGN, the
		 * qTD transfer size will be one page shorter, and the first qTD
		 * data buffer of each transfer will be page-unaligned.
		 */
		if ((unsigned long)buffer & (PKT_ALIGN - 1))
			xfr_sz--;
		/* Convert the qTD transfer size to bytes. */
		xfr_sz *= EHCI_PAGE_SIZE;
		/*
		 * Approximate by excess the number of qTDs that will be
		 * required for the data payload. The exact formula is way more
		 * complicated and saves at most 2 qTDs, i.e. a total of 128
		 * bytes.
		 */
		qtd_count += 2 + length / xfr_sz;
	}
/*
 * Threshold value based on the worst-case total size of the allocated qTDs for
 * a mass-storage transfer of 65535 blocks of 512 bytes.
 */
//#if CONFIG_SYS_MALLOC_LEN <= 64 + 128 * 1024
//#warning CONFIG_SYS_MALLOC_LEN may be too small for EHCI
//#endif

	// FIXME needs 128kB ram?

	allocation = malloc(sizeof(*allocation));
	qh = memalign(USB_DMA_MINALIGN, sizeof(*qh));
	qtd = memalign(USB_DMA_MINALIGN, qtd_count * sizeof(*qtd));
	if (!allocation || !qh || !qtd) {
		free(qtd);
		free(qh);
		free(allocation);
		return -ENOMEM;
	}
	allocation->qh = qh;
	allocation->qtd = qtd;
	allocation->next = NULL;

	memset(qh, 0, sizeof(struct QH));
	memset(qtd, 0, qtd_count * sizeof(*qtd));

	toggle = usb_gettoggle(dev, usb_pipeendpoint(pipe), usb_pipeout(pipe));

	/*
	 * Setup QH (3.6 in ehci-r10.pdf)
	 *
	 *   qh_link ................. 03-00 H
	 *   qh_endpt1 ............... 07-04 H
	 *   qh_endpt2 ............... 0B-08 H
	 * - qh_curtd
	 *   qh_overlay.qt_next ...... 13-10 H
	 * - qh_overlay.qt_altnext
	 */
	qh->qh_link = cpu_to_hc32(virt_to_phys(&ctrl->qh_list) | QH_LINK_TYPE_QH);
	c = (dev->speed != USB_SPEED_HIGH) && !usb_pipeendpoint(pipe);
	maxpacket = usb_maxpacket(dev, pipe);
	endpt = QH_ENDPT1_RL(8) | QH_ENDPT1_C(c) |
		QH_ENDPT1_MAXPKTLEN(maxpacket) | QH_ENDPT1_H(0) |
		QH_ENDPT1_DTC(QH_ENDPT1_DTC_DT_FROM_QTD) |
		QH_ENDPT1_ENDPT(usb_pipeendpoint(pipe)) | QH_ENDPT1_I(0) |
		QH_ENDPT1_DEVADDR(usb_pipedevice(pipe));

	/* Force FS for fsl HS quirk */
	uint8_t eps = ehci_encode_speed(dev->speed);
	if (!ctrl->has_fsl_erratum_a005275)
		endpt |= QH_ENDPT1_EPS(eps);
	else
		endpt |= QH_ENDPT1_EPS(ehci_encode_speed(QH_FULL_SPEED));

	qh->qh_endpt1 = cpu_to_hc32(endpt);
	endpt = QH_ENDPT2_MULT(1) | QH_ENDPT2_UFCMASK(0) | QH_ENDPT2_UFSMASK(0);
	qh->qh_endpt2 = cpu_to_hc32(endpt);
	ehci_update_endpt2_dev_n_port(dev, qh);
	flush_dcache_range((unsigned long)qh, ALIGN_END_ADDR(struct QH, qh, 1));
	qh->qh_overlay.qt_next = cpu_to_hc32(QT_NEXT_TERMINATE);
	qh->qh_overlay.qt_altnext = cpu_to_hc32(QT_NEXT_TERMINATE);

	tdp = &qh->qh_overlay.qt_next;
	if (req != NULL) {
		/*
		 * Setup request qTD (3.5 in ehci-r10.pdf)
		 *
		 *   qt_next ................ 03-00 H
		 *   qt_altnext ............. 07-04 H
		 *   qt_token ............... 0B-08 H
		 *
		 *   [ buffer, buffer_hi ] loaded with "req".
		 */
		qtd[qtd_counter].qt_next = cpu_to_hc32(QT_NEXT_TERMINATE);
		qtd[qtd_counter].qt_altnext = cpu_to_hc32(QT_NEXT_TERMINATE);
		token = QT_TOKEN_DT(0) | QT_TOKEN_TOTALBYTES(sizeof(*req)) |
			QT_TOKEN_IOC(0) | QT_TOKEN_CPAGE(0) | QT_TOKEN_CERR(3) |
			QT_TOKEN_PID(QT_TOKEN_PID_SETUP) |
			QT_TOKEN_STATUS(QT_TOKEN_STATUS_ACTIVE);
		qtd[qtd_counter].qt_token = cpu_to_hc32(token);
		if (ehci_td_buffer(&qtd[qtd_counter], req, sizeof(*req))) {
			goto fail;
		}
		/* Update previous qTD! */
		*tdp = cpu_to_hc32(virt_to_phys(&qtd[qtd_counter]));
		tdp = &qtd[qtd_counter++].qt_next;
		toggle = 1;
	}

	if (length > 0 || req == NULL) {
		uint8_t *buf_ptr = buffer;
		int left_length = length;

		do {
			/*
			 * Determine the size of this qTD transfer. By default,
			 * QT_BUFFER_CNT full pages can be used.
			 */
			int xfr_bytes = QT_BUFFER_CNT * EHCI_PAGE_SIZE;
			/*
			 * However, if the input buffer is not page-aligned, the
			 * portion of the first page before the buffer start
			 * offset within that page is unusable.
			 */
			xfr_bytes -= (unsigned long)buf_ptr & (EHCI_PAGE_SIZE - 1);
			/*
			 * In order to keep each packet within a qTD transfer,
			 * align the qTD transfer size to PKT_ALIGN.
			 */
			xfr_bytes &= ~(PKT_ALIGN - 1);
			/*
			 * This transfer may be shorter than the available qTD
			 * transfer size that has just been computed.
			 */
			xfr_bytes = min(xfr_bytes, left_length);

			/*
			 * Setup request qTD (3.5 in ehci-r10.pdf)
			 *
			 *   qt_next ................ 03-00 H
			 *   qt_altnext ............. 07-04 H
			 *   qt_token ............... 0B-08 H
			 *
			 *   [ buffer, buffer_hi ] loaded with "buffer".
			 */
			qtd[qtd_counter].qt_next =
					cpu_to_hc32(QT_NEXT_TERMINATE);
			qtd[qtd_counter].qt_altnext =
					cpu_to_hc32(QT_NEXT_TERMINATE);
			token = QT_TOKEN_DT(toggle) |
				QT_TOKEN_TOTALBYTES(xfr_bytes) |
				QT_TOKEN_IOC(req == NULL) | QT_TOKEN_CPAGE(0) |
				QT_TOKEN_CERR(3) |
				QT_TOKEN_PID(usb_pipein(pipe) ?
					QT_TOKEN_PID_IN : QT_TOKEN_PID_OUT) |
				QT_TOKEN_STATUS(QT_TOKEN_STATUS_ACTIVE);
			qtd[qtd_counter].qt_token = cpu_to_hc32(token);
			if (ehci_td_buffer(&qtd[qtd_counter], buf_ptr,
						xfr_bytes)) {
				goto fail;
			}
			/* Update previous qTD! */
			*tdp = cpu_to_hc32(virt_to_phys(&qtd[qtd_counter]));
			tdp = &qtd[qtd_counter++].qt_next;
			/*
			 * Data toggle has to be adjusted since the qTD transfer
			 * size is not always an even multiple of
			 * wMaxPacketSize.
			 */
			if ((xfr_bytes / maxpacket) & 1)
				toggle ^= 1;
			buf_ptr += xfr_bytes;
			left_length -= xfr_bytes;
		} while (left_length > 0);
	}

	if (req != NULL) {
		/*
		 * Setup request qTD (3.5 in ehci-r10.pdf)
		 *
		 *   qt_next ................ 03-00 H
		 *   qt_altnext ............. 07-04 H
		 *   qt_token ............... 0B-08 H
		 */
		qtd[qtd_counter].qt_next = cpu_to_hc32(QT_NEXT_TERMINATE);
		qtd[qtd_counter].qt_altnext = cpu_to_hc32(QT_NEXT_TERMINATE);
		token = QT_TOKEN_DT(1) | QT_TOKEN_TOTALBYTES(0) |
			QT_TOKEN_IOC(1) | QT_TOKEN_CPAGE(0) | QT_TOKEN_CERR(3) |
			QT_TOKEN_PID(usb_pipein(pipe) ?
				QT_TOKEN_PID_OUT : QT_TOKEN_PID_IN) |
			QT_TOKEN_STATUS(QT_TOKEN_STATUS_ACTIVE);
		qtd[qtd_counter].qt_token = cpu_to_hc32(token);
		/* Update previous qTD! */
		*tdp = cpu_to_hc32(virt_to_phys(&qtd[qtd_counter]));
		tdp = &qtd[qtd_counter++].qt_next;
	}

	ctrl->qh_list.qh_link = cpu_to_hc32(virt_to_phys(qh) | QH_LINK_TYPE_QH);
	async_linked = 1;

	/* Flush dcache */
	uint32_t end = ALIGN_END_ADDR(struct QH, &ctrl->qh_list, 1);
	flush_dcache_range((unsigned long)&ctrl->qh_list, end);
	flush_dcache_range((unsigned long)qh, ALIGN_END_ADDR(struct QH, qh, 1));
	flush_dcache_range((unsigned long)qtd,
			   ALIGN_END_ADDR(struct qTD, qtd, qtd_count));

	/* Set async. queue head pointer. */
	ehci_writel(&ctrl->hcor->or_asynclistaddr, virt_to_phys(&ctrl->qh_list));

	usbsts = ehci_readl(&ctrl->hcor->or_usbsts);
	ehci_writel(&ctrl->hcor->or_usbsts, (usbsts & 0x3f));

	/* Enable async. schedule. */
	cmd = ehci_readl(&ctrl->hcor->or_usbcmd);
	cmd |= CMD_ASE;
	ehci_writel(&ctrl->hcor->or_usbcmd, cmd);

	ret = handshake((uint32_t *)&ctrl->hcor->or_usbsts, STS_ASS, STS_ASS,
			100 * 1000);
	if (ret < 0) {
		goto fail;
	}

	/* Wait for TDs to be processed. */
	ts = get_timer(0);  // FIXME
	vtd = &qtd[qtd_counter - 1];
	timeout = timeout_ms ? timeout_ms : USB_TIMEOUT_MS(pipe);
	do {
		usb_proxy_iso_pump();
		usb_proxy_periodic_pump();
		usb_proxy_poll_maintenance();
		/* Invalidate dcache */
		invalidate_dcache_range((unsigned long)&ctrl->qh_list,
			ALIGN_END_ADDR(struct QH, &ctrl->qh_list, 1));
		invalidate_dcache_range((unsigned long)qh,
			ALIGN_END_ADDR(struct QH, qh, 1));
		invalidate_dcache_range((unsigned long)qtd,
			ALIGN_END_ADDR(struct qTD, qtd, qtd_count));

		token = hc32_to_cpu(vtd->qt_token);
		if (!(QT_TOKEN_GET_STATUS(token) & QT_TOKEN_STATUS_ACTIVE))
			break;
		//WATCHDOG_RESET(); // FIXME
	} while (get_timer(ts) < timeout);
	if (idle_bulk_poll && req == NULL && usb_pipein(pipe) &&
	    QT_TOKEN_GET_STATUS(token) == QT_TOKEN_STATUS_ACTIVE)
		idle_bulk_in_timeout = 1;

	/*
	 * Invalidate the memory area occupied by buffer
	 * Don't try to fix the buffer alignment, if it isn't properly
	 * aligned it's upper layer's fault so let invalidate_dcache_range()
	 * vow about it. But we have to fix the length as it's actual
	 * transfer length and can be unaligned. This is potentially
	 * dangerous operation, it's responsibility of the calling
	 * code to make sure enough space is reserved.
	 */
	if (buffer != NULL && length > 0)
		invalidate_dcache_range((unsigned long)buffer,
			ALIGN((unsigned long)buffer + length, ARCH_DMA_MINALIGN));


	if (ehci_unlink_async_qh(ctrl) == 0)
		descriptors_reclaimable = 1;

	ret = ehci_stop_async_schedule(ctrl);
	if (ret == 0)
		descriptors_reclaimable = 1;
	else
		ehci_recovery_required = 1;

	if (!ehci_dma_reclaimable(ret == 0, descriptors_reclaimable, 0)) {
		dev->status = USB_ST_CRC_ERR;
		dev->act_len = 0;
		goto fail;
	}

	token = hc32_to_cpu(qh->qh_overlay.qt_token);

	/*
	 * On timeout (qTD still ACTIVE after USB_TIMEOUT_MS) or HALTED,
	 * scrub the async-ring state so the next ehci_submit_async call
	 * starts fresh:
	 *
	 *   1. Point qh_list.qh_link back at qh_list itself so no later
	 *      schedule traversal reaches the retired heap QH.
	 *   2. Zero qh_list.qh_overlay and set its qt_next / qt_altnext
	 *      to TERMINATE — otherwise the HC, on the next async-enable,
	 *      may resume from the cached overlay state (which may still
	 *      have the HALTED bit or a stale qTD pointer).
	 *   3. Clear any W1C error bits in USBSTS so subsequent
	 *      handshakes don't misinterpret stale flags.
	 *   4. Ensure dev->status conveys a mapped result. An error-free
	 *      bulk-IN timeout is an idle endpoint (NAK); actual qTD error
	 *      bits remain a CRC/transaction failure.
	 *
	 * Without this cleanup, a USB ethernet device that glitched and
	 * left a stuck qTD would poison the next transfer's queue head
	 * (we saw "token=80020d50" on the retried bulk IN), cascading
	 * into further failures.
	 */
	if (QT_TOKEN_GET_STATUS(token) &
	    (QT_TOKEN_STATUS_ACTIVE | QT_TOKEN_STATUS_HALTED)) {
		ctrl->qh_list.qh_link = cpu_to_hc32(
			virt_to_phys(&ctrl->qh_list) | QH_LINK_TYPE_QH);
		memset(&ctrl->qh_list.qh_overlay, 0,
		       sizeof(ctrl->qh_list.qh_overlay));
		ctrl->qh_list.qh_overlay.qt_next =
			cpu_to_hc32(QH_LINK_TERMINATE);
		ctrl->qh_list.qh_overlay.qt_altnext =
			cpu_to_hc32(QH_LINK_TERMINATE);
		flush_dcache_range((unsigned long)&ctrl->qh_list,
			ALIGN_END_ADDR(struct QH, &ctrl->qh_list, 1));

		/* Clear W1C error status bits */
		uint32_t sts = ehci_readl(&ctrl->hcor->or_usbsts);
		ehci_writel(&ctrl->hcor->or_usbsts, sts & 0x3f);

		/*
		 * Only an explicitly marked open-ended bulk-IN poll converts an
		 * idle qTD deadline to NAK. Ordinary finite requests must surface
		 * the requested timeout rather than remain pending indefinitely.
		 * Real qTD transaction errors are decoded below.
		 */
		if (QT_TOKEN_GET_STATUS(token) & QT_TOKEN_STATUS_ACTIVE)
			dev->status = idle_bulk_in_timeout ?
				USB_ST_NAK_REC : USB_ST_TIMEOUT;
	}

	if (!(QT_TOKEN_GET_STATUS(token) & QT_TOKEN_STATUS_ACTIVE)) {
		//printf("TOKEN=%#lx\n", token);
		switch (QT_TOKEN_GET_STATUS(token) &
			~(QT_TOKEN_STATUS_SPLITXSTATE | QT_TOKEN_STATUS_PERR)) {
		case 0:
			toggle = QT_TOKEN_GET_DT(token);
			usb_settoggle(dev, usb_pipeendpoint(pipe),
				       usb_pipeout(pipe), toggle);
			dev->status = 0;
			break;
		case QT_TOKEN_STATUS_HALTED:
			dev->status = USB_ST_STALLED;
			break;
		case QT_TOKEN_STATUS_ACTIVE | QT_TOKEN_STATUS_DATBUFERR:
		case QT_TOKEN_STATUS_DATBUFERR:
			dev->status = USB_ST_BUF_ERR;
			break;
		case QT_TOKEN_STATUS_HALTED | QT_TOKEN_STATUS_BABBLEDET:
		case QT_TOKEN_STATUS_BABBLEDET:
			dev->status = USB_ST_BABBLE_DET;
			break;
		default:
			dev->status = USB_ST_CRC_ERR;
			if (QT_TOKEN_GET_STATUS(token) & QT_TOKEN_STATUS_HALTED)
				dev->status |= USB_ST_STALLED;
			break;
		}
		dev->act_len = length - QT_TOKEN_GET_TOTALBYTES(token);
	} else {
		dev->act_len = 0;
		//printf("dev=%u, usbsts=%#lx, p[1]=%#lx, p[2]=%#lx\n",
		//      dev->devnum, ehci_readl(&ctrl->hcor->or_usbsts),
		//      ehci_readl(&ctrl->hcor->or_portsc[0]),
		//      ehci_readl(&ctrl->hcor->or_portsc[1]));
	}

	ehci_release_async(allocation);
	return (dev->status != USB_ST_NOT_PROC) ? 0 : -1;

fail:
	if (async_linked && !descriptors_reclaimable &&
	    ehci_unlink_async_qh(ctrl) == 0)
		descriptors_reclaimable = 1;
	if (async_linked && ehci_stop_async_schedule(ctrl) == 0)
		descriptors_reclaimable = 1;
	if (!async_linked || descriptors_reclaimable)
		ehci_release_async(allocation);
	else
		ehci_quarantine_async(allocation);
	return -1;
}

int ehci_submit_async(struct usb_device *dev, unsigned long pipe, void *buffer,
		   int length, struct devrequest *req)
{
	return ehci_submit_async_internal(dev, pipe, buffer, length, req, 0, 0);
}

int ehci_submit_async_timeout(struct usb_device *dev, unsigned long pipe,
		   void *buffer, int length, struct devrequest *req,
		   unsigned long timeout_ms, int idle_bulk_poll)
{
	return ehci_submit_async_internal(dev, pipe, buffer, length, req,
					  timeout_ms, idle_bulk_poll);
}

static int ehci_submit_root(struct usb_device *dev, unsigned long pipe,
			    void *buffer, int length, struct devrequest *req)
{
	uint8_t tmpbuf[4];
	u16 typeReq;
	void *srcptr = NULL;
	int len, srclen;
	uint32_t reg;
	uint32_t *status_reg;
	int port = le16_to_cpu(req->index) & 0xff;
	struct ehci_ctrl *ctrl = ehci_get_ctrl(dev);

	srclen = 0;

	/*printf("req=%u (%#x), type=%u (%#x), value=%u, index=%u\n",
	      req->request, req->request,
	      req->requesttype, req->requesttype,
	      le16_to_cpu(req->value), le16_to_cpu(req->index));*/

	typeReq = req->request | req->requesttype << 8;

	switch (typeReq) {
	case USB_REQ_GET_STATUS | ((USB_RT_PORT | USB_DIR_IN) << 8):
	case USB_REQ_SET_FEATURE | ((USB_DIR_OUT | USB_RT_PORT) << 8):
	case USB_REQ_CLEAR_FEATURE | ((USB_DIR_OUT | USB_RT_PORT) << 8):
		status_reg = ctrl->ops.get_portsc_register(ctrl, port - 1);
		if (!status_reg)
			return -1;
		break;
	default:
		status_reg = NULL;
		break;
	}

	switch (typeReq) {
	case DeviceRequest | USB_REQ_GET_DESCRIPTOR:
		switch (le16_to_cpu(req->value) >> 8) {
		case USB_DT_DEVICE:
			//printf("USB_DT_DEVICE request\n");
			srcptr = &descriptor.device;
			srclen = descriptor.device.bLength;
			break;
		case USB_DT_CONFIG:
			//printf("USB_DT_CONFIG config\n");
			srcptr = &descriptor.config;
			srclen = descriptor.config.bLength +
					descriptor.interface.bLength +
					descriptor.endpoint.bLength;
			break;
		case USB_DT_STRING:
			//printf("USB_DT_STRING config\n");
			switch (le16_to_cpu(req->value) & 0xff) {
			case 0:	/* Language */
				srcptr = "\4\3\1\0";
				srclen = 4;
				break;
			case 1:	/* Vendor */
				srcptr = "\16\3u\0-\0b\0o\0o\0t\0";
				srclen = 14;
				break;
			case 2:	/* Product */
				srcptr = "\52\3E\0H\0C\0I\0 "
					 "\0H\0o\0s\0t\0 "
					 "\0C\0o\0n\0t\0r\0o\0l\0l\0e\0r\0";
				srclen = 42;
				break;
			default:
				goto unknown;
			}
			break;
		default:
			goto unknown;
		}
		break;
	case USB_REQ_GET_DESCRIPTOR | ((USB_DIR_IN | USB_RT_HUB) << 8):
		switch (le16_to_cpu(req->value) >> 8) {
		case USB_DT_HUB:
			//printf("[ehci] USB_DT_HUB config\n");
			srcptr = &descriptor.hub;
			srclen = descriptor.hub.bLength;
			break;
		default:
			goto unknown;
		}
		break;
	case USB_REQ_SET_ADDRESS | (USB_RECIP_DEVICE << 8):
		//printf("USB_REQ_SET_ADDRESS\n");
		ctrl->rootdev = le16_to_cpu(req->value);
		break;
	case DeviceOutRequest | USB_REQ_SET_CONFIGURATION:
		//printf("USB_REQ_SET_CONFIGURATION\n");
		/* Nothing to do */
		break;
	case USB_REQ_GET_STATUS | ((USB_DIR_IN | USB_RT_HUB) << 8):
		tmpbuf[0] = 1;	/* USB_STATUS_SELFPOWERED */
		tmpbuf[1] = 0;
		srcptr = tmpbuf;
		srclen = 2;
		break;
	case USB_REQ_GET_STATUS | ((USB_RT_PORT | USB_DIR_IN) << 8):
		memset(tmpbuf, 0, 4);
		reg = ehci_readl(status_reg);
		if (reg & EHCI_PS_CS)
			tmpbuf[0] |= USB_PORT_STAT_CONNECTION;
		if (reg & EHCI_PS_PE)
			tmpbuf[0] |= USB_PORT_STAT_ENABLE;
		if (reg & EHCI_PS_SUSP)
			tmpbuf[0] |= USB_PORT_STAT_SUSPEND;
		if (reg & EHCI_PS_OCA)
			tmpbuf[0] |= USB_PORT_STAT_OVERCURRENT;
		if (reg & EHCI_PS_PR)
			tmpbuf[0] |= USB_PORT_STAT_RESET;
		if (reg & EHCI_PS_PP)
			tmpbuf[1] |= USB_PORT_STAT_POWER >> 8;

		if (ehci_is_TDI()) {
			switch (ctrl->ops.get_port_speed(ctrl, reg)) {
			case PORTSC_PSPD_FS:
				break;
			case PORTSC_PSPD_LS:
				tmpbuf[1] |= USB_PORT_STAT_LOW_SPEED >> 8;
				break;
			case PORTSC_PSPD_HS:
			default:
				tmpbuf[1] |= USB_PORT_STAT_HIGH_SPEED >> 8;
				break;
			}
		} else {
			tmpbuf[1] |= USB_PORT_STAT_HIGH_SPEED >> 8;
		}

		if (reg & EHCI_PS_CSC)
			tmpbuf[2] |= USB_PORT_STAT_C_CONNECTION;
		if (reg & EHCI_PS_PEC)
			tmpbuf[2] |= USB_PORT_STAT_C_ENABLE;
		if (reg & EHCI_PS_OCC)
			tmpbuf[2] |= USB_PORT_STAT_C_OVERCURRENT;
		if (ctrl->portreset & (1 << port))
			tmpbuf[2] |= USB_PORT_STAT_C_RESET;

		srcptr = tmpbuf;
		srclen = 4;
		break;
	case USB_REQ_SET_FEATURE | ((USB_DIR_OUT | USB_RT_PORT) << 8):
		reg = ehci_readl(status_reg);
		reg &= ~EHCI_PS_CLEAR;
		switch (le16_to_cpu(req->value)) {
		case USB_PORT_FEAT_ENABLE:
			reg |= EHCI_PS_PE;
			ehci_writel(status_reg, reg);
			break;
		case USB_PORT_FEAT_POWER:
			if (HCS_PPC(ehci_readl(&ctrl->hccr->cr_hcsparams))) {
				reg |= EHCI_PS_PP;
				ehci_writel(status_reg, reg);
			}
			break;
		case USB_PORT_FEAT_RESET:
			if ((reg & (EHCI_PS_PE | EHCI_PS_CS)) == EHCI_PS_CS &&
			    !ehci_is_TDI() &&
			    EHCI_PS_IS_LOWSPEED(reg)) {
				/* Low speed device, give up ownership. */
				reg |= EHCI_PS_PO;
				ehci_writel(status_reg, reg);
				return -ENXIO;
			} else {
				int ret;

				/* Disable chirp for HS erratum */
				if (ctrl->has_fsl_erratum_a005275)
					reg |= PORTSC_FSL_PFSC;

				reg |= EHCI_PS_PR;
				reg &= ~EHCI_PS_PE;
				ehci_writel(status_reg, reg);
				/*
				 * caller must wait, then call GetPortStatus
				 * usb 2.0 specification say 50 ms resets on
				 * root
				 */
				ctrl->ops.powerup_fixup(ctrl, status_reg, &reg);

				ehci_writel(status_reg, reg & ~EHCI_PS_PR);
				/*
				 * A host controller must terminate the reset
				 * and stabilize the state of the port within
				 * 2 milliseconds
				 */
				ret = handshake(status_reg, EHCI_PS_PR, 0,
						2 * 1000);
				if (!ret) {
					reg = ehci_readl(status_reg);
					if ((reg & (EHCI_PS_PE | EHCI_PS_CS))
					    == EHCI_PS_CS && !ehci_is_TDI()) {
						reg &= ~EHCI_PS_CLEAR;
						reg |= EHCI_PS_PO;
						ehci_writel(status_reg, reg);
						return -ENXIO;
					} else {
						ctrl->portreset |= 1 << port;
					}
				} else {
				}
			}
			break;
		case USB_PORT_FEAT_TEST:
			ehci_shutdown(ctrl);
			reg &= ~(0xf << 16);
			reg |= ((le16_to_cpu(req->index) >> 8) & 0xf) << 16;
			ehci_writel(status_reg, reg);
			break;
		default:
			goto unknown;
		}
		/* unblock posted writes */
		(void) ehci_readl(&ctrl->hcor->or_usbcmd);
		break;
	case USB_REQ_CLEAR_FEATURE | ((USB_DIR_OUT | USB_RT_PORT) << 8):
		reg = ehci_readl(status_reg);
		reg &= ~EHCI_PS_CLEAR;
		switch (le16_to_cpu(req->value)) {
		case USB_PORT_FEAT_ENABLE:
			reg &= ~EHCI_PS_PE;
			break;
		case USB_PORT_FEAT_C_ENABLE:
			reg |= EHCI_PS_PE;
			break;
		case USB_PORT_FEAT_POWER:
			if (HCS_PPC(ehci_readl(&ctrl->hccr->cr_hcsparams)))
				reg &= ~EHCI_PS_PP;
			break;
		case USB_PORT_FEAT_C_CONNECTION:
			reg |= EHCI_PS_CSC;
			break;
		case USB_PORT_FEAT_OVER_CURRENT:
			reg |= EHCI_PS_OCC;
			break;
		case USB_PORT_FEAT_C_RESET:
			ctrl->portreset &= ~(1 << port);
			break;
		default:
			goto unknown;
		}
		ehci_writel(status_reg, reg);
		/* unblock posted write */
		(void) ehci_readl(&ctrl->hcor->or_usbcmd);
		break;
	default:
		goto unknown;
	}

	mdelay(1);
	len = min3(srclen, (int)le16_to_cpu(req->length), length);
	if (srcptr != NULL && len > 0)
		memcpy(buffer, srcptr, len);
	//else
	//	printf("[ehci] Len is 0\n");

	dev->act_len = len;
	dev->status = 0;
	return 0;

unknown:

	dev->act_len = 0;
	dev->status = USB_ST_STALLED;
	return -1;
}

static const struct ehci_ops default_ehci_ops = {
	.set_usb_mode		= ehci_set_usbmode,
	.get_port_speed		= ehci_get_port_speed,
	.powerup_fixup		= ehci_powerup_fixup,
	.get_portsc_register	= ehci_get_portsc_register,
};

static void ehci_setup_ops(struct ehci_ctrl *ctrl, const struct ehci_ops *ops)
{
	if (!ops) {
		ctrl->ops = default_ehci_ops;
	} else {
		ctrl->ops = *ops;
		if (!ctrl->ops.set_usb_mode)
			ctrl->ops.set_usb_mode = ehci_set_usbmode;
		if (!ctrl->ops.get_port_speed)
			ctrl->ops.get_port_speed = ehci_get_port_speed;
		if (!ctrl->ops.powerup_fixup)
			ctrl->ops.powerup_fixup = ehci_powerup_fixup;
		if (!ctrl->ops.get_portsc_register)
			ctrl->ops.get_portsc_register =
					ehci_get_portsc_register;
	}
}

void ehci_set_controller_priv(int index, void *priv, const struct ehci_ops *ops)
{
	struct ehci_ctrl *ctrl = &ehcic[index];

	ctrl->priv = priv;
	ehci_setup_ops(ctrl, ops);
}

void *ehci_get_controller_priv(int index)
{
	return ehcic[index].priv;
}

/*
 * Zynq AR47540: after reset, do not assert USBCMD.RS until ULPI post-reset
 * processing has completed, observed as PORTSC.PR clear on every root port.
 */
static int ehci_wait_ulpi_post_reset(struct ehci_ctrl *ctrl)
{
	int ports = HCS_N_PORTS(ehci_readl(&ctrl->hccr->cr_hcsparams));
	int port;

	for (port = 0; port < ports; port++) {
		unsigned long start = get_timer(0);
		uint32_t reg;

		do {
			reg = ehci_readl(&ctrl->hcor->or_portsc[port]);
			if (reg == ~(uint32_t)0)
				return -ENODEV;
			if (!(reg & EHCI_PS_PR))
				break;
			if (ehci_deadline_expired_u32((uint32_t)get_timer(0),
						      (uint32_t)start, 10U))
				return -ETIMEDOUT;
			udelay(5);
		} while (1);
	}
	return 0;
}

static int ehci_common_init(struct ehci_ctrl *ctrl, unsigned int tweaks)
{
	struct QH *qh_list;
	struct QH *periodic;
	uint32_t reg;
	uint32_t cmd;
	int i;

	/* Set the high address word (aka segment) for 64-bit controller */
	if (ehci_readl(&ctrl->hccr->cr_hccparams) & 1)
		ehci_writel(&ctrl->hcor->or_ctrldssegment, 0);

	qh_list = &ctrl->qh_list;

	/* Set head of reclaim list */
	memset(qh_list, 0, sizeof(*qh_list));
	qh_list->qh_link = cpu_to_hc32(virt_to_phys(qh_list) | QH_LINK_TYPE_QH);
	qh_list->qh_endpt1 = cpu_to_hc32(QH_ENDPT1_H(1) |
						QH_ENDPT1_EPS(USB_SPEED_HIGH));
	qh_list->qh_overlay.qt_next = cpu_to_hc32(QT_NEXT_TERMINATE);
	qh_list->qh_overlay.qt_altnext = cpu_to_hc32(QT_NEXT_TERMINATE);
	qh_list->qh_overlay.qt_token =
			cpu_to_hc32(QT_TOKEN_STATUS(QT_TOKEN_STATUS_HALTED));

	flush_dcache_range((unsigned long)qh_list,
			   ALIGN_END_ADDR(struct QH, qh_list, 1));

	/* Set async. queue head pointer. */
	ehci_writel(&ctrl->hcor->or_asynclistaddr, virt_to_phys(qh_list));

	/*
	 * Set up periodic list
	 * Step 1: Parent QH for all periodic transfers.
	 */
	ctrl->periodic_schedules = 0;
	periodic = &ctrl->periodic_queue;
	memset(periodic, 0, sizeof(*periodic));
	periodic->qh_link = cpu_to_hc32(QH_LINK_TERMINATE);
	periodic->qh_overlay.qt_next = cpu_to_hc32(QT_NEXT_TERMINATE);
	periodic->qh_overlay.qt_altnext = cpu_to_hc32(QT_NEXT_TERMINATE);

	flush_dcache_range((unsigned long)periodic,
			   ALIGN_END_ADDR(struct QH, periodic, 1));

	/*
	 * Step 2: Setup frame-list: Every microframe, USB tries the same list.
	 *         In particular, device specifications on polling frequency
	 *         are disregarded. Keyboards seem to send NAK/NYet reliably
	 *         when polled with an empty buffer.
	 *
	 *         Split Transactions will be spread across microframes using
	 *         S-mask and C-mask.
	 */
	if (ctrl->periodic_list == NULL)
		ctrl->periodic_list = memalign(4096, 1024 * 4); // FIXME dynamic allocation

	if (!ctrl->periodic_list) {
		printf("[ehci_common_init] ENOMEM\n");
		return -ENOMEM;
	}
	for (i = 0; i < 1024; i++) {
		ctrl->periodic_list[i] = cpu_to_hc32((unsigned long)periodic
						| QH_LINK_TYPE_QH);
	}

	flush_dcache_range((unsigned long)ctrl->periodic_list,
			   ALIGN_END_ADDR(uint32_t, ctrl->periodic_list,
					  1024));

	/* Set periodic list base address */
	ehci_writel(&ctrl->hcor->or_periodiclistbase,
		(unsigned long)ctrl->periodic_list);

	reg = ehci_readl(&ctrl->hccr->cr_hcsparams);
	descriptor.hub.bNbrPorts = HCS_N_PORTS(reg);
	printf("Register %lx NbrPorts %d\n", reg, descriptor.hub.bNbrPorts);
	/* Port Indicators */
	if (HCS_INDICATOR(reg))
		put_unaligned(get_unaligned(&descriptor.hub.wHubCharacteristics)
				| 0x80, &descriptor.hub.wHubCharacteristics);
	/* Port Power Control */
	if (HCS_PPC(reg))
		put_unaligned(get_unaligned(&descriptor.hub.wHubCharacteristics)
				| 0x01, &descriptor.hub.wHubCharacteristics);

	if (ehci_wait_ulpi_post_reset(ctrl) < 0) {
		printf("EHCI ULPI post-reset processing timed out\n");
		return -ETIMEDOUT;
	}

	/* Start the host controller. */
	cmd = ehci_readl(&ctrl->hcor->or_usbcmd);
	/*
	 * Philips, Intel, and maybe others need CMD_RUN before the
	 * root hub will detect new devices (why?); NEC doesn't
	 */
	cmd &= ~(CMD_LRESET|CMD_IAAD|CMD_PSE|CMD_ASE|CMD_RESET);
	cmd |= CMD_RUN;
	ehci_writel(&ctrl->hcor->or_usbcmd, cmd);

	if (!(tweaks & EHCI_TWEAK_NO_INIT_CF)) {
		/* take control over the ports */
		cmd = ehci_readl(&ctrl->hcor->or_configflag);
		cmd |= FLAG_CF;
		ehci_writel(&ctrl->hcor->or_configflag, cmd);
	}

	/* unblock posted write */
	cmd = ehci_readl(&ctrl->hcor->or_usbcmd);
	mdelay(5);
	reg = HC_VERSION(ehci_readl(&ctrl->hccr->cr_capbase));
	printf("USB EHCI %lx.%02lx\n", reg >> 8, reg & 0xff);

	return 0;
}

int ehci_hcd_stop(int index)
{
	return 0;
}

int usb_lowlevel_stop(int index)
{
	ehci_shutdown(&ehcic[index]);
	return ehci_hcd_stop(index);
}

int usb_lowlevel_init(int index, enum usb_init_type init, void **controller)
{
	struct ehci_ctrl *ctrl = &ehcic[index];
	unsigned int tweaks = 0;
	int rc;

	/**
	 * Set ops to default_ehci_ops, ehci_hcd_init should call
	 * ehci_set_controller_priv to change any of these function pointers.
	 */
	ctrl->ops = default_ehci_ops;

	rc = ehci_hcd_init(index, init, &ctrl->hccr, &ctrl->hcor);
	if (rc) {
		printf("[usb_lowlevel_init] rc: %d\n",rc);
		return rc;
	}
	if (!ctrl->hccr || !ctrl->hcor) {
		printf("[usb_lowlevel_init] hccr or hcor missing: %p %p\n",ctrl->hccr,ctrl->hcor);
		return -1;
	}
	if (init == USB_INIT_DEVICE)
		goto done;

	/* EHCI spec section 4.1 */
	// FIXME mntmn
	//if (ehci_reset(ctrl)) {
	//	printf("[usb_lowlevel_init] ehci_reset failed\n");
	//	return -1;
	//}
	printf("[usb_lowlevel_init] skipped ehci_reset\n");

#if defined(CONFIG_EHCI_HCD_INIT_AFTER_RESET)
	rc = ehci_hcd_init(index, init, &ctrl->hccr, &ctrl->hcor);
	if (rc)
		return rc;
#endif
	rc = ehci_common_init(ctrl, tweaks);
	if (rc) {
		printf("[usb_lowlevel_init] ehci_common_init failed\n");
		return rc;
	}

	ctrl->rootdev = 0;
done:
	*controller = &ehcic[index];
	return 0;
}

static int _ehci_submit_bulk_msg(struct usb_device *dev, unsigned long pipe,
				 void *buffer, int length)
{

	if (usb_pipetype(pipe) != PIPE_BULK) {
		printf("non-bulk pipe (type=%lu)", usb_pipetype(pipe));
		return -1;
	}
	return ehci_submit_async(dev, pipe, buffer, length, NULL);
}

static int _ehci_submit_control_msg(struct usb_device *dev, unsigned long pipe,
				    void *buffer, int length,
				    struct devrequest *setup)
{
	struct ehci_ctrl *ctrl = ehci_get_ctrl(dev);

	if (usb_pipetype(pipe) != PIPE_CONTROL) {
		printf("non-control pipe (type=%lu)", usb_pipetype(pipe));
		return -1;
	}

	if (usb_pipedevice(pipe) == ctrl->rootdev) {
		if (!ctrl->rootdev)
			dev->speed = USB_SPEED_HIGH;
		return ehci_submit_root(dev, pipe, buffer, length, setup);
	}
	return ehci_submit_async(dev, pipe, buffer, length, setup);
}

struct int_queue {
	int elementsize;
	unsigned long pipe;
	struct QH *first;
	struct QH *current;
	struct QH *last;
	struct qTD *tds;
	struct int_queue *next_quarantined;
	struct int_queue *next_active;
	int linked;
	int one_shot;
	struct ehci_periodic_plan plan;
};

static struct int_queue *quarantined_interrupt;

static void ehci_release_int_queue(struct int_queue *queue)
{
	if (!queue)
		return;
	free(queue->tds);
	free(queue->first);
	free(queue);
}

static void ehci_quarantine_int_queue(struct int_queue *queue)
{
	queue->next_quarantined = quarantined_interrupt;
	quarantined_interrupt = queue;
	ehci_recovery_required = 1;
}

static void ehci_reclaim_interrupt_after_reset(void)
{
	while (quarantined_interrupt) {
		struct int_queue *queue = quarantined_interrupt;

		quarantined_interrupt = queue->next_quarantined;
		ehci_release_int_queue(queue);
	}
	active_interrupt_queues = NULL;
}

#define NEXT_QH(qh) (struct QH *)((unsigned long)hc32_to_cpu((qh)->qh_link) & ~0x1f)

static int
enable_periodic(struct ehci_ctrl *ctrl)
{
	uint32_t cmd;
	struct ehci_hcor *hcor = ctrl->hcor;
	int ret;

	cmd = ehci_readl(&hcor->or_usbcmd);
	cmd |= CMD_PSE;
	ehci_writel(&hcor->or_usbcmd, cmd);

	ret = handshake((uint32_t *)&hcor->or_usbsts,
			STS_PSS, STS_PSS, 1000);
	if (ret < 0) {
		return -ETIMEDOUT;
	}
	udelay(100);
	return 0;
}

static int
disable_periodic(struct ehci_ctrl *ctrl)
{
	uint32_t cmd;
	struct ehci_hcor *hcor = ctrl->hcor;
	int ret;

	cmd = ehci_readl(&hcor->or_usbcmd);
	cmd &= ~CMD_PSE;
	ehci_writel(&hcor->or_usbcmd, cmd);

	ret = handshake((uint32_t *)&hcor->or_usbsts,
			STS_PSS, 0, 1000);
	if (ret < 0) {
		return -ETIMEDOUT;
	}
	return 0;
}

int ehci_periodic_schedule_pause(struct ehci_ctrl *ctrl)
{
	int result;

	if (!ctrl)
		return -EINVAL;
	if (ctrl->periodic_schedules <= 0)
		return 0;
	result = disable_periodic(ctrl);
	if (result < 0)
		ehci_recovery_required = 1;
	return result;
}

int ehci_periodic_schedule_resume(struct ehci_ctrl *ctrl, int delta)
{
	if (!ctrl || ctrl->periodic_schedules + delta < 0)
		return -EINVAL;
	ctrl->periodic_schedules += delta;
	if (ctrl->periodic_schedules > 0 && enable_periodic(ctrl) < 0) {
		ehci_recovery_required = 1;
		return -ETIMEDOUT;
	}
	return 0;
}

static unsigned ehci_interrupt_frame_interval(
	const struct int_queue *queue)
{
	unsigned interval =
		ehci_periodic_hardware_frame_interval(&queue->plan);

	return interval;
}

static void ehci_insert_interrupt_queue(struct int_queue *queue)
{
	struct int_queue **link = &active_interrupt_queues;
	unsigned interval = ehci_interrupt_frame_interval(queue);

	while (*link &&
	       ehci_interrupt_frame_interval(*link) >= interval)
		link = &(*link)->next_active;
	queue->next_active = *link;
	*link = queue;
}

static int ehci_remove_interrupt_queue(struct int_queue *queue)
{
	struct int_queue **link = &active_interrupt_queues;

	while (*link && *link != queue)
		link = &(*link)->next_active;
	if (!*link)
		return 0;
	*link = queue->next_active;
	queue->next_active = NULL;
	return 1;
}

static uint32_t *ehci_periodic_interrupt_link(struct ehci_ctrl *ctrl,
					      unsigned frame)
{
	uint32_t *link = &ctrl->periodic_list[frame];
	unsigned guard = 0;

	while (guard++ < 128U) {
		uint32_t value = hc32_to_cpu(*link);
		uint32_t type;

		if (value & QH_LINK_TERMINATE)
			return link;
		type = value & 0x06U;
		if (type == QH_LINK_TYPE_QH)
			return link;
		if (type != QH_LINK_TYPE_ITD &&
		    type != QH_LINK_TYPE_SITD)
			return NULL;
		link = (uint32_t *)(unsigned long)(value & ~0x1fU);
	}
	return NULL;
}

static int ehci_rebuild_interrupt_schedule(struct ehci_ctrl *ctrl)
{
	struct int_queue *queue;
	unsigned frame;

	for (queue = active_interrupt_queues; queue;
	     queue = queue->next_active) {
		struct int_queue *next = queue->next_active;
		uint32_t target;

		while (next && next->one_shot)
			next = next->next_active;
		target = (!queue->one_shot && next) ?
			(uint32_t)(unsigned long)next->first :
			(uint32_t)(unsigned long)&ctrl->periodic_queue;
		queue->last->qh_link =
			cpu_to_hc32(target | QH_LINK_TYPE_QH);
		flush_dcache_range((unsigned long)queue->last,
				   ALIGN_END_ADDR(struct QH, queue->last, 1));
	}

	for (frame = 0; frame < 1024U; frame++) {
		uint32_t *link = ehci_periodic_interrupt_link(ctrl, frame);
		uint32_t target;

		if (!link)
			return -EINVAL;
		queue = NULL;
		for (struct int_queue *candidate = active_interrupt_queues;
		     candidate; candidate = candidate->next_active) {
			if (candidate->one_shot &&
			    ehci_periodic_frame_due(
				    &candidate->plan, (uint16_t)frame)) {
				queue = candidate;
				break;
			}
		}
		if (!queue) {
			for (queue = active_interrupt_queues; queue;
			     queue = queue->next_active) {
				if (!queue->one_shot &&
				    ehci_periodic_frame_due(
					    &queue->plan, (uint16_t)frame))
					break;
			}
		}
		target = queue ?
			(uint32_t)(unsigned long)queue->first :
			(uint32_t)(unsigned long)&ctrl->periodic_queue;
		*link = cpu_to_hc32(target | QH_LINK_TYPE_QH);
		flush_dcache_range((unsigned long)link,
				   ALIGN_END_ADDR(uint32_t, link, 1));
	}
	return 0;
}

static struct int_queue *_ehci_create_int_queue(struct usb_device *dev,
			unsigned long pipe, int queuesize, int elementsize,
			void *buffer, int interval, int one_shot)
{
	struct ehci_ctrl *ctrl = ehci_get_ctrl(dev);
	struct int_queue *result = NULL;
	struct ehci_periodic_plan plan;
	uint32_t i, toggle;
	int split = dev->parent && dev->parent->parent;
	unsigned slot_seed = (unsigned)usb_pipeendpoint(pipe) +
			     (unsigned)dev->portnr;

	if (!ehci_periodic_build_plan(
		    dev->speed, (unsigned)interval, split,
		    dev->tt_think_time, dev->tt_multi, slot_seed, &plan))
		return NULL;
	if (ehci_recovery_required)
		return NULL;
	if (one_shot) {
		uint32_t frame_index = ehci_readl(&ctrl->hcor->or_frindex);

		plan.interval_microframes = 8192U;
		plan.frame_interval = 1024U;
		plan.frame_phase = (uint16_t)(
			((frame_index >> 3) + 2U) & 0x3ffU);
		plan.start_mask = 0x01U;
		plan.complete_mask = 0;
	}

	/*
	 * Each queue element must fit in one transaction and one qTD. Persistent
	 * queues are rearmed after completion instead of chaining transactions
	 * that could execute within the same service interval.
	 */
	if (elementsize > usb_maxpacket(dev, pipe)) {
		return NULL;
	}

	if (usb_pipetype(pipe) != PIPE_INTERRUPT) {
		return NULL;
	}

	/* limit to 4 full pages worth of data -
	 * we can safely fit them in a single TD,
	 * no matter the alignment
	 */
	if (elementsize >= 16384) {
		return NULL;
	}

	result = malloc(sizeof(*result));  // FIXME dynamic allocation
	if (!result) {
		goto fail1;
	}
	memset(result, 0, sizeof(*result));
	result->elementsize = elementsize;
	result->pipe = pipe;
	result->plan = plan;
	result->first = memalign(USB_DMA_MINALIGN,
				 sizeof(struct QH) * queuesize); // FIXME dynamic allocation
	if (!result->first) {
		goto fail2;
	}
	result->one_shot = one_shot;
	result->current = result->first;
	result->last = result->first + queuesize - 1;
	result->tds = memalign(USB_DMA_MINALIGN,
			       sizeof(struct qTD) * queuesize); // FIXME dynamic allocation
	if (!result->tds) {
		goto fail3;
	}
	memset(result->first, 0, sizeof(struct QH) * queuesize);
	memset(result->tds, 0, sizeof(struct qTD) * queuesize);

	toggle = usb_gettoggle(dev, usb_pipeendpoint(pipe), usb_pipeout(pipe));

	for (i = 0; i < queuesize; i++) {
		struct QH *qh = result->first + i;
		struct qTD *td = result->tds + i;
		void **buf = &qh->buffer;

		qh->qh_link = cpu_to_hc32((unsigned long)(qh+1) | QH_LINK_TYPE_QH);
		if (i == queuesize - 1)
			qh->qh_link = cpu_to_hc32(QH_LINK_TERMINATE);

		qh->qh_overlay.qt_next = cpu_to_hc32((unsigned long)td);
		qh->qh_overlay.qt_altnext = cpu_to_hc32(QT_NEXT_TERMINATE);
		qh->qh_endpt1 =
			cpu_to_hc32((0 << 28) | /* No NAK reload (ehci 4.9) */
			(usb_maxpacket(dev, pipe) << 16) | /* MPS */
			(1 << 14) |
			QH_ENDPT1_EPS(ehci_encode_speed(dev->speed)) |
			(usb_pipeendpoint(pipe) << 8) | /* Endpoint Number */
			(usb_pipedevice(pipe) << 0));
		qh->qh_endpt2 = cpu_to_hc32((1 << 30) |
			(uint32_t)plan.start_mask |
			((uint32_t)plan.complete_mask << 8));
		ehci_update_endpt2_dev_n_port(dev, qh);

		td->qt_next = cpu_to_hc32(QT_NEXT_TERMINATE);
		td->qt_altnext = cpu_to_hc32(QT_NEXT_TERMINATE);
		td->qt_token = cpu_to_hc32(
			QT_TOKEN_DT(toggle) |
			(elementsize << 16) |
			(3 << 10) | /* CERR=3 */
			((usb_pipein(pipe) ? 1 : 0) << 8) | /* IN/OUT token */
			0x80); /* active */
		td->qt_buffer[0] =
		    cpu_to_hc32((unsigned long)buffer + i * elementsize);
		td->qt_buffer[1] =
		    cpu_to_hc32((td->qt_buffer[0] + 0x1000) & ~0xfff);
		td->qt_buffer[2] =
		    cpu_to_hc32((td->qt_buffer[0] + 0x2000) & ~0xfff);
		td->qt_buffer[3] =
		    cpu_to_hc32((td->qt_buffer[0] + 0x3000) & ~0xfff);
		td->qt_buffer[4] =
		    cpu_to_hc32((td->qt_buffer[0] + 0x4000) & ~0xfff);

		*buf = buffer + i * elementsize;
		toggle ^= 1;
	}

	flush_dcache_range((unsigned long)buffer,
			   ALIGN_END_ADDR(char, buffer,
					  queuesize * elementsize));
	flush_dcache_range((unsigned long)result->first,
			   ALIGN_END_ADDR(struct QH, result->first,
					  queuesize));
	flush_dcache_range((unsigned long)result->tds,
			   ALIGN_END_ADDR(struct qTD, result->tds,
					  queuesize));

	if (ctrl->periodic_schedules > 0) {
		if (disable_periodic(ctrl) < 0) {
			ehci_recovery_required = 1;
			goto fail3;
		}
	}

	/* Build a power-of-two frame skeleton so bInterval > 1 queues are
	 * unreachable in frames where the endpoint must not be polled. */
	ehci_insert_interrupt_queue(result);
	result->linked = 1;
	if (ehci_rebuild_interrupt_schedule(ctrl) < 0) {
		ehci_recovery_required = 1;
		ehci_quarantine_int_queue(result);
		return NULL;
	}

	if (enable_periodic(ctrl) < 0) {
		ehci_quarantine_int_queue(result);
		return NULL;
	}
	ctrl->periodic_schedules++;

	return result;
fail3:
	if (result->tds)
		free(result->tds);
fail2:
	if (result->first)
		free(result->first);
	if (result)
		free(result);
fail1:
	return NULL;
}


static void *_ehci_poll_int_queue(struct usb_device *dev,
				  struct int_queue *queue)
{
	struct QH *cur = queue->current;
	struct qTD *cur_td;
	uint32_t token, toggle;
	uint8_t qtd_status;
	unsigned long pipe = queue->pipe;

	/* depleted queue */
	if (cur == NULL) {
		return NULL;
	}
	/* still active */
	cur_td = &queue->tds[queue->current - queue->first];
	invalidate_dcache_range((unsigned long)cur_td,
				ALIGN_END_ADDR(struct qTD, cur_td, 1));
	token = hc32_to_cpu(cur_td->qt_token);
	if (QT_TOKEN_GET_STATUS(token) & QT_TOKEN_STATUS_ACTIVE) {
		/*
		 * Silent hot path: mouse/keyboard NAK every microframe
		 * while idle, so printing here would block the UART and
		 * starve the Zorro bus handler. Diagnostics available via
		 * the completion / destroy paths.
		 */
		return NULL;
	}

	toggle = QT_TOKEN_GET_DT(token);
	usb_settoggle(dev, usb_pipeendpoint(pipe), usb_pipeout(pipe), toggle);

	/*
	 * Round-9 status decoding — propagate all error classes that
	 * a regular EHCI async path would surface. Mirroring the
	 * async/bulk path lets the Amiga driver see CRC/BUF/BABBLE
	 * errors exactly the way it expects.
	 */
	dev->act_len = queue->elementsize - QT_TOKEN_GET_TOTALBYTES(token);
	qtd_status = (uint8_t)(QT_TOKEN_GET_STATUS(token) &
		~(QT_TOKEN_STATUS_SPLITXSTATE | QT_TOKEN_STATUS_PERR));
	if (ehci_periodic_qtd_missed(qtd_status)) {
		/*
		 * The controller did not execute this periodic opportunity.
		 * Leave the interrupt request pending and rearm it instead of
		 * reporting a packet CRC error for a transaction that never ran.
		 */
		dev->status = USB_ST_NAK_REC;
	} else {
		switch (qtd_status) {
		case 0:
			dev->status = 0;
			break;
		case QT_TOKEN_STATUS_HALTED:
			dev->status = USB_ST_STALLED;
			break;
		case QT_TOKEN_STATUS_ACTIVE | QT_TOKEN_STATUS_DATBUFERR:
		case QT_TOKEN_STATUS_HALTED | QT_TOKEN_STATUS_DATBUFERR:
		case QT_TOKEN_STATUS_DATBUFERR:
			dev->status = USB_ST_BUF_ERR;
			break;
		case QT_TOKEN_STATUS_HALTED | QT_TOKEN_STATUS_BABBLEDET:
		case QT_TOKEN_STATUS_BABBLEDET:
			dev->status = USB_ST_BABBLE_DET;
			break;
		default:
			dev->status = USB_ST_CRC_ERR;
			break;
		}
	}

	if (cur != queue->last)
		queue->current++;
	else
		queue->current = NULL;

	invalidate_dcache_range((unsigned long)cur->buffer,
				ALIGN_END_ADDR(char, cur->buffer,
					       queue->elementsize));

	return cur->buffer;
}

static int _ehci_rearm_int_queue(struct usb_device *dev,
				 struct int_queue *queue)
{
	struct QH *qh;
	struct qTD *td;
	unsigned long pipe;
	uint32_t toggle;
	uint32_t address;

	if (!queue || !queue->linked || queue->first != queue->last)
		return -EINVAL;

	qh = queue->first;
	td = queue->tds;
	pipe = queue->pipe;
	toggle = usb_gettoggle(dev, usb_pipeendpoint(pipe), usb_pipeout(pipe));
	address = (uint32_t)(unsigned long)qh->buffer;

	memset(td, 0, sizeof(*td));
	td->qt_next = cpu_to_hc32(QT_NEXT_TERMINATE);
	td->qt_altnext = cpu_to_hc32(QT_NEXT_TERMINATE);
	td->qt_token = cpu_to_hc32(
		QT_TOKEN_DT(toggle) |
		(queue->elementsize << 16) |
		(3 << 10) |
		((usb_pipein(pipe) ? 1 : 0) << 8) |
		QT_TOKEN_STATUS_ACTIVE);
	td->qt_buffer[0] = cpu_to_hc32(address);
	td->qt_buffer[1] = cpu_to_hc32((address + 0x1000) & ~0xfff);
	td->qt_buffer[2] = cpu_to_hc32((address + 0x2000) & ~0xfff);
	td->qt_buffer[3] = cpu_to_hc32((address + 0x3000) & ~0xfff);
	td->qt_buffer[4] = cpu_to_hc32((address + 0x4000) & ~0xfff);
	flush_dcache_range((unsigned long)qh->buffer,
			   ALIGN_END_ADDR(char, qh->buffer, queue->elementsize));
	flush_dcache_range((unsigned long)td,
			   ALIGN_END_ADDR(struct qTD, td, 1));

	qh->qh_curtd = 0;
	memset(&qh->qh_overlay, 0, sizeof(qh->qh_overlay));
	qh->qh_overlay.qt_next = cpu_to_hc32((unsigned long)td);
	qh->qh_overlay.qt_altnext = cpu_to_hc32(QT_NEXT_TERMINATE);
	queue->current = qh;
	flush_dcache_range((unsigned long)qh,
			   ALIGN_END_ADDR(struct QH, qh, 1));
	return 0;
}

/* Do not free buffers associated with QHs, they're owned by someone else */
static int _ehci_destroy_int_queue(struct usb_device *dev,
				   struct int_queue *queue)
{
	struct ehci_ctrl *ctrl = ehci_get_ctrl(dev);
	int result = 0;

	if (!queue)
		return 0;
	if (!queue->linked) {
		ehci_release_int_queue(queue);
		return 0;
	}

	if (disable_periodic(ctrl) < 0) {
		ehci_quarantine_int_queue(queue);
		return -ETIMEDOUT;
	}
	if (ctrl->periodic_schedules > 0)
		ctrl->periodic_schedules--;

	if (!ehci_remove_interrupt_queue(queue) ||
	    ehci_rebuild_interrupt_schedule(ctrl) < 0) {
		ehci_recovery_required = 1;
		ehci_quarantine_int_queue(queue);
		return -ETIMEDOUT;
	}

	/* PSS=0 acknowledged retirement before the queue left every frame. */
	queue->linked = 0;
	if (ctrl->periodic_schedules > 0 && enable_periodic(ctrl) < 0) {
		ehci_recovery_required = 1;
		result = -ETIMEDOUT;
	}

	ehci_release_int_queue(queue);
	return result;
}

int ehci_controller_needs_recovery(void)
{
	return ehci_recovery_required;
}

int ehci_async_schedule_active(struct ehci_ctrl *ctrl)
{
	return ctrl && ctrl->hcor &&
	       (ehci_readl(&ctrl->hcor->or_usbsts) & STS_ASS) != 0;
}

int ehci_controller_recover(void)
{
	struct ehci_ctrl *ctrl = &ehcic[0];
	int result;

	if (!ehci_recovery_required)
		return 0;
	if (!ctrl->hccr || !ctrl->hcor)
		return -ENODEV;

	result = ehci_reset(ctrl);
	if (result < 0)
		return result;

	/* CMD_RESET acknowledgement is the only safe bulk reclaim fence. */
	ehci_reclaim_async_after_reset();
	ehci_reclaim_interrupt_after_reset();
	result = ehci_common_init(ctrl, 0);
	if (result < 0)
		return result;
	ehci_recovery_required = 0;
	return 0;
}


/*
 * Interrupt-xfer poll budget: keep it short so the ZZ9000 main loop can
 * return to servicing the Amiga Zorro bus quickly. An idle HID device
 * (mouse/keyboard) NAKs the IN token forever and the qTD never retires;
 * the old 100 ms budget starved Zorro DTACK and guru-reset the Amiga.
 * Keep this below the Zorro-bus blocking budget. A high-speed hub with
 * a few idle HID-style interrupt endpoints can otherwise make the ARM
 * spend most of its time polling EHCI instead of servicing the Amiga.
 * Missed reports are harmless: the Amiga-side driver keeps the IOR
 * queued and retries later.
 */
#define EHCI_INT_POLL_MS 2

static int _ehci_submit_int_msg(struct usb_device *dev, unsigned long pipe,
				void *buffer, int length, int interval)
{
	void *backbuffer;
	struct int_queue *queue;
	unsigned long timeout;
	int result = 0, ret;

	queue = _ehci_create_int_queue(dev, pipe, 1, length, buffer, interval,
				       0);
	if (!queue)
		return -1;

	timeout = get_timer(0);
	while ((backbuffer = _ehci_poll_int_queue(dev, queue)) == NULL)
		if (ehci_deadline_expired_u32((uint32_t)get_timer(0),
					      (uint32_t)timeout,
					      EHCI_INT_POLL_MS)) {
			/*
			 * No qTD retired within our poll window. For an
			 * interrupt endpoint this is normal USB behavior —
			 * the device NAK'd the IN token because it has no
			 * new report yet (idle mouse / keyboard).
			 *
			 * Report this to callers as a clean success with
			 * zero bytes, NOT an error. Returning an error
			 * (previously USB_ST_NAK_REC -> ZZUSB_STATUS_NAK
			 * -> UHIOERR_TIMEOUT on the Amiga side) makes
			 * Poseidon flag the endpoint as dead after a
			 * handful of idle polls. USB spec semantics: a
			 * NAK just means "no data this cycle, poll again."
			 */
			dev->status = 0;
			dev->act_len = 0;
			break;
		}

	/*
	 * If we fell through with a non-NULL backbuffer but it isn't
	 * our expected buffer pointer, treat that as a programming
	 * error (never seen in practice). Still destroy the queue
	 * before returning so the periodic list stays clean.
	 */
	if (backbuffer && backbuffer != buffer) {
		dev->status = USB_ST_BUF_ERR;
		result = -EINVAL;
	}

	ret = _ehci_destroy_int_queue(dev, queue);
	if (ret < 0 && result == 0)
		result = ret;

	return result;
}
/*
 * Execute one high-speed interrupt opportunity at microframe zero of a
 * selected near-future frame. Unlike a persistent qTD, this queue is linked
 * into only that frame-list slot and then unlinked, so software can honor
 * intervals longer than the 1024-frame EHCI list without exposing the
 * endpoint on adjacent frames or list rollovers.
 */
static int _ehci_submit_int_msg_once(struct usb_device *dev,
				     unsigned long pipe,
				     void *buffer, int length)
{
	struct ehci_ctrl *ctrl = ehci_get_ctrl(dev);
	struct int_queue *queue;
	void *backbuffer = NULL;
	unsigned long timeout;
	uint16_t target_frame;
	int result = 0;
	int ret;

	if (!ctrl || dev->speed != USB_SPEED_HIGH)
		return -EINVAL;
	queue = _ehci_create_int_queue(dev, pipe, 1, length, buffer, 4, 1);
	if (!queue)
		return -1;
	target_frame = queue->plan.frame_phase;
	timeout = get_timer(0);
	for (;;) {
		uint32_t frame_index;

		backbuffer = _ehci_poll_int_queue(dev, queue);
		if (backbuffer)
			break;
		frame_index = ehci_readl(&ctrl->hcor->or_frindex);
		{
			uint16_t frame = (uint16_t)(
				(frame_index >> 3) & 0x3ffU);
			uint16_t distance = (uint16_t)(
				(frame - target_frame) & 0x3ffU);

			if ((distance == 0U && (frame_index & 7U) != 0U) ||
			    (distance > 0U && distance < 512U)) {
				backbuffer = _ehci_poll_int_queue(dev, queue);
				if (!backbuffer) {
					dev->status = usb_pipein(pipe) ?
						0 : USB_ST_NAK_REC;
					dev->act_len = 0;
				}
				break;
			}
		}
		if (ehci_deadline_expired_u32((uint32_t)get_timer(0),
					      (uint32_t)timeout, 5U)) {
			dev->status = usb_pipein(pipe) ?
				0 : USB_ST_NAK_REC;
			dev->act_len = 0;
			break;
		}
	}
	if (backbuffer && backbuffer != buffer) {
		dev->status = USB_ST_BUF_ERR;
		result = -EINVAL;
	}
	ret = _ehci_destroy_int_queue(dev, queue);
	if (ret < 0 && result == 0)
		result = ret;
	return result;
}


int submit_bulk_msg(struct usb_device *dev, unsigned long pipe,
			    void *buffer, int length)
{
	return _ehci_submit_bulk_msg(dev, pipe, buffer, length);
}

int submit_control_msg(struct usb_device *dev, unsigned long pipe, void *buffer,
		   int length, struct devrequest *setup)
{
	return _ehci_submit_control_msg(dev, pipe, buffer, length, setup);
}

int submit_int_msg(struct usb_device *dev, unsigned long pipe,
		   void *buffer, int length, int interval)
{
	return _ehci_submit_int_msg(dev, pipe, buffer, length, interval);
}
int submit_int_msg_once(struct usb_device *dev, unsigned long pipe,
			void *buffer, int length)
{
	return _ehci_submit_int_msg_once(dev, pipe, buffer, length);
}


struct int_queue *create_int_queue(struct usb_device *dev,
		unsigned long pipe, int queuesize, int elementsize,
		void *buffer, int interval)
{
	return _ehci_create_int_queue(dev, pipe, queuesize, elementsize,
				      buffer, interval, 0);
}

void *poll_int_queue(struct usb_device *dev, struct int_queue *queue)
{
	return _ehci_poll_int_queue(dev, queue);
}

int rearm_int_queue(struct usb_device *dev, struct int_queue *queue)
{
	return _ehci_rearm_int_queue(dev, queue);
}

int destroy_int_queue(struct usb_device *dev, struct int_queue *queue)
{
	return _ehci_destroy_int_queue(dev, queue);
}

int ehci_register(struct ehci_ctrl *ctrl, struct ehci_hccr *hccr,
		  struct ehci_hcor *hcor, const struct ehci_ops *ops,
		  uint tweaks, enum usb_init_type init)
{
	//struct usb_bus_priv *priv = dev_get_uclass_priv(dev);
	//struct ehci_ctrl *ctrl = dev_get_priv(dev);
	int ret = -1;

	printf("%s: dev='%s', ctrl=%p, hccr=%p, hcor=%p, init=%d\n", __func__,
	      "zynq-ehci", ctrl, hccr, hcor, init);

	if (!ctrl || !hccr || !hcor)
		goto err;

	// FIXME
	//priv->desc_before_addr = true;

	ehci_setup_ops(ctrl, ops);
	ctrl->hccr = hccr;
	ctrl->hcor = hcor;
	ctrl->priv = ctrl;

	ctrl->init = init;
	if (ctrl->init == USB_INIT_DEVICE)
		goto done;

	ret = ehci_reset(ctrl);
	if (ret)
		goto err;

	if (ctrl->ops.init_after_reset) {
		ret = ctrl->ops.init_after_reset(ctrl);
		if (ret)
			goto err;
	}

	//ret = ehci_common_init(ctrl, tweaks);
	if (ret)
		goto err;
done:
	return 0;
err:
	free(ctrl);
	printf("%s: failed, ret=%d\n", __func__, ret);
	return ret;
}

#ifdef CONFIG_PHY
int ehci_setup_phy(struct udevice *dev, struct phy *phy, int index)
{
	int ret;

	if (!phy)
		return 0;

	ret = generic_phy_get_by_index(dev, index, phy);
	if (ret) {
		if (ret != -ENOENT) {
			dev_err(dev, "failed to get usb phy\n");
			return ret;
		}
	} else {
		ret = generic_phy_init(phy);
		if (ret) {
			dev_err(dev, "failed to init usb phy\n");
			return ret;
		}

		ret = generic_phy_power_on(phy);
		if (ret) {
			dev_err(dev, "failed to power on usb phy\n");
			return generic_phy_exit(phy);
		}
	}

	return 0;
}

int ehci_shutdown_phy(struct udevice *dev, struct phy *phy)
{
	int ret = 0;

	if (!phy)
		return 0;

	if (generic_phy_valid(phy)) {
		ret = generic_phy_power_off(phy);
		if (ret) {
			dev_err(dev, "failed to power off usb phy\n");
			return ret;
		}

		ret = generic_phy_exit(phy);
		if (ret) {
			dev_err(dev, "failed to power off usb phy\n");
			return ret;
		}
	}

	return 0;
}
#else
int ehci_setup_phy(struct udevice *dev, struct phy *phy, int index)
{
	return 0;
}

int ehci_shutdown_phy(struct udevice *dev, struct phy *phy)
{
	return 0;
}
#endif
