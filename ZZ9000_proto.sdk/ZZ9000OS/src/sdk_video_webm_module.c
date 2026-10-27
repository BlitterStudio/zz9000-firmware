#include "sdk_mailbox.h"
#include "sdk_video_backend.h"
#include "zzm_format.h"
#include "zzm_registry.h"
#include "zzm_webm_ops.h"

const struct SDKVideoDecoderOps *sdk_video_webm_ops(uint32_t codec)
{
	const struct zzm_webm_ops *ops;

	if (zzm_registry_sdk_status(ZZM_MODULE_WEBM) != SDK_STATUS_OK)
		return 0;
	ops = (const struct zzm_webm_ops *)zzm_registry_ops(ZZM_MODULE_WEBM,
	                                                     sizeof(*ops));
	if (!ops || ops->abi_version != ZZM_WEBM_OPS_ABI_VERSION ||
	    ops->size < sizeof(*ops) || !ops->find)
		return 0;
	return ops->find(codec);
}
