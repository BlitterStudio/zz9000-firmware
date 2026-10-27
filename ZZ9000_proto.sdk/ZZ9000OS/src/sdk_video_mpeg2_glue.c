#include "sdk_video_mpeg2_module.h"
#include "zzm_format.h"
#include "zzm_registry.h"

const struct SDKVideoDecoderOps *sdk_video_mpeg2_module_ops(
	uint32_t codec, uint32_t container)
{
	const struct zzm_mpeg2_module_ops *ops;

	ops = (const struct zzm_mpeg2_module_ops *)zzm_registry_ops(
		ZZM_MODULE_MPEG2, sizeof(*ops));
	if (!ops || ops->abi_version != ZZM_MPEG2_MODULE_OPS_ABI_VERSION ||
	    ops->size < sizeof(*ops) || !ops->find)
		return 0;
	return ops->find(codec, container);
}

const struct SDKVideoDecoderOps *sdk_video_backend_find(uint32_t codec,
	                                                     uint32_t container)
{
	if (codec == SDK_VIDEO_CODEC_MPEG1 || codec == SDK_VIDEO_CODEC_MPEG2)
		return sdk_video_mpeg2_module_ops(codec, container);
	if (container == SDK_VIDEO_CONTAINER_WEBM &&
	    (codec == SDK_VIDEO_CODEC_VP8 || codec == SDK_VIDEO_CODEC_VP9))
		return sdk_video_webm_ops(codec);
	return 0;
}
