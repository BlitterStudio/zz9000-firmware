/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
		(fullrate_path ? 280U : VIDEOCAP_CROP_H_COMPAT) :
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

static FILE *open_main_source(void)
{
	FILE *source = fopen("../../ZZ9000_proto.sdk/ZZ9000OS/src/main.c", "r");

	if (source == NULL)
		source = fopen("ZZ9000_proto.sdk/ZZ9000OS/src/main.c", "r");

	return source;
}

static int main_source_contract(void)
{
	FILE *source = open_main_source();
	char line[512];
	unsigned int revision_major = 0;
	unsigned int revision_minor = 0;
	unsigned int revision_major_defines = 0;
	unsigned int revision_minor_defines = 0;
	unsigned int firmware_capability_reads = 0;
	unsigned int runtime_mode_setters = 0;
	unsigned int runtime_vsync_setters = 0;
	unsigned int videocap_pack_writes = 0;
	unsigned int videocap_ops = 0;

	if (source == NULL) {
		printf("cannot open firmware main.c for source contract\n");
		return 0;
	}

	while (fgets(line, sizeof(line), source) != NULL) {
		if (sscanf(line, "#define REVISION_MAJOR %u", &revision_major) == 1)
			revision_major_defines++;
		if (sscanf(line, "#define REVISION_MINOR %u", &revision_minor) == 1)
			revision_minor_defines++;
		if (strstr(line, "data |= video_firmware_capabilities();") != NULL)
			firmware_capability_reads++;
		if (strstr(line, "video_set_videocap_video_mode(") != NULL)
			runtime_mode_setters++;
		if (strstr(line, "video_set_videocap_vsync(") != NULL)
			runtime_vsync_setters++;
		if (strstr(line, "video_formatter_write(videocap_control_pack(") != NULL)
			videocap_pack_writes++;
		if (strstr(line, "MNTVF_OP_VIDEOCAP);") != NULL)
			videocap_ops++;
	}
	fclose(source);

	if (revision_major_defines != 1U || revision_minor_defines != 1U ||
	    ((revision_major << 8) | revision_minor) != 0x0208U) {
		printf("firmware revision contract mismatch: defines=%u/%u revision=0x%04x\n",
		       revision_major_defines, revision_minor_defines,
		       (revision_major << 8) | revision_minor);
		return 0;
	}
	if (firmware_capability_reads != 1U) {
		printf("firmware capability contract mismatch: reads=%u\n",
		       firmware_capability_reads);
		return 0;
	}
	if (runtime_mode_setters != 1U || runtime_vsync_setters != 1U) {
		printf("runtime videocap setter contract mismatch: mode=%u vsync=%u\n",
		       runtime_mode_setters, runtime_vsync_setters);
		return 0;
	}
	if (videocap_pack_writes != 1U || videocap_ops != 1U) {
		printf("startup videocap op contract mismatch: writes=%u ops=%u\n",
		       videocap_pack_writes, videocap_ops);
		return 0;
	}

	return 1;
}

static int sample_output_identity(
		struct video_videocap_detection_state *detection,
		int requested_base_mode, int requested_profile,
		int *applied_base_mode, int *applied_profile)
{
	int stable = video_videocap_detection_stable(detection, 0, 0, 1,
		requested_base_mode, requested_profile);

	if (!stable ||
	    (*applied_base_mode == requested_base_mode &&
	     *applied_profile == requested_profile)) {
		return 0;
	}

	*applied_base_mode = requested_base_mode;
	*applied_profile = requested_profile;
	return 1;
}

static int output_profile_contract(void)
{
	struct video_videocap_runtime_request request;
	struct video_videocap_detection_state detection;
	int applied_base_mode = ZZVMODE_800x600;
	int applied_profile = ZZ_VIDEOCAP_OUTPUT_FULL_60;
	int reinits = 0;

	if (!video_videocap_centered_eligible(1U, 1U) ||
	    video_videocap_centered_eligible(1U, 0U) ||
	    video_videocap_centered_eligible(0U, 1U) ||
	    video_videocap_centered_eligible(0U, 0U)) {
		printf("centered hardware eligibility mismatch\n");
		return 0;
	}
	{
		const uint32_t modes[] = {
			ZZVMODE_1920x1080_60, ZZVMODE_1920x1080_50
		};
		const uint32_t profiles[] = {
			ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60,
			ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_50
		};
		unsigned int i, capabilities;

		for (i = 0; i < 2; i++) {
			for (capabilities = 0; capabilities < 4; capabilities++) {
				uint32_t viewport = capabilities & 1U;
				uint32_t fullrate = capabilities & 2U;
				uint32_t expected = capabilities == 3U ?
					profiles[i] : ZZ_VIDEOCAP_OUTPUT_FULL_60;
				request = video_videocap_sanitize_runtime_mode(
					modes[i], viewport, fullrate);
				if (!request.valid ||
				    request.base_mode != ZZVMODE_800x600 ||
				    request.output_profile != expected ||
				    video_videocap_effective_output_profile(
						profiles[i], viewport, fullrate) != expected) {
					printf("centered mode %u capability %u fallback mismatch\n",
					       modes[i], capabilities);
					return 0;
				}
			}
		}
	}

	request = video_videocap_sanitize_runtime_mode(ZZVMODE_720x576, 1U, 1U);
	if (!request.valid || request.base_mode != ZZVMODE_720x576 ||
	    request.output_profile != ZZ_VIDEOCAP_OUTPUT_FULL_60) {
		printf("legacy runtime request must clear centered identity\n");
		return 0;
	}

	request = video_videocap_sanitize_runtime_mode(99U, 1U, 1U);
	if (request.valid) {
		printf("invalid runtime videocap mode accepted\n");
		return 0;
	}

	/* Requested base mode and centered/full profile are independent parts of
	 * the stable identity. Each change applies once on the second matching
	 * vblank and does not reinitialize again while stable. */
	video_videocap_detection_reset(&detection);
	(void)sample_output_identity(&detection, ZZVMODE_800x600,
		ZZ_VIDEOCAP_OUTPUT_FULL_60, &applied_base_mode, &applied_profile);
	(void)sample_output_identity(&detection, ZZVMODE_800x600,
		ZZ_VIDEOCAP_OUTPUT_FULL_60, &applied_base_mode, &applied_profile);

	reinits += sample_output_identity(&detection, ZZVMODE_720x576,
		ZZ_VIDEOCAP_OUTPUT_FULL_60, &applied_base_mode, &applied_profile);
	reinits += sample_output_identity(&detection, ZZVMODE_720x576,
		ZZ_VIDEOCAP_OUTPUT_FULL_60, &applied_base_mode, &applied_profile);
	reinits += sample_output_identity(&detection, ZZVMODE_720x576,
		ZZ_VIDEOCAP_OUTPUT_FULL_60, &applied_base_mode, &applied_profile);
	if (reinits != 1) {
		printf("stable mode 1->6 reinit count=%d expected=1\n", reinits);
		return 0;
	}

	reinits += sample_output_identity(&detection, ZZVMODE_800x600,
		ZZ_VIDEOCAP_OUTPUT_FULL_60, &applied_base_mode, &applied_profile);
	reinits += sample_output_identity(&detection, ZZVMODE_800x600,
		ZZ_VIDEOCAP_OUTPUT_FULL_60, &applied_base_mode, &applied_profile);
	reinits += sample_output_identity(&detection, ZZVMODE_800x600,
		ZZ_VIDEOCAP_OUTPUT_FULL_60, &applied_base_mode, &applied_profile);
	if (reinits != 2) {
		printf("stable mode 6->1 cumulative reinits=%d expected=2\n", reinits);
		return 0;
	}

	reinits += sample_output_identity(&detection, ZZVMODE_800x600,
		ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60,
		&applied_base_mode, &applied_profile);
	reinits += sample_output_identity(&detection, ZZVMODE_800x600,
		ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60,
		&applied_base_mode, &applied_profile);
	reinits += sample_output_identity(&detection, ZZVMODE_800x600,
		ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60,
		&applied_base_mode, &applied_profile);
	if (reinits != 3) {
		printf("stable full->centered cumulative reinits=%d expected=3\n",
		       reinits);
		return 0;
	}

	/* Changing only refresh must still reconfigure exactly once. */
	reinits += sample_output_identity(&detection, ZZVMODE_800x600,
		ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_50,
		&applied_base_mode, &applied_profile);
	reinits += sample_output_identity(&detection, ZZVMODE_800x600,
		ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_50,
		&applied_base_mode, &applied_profile);
	reinits += sample_output_identity(&detection, ZZVMODE_800x600,
		ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_50,
		&applied_base_mode, &applied_profile);
	if (reinits != 4) {
		printf("centered 60->50 cumulative reinits=%d expected=4\n", reinits);
		return 0;
	}

	return 1;
}


int main(void)
{
	uint32_t data;
	uint32_t status_old;
	uint32_t status_new;

	if (!expect_u32("missing CFG uses safe filtered width",
	                VIDEOCAP_FULL_WIDTH_DEFAULT, 0U))
		return 20;

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

	data = videocap_control_pack(2U, 1U, 0U, 4095U, 0U, 1U);
	if (!expect_u32("horizontal automatic with vertical maximum", data,
	                VIDEOCAP_CROP_H_AUTO_FLAG | (4095U << 16) |
	                (VIDEOCAP_CROP_H_COMPAT << 4) | (1U << 2) | 2U))
		return 5;

	data = videocap_control_pack(1U, 0U, 0U, 4095U, 1U, 0U);
	if (!expect_u32("horizontal zero with vertical automatic", data,
	                VIDEOCAP_CROP_V_AUTO_FLAG |
	                (VIDEOCAP_CROP_V_COMPAT << 16) | 1U))
		return 6;

	data = videocap_control_pack(3U, 3U, 8191U, 8191U, 1U, 1U);
	if (!expect_u32("fields remain bounded", data,
	                (4095U << 16) | (4095U << 4) | (1U << 2) | 3U))
		return 7;

	if (!main_source_contract())
		return 8;

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
		return 9;

	if (!raw_is_valid(0U) || !raw_is_valid(2U) || raw_is_valid(3U) ||
	    raw_is_valid(1UL << 30)) {
		printf("raw validity contract mismatch\n");
		return 10;
	}

	data = VIDEOCAP_CROP_H_AUTO_FLAG | VIDEOCAP_CROP_V_AUTO_FLAG |
	       (1U << 2);
	if (!expect_u32("full-rate automatic effective",
	                effective_crop(data, 1), (40U << 16) | 280U) ||
	    !expect_u32("filtered automatic effective",
	                effective_crop(data & ~(1U << 2), 1),
	                (26U << 16) | 188U) ||
	    !expect_u32("compat automatic effective",
	                effective_crop(data, 0), (26U << 16) | 188U))
		return 11;

	data = VIDEOCAP_CROP_H_AUTO_FLAG | (4095U << 16) | (1U << 2) | 2U;
	if (!expect_u32("mixed automatic/custom effective",
	                effective_crop(data, 1), (4095U << 16) | 280U))
		return 12;

	data = (0U << 16) | (4095U << 4);
	if (!expect_u32("literal custom effective", effective_crop(data, 1),
	                (0U << 16) | 4095U))
		return 13;

	status_old = make_status(0xffU, 0xffU, 1, 0, 0);
	status_new = make_status(0U, 0U, 1, 0, 0);
	if (!request_complete(status_new, 0U) ||
	    request_complete(make_status(0U, 0xffU, 1, 0, 1), 0U) ||
	    request_complete(make_status(0U, 0U, 1, 1, 0), 0U) ||
	    snapshot_status_stable(status_old, status_new) ||
	    !snapshot_status_stable(status_new, status_new)) {
		printf("sequence completion/snapshot guard mismatch\n");
		return 14;
	}

	/* Capability/status reserve bits are intentionally not available for
	 * feature growth without a protocol-version change. */
	if ((VCAP_LIVE_CAPABILITY_VALUE & 0xf0U) != 0U ||
	    (VCAP_STATUS_STANDARD_VALID | VCAP_STATUS_NTSC |
	     VCAP_STATUS_REJECTED | VCAP_STATUS_APPLIED_VALID |
	     VCAP_STATUS_BUSY) != 0x0000e003UL) {
		printf("capability/status bit layout mismatch\n");
		return 15;
	}

	if (!output_profile_contract())
		return 16;

	if (!expect_u32("viewport position op", MNTVF_OP_VIEWPORT_POS, 28U) ||
	    !expect_u32("viewport size/commit op",
	                MNTVF_OP_VIEWPORT_SIZE_COMMIT, 29U) ||
	    !expect_u32("viewport container dimensions flag",
	                MNTVF_DIMENSIONS_VIEWPORT_CONTAINER_FLAG, 1U << 15))
		return 18;


	return 0;
}
