/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>

#include "video.h"

#define VCAP_LIVE_CAPABILITY       0x1400U
#define VCAP_LIVE_STATUS           0x1404U
#define VCAP_LIVE_APPLIED_RAW      0x1408U
#define VCAP_LIVE_EFFECTIVE_CROP   0x140cU
#define VCAP_LIVE_STAGED_RAW_HI    0x1410U
#define VCAP_LIVE_STAGED_RAW_LO    0x1412U
#define VCAP_LIVE_COMMIT           0x1414U
#define VCAP_LIVE_CAPABILITY_VALUE 0x564c010fUL
#define VCAP_LIVE_COMMIT_TOKEN     0xca1bU

#define VCAP_STATUS_REQUEST_SHIFT 24U
#define VCAP_STATUS_APPLIED_SHIFT 16U
#define VCAP_STATUS_STANDARD_VALID (1UL << 15)
#define VCAP_STATUS_NTSC           (1UL << 14)
#define VCAP_STATUS_REJECTED       (1UL << 13)
#define VCAP_STATUS_APPLIED_VALID  (1UL << 1)
#define VCAP_STATUS_BUSY           (1UL << 0)

static int raw_is_valid(uint32_t raw)
{
	return (raw & 0xc0000000UL) == 0U && (raw & 3U) <= 2U;
}

static uint32_t effective_crop(uint32_t raw, int compiled_fullrate)
{
	uint32_t fullrate_path = compiled_fullrate && ((raw & (1U << 2)) != 0U);
	uint32_t h = (raw & VIDEOCAP_CROP_H_AUTO_FLAG) != 0U ?
		(fullrate_path ? 279U : VIDEOCAP_CROP_H_COMPAT) :
		((raw >> 4) & 0xfffU);
	uint32_t v = (raw & VIDEOCAP_CROP_V_AUTO_FLAG) != 0U ?
		(fullrate_path ? 40U : VIDEOCAP_CROP_V_COMPAT) :
		((raw >> 16) & 0xfffU);

	return (v << 16) | h;
}

static uint32_t make_status(uint32_t requested, uint32_t applied,
	int valid, int rejected, int busy)
{
	return ((requested & 0xffU) << VCAP_STATUS_REQUEST_SHIFT) |
	       ((applied & 0xffU) << VCAP_STATUS_APPLIED_SHIFT) |
	       (valid ? VCAP_STATUS_APPLIED_VALID : 0U) |
	       (rejected ? VCAP_STATUS_REJECTED : 0U) |
	       (busy ? VCAP_STATUS_BUSY : 0U);
}

static int request_complete(uint32_t status, uint32_t expected)
{
	return ((status >> VCAP_STATUS_REQUEST_SHIFT) & 0xffU) == expected &&
	       ((status >> VCAP_STATUS_APPLIED_SHIFT) & 0xffU) == expected &&
	       (status & (VCAP_STATUS_BUSY | VCAP_STATUS_REJECTED)) == 0U;
}

static int snapshot_status_stable(uint32_t before, uint32_t after)
{
	uint32_t before_seq = (before >> VCAP_STATUS_APPLIED_SHIFT) & 0xffU;
	uint32_t after_seq = (after >> VCAP_STATUS_APPLIED_SHIFT) & 0xffU;

	return (before & VCAP_STATUS_APPLIED_VALID) != 0U &&
	       (after & VCAP_STATUS_APPLIED_VALID) != 0U &&
	       (before & (VCAP_STATUS_BUSY | VCAP_STATUS_REJECTED)) == 0U &&
	       (after & (VCAP_STATUS_BUSY | VCAP_STATUS_REJECTED)) == 0U &&
	       ((before >> VCAP_STATUS_REQUEST_SHIFT) & 0xffU) == before_seq &&
	       ((after >> VCAP_STATUS_REQUEST_SHIFT) & 0xffU) == after_seq &&
	       before_seq == after_seq;
}

static int expect_u32(const char *label, uint32_t actual, uint32_t expected)
{
	if (actual != expected) {
		printf("%s: got 0x%08lx expected 0x%08lx\n", label,
		       (unsigned long)actual, (unsigned long)expected);
		return 0;
	}

	return 1;
}

int main(void)
{
	uint32_t data;
	uint32_t status_old;
	uint32_t status_new;

	data = videocap_control_pack(0U, 1U,
	                             VIDEOCAP_CROP_H_COMPAT,
	                             VIDEOCAP_CROP_V_COMPAT,
	                             0U, 0U);
	if (!expect_u32("both axes automatic", data,
	                VIDEOCAP_CROP_H_AUTO_FLAG |
	                VIDEOCAP_CROP_V_AUTO_FLAG |
	                (VIDEOCAP_CROP_V_COMPAT << 16) |
	                (VIDEOCAP_CROP_H_COMPAT << 4) | (1U << 2)))
		return 1;

	data = videocap_control_pack(2U, 0U, 279U, 41U, 1U, 0U);
	if (!expect_u32("vertical automatic only", data,
	                VIDEOCAP_CROP_V_AUTO_FLAG |
	                (VIDEOCAP_CROP_V_COMPAT << 16) |
	                (279U << 4) | 2U))
		return 2;

	data = videocap_control_pack(3U, 0U, 280U, 42U, 0U, 1U);
	if (!expect_u32("horizontal automatic only", data,
	                VIDEOCAP_CROP_H_AUTO_FLAG |
	                (42U << 16) |
	                (VIDEOCAP_CROP_H_COMPAT << 4) | 3U))
		return 3;

	data = videocap_control_pack(1U, 1U, 0U, 4095U, 1U, 1U);
	if (!expect_u32("literal boundary values", data,
	                (4095U << 16) | (1U << 2) | 1U))
		return 4;

	data = videocap_control_pack(3U, 3U, 8191U, 8191U, 1U, 1U);
	if (!expect_u32("fields remain bounded", data,
	                (4095U << 16) | (4095U << 4) | (1U << 2) | 3U))
		return 5;

	if (!expect_u32("capability offset", VCAP_LIVE_CAPABILITY, 0x1400U) ||
	    !expect_u32("status offset", VCAP_LIVE_STATUS, 0x1404U) ||
	    !expect_u32("applied raw offset", VCAP_LIVE_APPLIED_RAW, 0x1408U) ||
	    !expect_u32("effective offset", VCAP_LIVE_EFFECTIVE_CROP, 0x140cU) ||
	    !expect_u32("stage high offset", VCAP_LIVE_STAGED_RAW_HI, 0x1410U) ||
	    !expect_u32("stage low offset", VCAP_LIVE_STAGED_RAW_LO, 0x1412U) ||
	    !expect_u32("commit offset", VCAP_LIVE_COMMIT, 0x1414U) ||
	    !expect_u32("capability value", VCAP_LIVE_CAPABILITY_VALUE,
	                0x564c010fUL) ||
	    !expect_u32("commit token", VCAP_LIVE_COMMIT_TOKEN, 0xca1bU))
		return 6;

	if (!raw_is_valid(0U) || !raw_is_valid(2U) || raw_is_valid(3U) ||
	    raw_is_valid(1UL << 30)) {
		printf("raw validity contract mismatch\n");
		return 7;
	}

	data = VIDEOCAP_CROP_H_AUTO_FLAG | VIDEOCAP_CROP_V_AUTO_FLAG |
	       (1U << 2);
	if (!expect_u32("full-rate automatic effective",
	                effective_crop(data, 1), (40U << 16) | 279U) ||
	    !expect_u32("filtered automatic effective",
	                effective_crop(data & ~(1U << 2), 1),
	                (26U << 16) | 188U) ||
	    !expect_u32("compat automatic effective",
	                effective_crop(data, 0), (26U << 16) | 188U))
		return 8;

	data = VIDEOCAP_CROP_H_AUTO_FLAG | (4095U << 16) | (1U << 2) | 2U;
	if (!expect_u32("mixed automatic/custom effective",
	                effective_crop(data, 1), (4095U << 16) | 279U))
		return 9;

	data = (0U << 16) | (4095U << 4);
	if (!expect_u32("literal custom effective", effective_crop(data, 1),
	                (0U << 16) | 4095U))
		return 10;

	status_old = make_status(0xffU, 0xffU, 1, 0, 0);
	status_new = make_status(0U, 0U, 1, 0, 0);
	if (!request_complete(status_new, 0U) ||
	    request_complete(make_status(0U, 0xffU, 1, 0, 1), 0U) ||
	    request_complete(make_status(0U, 0U, 1, 1, 0), 0U) ||
	    snapshot_status_stable(status_old, status_new) ||
	    !snapshot_status_stable(status_new, status_new)) {
		printf("sequence completion/snapshot guard mismatch\n");
		return 11;
	}

	/* Capability/status reserve bits are intentionally not available for
	 * feature growth without a protocol-version change. */
	if ((VCAP_LIVE_CAPABILITY_VALUE & 0xf0U) != 0U ||
	    (VCAP_STATUS_STANDARD_VALID | VCAP_STATUS_NTSC |
	     VCAP_STATUS_REJECTED | VCAP_STATUS_APPLIED_VALID |
	     VCAP_STATUS_BUSY) != 0x0000e003UL) {
		printf("capability/status bit layout mismatch\n");
		return 12;
	}

	return 0;
}
