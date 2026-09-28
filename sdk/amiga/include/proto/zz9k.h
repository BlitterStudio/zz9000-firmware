/*
 * AmigaOS inline caller interface for zz9k.library.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PROTO_ZZ9K_H
#define PROTO_ZZ9K_H

#include "clib/zz9k_protos.h"
#include "zz9k/library_vectors.h"
#include <exec/libraries.h>

#ifdef __cplusplus
extern "C" {
#endif

extern struct Library *ZZ9KBase;

#if defined(__GNUC__) && (defined(__amigaos__) || defined(__amiga__) || \
    defined(__AMIGA__))

#define ZZ9K_INLINE_CALL0(offset) \
  do { \
    register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase; \
    __asm volatile("jsr " #offset "(a6)" \
                   : "=r"(zz9k_d0) \
                   : "r"(zz9k_a6) \
                   : ZZ9K_INLINE_CLOBBERS); \
  } while (0)

#define ZZ9K_INLINE_CALL1(offset, in0) \
  do { \
    register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase; \
    register void *zz9k_a0 __asm("a0") = (void *)(in0); \
    __asm volatile("jsr " #offset "(a6)" \
                   : "=r"(zz9k_d0) \
                   : "r"(zz9k_a6), "r"(zz9k_a0) \
                   : ZZ9K_INLINE_CLOBBERS); \
  } while (0)

#define ZZ9K_INLINE_CLOBBERS "cc", "memory", "d1", "a0", "a1"

static __inline int __ZZ9KQueryCapsInline(ZZ9KCaps *caps)
{
  register int zz9k_d0 __asm("d0");
  ZZ9K_INLINE_CALL1(-30, caps);
  return zz9k_d0;
}
#define ZZ9KQueryCaps(caps) __ZZ9KQueryCapsInline((caps))

static __inline int __ZZ9KQueryServiceInline(uint32_t service_id,
                                            ZZ9KServiceInfo *service)
{
  register uint32_t zz9k_d0 __asm("d0") = service_id;
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register ZZ9KServiceInfo *zz9k_a0 __asm("a0") = service;
  __asm volatile("jsr -36(a6)"
                 : "+r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_a0)
                 : ZZ9K_INLINE_CLOBBERS);
  return (int)zz9k_d0;
}
#define ZZ9KQueryService(service_id, service) \
  __ZZ9KQueryServiceInline((service_id), (service))

static __inline int __ZZ9KPingInline(const uint8_t *payload,
                                    uint32_t payload_len,
                                    uint8_t *reply_payload,
                                    uint32_t *reply_len)
{
  register uint32_t zz9k_d0 __asm("d0") = payload_len;
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register const uint8_t *zz9k_a0 __asm("a0") = payload;
  register uint8_t *zz9k_a1 __asm("a1") = reply_payload;
  register uint32_t *zz9k_a2 __asm("a2") = reply_len;
  __asm volatile("jsr -42(a6)"
                 : "+r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_a0), "r"(zz9k_a1),
                   "r"(zz9k_a2)
                 : ZZ9K_INLINE_CLOBBERS);
  return (int)zz9k_d0;
}
#define ZZ9KPing(payload, payload_len, reply_payload, reply_len) \
  __ZZ9KPingInline((payload), (payload_len), (reply_payload), (reply_len))

static __inline int __ZZ9KCallInline(ZZ9KRequest *request,
                                    ZZ9KMailboxEntry *reply,
                                    uint32_t timeout_ticks)
{
  register uint32_t zz9k_d0 __asm("d0") = timeout_ticks;
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register ZZ9KRequest *zz9k_a0 __asm("a0") = request;
  register ZZ9KMailboxEntry *zz9k_a1 __asm("a1") = reply;
  __asm volatile("jsr -48(a6)"
                 : "+r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_a0), "r"(zz9k_a1)
                 : ZZ9K_INLINE_CLOBBERS);
  return (int)zz9k_d0;
}
#define ZZ9KCall(request, reply, timeout_ticks) \
  __ZZ9KCallInline((request), (reply), (timeout_ticks))

static __inline int __ZZ9KCallAsyncInline(ZZ9KAsyncRequest *async,
                                         const ZZ9KRequest *request,
                                         ZZ9KAsyncCallback callback,
                                         void *user_data)
{
  register int zz9k_d0 __asm("d0");
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register ZZ9KAsyncRequest *zz9k_a0 __asm("a0") = async;
  register const ZZ9KRequest *zz9k_a1 __asm("a1") = request;
  register ZZ9KAsyncCallback zz9k_a2 __asm("a2") = callback;
  register void *zz9k_a3 __asm("a3") = user_data;
  __asm volatile("jsr -54(a6)"
                 : "=r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_a0), "r"(zz9k_a1),
                   "r"(zz9k_a2), "r"(zz9k_a3)
                 : ZZ9K_INLINE_CLOBBERS);
  return zz9k_d0;
}
#define ZZ9KCallAsync(async, request, callback, user_data) \
  __ZZ9KCallAsyncInline((async), (request), (callback), (user_data))

static __inline int __ZZ9KCallAsyncBatchInline(ZZ9KAsyncRequest *asyncs,
                                              const ZZ9KRequest *requests,
                                              uint32_t count,
                                              ZZ9KAsyncCallback callback,
                                              void *user_data,
                                              uint32_t *queued)
{
  register uint32_t zz9k_d0 __asm("d0") = count;
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register ZZ9KAsyncRequest *zz9k_a0 __asm("a0") = asyncs;
  register const ZZ9KRequest *zz9k_a1 __asm("a1") = requests;
  register ZZ9KAsyncCallback zz9k_a2 __asm("a2") = callback;
  register void *zz9k_a3 __asm("a3") = user_data;
  register uint32_t *zz9k_a4 __asm("a4") = queued;
  __asm volatile("jsr -60(a6)"
                 : "+r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_a0), "r"(zz9k_a1),
                   "r"(zz9k_a2), "r"(zz9k_a3), "r"(zz9k_a4)
                 : ZZ9K_INLINE_CLOBBERS);
  return (int)zz9k_d0;
}
#define ZZ9KCallAsyncBatch(asyncs, requests, count, callback, user_data, \
                           queued) \
  __ZZ9KCallAsyncBatchInline((asyncs), (requests), (count), (callback), \
                             (user_data), (queued))

static __inline int __ZZ9KPollInline(uint32_t max_completions,
                                    uint32_t *completed)
{
  register uint32_t zz9k_d0 __asm("d0") = max_completions;
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register uint32_t *zz9k_a0 __asm("a0") = completed;
  __asm volatile("jsr -66(a6)"
                 : "+r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_a0)
                 : ZZ9K_INLINE_CLOBBERS);
  return (int)zz9k_d0;
}
#define ZZ9KPoll(max_completions, completed) \
  __ZZ9KPollInline((max_completions), (completed))

static __inline int __ZZ9KAllocSharedInline(uint32_t length,
                                           uint32_t alignment,
                                           uint32_t flags,
                                           ZZ9KSharedBuffer *buffer)
{
  register uint32_t zz9k_d0 __asm("d0") = length;
  register uint32_t zz9k_d1 __asm("d1") = alignment;
  register uint32_t zz9k_d2 __asm("d2") = flags;
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register ZZ9KSharedBuffer *zz9k_a0 __asm("a0") = buffer;
  __asm volatile("jsr -72(a6)"
                 : "+r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_d1), "r"(zz9k_d2),
                   "r"(zz9k_a0)
                 : ZZ9K_INLINE_CLOBBERS);
  return (int)zz9k_d0;
}
#define ZZ9KAllocShared(length, alignment, flags, buffer) \
  __ZZ9KAllocSharedInline((length), (alignment), (flags), (buffer))

static __inline int __ZZ9KFreeSharedInline(uint32_t handle)
{
  register uint32_t zz9k_d0 __asm("d0") = handle;
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  __asm volatile("jsr -78(a6)"
                 : "+r"(zz9k_d0)
                 : "r"(zz9k_a6)
                 : ZZ9K_INLINE_CLOBBERS);
  return (int)zz9k_d0;
}
#define ZZ9KFreeShared(handle) __ZZ9KFreeSharedInline((handle))

static __inline int __ZZ9KMemFillInline(uint32_t handle, uint32_t offset,
                                       uint32_t length, uint8_t value)
{
  register uint32_t zz9k_d0 __asm("d0") = handle;
  register uint32_t zz9k_d1 __asm("d1") = offset;
  register uint32_t zz9k_d2 __asm("d2") = length;
  register uint32_t zz9k_d3 __asm("d3") = value;
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  __asm volatile("jsr -84(a6)"
                 : "+r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_d1), "r"(zz9k_d2),
                   "r"(zz9k_d3)
                 : ZZ9K_INLINE_CLOBBERS);
  return (int)zz9k_d0;
}
#define ZZ9KMemFill(handle, offset, length, value) \
  __ZZ9KMemFillInline((handle), (offset), (length), (value))

static __inline int __ZZ9KMemCopyInline(uint32_t dst_handle,
                                       uint32_t dst_offset,
                                       uint32_t src_handle,
                                       uint32_t src_offset,
                                       uint32_t length)
{
  register uint32_t zz9k_d0 __asm("d0") = dst_handle;
  register uint32_t zz9k_d1 __asm("d1") = dst_offset;
  register uint32_t zz9k_d2 __asm("d2") = src_handle;
  register uint32_t zz9k_d3 __asm("d3") = src_offset;
  register uint32_t zz9k_d4 __asm("d4") = length;
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  __asm volatile("jsr -90(a6)"
                 : "+r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_d1), "r"(zz9k_d2),
                   "r"(zz9k_d3), "r"(zz9k_d4)
                 : ZZ9K_INLINE_CLOBBERS);
  return (int)zz9k_d0;
}
#define ZZ9KMemCopy(dst_handle, dst_offset, src_handle, src_offset, length) \
  __ZZ9KMemCopyInline((dst_handle), (dst_offset), (src_handle), \
                      (src_offset), (length))

static __inline int __ZZ9KAllocSurfaceInline(uint32_t width, uint32_t height,
                                            uint32_t format, uint32_t flags,
                                            ZZ9KSurface *surface)
{
  register uint32_t zz9k_d0 __asm("d0") = width;
  register uint32_t zz9k_d1 __asm("d1") = height;
  register uint32_t zz9k_d2 __asm("d2") = format;
  register uint32_t zz9k_d3 __asm("d3") = flags;
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register ZZ9KSurface *zz9k_a0 __asm("a0") = surface;
  __asm volatile("jsr -96(a6)"
                 : "+r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_d1), "r"(zz9k_d2),
                   "r"(zz9k_d3), "r"(zz9k_a0)
                 : ZZ9K_INLINE_CLOBBERS);
  return (int)zz9k_d0;
}
#define ZZ9KAllocSurface(width, height, format, flags, surface) \
  __ZZ9KAllocSurfaceInline((width), (height), (format), (flags), (surface))

static __inline int __ZZ9KAllocSurfaceExInline(uint32_t width,
                                              uint32_t height,
                                              uint32_t format,
                                              uint32_t flags,
                                              uint32_t pitch,
                                              ZZ9KSurface *surface)
{
  register uint32_t zz9k_d0 __asm("d0") = width;
  register uint32_t zz9k_d1 __asm("d1") = height;
  register uint32_t zz9k_d2 __asm("d2") = format;
  register uint32_t zz9k_d3 __asm("d3") = flags;
  register uint32_t zz9k_d4 __asm("d4") = pitch;
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register ZZ9KSurface *zz9k_a0 __asm("a0") = surface;
  __asm volatile("jsr -102(a6)"
                 : "+r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_d1), "r"(zz9k_d2),
                   "r"(zz9k_d3), "r"(zz9k_d4), "r"(zz9k_a0)
                 : ZZ9K_INLINE_CLOBBERS);
  return (int)zz9k_d0;
}
#define ZZ9KAllocSurfaceEx(width, height, format, flags, pitch, surface) \
  __ZZ9KAllocSurfaceExInline((width), (height), (format), (flags), \
                             (pitch), (surface))

static __inline int __ZZ9KFreeSurfaceInline(uint32_t handle)
{
  register uint32_t zz9k_d0 __asm("d0") = handle;
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  __asm volatile("jsr -108(a6)"
                 : "+r"(zz9k_d0)
                 : "r"(zz9k_a6)
                 : ZZ9K_INLINE_CLOBBERS);
  return (int)zz9k_d0;
}
#define ZZ9KFreeSurface(handle) __ZZ9KFreeSurfaceInline((handle))

static __inline int __ZZ9KMapFramebufferSurfaceInline(ZZ9KSurface *surface)
{
  register int zz9k_d0 __asm("d0");
  ZZ9K_INLINE_CALL1(-114, surface);
  return zz9k_d0;
}
#define ZZ9KMapFramebufferSurface(surface) \
  __ZZ9KMapFramebufferSurfaceInline((surface))

static __inline int __ZZ9KScaleImageInline(const ZZ9KScaleImageDesc *desc)
{
  register int zz9k_d0 __asm("d0");
  ZZ9K_INLINE_CALL1(-120, desc);
  return zz9k_d0;
}
#define ZZ9KScaleImage(desc) __ZZ9KScaleImageInline((desc))

static __inline int __ZZ9KReadDiagInline(ZZ9KDiagInfo *diag)
{
  register int zz9k_d0 __asm("d0");
  ZZ9K_INLINE_CALL1(-126, diag);
  return zz9k_d0;
}
#define ZZ9KReadDiag(diag) __ZZ9KReadDiagInline((diag))

static __inline int __ZZ9KCallAsyncMsgInline(ZZ9KAsyncRequest *async,
                                             const ZZ9KRequest *request,
                                             struct MsgPort *reply_port)
{
  register int zz9k_d0 __asm("d0");
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register ZZ9KAsyncRequest *zz9k_a0 __asm("a0") = async;
  register const ZZ9KRequest *zz9k_a1 __asm("a1") = request;
  register struct MsgPort *zz9k_a2 __asm("a2") = reply_port;
  __asm volatile("jsr -132(a6)"
                 : "=r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_a0), "r"(zz9k_a1),
                   "r"(zz9k_a2)
                 : ZZ9K_INLINE_CLOBBERS);
  return zz9k_d0;
}
#define ZZ9KCallAsyncMsg(async, request, reply_port) \
  __ZZ9KCallAsyncMsgInline((async), (request), (reply_port))

static __inline int __ZZ9KCallAsyncBatchMsgInline(ZZ9KAsyncRequest *asyncs,
                                                  const ZZ9KRequest *requests,
                                                  uint32_t count,
                                                  struct MsgPort *reply_port,
                                                  uint32_t *queued)
{
  register uint32_t zz9k_d0 __asm("d0") = count;
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register ZZ9KAsyncRequest *zz9k_a0 __asm("a0") = asyncs;
  register const ZZ9KRequest *zz9k_a1 __asm("a1") = requests;
  register struct MsgPort *zz9k_a2 __asm("a2") = reply_port;
  register uint32_t *zz9k_a3 __asm("a3") = queued;
  __asm volatile("jsr -138(a6)"
                 : "+r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_a0), "r"(zz9k_a1),
                   "r"(zz9k_a2), "r"(zz9k_a3)
                 : ZZ9K_INLINE_CLOBBERS);
  return (int)zz9k_d0;
}
#define ZZ9KCallAsyncBatchMsg(asyncs, requests, count, reply_port, queued) \
  __ZZ9KCallAsyncBatchMsgInline((asyncs), (requests), (count), \
                                (reply_port), (queued))

static __inline int __ZZ9KCancelAsyncInline(ZZ9KAsyncRequest *async)
{
  register int zz9k_d0 __asm("d0");
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register ZZ9KAsyncRequest *zz9k_a0 __asm("a0") = async;
  __asm volatile("jsr -144(a6)"
                 : "=r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_a0)
                 : ZZ9K_INLINE_CLOBBERS);
  return zz9k_d0;
}
#define ZZ9KCancelAsync(async) __ZZ9KCancelAsyncInline((async))

static __inline int __ZZ9KWaitAsyncInline(ZZ9KAsyncRequest *async,
                                          uint32_t timeout_polls,
                                          uint32_t *polls_run)
{
  register uint32_t zz9k_d0 __asm("d0") = timeout_polls;
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register ZZ9KAsyncRequest *zz9k_a0 __asm("a0") = async;
  register uint32_t *zz9k_a1 __asm("a1") = polls_run;
  __asm volatile("jsr -150(a6)"
                 : "+r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_a0), "r"(zz9k_a1)
                 : ZZ9K_INLINE_CLOBBERS);
  return (int)zz9k_d0;
}
#define ZZ9KWaitAsync(async, timeout_polls, polls_run) \
  __ZZ9KWaitAsyncInline((async), (timeout_polls), (polls_run))

static __inline int __ZZ9KWaitAsyncBatchInline(ZZ9KAsyncRequest *asyncs,
                                               uint32_t count,
                                               uint32_t timeout_polls,
                                               uint32_t *completed,
                                               uint32_t *polls_run)
{
  register uint32_t zz9k_d0 __asm("d0") = count;
  register uint32_t zz9k_d1 __asm("d1") = timeout_polls;
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register ZZ9KAsyncRequest *zz9k_a0 __asm("a0") = asyncs;
  register uint32_t *zz9k_a1 __asm("a1") = completed;
  register uint32_t *zz9k_a2 __asm("a2") = polls_run;
  __asm volatile("jsr -156(a6)"
                 : "+r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_d1), "r"(zz9k_a0),
                   "r"(zz9k_a1), "r"(zz9k_a2)
                 : ZZ9K_INLINE_CLOBBERS);
  return (int)zz9k_d0;
}
#define ZZ9KWaitAsyncBatch(asyncs, count, timeout_polls, completed, \
                           polls_run) \
  __ZZ9KWaitAsyncBatchInline((asyncs), (count), (timeout_polls), \
                             (completed), (polls_run))

static __inline int __ZZ9KDecodeImageInline(uint32_t opcode,
                                            const ZZ9KImageDecodeDesc *desc,
                                            ZZ9KImageDecodeResult *result)
{
  register uint32_t zz9k_d0 __asm("d0") = opcode;
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register const ZZ9KImageDecodeDesc *zz9k_a0 __asm("a0") = desc;
  register ZZ9KImageDecodeResult *zz9k_a1 __asm("a1") = result;
  __asm volatile("jsr -162(a6)"
                 : "+r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_a0), "r"(zz9k_a1)
                 : ZZ9K_INLINE_CLOBBERS);
  return (int)zz9k_d0;
}
#define ZZ9KDecodeImage(opcode, desc, result) \
  __ZZ9KDecodeImageInline((opcode), (desc), (result))

static __inline int __ZZ9KCryptoHashInline(const ZZ9KCryptoHashDesc *desc,
                                           ZZ9KCryptoResult *result)
{
  register int zz9k_d0 __asm("d0");
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register const ZZ9KCryptoHashDesc *zz9k_a0 __asm("a0") = desc;
  register ZZ9KCryptoResult *zz9k_a1 __asm("a1") = result;
  __asm volatile("jsr -168(a6)"
                 : "=r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_a0), "r"(zz9k_a1)
                 : ZZ9K_INLINE_CLOBBERS);
  return zz9k_d0;
}
#define ZZ9KCryptoHash(desc, result) \
  __ZZ9KCryptoHashInline((desc), (result))

static __inline int __ZZ9KCryptoHashBatchInline(
    const ZZ9KCryptoHashDesc *descs,
    ZZ9KCryptoResult *results,
    uint32_t count,
    uint32_t max_in_flight,
    uint32_t timeout_ticks)
{
  register uint32_t zz9k_d0 __asm("d0") = count;
  register uint32_t zz9k_d1 __asm("d1") = max_in_flight;
  register uint32_t zz9k_d2 __asm("d2") = timeout_ticks;
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register const ZZ9KCryptoHashDesc *zz9k_a0 __asm("a0") = descs;
  register ZZ9KCryptoResult *zz9k_a1 __asm("a1") = results;
  __asm volatile("jsr -174(a6)"
                 : "+r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_d1), "r"(zz9k_d2),
                   "r"(zz9k_a0), "r"(zz9k_a1)
                 : ZZ9K_INLINE_CLOBBERS);
  return (int)zz9k_d0;
}
#define ZZ9KCryptoHashBatch(descs, results, count, max_in_flight, \
                            timeout_ticks) \
  __ZZ9KCryptoHashBatchInline((descs), (results), (count), \
                              (max_in_flight), (timeout_ticks))

static __inline int __ZZ9KCryptoStreamInline(
    const ZZ9KCryptoStreamDesc *desc,
    ZZ9KCryptoResult *result)
{
  register int zz9k_d0 __asm("d0");
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register const ZZ9KCryptoStreamDesc *zz9k_a0 __asm("a0") = desc;
  register ZZ9KCryptoResult *zz9k_a1 __asm("a1") = result;
  __asm volatile("jsr -180(a6)"
                 : "=r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_a0), "r"(zz9k_a1)
                 : ZZ9K_INLINE_CLOBBERS);
  return zz9k_d0;
}
#define ZZ9KCryptoStream(desc, result) \
  __ZZ9KCryptoStreamInline((desc), (result))

static __inline int __ZZ9KCryptoStreamBatchInline(
    const ZZ9KCryptoStreamDesc *descs,
    ZZ9KCryptoResult *results,
    uint32_t count,
    uint32_t max_in_flight,
    uint32_t timeout_ticks)
{
  register uint32_t zz9k_d0 __asm("d0") = count;
  register uint32_t zz9k_d1 __asm("d1") = max_in_flight;
  register uint32_t zz9k_d2 __asm("d2") = timeout_ticks;
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register const ZZ9KCryptoStreamDesc *zz9k_a0 __asm("a0") = descs;
  register ZZ9KCryptoResult *zz9k_a1 __asm("a1") = results;
  __asm volatile("jsr -186(a6)"
                 : "+r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_d1), "r"(zz9k_d2),
                   "r"(zz9k_a0), "r"(zz9k_a1)
                 : ZZ9K_INLINE_CLOBBERS);
  return (int)zz9k_d0;
}
#define ZZ9KCryptoStreamBatch(descs, results, count, max_in_flight, \
                              timeout_ticks) \
  __ZZ9KCryptoStreamBatchInline((descs), (results), (count), \
                                (max_in_flight), (timeout_ticks))

static __inline int __ZZ9KCryptoAeadInline(
    const ZZ9KCryptoAeadDesc *desc,
    ZZ9KCryptoResult *result)
{
  register int zz9k_d0 __asm("d0");
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register const ZZ9KCryptoAeadDesc *zz9k_a0 __asm("a0") = desc;
  register ZZ9KCryptoResult *zz9k_a1 __asm("a1") = result;
  __asm volatile("jsr -192(a6)"
                 : "=r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_a0), "r"(zz9k_a1)
                 : ZZ9K_INLINE_CLOBBERS);
  return zz9k_d0;
}
#define ZZ9KCryptoAead(desc, result) \
  __ZZ9KCryptoAeadInline((desc), (result))

static __inline int __ZZ9KCryptoAeadBatchInline(
    const ZZ9KCryptoAeadDesc *descs,
    ZZ9KCryptoResult *results,
    uint32_t count,
    uint32_t max_in_flight,
    uint32_t timeout_ticks)
{
  register uint32_t zz9k_d0 __asm("d0") = count;
  register uint32_t zz9k_d1 __asm("d1") = max_in_flight;
  register uint32_t zz9k_d2 __asm("d2") = timeout_ticks;
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register const ZZ9KCryptoAeadDesc *zz9k_a0 __asm("a0") = descs;
  register ZZ9KCryptoResult *zz9k_a1 __asm("a1") = results;
  __asm volatile("jsr -198(a6)"
                 : "+r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_d1), "r"(zz9k_d2),
                   "r"(zz9k_a0), "r"(zz9k_a1)
                 : ZZ9K_INLINE_CLOBBERS);
  return (int)zz9k_d0;
}
#define ZZ9KCryptoAeadBatch(descs, results, count, max_in_flight, \
                            timeout_ticks) \
  __ZZ9KCryptoAeadBatchInline((descs), (results), (count), \
                              (max_in_flight), (timeout_ticks))

static __inline int __ZZ9KFillSurfaceInline(
    const ZZ9KSurfaceFillDesc *desc)
{
  register int zz9k_d0 __asm("d0");
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register const ZZ9KSurfaceFillDesc *zz9k_a0 __asm("a0") = desc;
  __asm volatile("jsr -204(a6)"
                 : "=r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_a0)
                 : ZZ9K_INLINE_CLOBBERS);
  return zz9k_d0;
}
#define ZZ9KFillSurface(desc) \
  __ZZ9KFillSurfaceInline((desc))

static __inline int __ZZ9KCopySurfaceInline(
    const ZZ9KSurfaceCopyDesc *desc)
{
  register int zz9k_d0 __asm("d0");
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register const ZZ9KSurfaceCopyDesc *zz9k_a0 __asm("a0") = desc;
  __asm volatile("jsr -210(a6)"
                 : "=r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_a0)
                 : ZZ9K_INLINE_CLOBBERS);
  return zz9k_d0;
}
#define ZZ9KCopySurface(desc) \
  __ZZ9KCopySurfaceInline((desc))

static __inline int __ZZ9KImageSessionBeginInline(
    const ZZ9KImageSessionBeginDesc *desc,
    ZZ9KImageSessionResult *result)
{
  register int zz9k_d0 __asm("d0");
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register const ZZ9KImageSessionBeginDesc *zz9k_a0 __asm("a0") = desc;
  register ZZ9KImageSessionResult *zz9k_a1 __asm("a1") = result;
  __asm volatile("jsr -216(a6)"
                 : "=r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_a0), "r"(zz9k_a1)
                 : ZZ9K_INLINE_CLOBBERS);
  return zz9k_d0;
}
#define ZZ9KImageSessionBegin(desc, result) \
  __ZZ9KImageSessionBeginInline((desc), (result))

static __inline int __ZZ9KImageSessionFeedInline(
    const ZZ9KImageSessionFeedDesc *desc,
    ZZ9KImageSessionResult *result)
{
  register int zz9k_d0 __asm("d0");
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register const ZZ9KImageSessionFeedDesc *zz9k_a0 __asm("a0") = desc;
  register ZZ9KImageSessionResult *zz9k_a1 __asm("a1") = result;
  __asm volatile("jsr -222(a6)"
                 : "=r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_a0), "r"(zz9k_a1)
                 : ZZ9K_INLINE_CLOBBERS);
  return zz9k_d0;
}
#define ZZ9KImageSessionFeed(desc, result) \
  __ZZ9KImageSessionFeedInline((desc), (result))

static __inline int __ZZ9KImageSessionCloseInline(uint32_t session,
                                                  uint32_t flags)
{
  register uint32_t zz9k_d0 __asm("d0") = session;
  register uint32_t zz9k_d1 __asm("d1") = flags;
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  __asm volatile("jsr -228(a6)"
                 : "+r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_d1)
                 : ZZ9K_INLINE_CLOBBERS);
  return (int)zz9k_d0;
}
#define ZZ9KImageSessionClose(session, flags) \
  __ZZ9KImageSessionCloseInline((session), (flags))

static __inline int __ZZ9KScaleImageClippedInline(
    const ZZ9KScaleImageClippedDesc *desc)
{
  register int zz9k_d0 __asm("d0");
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register const ZZ9KScaleImageClippedDesc *zz9k_a0 __asm("a0") = desc;
  __asm volatile("jsr -234(a6)"
                 : "=r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_a0)
                 : ZZ9K_INLINE_CLOBBERS);
  return zz9k_d0;
}
#define ZZ9KScaleImageClipped(desc) \
  __ZZ9KScaleImageClippedInline((desc))

static __inline int __ZZ9KDecodeJpegInline(
    const ZZ9KImageDecodeDesc *desc,
    ZZ9KImageDecodeResult *result)
{
  register int zz9k_d0 __asm("d0");
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register const ZZ9KImageDecodeDesc *zz9k_a0 __asm("a0") = desc;
  register ZZ9KImageDecodeResult *zz9k_a1 __asm("a1") = result;
  __asm volatile("jsr -240(a6)"
                 : "=r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_a0), "r"(zz9k_a1)
                 : ZZ9K_INLINE_CLOBBERS);
  return zz9k_d0;
}
#define ZZ9KDecodeJpeg(desc, result) \
  __ZZ9KDecodeJpegInline((desc), (result))

static __inline int __ZZ9KDecodePngInline(
    const ZZ9KImageDecodeDesc *desc,
    ZZ9KImageDecodeResult *result)
{
  register int zz9k_d0 __asm("d0");
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register const ZZ9KImageDecodeDesc *zz9k_a0 __asm("a0") = desc;
  register ZZ9KImageDecodeResult *zz9k_a1 __asm("a1") = result;
  __asm volatile("jsr -246(a6)"
                 : "=r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_a0), "r"(zz9k_a1)
                 : ZZ9K_INLINE_CLOBBERS);
  return zz9k_d0;
}
#define ZZ9KDecodePng(desc, result) \
  __ZZ9KDecodePngInline((desc), (result))

static __inline int __ZZ9KDecodeGifInline(
    const ZZ9KImageDecodeDesc *desc,
    ZZ9KImageDecodeResult *result)
{
  register int zz9k_d0 __asm("d0");
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register const ZZ9KImageDecodeDesc *zz9k_a0 __asm("a0") = desc;
  register ZZ9KImageDecodeResult *zz9k_a1 __asm("a1") = result;
  __asm volatile("jsr -252(a6)"
                 : "=r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_a0), "r"(zz9k_a1)
                 : ZZ9K_INLINE_CLOBBERS);
  return zz9k_d0;
}
#define ZZ9KDecodeGif(desc, result) \
  __ZZ9KDecodeGifInline((desc), (result))

static __inline int __ZZ9KDecodeMp3Inline(
    const ZZ9KAudioDecodeDesc *desc,
    ZZ9KAudioDecodeResult *result)
{
  register int zz9k_d0 __asm("d0");
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register const ZZ9KAudioDecodeDesc *zz9k_a0 __asm("a0") = desc;
  register ZZ9KAudioDecodeResult *zz9k_a1 __asm("a1") = result;
  __asm volatile("jsr -258(a6)"
                 : "=r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_a0), "r"(zz9k_a1)
                 : ZZ9K_INLINE_CLOBBERS);
  return zz9k_d0;
}
#define ZZ9KDecodeMp3(desc, result) \
  __ZZ9KDecodeMp3Inline((desc), (result))

static __inline int __ZZ9KAudioStreamBeginInline(
    const ZZ9KAudioStreamBeginDesc *desc,
    ZZ9KAudioStreamResult *result)
{
  register int zz9k_d0 __asm("d0");
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register const ZZ9KAudioStreamBeginDesc *zz9k_a0 __asm("a0") = desc;
  register ZZ9KAudioStreamResult *zz9k_a1 __asm("a1") = result;
  __asm volatile("jsr -264(a6)"
                 : "=r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_a0), "r"(zz9k_a1)
                 : ZZ9K_INLINE_CLOBBERS);
  return zz9k_d0;
}
#define ZZ9KAudioStreamBegin(desc, result) \
  __ZZ9KAudioStreamBeginInline((desc), (result))

static __inline int __ZZ9KAudioStreamFeedInline(
    const ZZ9KAudioStreamFeedDesc *desc,
    ZZ9KAudioStreamResult *result)
{
  register int zz9k_d0 __asm("d0");
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register const ZZ9KAudioStreamFeedDesc *zz9k_a0 __asm("a0") = desc;
  register ZZ9KAudioStreamResult *zz9k_a1 __asm("a1") = result;
  __asm volatile("jsr -270(a6)"
                 : "=r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_a0), "r"(zz9k_a1)
                 : ZZ9K_INLINE_CLOBBERS);
  return zz9k_d0;
}
#define ZZ9KAudioStreamFeed(desc, result) \
  __ZZ9KAudioStreamFeedInline((desc), (result))

static __inline int __ZZ9KAudioStreamReadInline(
    uint32_t session,
    uint32_t pcm_read,
    uint32_t flags,
    ZZ9KAudioStreamResult *result)
{
  register uint32_t zz9k_d0 __asm("d0") = session;
  register uint32_t zz9k_d1 __asm("d1") = pcm_read;
  register uint32_t zz9k_d2 __asm("d2") = flags;
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register ZZ9KAudioStreamResult *zz9k_a0 __asm("a0") = result;
  __asm volatile("jsr -276(a6)"
                 : "+r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_d1), "r"(zz9k_d2),
                   "r"(zz9k_a0)
                 : ZZ9K_INLINE_CLOBBERS);
  return (int)zz9k_d0;
}
#define ZZ9KAudioStreamRead(session, pcm_read, flags, result) \
  __ZZ9KAudioStreamReadInline((session), (pcm_read), (flags), (result))

static __inline int __ZZ9KAudioStreamCloseInline(
    uint32_t session,
    uint32_t flags,
    ZZ9KAudioStreamResult *result)
{
  register uint32_t zz9k_d0 __asm("d0") = session;
  register uint32_t zz9k_d1 __asm("d1") = flags;
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register ZZ9KAudioStreamResult *zz9k_a0 __asm("a0") = result;
  __asm volatile("jsr -282(a6)"
                 : "+r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_d1), "r"(zz9k_a0)
                 : ZZ9K_INLINE_CLOBBERS);
  return (int)zz9k_d0;
}
#define ZZ9KAudioStreamClose(session, flags, result) \
  __ZZ9KAudioStreamCloseInline((session), (flags), (result))

static __inline int __ZZ9KCryptoKeyExchangeInline(
    const ZZ9KCryptoKxDesc *desc,
    ZZ9KCryptoResult *result)
{
  register int zz9k_d0 __asm("d0");
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register const ZZ9KCryptoKxDesc *zz9k_a0 __asm("a0") = desc;
  register ZZ9KCryptoResult *zz9k_a1 __asm("a1") = result;
  __asm volatile("jsr -288(a6)"
                 : "=r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_a0), "r"(zz9k_a1)
                 : ZZ9K_INLINE_CLOBBERS);
  return zz9k_d0;
}
#define ZZ9KCryptoKeyExchange(desc, result) \
  __ZZ9KCryptoKeyExchangeInline((desc), (result))

static __inline int __ZZ9KCryptoVerifyInline(
    const ZZ9KCryptoVerifyDesc *desc,
    int *valid)
{
  register int zz9k_d0 __asm("d0");
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register const ZZ9KCryptoVerifyDesc *zz9k_a0 __asm("a0") = desc;
  register int *zz9k_a1 __asm("a1") = valid;
  __asm volatile("jsr -294(a6)"
                 : "=r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_a0), "r"(zz9k_a1)
                 : ZZ9K_INLINE_CLOBBERS);
  return zz9k_d0;
}
#define ZZ9KCryptoVerify(desc, valid) \
  __ZZ9KCryptoVerifyInline((desc), (valid))

static __inline int __ZZ9KAudioStreamPlayInline(
    uint32_t session,
    uint32_t flags,
    ZZ9KAudioStreamResult *result)
{
  register uint32_t zz9k_d0 __asm("d0") = session;
  register uint32_t zz9k_d1 __asm("d1") = flags;
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register ZZ9KAudioStreamResult *zz9k_a0 __asm("a0") = result;
  __asm volatile("jsr -300(a6)"
                 : "+r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_d1), "r"(zz9k_a0)
                 : ZZ9K_INLINE_CLOBBERS);
  return (int)zz9k_d0;
}
#define ZZ9KAudioStreamPlay(session, flags, result) \
  __ZZ9KAudioStreamPlayInline((session), (flags), (result))

static __inline int __ZZ9KAudioStreamStopInline(
    uint32_t session,
    uint32_t flags,
    ZZ9KAudioStreamResult *result)
{
  register uint32_t zz9k_d0 __asm("d0") = session;
  register uint32_t zz9k_d1 __asm("d1") = flags;
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register ZZ9KAudioStreamResult *zz9k_a0 __asm("a0") = result;
  __asm volatile("jsr -306(a6)"
                 : "+r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_d1), "r"(zz9k_a0)
                 : ZZ9K_INLINE_CLOBBERS);
  return (int)zz9k_d0;
}
#define ZZ9KAudioStreamStop(session, flags, result) \
  __ZZ9KAudioStreamStopInline((session), (flags), (result))

static __inline int __ZZ9KQueryPaletteInline(
    const ZZ9KPaletteQueryDesc *desc)
{
  register uint32_t zz9k_d0 __asm("d0");
  register struct Library *zz9k_a6 __asm("a6") = ZZ9KBase;
  register const ZZ9KPaletteQueryDesc *zz9k_a0 __asm("a0") = desc;
  __asm volatile("jsr -312(a6)"
                 : "=r"(zz9k_d0)
                 : "r"(zz9k_a6), "r"(zz9k_a0)
                 : ZZ9K_INLINE_CLOBBERS);
  return (int)zz9k_d0;
}
#define ZZ9KQueryPalette(desc) __ZZ9KQueryPaletteInline((desc))

#endif

#ifdef __cplusplus
}
#endif

#endif /* PROTO_ZZ9K_H */
