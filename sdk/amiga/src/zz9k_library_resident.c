/*
 * AmigaOS resident wrapper for zz9k.library.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/library.h"
#include "zz9k/library_vectors.h"
#include <SDI_compiler.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <exec/execbase.h>
#include <exec/interrupts.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <exec/ports.h>
#include <exec/resident.h>
#include <exec/semaphores.h>
#include <exec/tasks.h>
#include <hardware/intbits.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <stdint.h>
#include <string.h>

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

#define ZZ9K_LIB_IRQ_WAIT_MICROS 10UL

struct ZZ9KBase {
  struct Library library;
  BPTR segment;
  struct SignalSemaphore lock;
  ZZ9KLibrary core;
  struct Interrupt irq;
  struct Task *irq_task;
  ULONG irq_signal_mask;
  ULONG irq_hits;
  ULONG irq_last_status;
  int irq_int_bit;
  BYTE irq_signal_bit;
  UBYTE irq_installed;
  UBYTE irq_enabled;
  struct Process *dispatcher_process;
  struct Task *dispatcher_task;
  struct Task *dispatcher_start_task;
  struct Task *dispatcher_stop_task;
  ULONG dispatcher_signal_mask;
  ULONG dispatcher_start_signal_mask;
  ULONG dispatcher_stop_signal_mask;
  int dispatcher_start_status;
  BYTE dispatcher_signal_bit;
  UBYTE dispatcher_running;
  UBYTE dispatcher_starting;
  UBYTE dispatcher_stop_requested;
  int last_open_status;
};

struct ZZ9KTimerWait {
  struct MsgPort *port;
  struct timerequest *request;
  ULONG signal_mask;
  UBYTE device_open;
};

static struct ZZ9KBase *zz9k_lib_open(REG(a6, struct ZZ9KBase *base));
static BPTR zz9k_lib_close(REG(a6, struct ZZ9KBase *base));
static BPTR zz9k_lib_expunge(REG(a6, struct ZZ9KBase *base));
static ULONG zz9k_lib_null(void);
static struct ZZ9KBase *zz9k_lib_init(REG(a0, BPTR segment));
static ULONG zz9k_lib_irq_isr(REG(a1, struct ZZ9KBase *base));
static int zz9k_lib_wait_for_irq_event(ZZ9KLibrary *library,
                                       void *user_data);
static void zz9k_lib_dispatcher_proc(void);

static int zz9k_lib_query_caps(REG(a6, struct ZZ9KBase *base),
                               REG(a0, ZZ9KCaps *caps));
static int zz9k_lib_query_service(REG(a6, struct ZZ9KBase *base),
                                  REG(d0, uint32_t service_id),
                                  REG(a0, ZZ9KServiceInfo *service));
static int zz9k_lib_ping(REG(a6, struct ZZ9KBase *base),
                         REG(a0, const uint8_t *payload),
                         REG(d0, uint32_t payload_len),
                         REG(a1, uint8_t *reply_payload),
                         REG(a2, uint32_t *reply_len));
static int zz9k_lib_call(REG(a6, struct ZZ9KBase *base),
                         REG(a0, ZZ9KRequest *request),
                         REG(a1, ZZ9KMailboxEntry *reply),
                         REG(d0, uint32_t timeout_ticks));
static int zz9k_lib_call_async(REG(a6, struct ZZ9KBase *base),
                               REG(a0, ZZ9KAsyncRequest *async),
                               REG(a1, const ZZ9KRequest *request),
                               REG(a2, ZZ9KAsyncCallback callback),
                               REG(a3, void *user_data));
static int zz9k_lib_call_async_batch(REG(a6, struct ZZ9KBase *base),
                                     REG(a0, ZZ9KAsyncRequest *asyncs),
                                     REG(a1, const ZZ9KRequest *requests),
                                     REG(d0, uint32_t count),
                                     REG(a2, ZZ9KAsyncCallback callback),
                                     REG(a3, void *user_data),
                                     REG(a4, uint32_t *queued));
static int zz9k_lib_poll(REG(a6, struct ZZ9KBase *base),
                         REG(d0, uint32_t max_completions),
                         REG(a0, uint32_t *completed));
static int zz9k_lib_alloc_shared(REG(a6, struct ZZ9KBase *base),
                                 REG(d0, uint32_t length),
                                 REG(d1, uint32_t alignment),
                                 REG(d2, uint32_t flags),
                                 REG(a0, ZZ9KSharedBuffer *buffer));
static int zz9k_lib_free_shared(REG(a6, struct ZZ9KBase *base),
                                REG(d0, uint32_t handle));
static int zz9k_lib_mem_fill(REG(a6, struct ZZ9KBase *base),
                             REG(d0, uint32_t handle),
                             REG(d1, uint32_t offset),
                             REG(d2, uint32_t length),
                             REG(d3, uint8_t value));
static int zz9k_lib_mem_copy(REG(a6, struct ZZ9KBase *base),
                             REG(d0, uint32_t dst_handle),
                             REG(d1, uint32_t dst_offset),
                             REG(d2, uint32_t src_handle),
                             REG(d3, uint32_t src_offset),
                             REG(d4, uint32_t length));
static int zz9k_lib_alloc_surface(REG(a6, struct ZZ9KBase *base),
                                  REG(d0, uint32_t width),
                                  REG(d1, uint32_t height),
                                  REG(d2, uint32_t format),
                                  REG(d3, uint32_t flags),
                                  REG(a0, ZZ9KSurface *surface));
static int zz9k_lib_alloc_surface_ex(REG(a6, struct ZZ9KBase *base),
                                     REG(d0, uint32_t width),
                                     REG(d1, uint32_t height),
                                     REG(d2, uint32_t format),
                                     REG(d3, uint32_t flags),
                                     REG(d4, uint32_t pitch),
                                     REG(a0, ZZ9KSurface *surface));
static int zz9k_lib_free_surface(REG(a6, struct ZZ9KBase *base),
                                 REG(d0, uint32_t handle));
static int zz9k_lib_map_framebuffer_surface(REG(a6, struct ZZ9KBase *base),
                                            REG(a0, ZZ9KSurface *surface));
static int zz9k_lib_scale_image(REG(a6, struct ZZ9KBase *base),
                                REG(a0, const ZZ9KScaleImageDesc *desc));
static int zz9k_lib_scale_image_clipped(
    REG(a6, struct ZZ9KBase *base),
    REG(a0, const ZZ9KScaleImageClippedDesc *desc));
static int zz9k_lib_fill_surface(REG(a6, struct ZZ9KBase *base),
                                 REG(a0, const ZZ9KSurfaceFillDesc *desc));
static int zz9k_lib_copy_surface(REG(a6, struct ZZ9KBase *base),
                                 REG(a0, const ZZ9KSurfaceCopyDesc *desc));
static int zz9k_lib_read_diag(REG(a6, struct ZZ9KBase *base),
                              REG(a0, ZZ9KDiagInfo *diag));
static int zz9k_lib_call_async_msg(REG(a6, struct ZZ9KBase *base),
                                   REG(a0, ZZ9KAsyncRequest *async),
                                   REG(a1, const ZZ9KRequest *request),
                                   REG(a2, struct MsgPort *reply_port));
static int zz9k_lib_call_async_batch_msg(REG(a6, struct ZZ9KBase *base),
                                         REG(a0, ZZ9KAsyncRequest *asyncs),
                                         REG(a1, const ZZ9KRequest *requests),
                                         REG(d0, uint32_t count),
                                         REG(a2, struct MsgPort *reply_port),
                                         REG(a3, uint32_t *queued));
static int zz9k_lib_cancel_async(REG(a6, struct ZZ9KBase *base),
                                 REG(a0, ZZ9KAsyncRequest *async));
static int zz9k_lib_wait_async(REG(a6, struct ZZ9KBase *base),
                               REG(a0, ZZ9KAsyncRequest *async),
                               REG(d0, uint32_t timeout_polls),
                               REG(a1, uint32_t *polls_run));
static int zz9k_lib_wait_async_batch(REG(a6, struct ZZ9KBase *base),
                                     REG(a0, ZZ9KAsyncRequest *asyncs),
                                     REG(d0, uint32_t count),
                                     REG(d1, uint32_t timeout_polls),
                                     REG(a1, uint32_t *completed),
                                     REG(a2, uint32_t *polls_run));
static int zz9k_lib_decode_image(REG(a6, struct ZZ9KBase *base),
                                 REG(d0, uint32_t opcode),
                                 REG(a0, const ZZ9KImageDecodeDesc *desc),
                                 REG(a1, ZZ9KImageDecodeResult *result));
static int zz9k_lib_decode_jpeg(REG(a6, struct ZZ9KBase *base),
                                REG(a0, const ZZ9KImageDecodeDesc *desc),
                                REG(a1, ZZ9KImageDecodeResult *result));
static int zz9k_lib_decode_png(REG(a6, struct ZZ9KBase *base),
                               REG(a0, const ZZ9KImageDecodeDesc *desc),
                               REG(a1, ZZ9KImageDecodeResult *result));
static int zz9k_lib_decode_gif(REG(a6, struct ZZ9KBase *base),
                               REG(a0, const ZZ9KImageDecodeDesc *desc),
                               REG(a1, ZZ9KImageDecodeResult *result));
static int zz9k_lib_decode_mp3(REG(a6, struct ZZ9KBase *base),
                               REG(a0, const ZZ9KAudioDecodeDesc *desc),
                               REG(a1, ZZ9KAudioDecodeResult *result));
static int zz9k_lib_audio_stream_begin(
    REG(a6, struct ZZ9KBase *base),
    REG(a0, const ZZ9KAudioStreamBeginDesc *desc),
    REG(a1, ZZ9KAudioStreamResult *result));
static int zz9k_lib_audio_stream_feed(
    REG(a6, struct ZZ9KBase *base),
    REG(a0, const ZZ9KAudioStreamFeedDesc *desc),
    REG(a1, ZZ9KAudioStreamResult *result));
static int zz9k_lib_audio_stream_read(REG(a6, struct ZZ9KBase *base),
                                      REG(d0, uint32_t session),
                                      REG(d1, uint32_t pcm_read),
                                      REG(d2, uint32_t flags),
                                      REG(a0, ZZ9KAudioStreamResult *result));
static int zz9k_lib_audio_stream_close(REG(a6, struct ZZ9KBase *base),
                                       REG(d0, uint32_t session),
                                       REG(d1, uint32_t flags),
                                       REG(a0, ZZ9KAudioStreamResult *result));
static int zz9k_lib_image_session_begin(
    REG(a6, struct ZZ9KBase *base),
    REG(a0, const ZZ9KImageSessionBeginDesc *desc),
    REG(a1, ZZ9KImageSessionResult *result));
static int zz9k_lib_image_session_feed(
    REG(a6, struct ZZ9KBase *base),
    REG(a0, const ZZ9KImageSessionFeedDesc *desc),
    REG(a1, ZZ9KImageSessionResult *result));
static int zz9k_lib_image_session_close(REG(a6, struct ZZ9KBase *base),
                                        REG(d0, uint32_t session),
                                        REG(d1, uint32_t flags));
static int zz9k_lib_crypto_hash(REG(a6, struct ZZ9KBase *base),
                                REG(a0, const ZZ9KCryptoHashDesc *desc),
                                REG(a1, ZZ9KCryptoResult *result));
static int zz9k_lib_crypto_hash_batch(REG(a6, struct ZZ9KBase *base),
                                      REG(a0, const ZZ9KCryptoHashDesc *descs),
                                      REG(a1, ZZ9KCryptoResult *results),
                                      REG(d0, uint32_t count),
                                      REG(d1, uint32_t max_in_flight),
                                      REG(d2, uint32_t timeout_ticks));
static int zz9k_lib_crypto_stream(REG(a6, struct ZZ9KBase *base),
                                  REG(a0, const ZZ9KCryptoStreamDesc *desc),
                                  REG(a1, ZZ9KCryptoResult *result));
static int zz9k_lib_crypto_stream_batch(
    REG(a6, struct ZZ9KBase *base),
    REG(a0, const ZZ9KCryptoStreamDesc *descs),
    REG(a1, ZZ9KCryptoResult *results),
    REG(d0, uint32_t count),
    REG(d1, uint32_t max_in_flight),
    REG(d2, uint32_t timeout_ticks));
static int zz9k_lib_crypto_aead(REG(a6, struct ZZ9KBase *base),
                                REG(a0, const ZZ9KCryptoAeadDesc *desc),
                                REG(a1, ZZ9KCryptoResult *result));
static int zz9k_lib_crypto_aead_batch(
    REG(a6, struct ZZ9KBase *base),
    REG(a0, const ZZ9KCryptoAeadDesc *descs),
    REG(a1, ZZ9KCryptoResult *results),
    REG(d0, uint32_t count),
    REG(d1, uint32_t max_in_flight),
    REG(d2, uint32_t timeout_ticks));
static int zz9k_lib_crypto_kx(REG(a6, struct ZZ9KBase *base),
                              REG(a0, const ZZ9KCryptoKxDesc *desc),
                              REG(a1, ZZ9KCryptoResult *result));
static int zz9k_lib_crypto_verify(REG(a6, struct ZZ9KBase *base),
                                  REG(a0, const ZZ9KCryptoVerifyDesc *desc),
                                  REG(a1, int *valid));
static int zz9k_lib_audio_stream_play(REG(a6, struct ZZ9KBase *base),
                                      REG(d0, uint32_t session),
                                      REG(d1, uint32_t flags),
                                      REG(a0, ZZ9KAudioStreamResult *result));
static int zz9k_lib_audio_stream_stop(REG(a6, struct ZZ9KBase *base),
                                      REG(d0, uint32_t session),
                                      REG(d1, uint32_t flags),
                                      REG(a0, ZZ9KAudioStreamResult *result));
static int zz9k_lib_query_palette(REG(a6, struct ZZ9KBase *base),
                                  REG(a0, const ZZ9KPaletteQueryDesc *desc));

static const APTR zz9k_lib_vectors[] = {
  (APTR)zz9k_lib_open,
  (APTR)zz9k_lib_close,
  (APTR)zz9k_lib_expunge,
  (APTR)zz9k_lib_null,
  (APTR)zz9k_lib_query_caps,
  (APTR)zz9k_lib_query_service,
  (APTR)zz9k_lib_ping,
  (APTR)zz9k_lib_call,
  (APTR)zz9k_lib_call_async,
  (APTR)zz9k_lib_call_async_batch,
  (APTR)zz9k_lib_poll,
  (APTR)zz9k_lib_alloc_shared,
  (APTR)zz9k_lib_free_shared,
  (APTR)zz9k_lib_mem_fill,
  (APTR)zz9k_lib_mem_copy,
  (APTR)zz9k_lib_alloc_surface,
  (APTR)zz9k_lib_alloc_surface_ex,
  (APTR)zz9k_lib_free_surface,
  (APTR)zz9k_lib_map_framebuffer_surface,
  (APTR)zz9k_lib_scale_image,
  (APTR)zz9k_lib_read_diag,
  (APTR)zz9k_lib_call_async_msg,
  (APTR)zz9k_lib_call_async_batch_msg,
  (APTR)zz9k_lib_cancel_async,
  (APTR)zz9k_lib_wait_async,
  (APTR)zz9k_lib_wait_async_batch,
  (APTR)zz9k_lib_decode_image,
  (APTR)zz9k_lib_crypto_hash,
  (APTR)zz9k_lib_crypto_hash_batch,
  (APTR)zz9k_lib_crypto_stream,
  (APTR)zz9k_lib_crypto_stream_batch,
  (APTR)zz9k_lib_crypto_aead,
  (APTR)zz9k_lib_crypto_aead_batch,
  (APTR)zz9k_lib_fill_surface,
  (APTR)zz9k_lib_copy_surface,
  (APTR)zz9k_lib_image_session_begin,
  (APTR)zz9k_lib_image_session_feed,
  (APTR)zz9k_lib_image_session_close,
  (APTR)zz9k_lib_scale_image_clipped,
  (APTR)zz9k_lib_decode_jpeg,
  (APTR)zz9k_lib_decode_png,
  (APTR)zz9k_lib_decode_gif,
  (APTR)zz9k_lib_decode_mp3,
  (APTR)zz9k_lib_audio_stream_begin,
  (APTR)zz9k_lib_audio_stream_feed,
  (APTR)zz9k_lib_audio_stream_read,
  (APTR)zz9k_lib_audio_stream_close,
  (APTR)zz9k_lib_crypto_kx,
  (APTR)zz9k_lib_crypto_verify,
  (APTR)zz9k_lib_audio_stream_play,
  (APTR)zz9k_lib_audio_stream_stop,
  (APTR)zz9k_lib_query_palette,
  (APTR)-1
};

static const struct Resident zz9k_romtag __attribute__((used)) = {
  RTC_MATCHWORD,
  (struct Resident *)&zz9k_romtag,
  (APTR)(&zz9k_romtag + 1),
  0,
  ZZ9K_LIBRARY_VERSION,
  NT_LIBRARY,
  0,
  (char *)ZZ9K_LIBRARY_NAME,
  (char *)ZZ9K_LIBRARY_ID_STRING,
  (APTR)zz9k_lib_init
};

static ULONG zz9k_lib_irq_isr(REG(a1, struct ZZ9KBase *base))
{
  uint16_t status;

  if (!base || !base->core.ctx) {
    return 0;
  }

  if (zz9k_interrupt_status(base->core.ctx, &status) != ZZ9K_STATUS_OK) {
    return 0;
  }
  if ((status & ZZ9K_INTERRUPT_SDK) == 0) {
    return 0;
  }

  base->irq_last_status = status;
  base->irq_hits++;
  zz9k_completion_irq_ack(base->core.ctx);

  if (base->irq_task && base->irq_signal_mask) {
    Signal(base->irq_task, base->irq_signal_mask);
  }

  return 1;
}

static int zz9k_lib_irq_install_locked(struct ZZ9KBase *base)
{
  if (!base || !base->core.ctx) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (!zz9k_completion_irq_supported(base->core.ctx)) {
    return ZZ9K_STATUS_UNSUPPORTED;
  }
  if (base->irq_installed) {
    return ZZ9K_STATUS_OK;
  }

  /* INT2 vs INT6: ENV:ZZ9K_INT2, then the ZZ9000.CFG `int2` key
   * (shared decision in zz9k_sdk_use_int2 so every SDK interrupt
   * consumer picks the same line). */
  base->irq_int_bit = zz9k_sdk_use_int2(base->core.ctx) ? INTB_PORTS
                                                        : INTB_EXTER;
  memset(&base->irq, 0, sizeof(base->irq));
  base->irq.is_Node.ln_Type = NT_INTERRUPT;
  base->irq.is_Node.ln_Pri = 0;
  base->irq.is_Node.ln_Name = (char *)"zz9k.library SDK IRQ";
  base->irq.is_Data = base;
  base->irq.is_Code = (void *)zz9k_lib_irq_isr;

  Forbid();
  AddIntServer(base->irq_int_bit, &base->irq);
  Permit();

  base->irq_installed = 1;
  return ZZ9K_STATUS_OK;
}

static void zz9k_lib_irq_disable_locked(struct ZZ9KBase *base)
{
  if (!base || !base->core.ctx || !base->irq_enabled) {
    return;
  }

  (void)zz9k_completion_irq_enable(base->core.ctx, 0);
  (void)zz9k_completion_irq_ack(base->core.ctx);
  base->irq_enabled = 0;
}

static void zz9k_lib_irq_remove_locked(struct ZZ9KBase *base)
{
  if (!base || !base->irq_installed) {
    return;
  }

  Forbid();
  RemIntServer(base->irq_int_bit, &base->irq);
  Permit();

  base->irq_installed = 0;
  base->irq_task = 0;
  base->irq_signal_mask = 0;
}

static void zz9k_lib_irq_stop_locked(struct ZZ9KBase *base)
{
  zz9k_lib_irq_disable_locked(base);
  zz9k_lib_irq_remove_locked(base);
}

static int zz9k_lib_prepare_irq_wait_locked(struct ZZ9KBase *base)
{
  BYTE signal_bit;
  int status;

  if (!base || !base->core.ctx) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (!zz9k_completion_irq_supported(base->core.ctx)) {
    return ZZ9K_STATUS_UNSUPPORTED;
  }

  signal_bit = AllocSignal(-1);
  if (signal_bit < 0) {
    return ZZ9K_STATUS_NO_MEMORY;
  }

  status = zz9k_lib_irq_install_locked(base);
  if (status != ZZ9K_STATUS_OK) {
    FreeSignal(signal_bit);
    return status;
  }

  base->irq_task = FindTask(0);
  base->irq_signal_bit = signal_bit;
  base->irq_signal_mask = 1UL << signal_bit;
  (void)SetSignal(0, base->irq_signal_mask);
  (void)zz9k_completion_irq_ack(base->core.ctx);

  status = zz9k_completion_irq_enable(base->core.ctx, 1);
  if (status != ZZ9K_STATUS_OK) {
    base->irq_task = 0;
    base->irq_signal_mask = 0;
    base->irq_signal_bit = -1;
    FreeSignal(signal_bit);
    return status;
  }

  base->irq_enabled = 1;
  return ZZ9K_STATUS_OK;
}

static void zz9k_lib_finish_irq_wait_locked(struct ZZ9KBase *base)
{
  BYTE signal_bit;

  if (!base) {
    return;
  }

  signal_bit = base->irq_signal_bit;
  zz9k_lib_irq_disable_locked(base);
  base->irq_task = 0;
  base->irq_signal_mask = 0;
  base->irq_signal_bit = -1;
  if (signal_bit >= 0) {
    FreeSignal(signal_bit);
  }
}

static int zz9k_lib_timer_wait_open(struct ZZ9KTimerWait *timer)
{
  if (!timer) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(timer, 0, sizeof(*timer));
  timer->port = CreateMsgPort();
  if (!timer->port) {
    return ZZ9K_STATUS_NO_MEMORY;
  }

  timer->request = (struct timerequest *)CreateIORequest(
      timer->port, sizeof(struct timerequest));
  if (!timer->request) {
    DeleteMsgPort(timer->port);
    memset(timer, 0, sizeof(*timer));
    return ZZ9K_STATUS_NO_MEMORY;
  }

  if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
                 (struct IORequest *)timer->request, 0) != 0) {
    DeleteIORequest((struct IORequest *)timer->request);
    DeleteMsgPort(timer->port);
    memset(timer, 0, sizeof(*timer));
    return ZZ9K_STATUS_UNSUPPORTED;
  }

  timer->device_open = 1;
  timer->signal_mask = 1UL << timer->port->mp_SigBit;
  return ZZ9K_STATUS_OK;
}

static void zz9k_lib_timer_wait_close(struct ZZ9KTimerWait *timer)
{
  if (!timer) {
    return;
  }

  if (timer->device_open) {
    CloseDevice((struct IORequest *)timer->request);
  }
  if (timer->request) {
    DeleteIORequest((struct IORequest *)timer->request);
  }
  if (timer->port) {
    DeleteMsgPort(timer->port);
  }
  memset(timer, 0, sizeof(*timer));
}

static int zz9k_lib_wait_for_irq_event(ZZ9KLibrary *library,
                                       void *user_data)
{
  struct ZZ9KBase *base;
  struct ZZ9KTimerWait timer;
  ULONG wait_mask;
  ULONG signals;
  int timer_ok;

  (void)library;
  base = (struct ZZ9KBase *)user_data;
  if (!base || !base->irq_signal_mask) {
    return ZZ9K_STATUS_OK;
  }

  timer_ok = zz9k_lib_timer_wait_open(&timer) == ZZ9K_STATUS_OK;
  wait_mask = base->irq_signal_mask | SIGBREAKF_CTRL_C;
  if (timer_ok) {
    timer.request->tr_node.io_Command = TR_ADDREQUEST;
    timer.request->tr_time.tv_secs = 0;
    timer.request->tr_time.tv_micro = ZZ9K_LIB_IRQ_WAIT_MICROS;
    SendIO((struct IORequest *)timer.request);
    wait_mask |= timer.signal_mask;
  }

  signals = Wait(wait_mask);

  if (timer_ok) {
    if (!CheckIO((struct IORequest *)timer.request)) {
      AbortIO((struct IORequest *)timer.request);
    }
    WaitIO((struct IORequest *)timer.request);
    zz9k_lib_timer_wait_close(&timer);
  }

  if (signals & SIGBREAKF_CTRL_C) {
    return ZZ9K_STATUS_CANCELLED;
  }

  return ZZ9K_STATUS_OK;
}

static void zz9k_lib_signal_dispatcher_start_locked(struct ZZ9KBase *base,
                                                    int status)
{
  base->dispatcher_start_status = status;
  if (base->dispatcher_start_task && base->dispatcher_start_signal_mask) {
    Signal(base->dispatcher_start_task, base->dispatcher_start_signal_mask);
  }
}

static void zz9k_lib_signal_dispatcher_stop(struct Task *task,
                                            ULONG signal_mask)
{
  if (task && signal_mask) {
    Signal(task, signal_mask);
  }
}

static void zz9k_lib_dispatcher_proc(void)
{
  struct Process *process;
  struct ZZ9KBase *base;
  struct Task *stop_task;
  BYTE signal_bit;
  ULONG signal_mask;
  ULONG stop_signal_mask;
  int status;

  process = (struct Process *)FindTask(0);
  base = (struct ZZ9KBase *)process->pr_Task.tc_UserData;
  signal_bit = AllocSignal(-1);
  if (!base || signal_bit < 0) {
    if (base) {
      ObtainSemaphore(&base->lock);
      base->dispatcher_process = 0;
      base->dispatcher_starting = 0;
      zz9k_lib_signal_dispatcher_start_locked(base,
          ZZ9K_STATUS_NO_MEMORY);
      ReleaseSemaphore(&base->lock);
    }
    return;
  }

  signal_mask = 1UL << signal_bit;
  ObtainSemaphore(&base->lock);
  if (base->dispatcher_stop_requested) {
    base->dispatcher_process = 0;
    base->dispatcher_starting = 0;
    zz9k_lib_signal_dispatcher_start_locked(base,
        ZZ9K_STATUS_CANCELLED);
    stop_task = base->dispatcher_stop_task;
    stop_signal_mask = base->dispatcher_stop_signal_mask;
    base->dispatcher_stop_task = 0;
    base->dispatcher_stop_signal_mask = 0;
    ReleaseSemaphore(&base->lock);
    zz9k_lib_signal_dispatcher_stop(stop_task, stop_signal_mask);
    FreeSignal(signal_bit);
    return;
  }

  base->dispatcher_task = (struct Task *)process;
  base->dispatcher_signal_bit = signal_bit;
  base->dispatcher_signal_mask = signal_mask;
  base->irq_task = (struct Task *)process;
  base->irq_signal_mask = signal_mask;
  status = zz9k_lib_irq_install_locked(base);
  if (status == ZZ9K_STATUS_OK) {
    (void)zz9k_completion_irq_ack(base->core.ctx);
    status = zz9k_completion_irq_enable(base->core.ctx, 1);
  }
  if (status != ZZ9K_STATUS_OK) {
    base->dispatcher_process = 0;
    base->dispatcher_task = 0;
    base->dispatcher_signal_bit = -1;
    base->dispatcher_signal_mask = 0;
    base->dispatcher_starting = 0;
    base->irq_task = 0;
    base->irq_signal_mask = 0;
    zz9k_lib_signal_dispatcher_start_locked(base, status);
    ReleaseSemaphore(&base->lock);
    FreeSignal(signal_bit);
    return;
  }

  base->irq_enabled = 1;
  base->dispatcher_running = 1;
  base->dispatcher_starting = 0;
  zz9k_lib_signal_dispatcher_start_locked(base, ZZ9K_STATUS_OK);
  ReleaseSemaphore(&base->lock);

  Signal((struct Task *)process, signal_mask);

  for (;;) {
    ULONG signals;

    signals = Wait(signal_mask | SIGBREAKF_CTRL_C);
    if (signals & SIGBREAKF_CTRL_C) {
      break;
    }

    for (;;) {
      uint32_t completed;

      completed = 0;
      ObtainSemaphore(&base->lock);
      if (base->dispatcher_stop_requested) {
        ReleaseSemaphore(&base->lock);
        signals = SIGBREAKF_CTRL_C;
        break;
      }
      status = ZZ9KPoll(&base->core, 16, &completed);
      ReleaseSemaphore(&base->lock);
      if (status != ZZ9K_STATUS_OK || completed == 0) {
        break;
      }
    }
    if (signals & SIGBREAKF_CTRL_C) {
      break;
    }
  }

  ObtainSemaphore(&base->lock);
  zz9k_lib_irq_disable_locked(base);
  zz9k_lib_irq_remove_locked(base);
  base->dispatcher_process = 0;
  base->dispatcher_task = 0;
  base->dispatcher_signal_bit = -1;
  base->dispatcher_signal_mask = 0;
  base->dispatcher_running = 0;
  base->dispatcher_starting = 0;
  base->dispatcher_stop_requested = 0;
  stop_task = base->dispatcher_stop_task;
  stop_signal_mask = base->dispatcher_stop_signal_mask;
  base->dispatcher_stop_task = 0;
  base->dispatcher_stop_signal_mask = 0;
  ReleaseSemaphore(&base->lock);

  zz9k_lib_signal_dispatcher_stop(stop_task, stop_signal_mask);
  FreeSignal(signal_bit);
}

static int zz9k_lib_dispatcher_ensure(struct ZZ9KBase *base)
{
  struct Process *process;
  BYTE signal_bit;
  ULONG signal_mask;
  int status;

  if (!base) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  signal_bit = AllocSignal(-1);
  if (signal_bit < 0) {
    return ZZ9K_STATUS_NO_MEMORY;
  }
  signal_mask = 1UL << signal_bit;

  ObtainSemaphore(&base->lock);
  if (base->dispatcher_process || base->dispatcher_running ||
      base->dispatcher_starting) {
    ReleaseSemaphore(&base->lock);
    FreeSignal(signal_bit);
    return ZZ9K_STATUS_OK;
  }

  if (!base->core.ctx || !zz9k_completion_irq_supported(base->core.ctx)) {
    ReleaseSemaphore(&base->lock);
    FreeSignal(signal_bit);
    return ZZ9K_STATUS_UNSUPPORTED;
  }

  base->dispatcher_start_task = FindTask(0);
  base->dispatcher_start_signal_mask = signal_mask;
  base->dispatcher_start_status = ZZ9K_STATUS_BUSY;
  base->dispatcher_starting = 1;
  base->dispatcher_stop_requested = 0;

  Forbid();
  process = CreateNewProcTags(NP_Entry, (ULONG)zz9k_lib_dispatcher_proc,
                              NP_Name,
                              (ULONG)"zz9k.library dispatcher",
                              NP_Priority, 0, TAG_DONE);
  if (process) {
    process->pr_Task.tc_UserData = base;
  }
  Permit();

  if (!process) {
    base->dispatcher_start_task = 0;
    base->dispatcher_start_signal_mask = 0;
    base->dispatcher_start_status = ZZ9K_STATUS_NO_MEMORY;
    base->dispatcher_starting = 0;
    ReleaseSemaphore(&base->lock);
    FreeSignal(signal_bit);
    return ZZ9K_STATUS_NO_MEMORY;
  }

  base->dispatcher_process = process;
  ReleaseSemaphore(&base->lock);

  Wait(signal_mask);

  ObtainSemaphore(&base->lock);
  status = base->dispatcher_start_status;
  base->dispatcher_start_task = 0;
  base->dispatcher_start_signal_mask = 0;
  if (status != ZZ9K_STATUS_OK && !base->dispatcher_running) {
    base->dispatcher_process = 0;
  }
  ReleaseSemaphore(&base->lock);

  FreeSignal(signal_bit);
  return status;
}

static void zz9k_lib_dispatcher_stop(struct ZZ9KBase *base)
{
  BYTE signal_bit;
  ULONG signal_mask;
  int should_wait;

  if (!base) {
    return;
  }

  signal_bit = AllocSignal(-1);
  signal_mask = signal_bit >= 0 ? (1UL << signal_bit) : 0;
  should_wait = 0;

  ObtainSemaphore(&base->lock);
  if (base->dispatcher_process || base->dispatcher_running ||
      base->dispatcher_starting) {
    base->dispatcher_stop_requested = 1;
    if (signal_bit >= 0) {
      base->dispatcher_stop_task = FindTask(0);
      base->dispatcher_stop_signal_mask = signal_mask;
      should_wait = 1;
    }
    if (base->dispatcher_task) {
      Signal(base->dispatcher_task, SIGBREAKF_CTRL_C);
    }
  }
  ReleaseSemaphore(&base->lock);

  if (should_wait) {
    Wait(signal_mask);
  }
  if (signal_bit >= 0) {
    FreeSignal(signal_bit);
  }
}

static int zz9k_lib_open_core_locked(struct ZZ9KBase *base)
{
  int status;

  if (base->core.ctx) {
    return ZZ9K_STATUS_OK;
  }

  ZZ9KInit(&base->core);
  status = ZZ9KOpen(&base->core);
  base->last_open_status = status;
  return status;
}

static int zz9k_lib_enter(struct ZZ9KBase *base)
{
  int status;

  if (!base) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  ObtainSemaphore(&base->lock);
  status = zz9k_lib_open_core_locked(base);
  if (status != ZZ9K_STATUS_OK) {
    ReleaseSemaphore(&base->lock);
  }

  return status;
}

static void zz9k_lib_leave(struct ZZ9KBase *base)
{
  if (base) {
    ReleaseSemaphore(&base->lock);
  }
}

static struct ZZ9KBase *zz9k_lib_open(REG(a6, struct ZZ9KBase *base))
{
  if (!base) {
    return 0;
  }

  base->library.lib_OpenCnt++;
  base->library.lib_Flags &= (uint8_t)~LIBF_DELEXP;
  return base;
}

static BPTR zz9k_lib_close(REG(a6, struct ZZ9KBase *base))
{
  BPTR segment;

  if (!base || base->library.lib_OpenCnt == 0) {
    return 0;
  }

  base->library.lib_OpenCnt--;
  if (base->library.lib_OpenCnt == 0) {
    zz9k_lib_dispatcher_stop(base);
    ObtainSemaphore(&base->lock);
    zz9k_lib_irq_stop_locked(base);
    ZZ9KClose(&base->core);
    ReleaseSemaphore(&base->lock);
    if (base->library.lib_Flags & LIBF_DELEXP) {
      segment = zz9k_lib_expunge(base);
      return segment;
    }
  }

  return 0;
}

static BPTR zz9k_lib_expunge(REG(a6, struct ZZ9KBase *base))
{
  BPTR segment;

  if (!base) {
    return 0;
  }
  if (base->library.lib_OpenCnt != 0) {
    base->library.lib_Flags |= LIBF_DELEXP;
    return 0;
  }

  Remove((struct Node *)base);
  if (DOSBase) {
    CloseLibrary((struct Library *)DOSBase);
    DOSBase = 0;
  }
  segment = base->segment;
  FreeMem((uint8_t *)base - base->library.lib_NegSize,
          base->library.lib_NegSize + base->library.lib_PosSize);
  return segment;
}

static ULONG zz9k_lib_null(void)
{
  return 0;
}

static struct ZZ9KBase *zz9k_lib_init(REG(a0, BPTR segment))
{
  struct ZZ9KBase *base;

  SysBase = *(struct ExecBase **)4;
  DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library",
                                             36);
  if (!DOSBase) {
    return 0;
  }

  base = (struct ZZ9KBase *)MakeLibrary(
      (CONST_APTR)zz9k_lib_vectors, 0, 0, sizeof(*base), 0);
  if (!base) {
    CloseLibrary((struct Library *)DOSBase);
    DOSBase = 0;
    return 0;
  }

  base->library.lib_Node.ln_Type = NT_LIBRARY;
  base->library.lib_Node.ln_Name = (char *)ZZ9K_LIBRARY_NAME;
  base->library.lib_Flags = LIBF_CHANGED | LIBF_SUMUSED;
  base->library.lib_Version = ZZ9K_LIBRARY_VERSION;
  base->library.lib_Revision = ZZ9K_LIBRARY_REVISION;
  base->library.lib_IdString = (APTR)ZZ9K_LIBRARY_ID_STRING;
  base->segment = segment;
  base->last_open_status = ZZ9K_STATUS_OK;
  base->irq_signal_bit = -1;
  base->dispatcher_signal_bit = -1;
  InitSemaphore(&base->lock);
  ZZ9KInit(&base->core);

  AddLibrary((struct Library *)base);
  return base;
}

static int zz9k_lib_query_caps(REG(a6, struct ZZ9KBase *base),
                               REG(a0, ZZ9KCaps *caps))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KQueryCaps(&base->core, caps);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_query_service(REG(a6, struct ZZ9KBase *base),
                                  REG(d0, uint32_t service_id),
                                  REG(a0, ZZ9KServiceInfo *service))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KQueryService(&base->core, service_id, service);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_ping(REG(a6, struct ZZ9KBase *base),
                         REG(a0, const uint8_t *payload),
                         REG(d0, uint32_t payload_len),
                         REG(a1, uint8_t *reply_payload),
                         REG(a2, uint32_t *reply_len))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KPing(&base->core, payload, payload_len, reply_payload,
                    reply_len);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_call(REG(a6, struct ZZ9KBase *base),
                         REG(a0, ZZ9KRequest *request),
                         REG(a1, ZZ9KMailboxEntry *reply),
                         REG(d0, uint32_t timeout_ticks))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KCall(&base->core, request, reply, timeout_ticks);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_call_async(REG(a6, struct ZZ9KBase *base),
                               REG(a0, ZZ9KAsyncRequest *async),
                               REG(a1, const ZZ9KRequest *request),
                               REG(a2, ZZ9KAsyncCallback callback),
                               REG(a3, void *user_data))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KCallAsync(&base->core, async, request, callback, user_data);
  zz9k_lib_leave(base);
  if (status == ZZ9K_STATUS_QUEUED) {
    (void)zz9k_lib_dispatcher_ensure(base);
  }
  return status;
}

static int zz9k_lib_call_async_batch(REG(a6, struct ZZ9KBase *base),
                                     REG(a0, ZZ9KAsyncRequest *asyncs),
                                     REG(a1, const ZZ9KRequest *requests),
                                     REG(d0, uint32_t count),
                                     REG(a2, ZZ9KAsyncCallback callback),
                                     REG(a3, void *user_data),
                                     REG(a4, uint32_t *queued))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KCallAsyncBatch(&base->core, asyncs, requests, count,
                              callback, user_data, queued);
  zz9k_lib_leave(base);
  if (status == ZZ9K_STATUS_QUEUED) {
    (void)zz9k_lib_dispatcher_ensure(base);
  }
  return status;
}

static int zz9k_lib_poll(REG(a6, struct ZZ9KBase *base),
                         REG(d0, uint32_t max_completions),
                         REG(a0, uint32_t *completed))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KPoll(&base->core, max_completions, completed);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_alloc_shared(REG(a6, struct ZZ9KBase *base),
                                 REG(d0, uint32_t length),
                                 REG(d1, uint32_t alignment),
                                 REG(d2, uint32_t flags),
                                 REG(a0, ZZ9KSharedBuffer *buffer))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KAllocShared(&base->core, length, alignment, flags, buffer);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_free_shared(REG(a6, struct ZZ9KBase *base),
                                REG(d0, uint32_t handle))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KFreeShared(&base->core, handle);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_mem_fill(REG(a6, struct ZZ9KBase *base),
                             REG(d0, uint32_t handle),
                             REG(d1, uint32_t offset),
                             REG(d2, uint32_t length),
                             REG(d3, uint8_t value))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KMemFill(&base->core, handle, offset, length, value);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_mem_copy(REG(a6, struct ZZ9KBase *base),
                             REG(d0, uint32_t dst_handle),
                             REG(d1, uint32_t dst_offset),
                             REG(d2, uint32_t src_handle),
                             REG(d3, uint32_t src_offset),
                             REG(d4, uint32_t length))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KMemCopy(&base->core, dst_handle, dst_offset, src_handle,
                       src_offset, length);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_alloc_surface(REG(a6, struct ZZ9KBase *base),
                                  REG(d0, uint32_t width),
                                  REG(d1, uint32_t height),
                                  REG(d2, uint32_t format),
                                  REG(d3, uint32_t flags),
                                  REG(a0, ZZ9KSurface *surface))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KAllocSurface(&base->core, width, height, format, flags,
                            surface);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_alloc_surface_ex(REG(a6, struct ZZ9KBase *base),
                                     REG(d0, uint32_t width),
                                     REG(d1, uint32_t height),
                                     REG(d2, uint32_t format),
                                     REG(d3, uint32_t flags),
                                     REG(d4, uint32_t pitch),
                                     REG(a0, ZZ9KSurface *surface))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KAllocSurfaceEx(&base->core, width, height, format, flags,
                              pitch, surface);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_free_surface(REG(a6, struct ZZ9KBase *base),
                                 REG(d0, uint32_t handle))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KFreeSurface(&base->core, handle);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_map_framebuffer_surface(REG(a6,
                                                struct ZZ9KBase *base),
                                            REG(a0, ZZ9KSurface *surface))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KMapFramebufferSurface(&base->core, surface);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_scale_image(REG(a6, struct ZZ9KBase *base),
                                REG(a0, const ZZ9KScaleImageDesc *desc))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KScaleImage(&base->core, desc);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_scale_image_clipped(
    REG(a6, struct ZZ9KBase *base),
    REG(a0, const ZZ9KScaleImageClippedDesc *desc))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KScaleImageClipped(&base->core, desc);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_fill_surface(REG(a6, struct ZZ9KBase *base),
                                 REG(a0, const ZZ9KSurfaceFillDesc *desc))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KFillSurface(&base->core, desc);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_copy_surface(REG(a6, struct ZZ9KBase *base),
                                 REG(a0, const ZZ9KSurfaceCopyDesc *desc))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KCopySurface(&base->core, desc);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_read_diag(REG(a6, struct ZZ9KBase *base),
                              REG(a0, ZZ9KDiagInfo *diag))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KReadDiag(&base->core, diag);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_call_async_msg(REG(a6, struct ZZ9KBase *base),
                                   REG(a0, ZZ9KAsyncRequest *async),
                                   REG(a1, const ZZ9KRequest *request),
                                   REG(a2, struct MsgPort *reply_port))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KCallAsyncMsg(&base->core, async, request, reply_port);
  zz9k_lib_leave(base);
  if (status == ZZ9K_STATUS_QUEUED) {
    (void)zz9k_lib_dispatcher_ensure(base);
  }
  return status;
}

static int zz9k_lib_call_async_batch_msg(REG(a6, struct ZZ9KBase *base),
                                         REG(a0, ZZ9KAsyncRequest *asyncs),
                                         REG(a1, const ZZ9KRequest *requests),
                                         REG(d0, uint32_t count),
                                         REG(a2, struct MsgPort *reply_port),
                                         REG(a3, uint32_t *queued))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KCallAsyncBatchMsg(&base->core, asyncs, requests, count,
                                 reply_port, queued);
  zz9k_lib_leave(base);
  if (status == ZZ9K_STATUS_QUEUED) {
    (void)zz9k_lib_dispatcher_ensure(base);
  }
  return status;
}

static int zz9k_lib_cancel_async(REG(a6, struct ZZ9KBase *base),
                                 REG(a0, ZZ9KAsyncRequest *async))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KCancelAsync(&base->core, async);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_wait_async(REG(a6, struct ZZ9KBase *base),
                               REG(a0, ZZ9KAsyncRequest *async),
                               REG(d0, uint32_t timeout_polls),
                               REG(a1, uint32_t *polls_run))
{
  int irq_wait;
  int status;

  if (polls_run) {
    *polls_run = 0;
  }

  status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  irq_wait = !base->dispatcher_running && !base->dispatcher_starting &&
             zz9k_lib_prepare_irq_wait_locked(base) == ZZ9K_STATUS_OK;
  if (irq_wait) {
    (void)ZZ9KSetEventWaiter(&base->core, zz9k_lib_wait_for_irq_event,
                             base);
  }

  status = ZZ9KWaitAsync(&base->core, async, timeout_polls, polls_run);

  if (irq_wait) {
    (void)ZZ9KSetEventWaiter(&base->core, 0, 0);
    zz9k_lib_finish_irq_wait_locked(base);
  }

  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_wait_async_batch(REG(a6, struct ZZ9KBase *base),
                                     REG(a0, ZZ9KAsyncRequest *asyncs),
                                     REG(d0, uint32_t count),
                                     REG(d1, uint32_t timeout_polls),
                                     REG(a1, uint32_t *completed),
                                     REG(a2, uint32_t *polls_run))
{
  int irq_wait;
  int status;

  if (completed) {
    *completed = 0;
  }
  if (polls_run) {
    *polls_run = 0;
  }

  status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  irq_wait = !base->dispatcher_running && !base->dispatcher_starting &&
             zz9k_lib_prepare_irq_wait_locked(base) == ZZ9K_STATUS_OK;
  if (irq_wait) {
    (void)ZZ9KSetEventWaiter(&base->core, zz9k_lib_wait_for_irq_event,
                             base);
  }

  status = ZZ9KWaitAsyncBatch(&base->core, asyncs, count, timeout_polls,
                              completed, polls_run);

  if (irq_wait) {
    (void)ZZ9KSetEventWaiter(&base->core, 0, 0);
    zz9k_lib_finish_irq_wait_locked(base);
  }

  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_decode_image(REG(a6, struct ZZ9KBase *base),
                                 REG(d0, uint32_t opcode),
                                 REG(a0, const ZZ9KImageDecodeDesc *desc),
                                 REG(a1, ZZ9KImageDecodeResult *result))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KDecodeImage(&base->core, opcode, desc, result);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_decode_jpeg(REG(a6, struct ZZ9KBase *base),
                                REG(a0, const ZZ9KImageDecodeDesc *desc),
                                REG(a1, ZZ9KImageDecodeResult *result))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KDecodeJpeg(&base->core, desc, result);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_decode_png(REG(a6, struct ZZ9KBase *base),
                               REG(a0, const ZZ9KImageDecodeDesc *desc),
                               REG(a1, ZZ9KImageDecodeResult *result))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KDecodePng(&base->core, desc, result);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_decode_gif(REG(a6, struct ZZ9KBase *base),
                               REG(a0, const ZZ9KImageDecodeDesc *desc),
                               REG(a1, ZZ9KImageDecodeResult *result))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KDecodeGif(&base->core, desc, result);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_decode_mp3(REG(a6, struct ZZ9KBase *base),
                               REG(a0, const ZZ9KAudioDecodeDesc *desc),
                               REG(a1, ZZ9KAudioDecodeResult *result))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KDecodeMp3(&base->core, desc, result);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_audio_stream_begin(
    REG(a6, struct ZZ9KBase *base),
    REG(a0, const ZZ9KAudioStreamBeginDesc *desc),
    REG(a1, ZZ9KAudioStreamResult *result))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KAudioStreamBegin(&base->core, desc, result);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_audio_stream_feed(
    REG(a6, struct ZZ9KBase *base),
    REG(a0, const ZZ9KAudioStreamFeedDesc *desc),
    REG(a1, ZZ9KAudioStreamResult *result))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KAudioStreamFeed(&base->core, desc, result);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_audio_stream_read(REG(a6, struct ZZ9KBase *base),
                                      REG(d0, uint32_t session),
                                      REG(d1, uint32_t pcm_read),
                                      REG(d2, uint32_t flags),
                                      REG(a0, ZZ9KAudioStreamResult *result))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KAudioStreamRead(&base->core, session, pcm_read, flags, result);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_audio_stream_close(REG(a6, struct ZZ9KBase *base),
                                       REG(d0, uint32_t session),
                                       REG(d1, uint32_t flags),
                                       REG(a0, ZZ9KAudioStreamResult *result))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KAudioStreamClose(&base->core, session, flags, result);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_image_session_begin(
    REG(a6, struct ZZ9KBase *base),
    REG(a0, const ZZ9KImageSessionBeginDesc *desc),
    REG(a1, ZZ9KImageSessionResult *result))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KImageSessionBegin(&base->core, desc, result);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_image_session_feed(
    REG(a6, struct ZZ9KBase *base),
    REG(a0, const ZZ9KImageSessionFeedDesc *desc),
    REG(a1, ZZ9KImageSessionResult *result))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KImageSessionFeed(&base->core, desc, result);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_image_session_close(REG(a6, struct ZZ9KBase *base),
                                        REG(d0, uint32_t session),
                                        REG(d1, uint32_t flags))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KImageSessionClose(&base->core, session, flags);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_crypto_hash(REG(a6, struct ZZ9KBase *base),
                                REG(a0, const ZZ9KCryptoHashDesc *desc),
                                REG(a1, ZZ9KCryptoResult *result))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KCryptoHash(&base->core, desc, result);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_crypto_hash_batch(REG(a6, struct ZZ9KBase *base),
                                      REG(a0, const ZZ9KCryptoHashDesc *descs),
                                      REG(a1, ZZ9KCryptoResult *results),
                                      REG(d0, uint32_t count),
                                      REG(d1, uint32_t max_in_flight),
                                      REG(d2, uint32_t timeout_ticks))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KCryptoHashBatch(&base->core, descs, results, count,
                               max_in_flight, timeout_ticks);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_crypto_stream(REG(a6, struct ZZ9KBase *base),
                                  REG(a0, const ZZ9KCryptoStreamDesc *desc),
                                  REG(a1, ZZ9KCryptoResult *result))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KCryptoStream(&base->core, desc, result);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_crypto_stream_batch(
    REG(a6, struct ZZ9KBase *base),
    REG(a0, const ZZ9KCryptoStreamDesc *descs),
    REG(a1, ZZ9KCryptoResult *results),
    REG(d0, uint32_t count),
    REG(d1, uint32_t max_in_flight),
    REG(d2, uint32_t timeout_ticks))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KCryptoStreamBatch(&base->core, descs, results, count,
                                 max_in_flight, timeout_ticks);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_crypto_aead(REG(a6, struct ZZ9KBase *base),
                                REG(a0, const ZZ9KCryptoAeadDesc *desc),
                                REG(a1, ZZ9KCryptoResult *result))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KCryptoAead(&base->core, desc, result);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_crypto_aead_batch(
    REG(a6, struct ZZ9KBase *base),
    REG(a0, const ZZ9KCryptoAeadDesc *descs),
    REG(a1, ZZ9KCryptoResult *results),
    REG(d0, uint32_t count),
    REG(d1, uint32_t max_in_flight),
    REG(d2, uint32_t timeout_ticks))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KCryptoAeadBatch(&base->core, descs, results, count,
                               max_in_flight, timeout_ticks);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_crypto_kx(REG(a6, struct ZZ9KBase *base),
                              REG(a0, const ZZ9KCryptoKxDesc *desc),
                              REG(a1, ZZ9KCryptoResult *result))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KCryptoKeyExchange(&base->core, desc, result);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_crypto_verify(REG(a6, struct ZZ9KBase *base),
                                  REG(a0, const ZZ9KCryptoVerifyDesc *desc),
                                  REG(a1, int *valid))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KCryptoVerify(&base->core, desc, valid);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_audio_stream_play(REG(a6, struct ZZ9KBase *base),
                                      REG(d0, uint32_t session),
                                      REG(d1, uint32_t flags),
                                      REG(a0, ZZ9KAudioStreamResult *result))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KAudioStreamPlay(&base->core, session, flags, result);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_audio_stream_stop(REG(a6, struct ZZ9KBase *base),
                                      REG(d0, uint32_t session),
                                      REG(d1, uint32_t flags),
                                      REG(a0, ZZ9KAudioStreamResult *result))
{
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KAudioStreamStop(&base->core, session, flags, result);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_query_palette(REG(a6, struct ZZ9KBase *base),
                                  REG(a0, const ZZ9KPaletteQueryDesc *desc))
{
  int status = zz9k_lib_enter(base);

  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = ZZ9KQueryPalette(&base->core, desc);
  zz9k_lib_leave(base);
  return status;
}
