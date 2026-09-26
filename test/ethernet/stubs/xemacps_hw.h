/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TEST_ETHERNET_XEMACPS_HW_H
#define TEST_ETHERNET_XEMACPS_HW_H

#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

#define XEMACPS_NWCFG_OFFSET           0x00000004U
#define XEMACPS_HASHL_OFFSET           0x00000080U
#define XEMACPS_HASHH_OFFSET           0x00000084U
#define XEMACPS_NWCFG_MCASTHASHEN_MASK 0x00000040U

u32 XEmacPs_ReadReg(u32 BaseAddress, u32 RegOffset);
void XEmacPs_WriteReg(u32 BaseAddress, u32 RegOffset, u32 Data);

#endif
