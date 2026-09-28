/*
 * Native-testable core for the AmigaOS zz9k.library facade.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_LIBRARY_H
#define ZZ9K_LIBRARY_H

#include "zz9k/host.h"

#if defined(__amigaos__) || defined(__amiga__) || defined(__AMIGA__) || \
    defined(__VBCC__)
#define ZZ9K_LIBRARY_AMIGA 1
#include <exec/ports.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ZZ9KAsyncRequest ZZ9KAsyncRequest;
typedef struct ZZ9KLibrary ZZ9KLibrary;
typedef void (*ZZ9KAsyncCallback)(ZZ9KAsyncRequest *request,
                                  void *user_data);
typedef int (*ZZ9KEventWaiter)(ZZ9KLibrary *library, void *user_data);

struct ZZ9KAsyncRequest {
#if ZZ9K_LIBRARY_AMIGA
  struct Message message;
#endif
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  uint32_t request_id;
  int status;
  uint8_t completed;
  uint8_t queued;
  ZZ9KAsyncCallback callback;
  void *user_data;
#if ZZ9K_LIBRARY_AMIGA
  struct MsgPort *reply_port;
#endif
  ZZ9KAsyncRequest *next;
};

struct ZZ9KLibrary {
  ZZ9KContext *ctx;
  int owns_context;
  ZZ9KAsyncRequest *pending_head;
  ZZ9KEventWaiter event_waiter;
  void *event_user_data;
};

#ifndef ZZ9K_LIBRARY_TYPES_ONLY

void ZZ9KInit(ZZ9KLibrary *library);
int ZZ9KOpen(ZZ9KLibrary *library);
void ZZ9KClose(ZZ9KLibrary *library);
int ZZ9KAttachContext(ZZ9KLibrary *library, ZZ9KContext *ctx,
                      int take_ownership);
int ZZ9KSetEventWaiter(ZZ9KLibrary *library, ZZ9KEventWaiter waiter,
                       void *user_data);

int ZZ9KQueryCaps(ZZ9KLibrary *library, ZZ9KCaps *caps);
int ZZ9KQueryService(ZZ9KLibrary *library, uint32_t service_id,
                     ZZ9KServiceInfo *service);
int ZZ9KPing(ZZ9KLibrary *library, const uint8_t *payload,
             uint32_t payload_len, uint8_t *reply_payload,
             uint32_t *reply_len);
int ZZ9KCall(ZZ9KLibrary *library, ZZ9KRequest *request,
             ZZ9KMailboxEntry *reply, uint32_t timeout_ticks);
int ZZ9KCallAsync(ZZ9KLibrary *library, ZZ9KAsyncRequest *async,
                  const ZZ9KRequest *request, ZZ9KAsyncCallback callback,
                  void *user_data);
int ZZ9KCallAsyncBatch(ZZ9KLibrary *library, ZZ9KAsyncRequest *asyncs,
                       const ZZ9KRequest *requests, uint32_t count,
                       ZZ9KAsyncCallback callback, void *user_data,
                       uint32_t *queued);
int ZZ9KCancelAsync(ZZ9KLibrary *library, ZZ9KAsyncRequest *async);
int ZZ9KWaitAsync(ZZ9KLibrary *library, ZZ9KAsyncRequest *async,
                  uint32_t timeout_polls, uint32_t *polls_run);
int ZZ9KWaitAsyncBatch(ZZ9KLibrary *library, ZZ9KAsyncRequest *asyncs,
                       uint32_t count, uint32_t timeout_polls,
                       uint32_t *completed, uint32_t *polls_run);
#if ZZ9K_LIBRARY_AMIGA
int ZZ9KCallAsyncMsg(ZZ9KLibrary *library, ZZ9KAsyncRequest *async,
                     const ZZ9KRequest *request, struct MsgPort *reply_port);
int ZZ9KCallAsyncBatchMsg(ZZ9KLibrary *library, ZZ9KAsyncRequest *asyncs,
                          const ZZ9KRequest *requests, uint32_t count,
                          struct MsgPort *reply_port, uint32_t *queued);
#endif
int ZZ9KPoll(ZZ9KLibrary *library, uint32_t max_completions,
             uint32_t *completed);

int ZZ9KAllocShared(ZZ9KLibrary *library, uint32_t length,
                    uint32_t alignment, uint32_t flags,
                    ZZ9KSharedBuffer *buffer);
int ZZ9KFreeShared(ZZ9KLibrary *library, uint32_t handle);
int ZZ9KMemFill(ZZ9KLibrary *library, uint32_t handle, uint32_t offset,
                uint32_t length, uint8_t value);
int ZZ9KMemCopy(ZZ9KLibrary *library, uint32_t dst_handle,
                uint32_t dst_offset, uint32_t src_handle,
                uint32_t src_offset, uint32_t length);
int ZZ9KAllocSurface(ZZ9KLibrary *library, uint32_t width, uint32_t height,
                     uint32_t format, uint32_t flags,
                     ZZ9KSurface *surface);
int ZZ9KAllocSurfaceEx(ZZ9KLibrary *library, uint32_t width, uint32_t height,
                       uint32_t format, uint32_t flags, uint32_t pitch,
                       ZZ9KSurface *surface);
int ZZ9KFreeSurface(ZZ9KLibrary *library, uint32_t handle);
int ZZ9KMapFramebufferSurface(ZZ9KLibrary *library, ZZ9KSurface *surface);
int ZZ9KScaleImage(ZZ9KLibrary *library, const ZZ9KScaleImageDesc *desc);
int ZZ9KScaleImageClipped(ZZ9KLibrary *library,
                          const ZZ9KScaleImageClippedDesc *desc);
int ZZ9KFillSurface(ZZ9KLibrary *library, const ZZ9KSurfaceFillDesc *desc);
int ZZ9KCopySurface(ZZ9KLibrary *library, const ZZ9KSurfaceCopyDesc *desc);
int ZZ9KDecodeImage(ZZ9KLibrary *library, uint32_t opcode,
                    const ZZ9KImageDecodeDesc *desc,
                    ZZ9KImageDecodeResult *result);
int ZZ9KDecodeJpeg(ZZ9KLibrary *library,
                   const ZZ9KImageDecodeDesc *desc,
                   ZZ9KImageDecodeResult *result);
int ZZ9KDecodePng(ZZ9KLibrary *library,
                  const ZZ9KImageDecodeDesc *desc,
                  ZZ9KImageDecodeResult *result);
int ZZ9KDecodeGif(ZZ9KLibrary *library,
                  const ZZ9KImageDecodeDesc *desc,
                  ZZ9KImageDecodeResult *result);
int ZZ9KDecodeMp3(ZZ9KLibrary *library,
                  const ZZ9KAudioDecodeDesc *desc,
                  ZZ9KAudioDecodeResult *result);
int ZZ9KAudioStreamBegin(ZZ9KLibrary *library,
                         const ZZ9KAudioStreamBeginDesc *desc,
                         ZZ9KAudioStreamResult *result);
int ZZ9KAudioStreamFeed(ZZ9KLibrary *library,
                        const ZZ9KAudioStreamFeedDesc *desc,
                        ZZ9KAudioStreamResult *result);
int ZZ9KAudioStreamRead(ZZ9KLibrary *library, uint32_t session,
                        uint32_t pcm_read, uint32_t flags,
                        ZZ9KAudioStreamResult *result);
int ZZ9KAudioStreamClose(ZZ9KLibrary *library, uint32_t session,
                         uint32_t flags,
                         ZZ9KAudioStreamResult *result);
int ZZ9KAudioStreamPlay(ZZ9KLibrary *library, uint32_t session,
                        uint32_t flags,
                        ZZ9KAudioStreamResult *result);
int ZZ9KQueryPalette(ZZ9KLibrary *library, const ZZ9KPaletteQueryDesc *desc);
int ZZ9KAudioStreamStop(ZZ9KLibrary *library, uint32_t session,
                        uint32_t flags,
                        ZZ9KAudioStreamResult *result);
int ZZ9KImageSessionBegin(ZZ9KLibrary *library,
                          const ZZ9KImageSessionBeginDesc *desc,
                          ZZ9KImageSessionResult *result);
int ZZ9KImageSessionFeed(ZZ9KLibrary *library,
                         const ZZ9KImageSessionFeedDesc *desc,
                         ZZ9KImageSessionResult *result);
int ZZ9KImageSessionClose(ZZ9KLibrary *library, uint32_t session,
                          uint32_t flags);
int ZZ9KCryptoHash(ZZ9KLibrary *library, const ZZ9KCryptoHashDesc *desc,
                   ZZ9KCryptoResult *result);
int ZZ9KCryptoHashBatch(ZZ9KLibrary *library,
                        const ZZ9KCryptoHashDesc *descs,
                        ZZ9KCryptoResult *results,
                        uint32_t count, uint32_t max_in_flight,
                        uint32_t timeout_ticks);
int ZZ9KCryptoStream(ZZ9KLibrary *library,
                     const ZZ9KCryptoStreamDesc *desc,
                     ZZ9KCryptoResult *result);
int ZZ9KCryptoStreamBatch(ZZ9KLibrary *library,
                          const ZZ9KCryptoStreamDesc *descs,
                          ZZ9KCryptoResult *results,
                          uint32_t count, uint32_t max_in_flight,
                          uint32_t timeout_ticks);
int ZZ9KCryptoAead(ZZ9KLibrary *library, const ZZ9KCryptoAeadDesc *desc,
                   ZZ9KCryptoResult *result);
int ZZ9KCryptoAeadBatch(ZZ9KLibrary *library,
                        const ZZ9KCryptoAeadDesc *descs,
                        ZZ9KCryptoResult *results,
                        uint32_t count, uint32_t max_in_flight,
                        uint32_t timeout_ticks);
int ZZ9KCryptoKeyExchange(ZZ9KLibrary *library,
                          const ZZ9KCryptoKxDesc *desc,
                          ZZ9KCryptoResult *result);
int ZZ9KCryptoVerify(ZZ9KLibrary *library,
                     const ZZ9KCryptoVerifyDesc *desc,
                     int *valid);
int ZZ9KReadDiag(ZZ9KLibrary *library, ZZ9KDiagInfo *diag);

#endif /* ZZ9K_LIBRARY_TYPES_ONLY */

#ifdef __cplusplus
}
#endif

#endif /* ZZ9K_LIBRARY_H */
