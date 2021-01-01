/*
 * MNT ZZ9000 Amiga Graphics and Coprocessor Card Operating System (ZZ9000OS)
 *
 * Copyright (C) 2020, Lukas F. Hartmann <lukas@mntre.com>
 *                     MNT Research GmbH, Berlin
 *                     https://mntre.com
 *
 * More Info: https://mntre.com/zz9000
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * GNU General Public License v3.0 or later
 *
 * https://spdx.org/licenses/GPL-3.0-or-later.html
 *
 */

// Registers offsets relative to the register base, although the offset on the ARM side is always 0.
enum zz_reg_offsets {
  REG_ZZ_UNUSED_REG00   = 0x00,
  REG_ZZ_MODE           = 0x02,
  REG_ZZ_CONFIG         = 0x04,
  REG_ZZ_SPRITE_X       = 0x06,
  REG_ZZ_SPRITE_Y       = 0x08,
  REG_ZZ_PAN_HI         = 0x0A,
  REG_ZZ_PAN_LO         = 0x0C,
  REG_ZZ_VCAP_MODE      = 0x0E,
  
  REG_ZZ_X1             = 0x10,
  REG_ZZ_Y1             = 0x12,
  REG_ZZ_X2             = 0x14,
  REG_ZZ_Y2             = 0x16,
  REG_ZZ_ROW_PITCH      = 0x18,
  REG_ZZ_X3             = 0x1A,
  REG_ZZ_Y3             = 0x1C,
  REG_ZZ_RGB_HI         = 0x1E,

  REG_ZZ_RGB_LO         = 0x20,
  REG_ZZ_FILLRECT       = 0x22,
  REG_ZZ_COPYRECT       = 0x24,
  REG_ZZ_FILLTEMPLATE   = 0x26,
  REG_ZZ_BLIT_SRC_HI    = 0x28,
  REG_ZZ_BLIT_SRC_LO    = 0x2A,
  REG_ZZ_BLIT_DST_HI    = 0x2C,
  REG_ZZ_BLIT_DST_LO    = 0x2E,

  REG_ZZ_COLORMODE      = 0x30,
  REG_ZZ_SRC_PITCH      = 0x32,
  REG_ZZ_RGB2_HI        = 0x34,
  REG_ZZ_RGB2_LO        = 0x36,
  REG_ZZ_P2C            = 0x38,
  REG_ZZ_DRAWLINE       = 0x3A,
  REG_ZZ_P2D            = 0x3C,
  REG_ZZ_INVERTRECT     = 0x3E,

  REG_ZZ_USER1          = 0x40,
  REG_ZZ_USER2          = 0x42,
  REG_ZZ_USER3          = 0x44,
  REG_ZZ_USER4          = 0x46,
  REG_ZZ_SPRITE_BITMAP  = 0x48,
  REG_ZZ_SPRITE_COLORS  = 0x4A,
  REG_ZZ_VBLANK_STATUS  = 0x4C,
  REG_ZZ_UNUSED_REG4E   = 0x4E,

  REG_ZZ_UNUSED_REG50   = 0x50,
  REG_ZZ_UNUSED_REG52   = 0x52,
  REG_ZZ_UNUSED_REG54   = 0x54,
  REG_ZZ_UNUSED_REG56   = 0x56,
  REG_ZZ_UNUSED_REG58   = 0x58,
  REG_ZZ_BITTER_DMA_OP  = 0x5A,
  REG_ZZ_ACC_OP         = 0x5C,
  REG_ZZ_SET_SPLIT_POS  = 0x5E,

  REG_ZZ_UNUSED_REG60   = 0x60,
  REG_ZZ_UNUSED_REG62   = 0x62,
  REG_ZZ_UNUSED_REG64   = 0x64,
  REG_ZZ_UNUSED_REG66   = 0x66,
  REG_ZZ_UNUSED_REG68   = 0x68,
  REG_ZZ_UNUSED_REG6A   = 0x6A,
  REG_ZZ_UNUSED_REG6C   = 0x6C,
  REG_ZZ_UNUSED_REG6E   = 0x6E,

  REG_ZZ_UNUSED_REG70   = 0x70,
  REG_ZZ_UNUSED_REG72   = 0x72,
  REG_ZZ_UNUSED_REG74   = 0x74,
  REG_ZZ_UNUSED_REG76   = 0x76,
  REG_ZZ_UNUSED_REG78   = 0x78,
  REG_ZZ_UNUSED_REG7A   = 0x7A,
  REG_ZZ_UNUSED_REG7C   = 0x7C,
  REG_ZZ_UNUSED_REG7E   = 0x7E,

  REG_ZZ_ETH_TX         = 0x80,
  REG_ZZ_ETH_RX         = 0x82,
  REG_ZZ_ETH_MAC_HI     = 0x84,
  REG_ZZ_ETH_MAC_HI2    = 0x86,
  REG_ZZ_ETH_MAC_LO     = 0x88,
  REG_ZZ_UNUSED_REG8A   = 0x8A,
  REG_ZZ_UNUSED_REG8C   = 0x8C,
  REG_ZZ_UNUSED_REG8E   = 0x8E,

  REG_ZZ_ARM_RUN_HI     = 0x90,
  REG_ZZ_ARM_RUN_LO     = 0x92,
  REG_ZZ_ARM_ARGC       = 0x94,
  REG_ZZ_ARM_ARGV0      = 0x96,
  REG_ZZ_ARM_ARGV1      = 0x98,
  REG_ZZ_ARM_ARGV2      = 0x9A,
  REG_ZZ_ARM_ARGV3      = 0x9C,
  REG_ZZ_ARM_ARGV4      = 0x9E,

  REG_ZZ_ARM_ARGV5      = 0xA0,
  REG_ZZ_ARM_ARGV6      = 0xA2,
  REG_ZZ_ARM_ARGV7      = 0xA4,
  REG_ZZ_UNUSED_REGA6   = 0xA6,
  REG_ZZ_UNUSED_REGA8   = 0xA8,
  REG_ZZ_UNUSED_REGAA   = 0xAA,
  REG_ZZ_UNUSED_REGAC   = 0xAC,
  REG_ZZ_UNUSED_REGAE   = 0xAE,

  REG_ZZ_ARM_EV_SERIAL  = 0xB0,
  REG_ZZ_ARM_EV_CODE    = 0xB2,
  REG_ZZ_UNUSED_REGB4   = 0xB4,
  REG_ZZ_UNUSED_REGB6   = 0xB6,
  REG_ZZ_UNUSED_REGB8   = 0xB8,
  REG_ZZ_UNUSED_REGBA   = 0xBA,
  REG_ZZ_UNUSED_REGBC   = 0xBC,
  REG_ZZ_UNUSED_REGBE   = 0xBE,

  REG_ZZ_FW_VERSION     = 0xC0,
  REG_ZZ_UNUSED_REGC2   = 0xC2,
  REG_ZZ_UNUSED_REGC4   = 0xC4,
  REG_ZZ_UNUSED_REGC6   = 0xC6,
  REG_ZZ_UNUSED_REGC8   = 0xC8,
  REG_ZZ_UNUSED_REGCA   = 0xCA,
  REG_ZZ_UNUSED_REGCC   = 0xCC,
  REG_ZZ_UNUSED_REGCE   = 0xCE,

  REG_ZZ_USBBLK_TX_HI   = 0xD0,
  REG_ZZ_USBBLK_TX_LO   = 0xD2,
  REG_ZZ_USBBLK_RX_HI   = 0xD4,
  REG_ZZ_USBBLK_RX_LO   = 0xD6,
  REG_ZZ_USB_STATUS     = 0xD8,
  REG_ZZ_USB_BUFSEL     = 0xDA,
  REG_ZZ_USB_CAPACITY   = 0xDC,
  REG_ZZ_UNUSED_REGDE   = 0xDE,

  REG_ZZ_TEMPERATURE    = 0xE0,
  REG_ZZ_VOLTAGE_AUX    = 0xE2,
  REG_ZZ_VOLTAGE_INT    = 0xE4,
  REG_ZZ_UNUSED_REGE6   = 0xE6,
  REG_ZZ_UNUSED_REGE8   = 0xE8,
  REG_ZZ_UNUSED_REGEA   = 0xEA,
  REG_ZZ_UNUSED_REGEC   = 0xEC,
  REG_ZZ_UNUSED_REGEE   = 0xEE,

  REG_ZZ_UNUSED_REGF0   = 0xF0,
  REG_ZZ_UNUSED_REGF2   = 0xF2,
  REG_ZZ_UNUSED_REGF4   = 0xF4,
  REG_ZZ_UNUSED_REGF6   = 0xF6,
  REG_ZZ_UNUSED_REGF8   = 0xF8,
  REG_ZZ_UNUSED_REGFA   = 0xFA,
  REG_ZZ_DEBUG          = 0xFC,
  REG_ZZ_UNUSED_REGFE   = 0xFE,
};
