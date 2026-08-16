/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_APERTURE_LAYOUT_H
#define SDK_APERTURE_LAYOUT_H

#include <stdint.h>

/*
 * Generation-1 Zorro II aperture contract.
 *
 * The FPGA exposes the exact aperture byte count to the ARM through AXI slot
 * 7 and exposes SDK_APERTURE_INFO_WORD() to the host at board offset 0x111c.
 * A matching host writes SDK_APERTURE_ACK_TOKEN to that register before it
 * uses any generation-1 dynamic region.  The acknowledgement is deliberately
 * generation-specific: a newer layout can never silently reuse these ranges.
 */
#define SDK_APERTURE_LAYOUT_GENERATION       1U
#define SDK_APERTURE_INFO_MAGIC              0x5a000000UL
#define SDK_APERTURE_INFO_MAGIC_MASK         0xff000000UL
#define SDK_APERTURE_INFO_GENERATION_SHIFT   16U
#define SDK_APERTURE_INFO_GENERATION_MASK    0x00ff0000UL
#define SDK_APERTURE_INFO_FLAG_VALID         0x00000100UL
#define SDK_APERTURE_INFO_FLAG_HOST_WINDOW   0x00000400UL
#define SDK_APERTURE_INFO_FLAG_PIP_POOL      0x00000200UL
#define SDK_APERTURE_INFO_SIZE_MIB_MASK      0x000000ffUL
#define SDK_APERTURE_ACK_TOKEN               0xa501U

#define SDK_APERTURE_BYTES_2M                0x00200000UL
#define SDK_APERTURE_BYTES_4M                0x00400000UL
#define SDK_APERTURE_BYTES_8M                0x00800000UL
#define SDK_APERTURE_FRAMEBUFFER_BASE        0x00010000UL
#define SDK_APERTURE_ARM_ADDRESS_ADJUSTMENT  0x001f0000UL

#define SDK_APERTURE_FLAG_VALID              (1U << 0)
#define SDK_APERTURE_FLAG_ACKED              (1U << 1)
#define SDK_APERTURE_FLAG_HOST_WINDOW        (1U << 2)
#define SDK_APERTURE_FLAG_PIP_POOL           (1U << 3)
#define SDK_APERTURE_FLAG_LEGACY             (1U << 4)

#define SDK_APERTURE_DIAG_STATE_LEGACY         0U
#define SDK_APERTURE_DIAG_STATE_UNACKNOWLEDGED 1U
#define SDK_APERTURE_DIAG_STATE_ACTIVE         2U
#define SDK_APERTURE_DIAG_STATE_INVALID        3U

struct sdk_aperture_region {
	uint32_t base;
	uint32_t size;
};

/* This maps byte-for-byte to the 48-byte QUERY_APERTURE_LAYOUT payload. */
struct sdk_aperture_layout {
	uint32_t profile;
	uint32_t aperture_size;
	struct sdk_aperture_region framebuffer;
	struct sdk_aperture_region pip;
	struct sdk_aperture_region template_scratch;
	struct sdk_aperture_region host_window;
	struct sdk_aperture_region audio;
};

#define SDK_APERTURE_PROFILE(generation, flags) \
	((((generation) & 0xffffU) << 16) | ((flags) & 0xffffU))
#define SDK_APERTURE_PROFILE_GENERATION(profile) (((profile) >> 16) & 0xffffU)
#define SDK_APERTURE_PROFILE_FLAGS(profile)      ((profile) & 0xffffU)

#define SDK_APERTURE_INFO_WORD(size_mib, flags) \
	(SDK_APERTURE_INFO_MAGIC | \
	 (SDK_APERTURE_LAYOUT_GENERATION << SDK_APERTURE_INFO_GENERATION_SHIFT) | \
	 ((flags) & (SDK_APERTURE_INFO_FLAG_VALID | \
	             SDK_APERTURE_INFO_FLAG_HOST_WINDOW | \
	             SDK_APERTURE_INFO_FLAG_PIP_POOL)) | \
	 ((size_mib) & SDK_APERTURE_INFO_SIZE_MIB_MASK))

typedef char sdk_aperture_layout_must_be_48_bytes[
	(sizeof(struct sdk_aperture_layout) == 48U) ? 1 : -1];

int sdk_aperture_layout_compute(uint32_t aperture_size,
				struct sdk_aperture_layout *layout);
int sdk_aperture_layout_validate(const struct sdk_aperture_layout *layout);
uint32_t sdk_aperture_layout_info_word(const struct sdk_aperture_layout *layout);

/* Runtime state, initialized once the ARM can read the FPGA AXI registers. */
void sdk_aperture_runtime_init(uint32_t fpga_aperture_size, int is_zorro3);
int sdk_aperture_runtime_ack(void);
const struct sdk_aperture_layout *sdk_aperture_runtime_layout(void);
uint32_t sdk_aperture_runtime_flags(void);
int sdk_aperture_runtime_is_zorro3(void);
int sdk_aperture_runtime_is_legacy(void);
uint32_t sdk_aperture_runtime_reported_size(void);
uint32_t sdk_aperture_runtime_diag_state(void);
uint32_t sdk_aperture_host_window_address(void);
uint32_t sdk_aperture_host_window_size(void);
uint32_t sdk_aperture_framebuffer_size(void);
uint32_t sdk_aperture_gfxdata_address(uint32_t z3_scratch_address);

#endif
