/*
 * Core implementation for the AmigaOS zz9k.library facade.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/library.h"
#include <string.h>

#if ZZ9K_LIBRARY_AMIGA
#include <proto/exec.h>
#endif

static int zz9k_library_has_context(const ZZ9KLibrary *library)
{
  return library && library->ctx;
}

static ZZ9KAsyncRequest *zz9k_find_pending(ZZ9KLibrary *library,
                                           uint32_t request_id,
                                           ZZ9KAsyncRequest ***previous_next)
{
  ZZ9KAsyncRequest **link;

  link = &library->pending_head;
  while (*link) {
    if ((*link)->request_id == request_id) {
      if (previous_next) {
        *previous_next = link;
      }
      return *link;
    }
    link = &(*link)->next;
  }

  return 0;
}

static void zz9k_cancel_pending(ZZ9KLibrary *library)
{
  ZZ9KAsyncRequest *async;
  ZZ9KAsyncRequest *next;

  async = library->pending_head;
  library->pending_head = 0;
  while (async) {
    next = async->next;
    async->next = 0;
    async->queued = 0;
    async->completed = 1;
    async->status = ZZ9K_STATUS_CANCELLED;
    async = next;
  }
}

static uint32_t zz9k_count_batch_completed(const ZZ9KAsyncRequest *asyncs,
                                           uint32_t count)
{
  uint32_t done;
  uint32_t i;

  done = 0;
  for (i = 0; i < count; i++) {
    if (asyncs[i].completed) {
      done++;
    }
  }

  return done;
}

static int zz9k_batch_waitable(const ZZ9KAsyncRequest *asyncs,
                               uint32_t count)
{
  uint32_t i;

  for (i = 0; i < count; i++) {
    if (!asyncs[i].completed && !asyncs[i].queued) {
      return 0;
    }
  }

  return 1;
}

static int zz9k_wait_for_transport_event(ZZ9KLibrary *library)
{
  if (!library->event_waiter) {
    return ZZ9K_STATUS_OK;
  }

  return library->event_waiter(library, library->event_user_data);
}

static int zz9k_dispatch_reply(ZZ9KLibrary *library,
                               const ZZ9KMailboxEntry *reply)
{
  ZZ9KAsyncRequest **previous_next;
  ZZ9KAsyncRequest *async;

  async = zz9k_find_pending(library, reply->request_id, &previous_next);
  if (!async) {
    return ZZ9K_STATUS_OK;
  }

  *previous_next = async->next;
  async->next = 0;
  async->reply = *reply;
  async->status = reply->status;
  async->queued = 0;
  async->completed = 1;

  if (async->callback) {
    async->callback(async, async->user_data);
  }
#if ZZ9K_LIBRARY_AMIGA
  if (async->reply_port) {
    async->message.mn_Length = (uint16_t)sizeof(*async);
    PutMsg(async->reply_port, &async->message);
  }
#endif

  return ZZ9K_STATUS_OK;
}

void ZZ9KInit(ZZ9KLibrary *library)
{
  if (library) {
    memset(library, 0, sizeof(*library));
  }
}

int ZZ9KOpen(ZZ9KLibrary *library)
{
  ZZ9KContext *ctx;
  int status;

  if (!library) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  ZZ9KClose(library);
  ctx = 0;
  status = zz9k_open(&ctx);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  library->ctx = ctx;
  library->owns_context = 1;
  library->pending_head = 0;
  return ZZ9K_STATUS_OK;
}

void ZZ9KClose(ZZ9KLibrary *library)
{
  if (!library) {
    return;
  }

  zz9k_cancel_pending(library);
  if (library->ctx && library->owns_context) {
    zz9k_close(library->ctx);
  }

  library->ctx = 0;
  library->owns_context = 0;
  library->event_waiter = 0;
  library->event_user_data = 0;
}

int ZZ9KAttachContext(ZZ9KLibrary *library, ZZ9KContext *ctx,
                      int take_ownership)
{
  if (!library || !ctx) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  ZZ9KClose(library);
  library->ctx = ctx;
  library->owns_context = take_ownership ? 1 : 0;
  library->pending_head = 0;
  return ZZ9K_STATUS_OK;
}

int ZZ9KSetEventWaiter(ZZ9KLibrary *library, ZZ9KEventWaiter waiter,
                       void *user_data)
{
  if (!library) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  library->event_waiter = waiter;
  library->event_user_data = user_data;
  return ZZ9K_STATUS_OK;
}

int ZZ9KQueryCaps(ZZ9KLibrary *library, ZZ9KCaps *caps)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_query_caps(library->ctx, caps);
}

int ZZ9KQueryService(ZZ9KLibrary *library, uint32_t service_id,
                     ZZ9KServiceInfo *service)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_query_service(library->ctx, service_id, service);
}

int ZZ9KPing(ZZ9KLibrary *library, const uint8_t *payload,
             uint32_t payload_len, uint8_t *reply_payload,
             uint32_t *reply_len)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_ping(library->ctx, payload, payload_len, reply_payload,
                   reply_len);
}

int ZZ9KCall(ZZ9KLibrary *library, ZZ9KRequest *request,
             ZZ9KMailboxEntry *reply, uint32_t timeout_ticks)
{
  uint32_t request_id;
  uint32_t ticks;
  int status;

  if (!zz9k_library_has_context(library) || !request || !reply) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  status = zz9k_submit(library->ctx, request, &request_id);
  if (status != ZZ9K_STATUS_QUEUED) {
    return status;
  }

  for (ticks = 0; ticks < timeout_ticks; ticks++) {
    status = zz9k_poll(library->ctx, reply);
    if (status == ZZ9K_STATUS_BUSY) {
      continue;
    }
    if (status != ZZ9K_STATUS_OK) {
      return status;
    }
    if (reply->request_id == request_id) {
      return reply->status;
    }

    status = zz9k_dispatch_reply(library, reply);
    if (status != ZZ9K_STATUS_OK) {
      return status;
    }
  }

  return ZZ9K_STATUS_TIMEOUT;
}

int ZZ9KCallAsync(ZZ9KLibrary *library, ZZ9KAsyncRequest *async,
                  const ZZ9KRequest *request, ZZ9KAsyncCallback callback,
                  void *user_data)
{
  uint32_t request_id;
  int status;

  if (!zz9k_library_has_context(library) || !async || !request) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (async->queued) {
    return ZZ9K_STATUS_BUSY;
  }

  memset(async, 0, sizeof(*async));
  async->request = *request;
  async->callback = callback;
  async->user_data = user_data;
  async->status = ZZ9K_STATUS_QUEUED;

  status = zz9k_submit(library->ctx, &async->request, &request_id);
  if (status != ZZ9K_STATUS_QUEUED) {
    memset(async, 0, sizeof(*async));
    return status;
  }

  async->request_id = request_id;
  async->request.entry.request_id = request_id;
  async->queued = 1;
  async->next = library->pending_head;
  library->pending_head = async;
  return ZZ9K_STATUS_QUEUED;
}

int ZZ9KCallAsyncBatch(ZZ9KLibrary *library, ZZ9KAsyncRequest *asyncs,
                       const ZZ9KRequest *requests, uint32_t count,
                       ZZ9KAsyncCallback callback, void *user_data,
                       uint32_t *queued)
{
  uint32_t done;
  int status;

  if (!zz9k_library_has_context(library) || !asyncs || !requests ||
      count == 0 || !queued) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  *queued = 0;
  done = 0;
  while (done < count) {
    status = ZZ9KCallAsync(library, &asyncs[done], &requests[done],
                           callback, user_data);
    if (status != ZZ9K_STATUS_QUEUED) {
      if (done != 0) {
        return ZZ9K_STATUS_QUEUED;
      }
      return status;
    }

    done++;
    *queued = done;
  }

  return ZZ9K_STATUS_QUEUED;
}

int ZZ9KCancelAsync(ZZ9KLibrary *library, ZZ9KAsyncRequest *async)
{
  ZZ9KAsyncRequest **previous_next;

  if (!zz9k_library_has_context(library) || !async || !async->queued) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  if (zz9k_find_pending(library, async->request_id, &previous_next) !=
      async) {
    return ZZ9K_STATUS_NOT_FOUND;
  }

  *previous_next = async->next;
  async->next = 0;
  async->queued = 0;
  async->completed = 1;
  async->status = ZZ9K_STATUS_CANCELLED;
  memset(&async->reply, 0, sizeof(async->reply));
  async->reply.request_id = async->request_id;
  async->reply.opcode = async->request.entry.opcode;
  async->reply.status = ZZ9K_STATUS_CANCELLED;

  if (async->callback) {
    async->callback(async, async->user_data);
  }
#if ZZ9K_LIBRARY_AMIGA
  if (async->reply_port) {
    async->message.mn_Length = (uint16_t)sizeof(*async);
    PutMsg(async->reply_port, &async->message);
  }
#endif

  return ZZ9K_STATUS_OK;
}

int ZZ9KWaitAsync(ZZ9KLibrary *library, ZZ9KAsyncRequest *async,
                  uint32_t timeout_polls, uint32_t *polls_run)
{
  uint32_t polls;
  uint32_t completed;
  int status;

  if (polls_run) {
    *polls_run = 0;
  }
  if (!zz9k_library_has_context(library) || !async) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (async->completed) {
    return async->status;
  }
  if (!async->queued) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  polls = 0;
  while (polls < timeout_polls) {
    completed = 0;
    status = ZZ9KPoll(library, 1, &completed);
    if (status != ZZ9K_STATUS_OK) {
      return status;
    }

    polls++;
    if (polls_run) {
      *polls_run = polls;
    }
    if (async->completed) {
      return async->status;
    }
    if (!async->queued) {
      return ZZ9K_STATUS_BAD_REQUEST;
    }
    if (completed == 0 && polls < timeout_polls) {
      status = zz9k_wait_for_transport_event(library);
      if (status != ZZ9K_STATUS_OK) {
        return status;
      }
    }
  }

  return ZZ9K_STATUS_TIMEOUT;
}

int ZZ9KWaitAsyncBatch(ZZ9KLibrary *library, ZZ9KAsyncRequest *asyncs,
                       uint32_t count, uint32_t timeout_polls,
                       uint32_t *completed, uint32_t *polls_run)
{
  uint32_t done;
  uint32_t polls;
  uint32_t poll_completed;
  int status;

  if (completed) {
    *completed = 0;
  }
  if (polls_run) {
    *polls_run = 0;
  }
  if (!zz9k_library_has_context(library) || !asyncs || count == 0) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (!zz9k_batch_waitable(asyncs, count)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  done = zz9k_count_batch_completed(asyncs, count);
  if (completed) {
    *completed = done;
  }
  if (done == count) {
    return ZZ9K_STATUS_OK;
  }

  polls = 0;
  while (polls < timeout_polls) {
    poll_completed = 0;
    status = ZZ9KPoll(library, 1, &poll_completed);
    if (status != ZZ9K_STATUS_OK) {
      return status;
    }

    polls++;
    done = zz9k_count_batch_completed(asyncs, count);
    if (completed) {
      *completed = done;
    }
    if (polls_run) {
      *polls_run = polls;
    }
    if (done == count) {
      return ZZ9K_STATUS_OK;
    }
    if (!zz9k_batch_waitable(asyncs, count)) {
      return ZZ9K_STATUS_BAD_REQUEST;
    }
    if (poll_completed == 0 && polls < timeout_polls) {
      status = zz9k_wait_for_transport_event(library);
      if (status != ZZ9K_STATUS_OK) {
        return status;
      }
    }
  }

  return ZZ9K_STATUS_TIMEOUT;
}

#if ZZ9K_LIBRARY_AMIGA
int ZZ9KCallAsyncMsg(ZZ9KLibrary *library, ZZ9KAsyncRequest *async,
                     const ZZ9KRequest *request, struct MsgPort *reply_port)
{
  int status;

  if (!reply_port) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  status = ZZ9KCallAsync(library, async, request, 0, 0);
  if (status == ZZ9K_STATUS_QUEUED) {
    async->message.mn_ReplyPort = 0;
    async->message.mn_Length = (uint16_t)sizeof(*async);
    async->reply_port = reply_port;
  }

  return status;
}

int ZZ9KCallAsyncBatchMsg(ZZ9KLibrary *library, ZZ9KAsyncRequest *asyncs,
                          const ZZ9KRequest *requests, uint32_t count,
                          struct MsgPort *reply_port, uint32_t *queued)
{
  uint32_t done;
  int status;

  if (!asyncs || !requests || count == 0 || !reply_port || !queued) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  *queued = 0;
  done = 0;
  while (done < count) {
    status = ZZ9KCallAsyncMsg(library, &asyncs[done], &requests[done],
                              reply_port);
    if (status != ZZ9K_STATUS_QUEUED) {
      if (done != 0) {
        return ZZ9K_STATUS_QUEUED;
      }
      return status;
    }

    done++;
    *queued = done;
  }

  return ZZ9K_STATUS_QUEUED;
}
#endif

int ZZ9KPoll(ZZ9KLibrary *library, uint32_t max_completions,
             uint32_t *completed)
{
  ZZ9KMailboxEntry reply;
  uint32_t done;
  int status;

  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  done = 0;
  while (done < max_completions) {
    memset(&reply, 0, sizeof(reply));
    status = zz9k_poll(library->ctx, &reply);
    if (status == ZZ9K_STATUS_BUSY) {
      break;
    }
    if (status != ZZ9K_STATUS_OK) {
      return status;
    }

    status = zz9k_dispatch_reply(library, &reply);
    if (status != ZZ9K_STATUS_OK) {
      return status;
    }
    done++;
  }

  if (completed) {
    *completed = done;
  }
  return ZZ9K_STATUS_OK;
}

int ZZ9KAllocShared(ZZ9KLibrary *library, uint32_t length,
                    uint32_t alignment, uint32_t flags,
                    ZZ9KSharedBuffer *buffer)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_alloc_shared(library->ctx, length, alignment, flags, buffer);
}

int ZZ9KFreeShared(ZZ9KLibrary *library, uint32_t handle)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_free_shared(library->ctx, handle);
}

int ZZ9KMemFill(ZZ9KLibrary *library, uint32_t handle, uint32_t offset,
                uint32_t length, uint8_t value)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_mem_fill(library->ctx, handle, offset, length, value);
}

int ZZ9KMemCopy(ZZ9KLibrary *library, uint32_t dst_handle,
                uint32_t dst_offset, uint32_t src_handle,
                uint32_t src_offset, uint32_t length)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_mem_copy(library->ctx, dst_handle, dst_offset, src_handle,
                       src_offset, length);
}

int ZZ9KAllocSurface(ZZ9KLibrary *library, uint32_t width, uint32_t height,
                     uint32_t format, uint32_t flags,
                     ZZ9KSurface *surface)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_alloc_surface(library->ctx, width, height, format, flags,
                            surface);
}

int ZZ9KAllocSurfaceEx(ZZ9KLibrary *library, uint32_t width, uint32_t height,
                       uint32_t format, uint32_t flags, uint32_t pitch,
                       ZZ9KSurface *surface)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_alloc_surface_ex(library->ctx, width, height, format, flags,
                               pitch, surface);
}

int ZZ9KFreeSurface(ZZ9KLibrary *library, uint32_t handle)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_free_surface(library->ctx, handle);
}

int ZZ9KMapFramebufferSurface(ZZ9KLibrary *library, ZZ9KSurface *surface)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_map_framebuffer_surface(library->ctx, surface);
}

int ZZ9KScaleImage(ZZ9KLibrary *library, const ZZ9KScaleImageDesc *desc)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_scale_image(library->ctx, desc);
}

int ZZ9KScaleImageClipped(ZZ9KLibrary *library,
                          const ZZ9KScaleImageClippedDesc *desc)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_scale_image_clipped(library->ctx, desc);
}

int ZZ9KFillSurface(ZZ9KLibrary *library, const ZZ9KSurfaceFillDesc *desc)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_fill_surface(library->ctx, desc);
}

int ZZ9KCopySurface(ZZ9KLibrary *library, const ZZ9KSurfaceCopyDesc *desc)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_copy_surface(library->ctx, desc);
}

int ZZ9KDecodeImage(ZZ9KLibrary *library, uint32_t opcode,
                    const ZZ9KImageDecodeDesc *desc,
                    ZZ9KImageDecodeResult *result)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_decode_image(library->ctx, opcode, desc, result);
}

int ZZ9KDecodeJpeg(ZZ9KLibrary *library,
                   const ZZ9KImageDecodeDesc *desc,
                   ZZ9KImageDecodeResult *result)
{
  return ZZ9KDecodeImage(library, ZZ9K_OP_DECODE_JPEG, desc, result);
}

int ZZ9KDecodePng(ZZ9KLibrary *library,
                  const ZZ9KImageDecodeDesc *desc,
                  ZZ9KImageDecodeResult *result)
{
  return ZZ9KDecodeImage(library, ZZ9K_OP_DECODE_PNG, desc, result);
}

int ZZ9KDecodeGif(ZZ9KLibrary *library,
                  const ZZ9KImageDecodeDesc *desc,
                  ZZ9KImageDecodeResult *result)
{
  return ZZ9KDecodeImage(library, ZZ9K_OP_DECODE_GIF, desc, result);
}

int ZZ9KDecodeMp3(ZZ9KLibrary *library,
                  const ZZ9KAudioDecodeDesc *desc,
                  ZZ9KAudioDecodeResult *result)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_decode_mp3(library->ctx, desc, result);
}

int ZZ9KAudioStreamBegin(ZZ9KLibrary *library,
                         const ZZ9KAudioStreamBeginDesc *desc,
                         ZZ9KAudioStreamResult *result)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_audio_stream_begin(library->ctx, desc, result);
}

int ZZ9KAudioStreamFeed(ZZ9KLibrary *library,
                        const ZZ9KAudioStreamFeedDesc *desc,
                        ZZ9KAudioStreamResult *result)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_audio_stream_feed(library->ctx, desc, result);
}

int ZZ9KAudioStreamRead(ZZ9KLibrary *library, uint32_t session,
                        uint32_t pcm_read, uint32_t flags,
                        ZZ9KAudioStreamResult *result)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_audio_stream_read(library->ctx, session, pcm_read, flags,
                                result);
}

int ZZ9KAudioStreamClose(ZZ9KLibrary *library, uint32_t session,
                         uint32_t flags,
                         ZZ9KAudioStreamResult *result)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_audio_stream_close(library->ctx, session, flags, result);
}

int ZZ9KAudioStreamPlay(ZZ9KLibrary *library, uint32_t session,
                        uint32_t flags,
                        ZZ9KAudioStreamResult *result)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_audio_stream_play(library->ctx, session, flags, result);
}

int ZZ9KAudioStreamStop(ZZ9KLibrary *library, uint32_t session,
                        uint32_t flags,
                        ZZ9KAudioStreamResult *result)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_audio_stream_stop(library->ctx, session, flags, result);
}

int ZZ9KQueryPalette(ZZ9KLibrary *library, const ZZ9KPaletteQueryDesc *desc)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_query_palette(library->ctx, desc);
}

int ZZ9KImageSessionBegin(ZZ9KLibrary *library,
                          const ZZ9KImageSessionBeginDesc *desc,
                          ZZ9KImageSessionResult *result)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_image_session_begin(library->ctx, desc, result);
}

int ZZ9KImageSessionFeed(ZZ9KLibrary *library,
                         const ZZ9KImageSessionFeedDesc *desc,
                         ZZ9KImageSessionResult *result)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_image_session_feed(library->ctx, desc, result);
}

int ZZ9KImageSessionClose(ZZ9KLibrary *library, uint32_t session,
                          uint32_t flags)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_image_session_close(library->ctx, session, flags);
}

int ZZ9KCryptoHash(ZZ9KLibrary *library, const ZZ9KCryptoHashDesc *desc,
                   ZZ9KCryptoResult *result)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_crypto_hash(library->ctx, desc, result);
}

int ZZ9KCryptoHashBatch(ZZ9KLibrary *library,
                        const ZZ9KCryptoHashDesc *descs,
                        ZZ9KCryptoResult *results,
                        uint32_t count, uint32_t max_in_flight,
                        uint32_t timeout_ticks)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_crypto_hash_batch(library->ctx, descs, results, count,
                                max_in_flight, timeout_ticks);
}

int ZZ9KCryptoStream(ZZ9KLibrary *library,
                     const ZZ9KCryptoStreamDesc *desc,
                     ZZ9KCryptoResult *result)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_crypto_stream(library->ctx, desc, result);
}

int ZZ9KCryptoStreamBatch(ZZ9KLibrary *library,
                          const ZZ9KCryptoStreamDesc *descs,
                          ZZ9KCryptoResult *results,
                          uint32_t count, uint32_t max_in_flight,
                          uint32_t timeout_ticks)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_crypto_stream_batch(library->ctx, descs, results, count,
                                  max_in_flight, timeout_ticks);
}

int ZZ9KCryptoAead(ZZ9KLibrary *library, const ZZ9KCryptoAeadDesc *desc,
                   ZZ9KCryptoResult *result)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_crypto_aead(library->ctx, desc, result);
}

int ZZ9KCryptoAeadBatch(ZZ9KLibrary *library,
                        const ZZ9KCryptoAeadDesc *descs,
                        ZZ9KCryptoResult *results,
                        uint32_t count, uint32_t max_in_flight,
                        uint32_t timeout_ticks)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_crypto_aead_batch(library->ctx, descs, results, count,
                                max_in_flight, timeout_ticks);
}

int ZZ9KCryptoKeyExchange(ZZ9KLibrary *library,
                          const ZZ9KCryptoKxDesc *desc,
                          ZZ9KCryptoResult *result)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_crypto_kx(library->ctx, desc, result);
}

int ZZ9KCryptoVerify(ZZ9KLibrary *library,
                     const ZZ9KCryptoVerifyDesc *desc,
                     int *valid)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_crypto_verify(library->ctx, desc, valid);
}

int ZZ9KReadDiag(ZZ9KLibrary *library, ZZ9KDiagInfo *diag)
{
  if (!zz9k_library_has_context(library)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  return zz9k_read_diag(library->ctx, diag);
}
