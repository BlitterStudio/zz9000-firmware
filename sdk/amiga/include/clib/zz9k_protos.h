/*
 * C prototypes for zz9k.library LVO calls.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CLIB_ZZ9K_PROTOS_H
#define CLIB_ZZ9K_PROTOS_H

#define ZZ9K_LIBRARY_TYPES_ONLY
#include "zz9k/library.h"
#undef ZZ9K_LIBRARY_TYPES_ONLY
#include <exec/ports.h>

#ifdef __cplusplus
extern "C" {
#endif

int ZZ9KQueryCaps(ZZ9KCaps *caps);
int ZZ9KQueryService(uint32_t service_id, ZZ9KServiceInfo *service);
int ZZ9KPing(const uint8_t *payload, uint32_t payload_len, uint8_t *reply_payload, uint32_t *reply_len);
int ZZ9KCall(ZZ9KRequest *request, ZZ9KMailboxEntry *reply, uint32_t timeout_ticks);
int ZZ9KCallAsync(ZZ9KAsyncRequest *async, const ZZ9KRequest *request, ZZ9KAsyncCallback callback, void *user_data);
int ZZ9KCallAsyncBatch(ZZ9KAsyncRequest *asyncs, const ZZ9KRequest *requests, uint32_t count, ZZ9KAsyncCallback callback, void *user_data, uint32_t *queued);
int ZZ9KPoll(uint32_t max_completions, uint32_t *completed);
int ZZ9KAllocShared(uint32_t length, uint32_t alignment, uint32_t flags, ZZ9KSharedBuffer *buffer);
int ZZ9KFreeShared(uint32_t handle);
int ZZ9KMemFill(uint32_t handle, uint32_t offset, uint32_t length, uint8_t value);
int ZZ9KMemCopy(uint32_t dst_handle, uint32_t dst_offset, uint32_t src_handle, uint32_t src_offset, uint32_t length);
int ZZ9KAllocSurface(uint32_t width, uint32_t height, uint32_t format, uint32_t flags, ZZ9KSurface *surface);
int ZZ9KAllocSurfaceEx(uint32_t width, uint32_t height, uint32_t format, uint32_t flags, uint32_t pitch, ZZ9KSurface *surface);
int ZZ9KFreeSurface(uint32_t handle);
int ZZ9KMapFramebufferSurface(ZZ9KSurface *surface);
int ZZ9KScaleImage(const ZZ9KScaleImageDesc *desc);
int ZZ9KScaleImageClipped(const ZZ9KScaleImageClippedDesc *desc);
int ZZ9KFillSurface(const ZZ9KSurfaceFillDesc *desc);
int ZZ9KCopySurface(const ZZ9KSurfaceCopyDesc *desc);
int ZZ9KReadDiag(ZZ9KDiagInfo *diag);
int ZZ9KCallAsyncMsg(ZZ9KAsyncRequest *async, const ZZ9KRequest *request, struct MsgPort *reply_port);
int ZZ9KCallAsyncBatchMsg(ZZ9KAsyncRequest *asyncs, const ZZ9KRequest *requests, uint32_t count, struct MsgPort *reply_port, uint32_t *queued);
int ZZ9KCancelAsync(ZZ9KAsyncRequest *async);
int ZZ9KWaitAsync(ZZ9KAsyncRequest *async, uint32_t timeout_polls, uint32_t *polls_run);
int ZZ9KWaitAsyncBatch(ZZ9KAsyncRequest *asyncs, uint32_t count, uint32_t timeout_polls, uint32_t *completed, uint32_t *polls_run);
int ZZ9KDecodeImage(uint32_t opcode, const ZZ9KImageDecodeDesc *desc, ZZ9KImageDecodeResult *result);
int ZZ9KDecodeJpeg(const ZZ9KImageDecodeDesc *desc, ZZ9KImageDecodeResult *result);
int ZZ9KDecodePng(const ZZ9KImageDecodeDesc *desc, ZZ9KImageDecodeResult *result);
int ZZ9KDecodeGif(const ZZ9KImageDecodeDesc *desc, ZZ9KImageDecodeResult *result);
int ZZ9KDecodeMp3(const ZZ9KAudioDecodeDesc *desc, ZZ9KAudioDecodeResult *result);
int ZZ9KAudioStreamBegin(const ZZ9KAudioStreamBeginDesc *desc, ZZ9KAudioStreamResult *result);
int ZZ9KAudioStreamFeed(const ZZ9KAudioStreamFeedDesc *desc, ZZ9KAudioStreamResult *result);
int ZZ9KAudioStreamRead(uint32_t session, uint32_t pcm_read, uint32_t flags, ZZ9KAudioStreamResult *result);
int ZZ9KAudioStreamClose(uint32_t session, uint32_t flags, ZZ9KAudioStreamResult *result);
int ZZ9KCryptoKeyExchange(const ZZ9KCryptoKxDesc *desc, ZZ9KCryptoResult *result);
int ZZ9KCryptoVerify(const ZZ9KCryptoVerifyDesc *desc, int *valid);
int ZZ9KAudioStreamPlay(uint32_t session, uint32_t flags, ZZ9KAudioStreamResult *result);
int ZZ9KAudioStreamStop(uint32_t session, uint32_t flags, ZZ9KAudioStreamResult *result);
int ZZ9KQueryPalette(const ZZ9KPaletteQueryDesc *desc);
int ZZ9KCryptoHash(const ZZ9KCryptoHashDesc *desc, ZZ9KCryptoResult *result);
int ZZ9KCryptoHashBatch(const ZZ9KCryptoHashDesc *descs, ZZ9KCryptoResult *results, uint32_t count, uint32_t max_in_flight, uint32_t timeout_ticks);
int ZZ9KCryptoStream(const ZZ9KCryptoStreamDesc *desc, ZZ9KCryptoResult *result);
int ZZ9KCryptoStreamBatch(const ZZ9KCryptoStreamDesc *descs, ZZ9KCryptoResult *results, uint32_t count, uint32_t max_in_flight, uint32_t timeout_ticks);
int ZZ9KCryptoAead(const ZZ9KCryptoAeadDesc *desc, ZZ9KCryptoResult *result);
int ZZ9KCryptoAeadBatch(const ZZ9KCryptoAeadDesc *descs, ZZ9KCryptoResult *results, uint32_t count, uint32_t max_in_flight, uint32_t timeout_ticks);
int ZZ9KImageSessionBegin(const ZZ9KImageSessionBeginDesc *desc, ZZ9KImageSessionResult *result);
int ZZ9KImageSessionFeed(const ZZ9KImageSessionFeedDesc *desc, ZZ9KImageSessionResult *result);
int ZZ9KImageSessionClose(uint32_t session, uint32_t flags);

#ifdef __cplusplus
}
#endif

#endif /* CLIB_ZZ9K_PROTOS_H */
