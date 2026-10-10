/*
 * Test-local stub for sdk_video_mpeg2_backend_ops.
 * Used only in test suites that link sdk_video_plmpeg.c without sdk_video_mpeg2.c.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_video_backend.h"

const struct SDKVideoDecoderOps *sdk_video_mpeg2_backend_ops(void)
{
	return 0;
}

const struct SDKVideoDecoderOps *sdk_video_webm_ops(uint32_t codec)
{
	(void)codec;
	return 0;
}
