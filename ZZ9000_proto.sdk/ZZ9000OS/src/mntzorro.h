/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "platform.h"
#include "memorymap.h"
#include "xil_io.h"

// FIXME!
#define MNTZ_BASE_ADDR 0x43C00000

#define MNTZORRO_REG0 0
#define MNTZORRO_REG1 4
#define MNTZORRO_REG2 8
#define MNTZORRO_REG3 12
#define MNTZORRO_REG4 16
#define MNTZORRO_REG5 20
#define MNTZORRO_REG6 24
#define MNTZORRO_REG7 28

/* Read direction of REG6: generation-2 host aperture acknowledgement. */
#define MNTZORRO_APERTURE_ACK_STATUS 0xa5020001UL

#define MNTZORRO_STATUS_SDK_IRQ_ACK     (1UL << 18)
#define MNTZORRO_STATUS_SDK_DOORBELL    (1UL << 19)
#define MNTZORRO_STATUS_VCAP_VIEWPORT   (1UL << 15)

/* REG3 bit 14: the loaded bitstream carries the source-sync output
 * controller (delay-locked output cadence from the measured capture
 * anchor). Old bitstreams keep bits [14:8] zero. */
#define MNTZORRO_STATUS_VCAP_SOURCE_SYNC (1UL << 14)
#define MNTZORRO_STATUS_VCAP_FULLRATE   (1UL << 16)

/* REG3 bit 13: the bitstream exposes the source-sync diagnostic bus on
 * the REG4/REG5 read direction (see MNTZORRO_REG4_RD_SOURCE_SYNC_DIAG_LO
 * below). Non-diagnostic bitstreams keep bit 13 clear; only use the
 * readback when this capability is set. */
#define MNTZORRO_STATUS_VCAP_SOURCE_SYNC_DIAG (1UL << 13)

/* Read direction of REG4/REG5 (their write meanings are unchanged:
 * ethernet RX-buffer return on REG4, interrupt/codec control on REG5).
 * A REG4 read returns the low 32 bits of the coherent source-sync
 * diagnostic snapshot and latches all 64 bits; the immediately
 * following REG5 read returns the latched high 32 bits. Layout, from
 * the sync controller:
 *   [63:48] last completed anchor interval (output lines)
 *   [47:32] anchor age at last frame wrap (output lines; pre-edge:
 *           sampled before the wrap update)
 *   [31:20] last completed raster frame total (raster_y + 1 at wrap)
 *   [19:16] rejected anchor interval count (saturates at 15)
 *   [15:12] cfg_changed_d1 cycle count (saturates at 15)
 *   [11:8]  anchor event count (modulo 16)
 *   [7] most recent wrap was max-forced  [6] current raw interlace
 *   [5] sticky source watchdog fired     [4] bounds_valid
 *   [3] video_hidden                     [2] picture_ok
 *   [1] locked                           [0] enable
 */
#define MNTZORRO_REG4_RD_SOURCE_SYNC_DIAG_LO MNTZORRO_REG4
#define MNTZORRO_REG5_RD_SOURCE_SYNC_DIAG_HI MNTZORRO_REG5

#define MNTZORRO_CTRL_SDK_IRQ_ACK_CLEAR  (1UL << 28)
#define MNTZORRO_CTRL_SDK_DOORBELL_CLEAR (1UL << 29)

#define mntzorro_read(BaseAddress, RegOffset) \
  Xil_In32((BaseAddress) + (RegOffset))

#define mntzorro_write(BaseAddress, RegOffset, Data) \
  	Xil_Out32((BaseAddress) + (RegOffset), (u32)(Data))


/*const char* zstates[53] = { "RESET   ", "Z2_CONF ", "Z2_IDLE ", "WAIT_WRI",
			"WAIT_WR2", "Z2WRIFIN", "WAIT_RD ", "WAIT_RD2", "WAIT_RD3",
			"CONFIGED", "CONF_CLR", "D_Z2_Z3 ", "Z3_IDLE ", "Z3_WRITE_UPP",
			"Z3_WRITE_LOW", "Z3_READ_UP", "Z3_READ_LOW", "Z3_READ_DLY",
			"Z3_READ_DLY1", "Z3_READ_DLY2", "Z3_WRITE_PRE", "Z3_WRITE_FIN",
			"Z3_ENDCYCLE", "Z3_DTACK", "Z3_CONFIG", "Z2_REGWRITE", "REGWRITE",
			"REGREAD", "Z2_REGR_POST", "Z3_REGR_POST", "Z3_REGWRITE",
			"Z2_REGREAD", "Z3_REGREAD", "NONE_33", "Z2_PRE_CONF", "Z2_ENDCYCLE",
			"NONE_36", "NONE_37", "NONE_38", "RESET_DVID", "COLD", "WR2B",
			"WR2C", "Z3DMA1", "Z3DMA2", "Z3_AUTOCONF_RD", "Z3_AUTOCONF_WR",
			"Z3_AUTOCONF_RD_DLY", "Z3_AUTOCONF_RD_DLY2", "Z3_REGWRITE_PRE",
			"Z3_REGREAD_PRE", "Z3_WRITE_PRE2", "UNDEF", };*/
