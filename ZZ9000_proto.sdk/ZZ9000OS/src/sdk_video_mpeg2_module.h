/* MPEG decoder module handoff between the BOOT glue and the loaded module. */
#ifndef SDK_VIDEO_MPEG2_MODULE_H
#define SDK_VIDEO_MPEG2_MODULE_H

#include <stdint.h>
#include "sdk_video_backend.h"

#define ZZM_MPEG2_MODULE_OPS_ABI_VERSION 1U

struct zzm_mpeg2_module_ops {
	uint32_t abi_version;
	uint32_t size;
	const struct SDKVideoDecoderOps *(*find)(uint32_t codec,
	                                         uint32_t container);
};

const struct SDKVideoDecoderOps *sdk_video_mpeg2_module_ops(
	uint32_t codec, uint32_t container);

#endif /* SDK_VIDEO_MPEG2_MODULE_H */
