/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "video.h"

static int expect_u32(const char *label, uint32_t actual, uint32_t expected)
{
	if (actual != expected) {
		printf("%s: got 0x%08lx expected 0x%08lx\n", label,
		       (unsigned long)actual, (unsigned long)expected);
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
	if (!video_videocap_source_sync_eligible(1U, 1U, 1U) ||
	    video_videocap_source_sync_eligible(1U, 1U, 0U) ||
	    video_videocap_source_sync_eligible(1U, 0U, 1U) ||
	    video_videocap_source_sync_eligible(0U, 1U, 1U) ||
	    video_videocap_source_sync_eligible(0U, 0U, 0U)) {
		printf("source-sync hardware eligibility mismatch\n");
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

		/* The fixed centered pair depends only on viewport+fullrate:
		 * the source-sync controller bit must not change their
		 * selection. */
		for (i = 0; i < 2; i++) {
			for (capabilities = 0; capabilities < 8; capabilities++) {
				uint32_t viewport = capabilities & 1U;
				uint32_t fullrate = capabilities & 2U;
				uint32_t source_sync = capabilities & 4U;
				uint32_t expected = (capabilities & 3U) == 3U ?
					profiles[i] :
					ZZ_VIDEOCAP_OUTPUT_FULL_60;
				request = video_videocap_sanitize_runtime_mode(
					modes[i], viewport, fullrate,
					source_sync);
				if (!request.valid ||
				    request.base_mode != ZZVMODE_800x600 ||
				    request.output_profile != expected ||
				    video_videocap_effective_output_profile(
						profiles[i], viewport, fullrate,
						source_sync) != expected) {
					printf("centered mode %u capability %u fallback mismatch\n",
					       modes[i], capabilities);
					return 0;
				}
			}
		}
	}

	/* The virtual id resolves to MATCH only on a complete stack; every
	 * missing prerequisite uses the shared full_60 fallback. */
	{
		unsigned int capabilities;

		for (capabilities = 0; capabilities < 8; capabilities++) {
			uint32_t viewport = capabilities & 1U;
			uint32_t fullrate = capabilities & 2U;
			uint32_t source_sync = capabilities & 4U;
			uint32_t expected = capabilities == 7U ?
				ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_MATCH :
				ZZ_VIDEOCAP_OUTPUT_FULL_60;

			request = video_videocap_sanitize_runtime_mode(
				ZZVMODE_CENTERED_1080P_MATCH, viewport,
				fullrate, source_sync);
			if (!request.valid ||
			    request.base_mode != ZZVMODE_800x600 ||
			    request.output_profile != expected ||
			    video_videocap_effective_output_profile(
					ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_MATCH,
					viewport, fullrate, source_sync) !=
			    expected) {
				printf("matched id capability %u gating mismatch\n",
				       capabilities);
				return 0;
			}
		}
	}

	request = video_videocap_sanitize_runtime_mode(ZZVMODE_720x576,
		1U, 1U, 1U);
	if (!request.valid || request.base_mode != ZZVMODE_720x576 ||
	    request.output_profile != ZZ_VIDEOCAP_OUTPUT_FULL_60) {
		printf("legacy runtime request must clear centered identity\n");
		return 0;
	}

	request = video_videocap_sanitize_runtime_mode(99U, 1U, 1U, 1U);
	if (request.valid) {
		printf("invalid runtime videocap mode accepted\n");
		return 0;
	}

	request = video_videocap_sanitize_runtime_mode(
		ZZVMODE_CENTERED_1080P_MATCH + 1U, 1U, 1U, 1U);
	if (request.valid) {
		printf("unassigned virtual runtime id accepted\n");
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

	/* The ARM-private width-only word must carry exactly the marker bit
	 * and the width bit: the control engine masks every other field, so
	 * stray payload bits would silently widen the update. */
	data = videocap_control_width_only(1U);
	if (!expect_u32("width-only set word", data,
	                VIDEOCAP_WIDTH_ONLY_FLAG | (1U << 2)))
		return 8;

	data = videocap_control_width_only(0U);
	if (!expect_u32("width-only clear word", data,
	                VIDEOCAP_WIDTH_ONLY_FLAG))
		return 9;

	data = videocap_control_width_only(3U);
	if (!expect_u32("width-only bounds masked", data,
	                VIDEOCAP_WIDTH_ONLY_FLAG | (1U << 2)))
		return 10;

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
