#ifndef ZZM_WEBM_OPS_H
#define ZZM_WEBM_OPS_H

#include <stdint.h>
#include "sdk_video_backend.h"

#define ZZM_WEBM_OPS_ABI_VERSION 1U

struct zzm_webm_ops {
	uint32_t abi_version;
	uint32_t size;
	const struct SDKVideoDecoderOps *(*find)(uint32_t codec);
};

#endif /* ZZM_WEBM_OPS_H */
