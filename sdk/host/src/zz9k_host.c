/*
 * ZZ9000 SDK v2 Amiga-side host API skeleton.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/host.h"
#if !defined(__amigaos__) && !defined(__amiga__) && !defined(__AMIGA__) && \
    !defined(__VBCC__)
#include "zz9k/library.h"
#endif
#include "zz9k/library_vectors.h"
#include "zz9k/caps.h"
#include "zz9k/reply.h"
#include "zz9k/request.h"
#include "zz9k/shared.h"
#include <errno.h>
#include <string.h>

#if defined(__amigaos__) || defined(__amiga__) || defined(__AMIGA__) || \
    defined(__VBCC__)
#define ZZ9K_HOST_AMIGA 1
#else
#define ZZ9K_HOST_AMIGA 0
#endif

#if ZZ9K_HOST_AMIGA && !defined(ZZ9K_HOST_LIBRARY_BUILD)
#include <proto/zz9k.h>
#endif

#if !defined(ZZ9K_HOST_LIBRARY_BUILD)
typedef struct ZZ9KLibraryTransport {
  int (*open)(void *user, ZZ9KBoard *board, void **base,
              uint16_t *version, uint16_t *revision);
  void (*close)(void *base);
  int (*call)(void *base, ZZ9KRequest *request, ZZ9KMailboxEntry *reply,
              uint32_t timeout_ticks);
  int (*call_async)(void *base, ZZ9KAsyncRequest *async,
                    const ZZ9KRequest *request,
                    ZZ9KAsyncCallback callback, void *callback_user_data);
  int (*poll)(void *base, uint32_t max_completions, uint32_t *completed);
  int (*cancel)(void *base, ZZ9KAsyncRequest *async);
  void *user;
} ZZ9KLibraryTransport;
#endif

#if !ZZ9K_HOST_AMIGA
static const ZZ9KLibraryTransport *zz9k_library_transport_for_test;

void zz9k_set_library_transport_for_test(
    const ZZ9KLibraryTransport *transport)
{
  zz9k_library_transport_for_test = transport;
}
#endif

#if ZZ9K_HOST_AMIGA
struct Library *ZZ9KBase;
#endif

#if ZZ9K_HOST_AMIGA
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <libraries/configvars.h>
#include <libraries/expansion.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <exec/interrupts.h>
#include <exec/tasks.h>
#include <exec/ports.h>
#include <hardware/intbits.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <proto/dos.h>
#include <stdlib.h>
#else
#include <stdlib.h>
#endif

#if ZZ9K_HOST_AMIGA
/* FindConfigDev() dispatches through this global library base. Standalone
 * tools normally receive a definition from libnix's auto-open stubs, but a
 * resident library linked with -nostdlib (e.g. amissl.library carrying the
 * provider) has none, so the definition lives here. zz9k_find_board() opens
 * expansion.library itself and save/restores this base around the call. */
struct ExpansionBase *ExpansionBase;
#endif

#if ZZ9K_HOST_AMIGA
/* GetVar() dispatches through this global library base. Compiled -fcommon,
 * this tentative definition merges with (and yields to) whatever DOSBase a
 * given link target already provides -- libnix startup for normal tools,
 * clib2 for the amissl os3 build, or the resident library's own copy -- and
 * supplies the symbol for any consumer that provides none.
 * zz9k_sync_wait_timeout_ms() opens dos.library itself and save/restores this
 * base around the call. */
struct DosLibrary *DOSBase;
#endif
void zz9k_audio_ring_cache_flush(const volatile void *address, uint32_t length)
{
#if ZZ9K_HOST_AMIGA
  if (address && length != 0U) {
    CacheClearE((APTR)(uintptr_t)address, (ULONG)length, CACRF_ClearD);
  }
#else
  (void)address;
  (void)length;
#endif
}

void zz9k_audio_ring_cache_invalidate(const volatile void *address,
                                      uint32_t length)
{
#if ZZ9K_HOST_AMIGA
  if (address && length != 0U) {
    /* The firmware-owned line is never written by the host, so clearing its
     * clean CPU cache lines is an invalidate without a stale writeback. */
    CacheClearE((APTR)(uintptr_t)address, (ULONG)length, CACRF_ClearD);
  }
#else
  (void)address;
  (void)length;
#endif
}



#define ZZ9K_SYNC_COOKIE_MASK 0x5aa55aa5UL
#define ZZ9K_MAILBOX_SEMAPHORE_NAME "zz9000.sdk.mailbox"

#if ZZ9K_HOST_AMIGA
typedef struct ZZ9KMailboxSemaphore {
  struct SignalSemaphore semaphore;
  char name[sizeof(ZZ9K_MAILBOX_SEMAPHORE_NAME)];
} ZZ9KMailboxSemaphore;
#endif
#if !defined(ZZ9K_HOST_LIBRARY_BUILD)
typedef struct ZZ9KStaticLibraryPending {
  ZZ9KAsyncRequest async;
  struct ZZ9KStaticLibraryPending *next;
} ZZ9KStaticLibraryPending;
#endif

struct ZZ9KContext {
  ZZ9KBoard board;
  volatile ZZ9KMailboxDescriptor *mailbox;
  volatile ZZ9KMailboxWireEntry *request_ring;
  volatile ZZ9KMailboxWireEntry *completion_ring;
  volatile uint16_t *doorbell;
  volatile uint16_t *irq_ack;
  volatile uint16_t *sdk_irq_control;
  uint32_t capability_bits;
  ZZ9KApertureLayout aperture_layout;
  uint32_t request_ring_entries;
  uint32_t completion_ring_entries;
  uint32_t next_request_id;
  uint32_t sync_cookie_mask;
  uint32_t offload_timeout_ms;
  uint32_t sync_wait_timeout_ms;   /* armed-wait ENV bound, read once; 0 = unread */
  uint32_t wait_heartbeats;        /* coarse clock when timer.device is unusable */
  uint16_t client_id;
  unsigned char irq_armed;
  unsigned char late_irq_expected; /* cancelled op still in flight */
  unsigned char aperture_layout_valid;
  unsigned char library_transport;
#if !defined(ZZ9K_HOST_LIBRARY_BUILD)
  const ZZ9KLibraryTransport *library_transport_ops;
  void *library_base;
  ZZ9KStaticLibraryPending *library_pending;
#endif
  ZZ9KCompletionDispatcher completion_dispatcher;
  void *completion_dispatcher_user_data;
#if ZZ9K_HOST_AMIGA
  struct Interrupt irq;
  struct Task *irq_task;
  unsigned long irq_signal_mask;
  int irq_signal_bit;
  int irq_int_bit;
  struct MsgPort *timer_port;
  struct timerequest *timer_request;
  int timer_open;
#endif
};

static int zz9k_library_version_supported(uint16_t version,
                                          uint16_t revision)
{
  /* 36 routes foreign completions; 34 and 35 drop them. */
  return version == ZZ9K_LIBRARY_VERSION &&
         revision >= ZZ9K_LIBRARY_MIN_REVISION_STATIC_PROXY;
}

static int zz9k_board_range_fits(uint32_t board_size,
                                 uint32_t board_offset, uint32_t length);

/* The direct-ring delivery carves a fixed 48-KiB Zorro II direct
 * region out of each profile's host-window heap (KTD2): generation 2
 * shrinks every heap by this amount and leaves the gap between the
 * heap's new end and the audio region as firmware-reserved,
 * unreported space the ring grants live in. */
#define ZZ9K_Z2_DIRECT_REGION_CARVE 0x0000c000UL

static int zz9k_expected_aperture_layout_gen(uint32_t aperture_size,
                                             uint32_t generation,
                                             ZZ9KApertureLayout *layout)
{
  uint32_t flags;

  if (!layout || generation < ZZ9K_APERTURE_LAYOUT_GENERATION_1 ||
      generation > ZZ9K_APERTURE_LAYOUT_GENERATION_2) {
    return 0;
  }
  memset(layout, 0, sizeof(*layout));
  flags = ZZ9K_APERTURE_FLAG_VALID | ZZ9K_APERTURE_FLAG_HOST_WINDOW;
  layout->aperture_size = aperture_size;
  layout->framebuffer_base = 0x00010000UL;
  layout->template_size = 0x00010000UL;
  layout->audio_size = 0x00010000UL;

  switch (aperture_size) {
  case 0x00200000UL:
    layout->framebuffer_size = 0x001c0000UL;
    layout->template_base = 0x001d0000UL;
    layout->host_base = 0x001e0000UL;
    layout->host_size = 0x00010000UL;
    layout->audio_base = 0x001f0000UL;
    break;
  case 0x00400000UL:
    flags |= ZZ9K_APERTURE_FLAG_PIP;
    layout->framebuffer_size = 0x00388000UL;
    layout->pip_base = 0x00398000UL;
    layout->pip_size = 0x00038000UL;
    layout->template_base = 0x003d0000UL;
    layout->host_base = 0x003e0000UL;
    layout->host_size = 0x00010000UL;
    layout->audio_base = 0x003f0000UL;
    break;
  case 0x00800000UL:
    flags |= ZZ9K_APERTURE_FLAG_PIP;
    layout->framebuffer_size = 0x00770000UL;
    layout->pip_base = 0x00780000UL;
    layout->pip_size = 0x00040000UL;
    layout->template_base = 0x007c0000UL;
    layout->host_base = 0x007d0000UL;
    layout->host_size = 0x00020000UL;
    layout->audio_base = 0x007f0000UL;
    break;
  default:
    memset(layout, 0, sizeof(*layout));
    return 0;
  }

  if (generation >= ZZ9K_APERTURE_LAYOUT_GENERATION_2) {
    layout->host_size -= ZZ9K_Z2_DIRECT_REGION_CARVE;
  }
  layout->profile = ZZ9K_APERTURE_PROFILE(generation, flags);
  return 1;
}

static int zz9k_expected_aperture_layout(uint32_t aperture_size,
                                         ZZ9KApertureLayout *layout)
{
  return zz9k_expected_aperture_layout_gen(
      aperture_size, ZZ9K_APERTURE_LAYOUT_GENERATION_1, layout);
}

static int zz9k_aperture_layout_matches(const ZZ9KBoard *board,
                                        const ZZ9KApertureLayout *layout)
{
  ZZ9KApertureLayout expected;
  uint32_t generation;

  if (!board || !layout || board->zorro_version != 2U ||
      board->board_size == 0U ||
      layout->aperture_size != board->board_size) {
    return 0;
  }
  /* Both layout generations are live on the wire (generation 2 is
   * the direct-ring carve; generation 1 is every firmware before
   * it): the reported generation picks which heap profile to
   * expect. */
  generation = (layout->profile & ZZ9K_APERTURE_LAYOUT_GENERATION_MASK) >>
               ZZ9K_APERTURE_LAYOUT_GENERATION_SHIFT;
  if (!zz9k_expected_aperture_layout_gen(board->board_size, generation,
                                         &expected)) {
    return 0;
  }
  if (!zz9k_board_range_fits(layout->aperture_size,
                             layout->framebuffer_base,
                             layout->framebuffer_size) ||
      !zz9k_board_range_fits(layout->aperture_size,
                             layout->template_base,
                             layout->template_size) ||
      !zz9k_board_range_fits(layout->aperture_size,
                             layout->host_base,
                             layout->host_size) ||
      !zz9k_board_range_fits(layout->aperture_size,
                             layout->audio_base,
                             layout->audio_size) ||
      ((layout->pip_base != 0U || layout->pip_size != 0U) &&
       !zz9k_board_range_fits(layout->aperture_size,
                              layout->pip_base,
                              layout->pip_size))) {
    return 0;
  }

  return (layout->profile == expected.profile ||
          layout->profile ==
              (expected.profile | ZZ9K_APERTURE_FLAG_ACKED)) &&
         layout->framebuffer_base == expected.framebuffer_base &&
         layout->framebuffer_size == expected.framebuffer_size &&
         layout->pip_base == expected.pip_base &&
         layout->pip_size == expected.pip_size &&
         layout->template_base == expected.template_base &&
         layout->template_size == expected.template_size &&
         layout->host_base == expected.host_base &&
         layout->host_size == expected.host_size &&
         layout->audio_base == expected.audio_base &&
         layout->audio_size == expected.audio_size;
}

#if ZZ9K_HOST_AMIGA
static int zz9k_timer_open(ZZ9KContext *ctx);
static void zz9k_timer_close(ZZ9KContext *ctx);
#endif

static void *zz9k_alloc_host(size_t size)
{
#if ZZ9K_HOST_AMIGA
  return AllocVec(size, MEMF_PUBLIC | MEMF_CLEAR);
#else
  return malloc(size);
#endif
}

static void zz9k_free_host(void *ptr)
{
#if ZZ9K_HOST_AMIGA
  if (ptr) {
    FreeVec(ptr);
  }
#else
  free(ptr);
#endif
}

#if ZZ9K_HOST_AMIGA
static struct SignalSemaphore *zz9k_mailbox_semaphore(void)
{
  static struct SignalSemaphore *cached;
  ZZ9KMailboxSemaphore *candidate;
  struct SignalSemaphore *semaphore;

  if (cached) {
    return cached;
  }

  candidate = (ZZ9KMailboxSemaphore *)zz9k_alloc_host(sizeof(*candidate));
  if (candidate) {
    InitSemaphore(&candidate->semaphore);
    strcpy(candidate->name, ZZ9K_MAILBOX_SEMAPHORE_NAME);
    candidate->semaphore.ss_Link.ln_Name = candidate->name;
  }

  Forbid();
  semaphore = FindSemaphore((STRPTR)ZZ9K_MAILBOX_SEMAPHORE_NAME);
  if (!semaphore && candidate) {
    AddSemaphore(&candidate->semaphore);
    semaphore = &candidate->semaphore;
    candidate = 0;
  }
  Permit();

  if (candidate) {
    zz9k_free_host(candidate);
  }

  cached = semaphore;
  return cached;
}
#endif

static int zz9k_mailbox_lock(ZZ9KContext *ctx)
{
  (void)ctx;
#if ZZ9K_HOST_AMIGA
  {
    struct SignalSemaphore *semaphore;

    semaphore = zz9k_mailbox_semaphore();
    if (!semaphore) {
      return ZZ9K_STATUS_NO_MEMORY;
    }
    ObtainSemaphore(semaphore);
  }
#endif
  return ZZ9K_STATUS_OK;
}

static void zz9k_mailbox_unlock(ZZ9KContext *ctx)
{
  (void)ctx;
#if ZZ9K_HOST_AMIGA
  {
    struct SignalSemaphore *semaphore;

    semaphore = zz9k_mailbox_semaphore();
    if (semaphore) {
      ReleaseSemaphore(semaphore);
    }
  }
#endif
}

static uint32_t zz9k_context_next_request_id(ZZ9KContext *ctx)
{
  uint32_t request_id;

  request_id = ctx->next_request_id++;
  if (ctx->next_request_id == 0) {
    ctx->next_request_id = 1;
  }
  return request_id;
}

const char *zz9k_status_name(int status)
{
  return zz9k_status_text(status);
}

const char *zz9k_service_name(uint32_t service_id)
{
  return zz9k_service_text(service_id);
}

int zz9k_service_advertised_by_caps(uint32_t service_id,
                                    uint32_t capability_bits)
{
  return zz9k_service_advertised_by_capabilities(service_id, capability_bits);
}

#if !ZZ9K_HOST_AMIGA
static void zz9k_copy_to_volatile(volatile uint8_t *dst, const uint8_t *src,
                                  uint32_t length)
{
  uint32_t i;

  for (i = 0; i < length; i++) {
    dst[i] = src[i];
  }
}
#endif

static void zz9k_copy_from_volatile(uint8_t *dst, const volatile uint8_t *src,
                                    uint32_t length)
{
  uint32_t i;

  for (i = 0; i < length; i++) {
    dst[i] = src[i];
  }
}

#if !ZZ9K_HOST_AMIGA
static void (*zz9k_idle_hook_for_test)(void);

void zz9k_set_idle_hook_for_test(void (*hook)(void))
{
  zz9k_idle_hook_for_test = hook;
}

static uint32_t (*zz9k_now_hook_for_test)(void);

void zz9k_set_now_hook_for_test(uint32_t (*hook)(void))
{
  zz9k_now_hook_for_test = hook;
}

static int (*zz9k_block_hook_for_test)(void);

void zz9k_set_block_hook_for_test(int (*hook)(void))
{
  zz9k_block_hook_for_test = hook;
}

void zz9k_set_armed_for_test(ZZ9KContext *ctx, int armed)
{
  if (ctx) {
    ctx->irq_armed = (unsigned char)(armed ? 1 : 0);
  }
}
#endif

/* Idle spin between completion polls. The head of the schedule is
 * fine-grained (poll every ~500 spins for the first 16 polls) so fast ops
 * (ping, inline status, small crypto) are discovered within ~1 ms instead of
 * being overshot by exponential gaps; from poll 16 the gap doubles up to the
 * pre-existing flat 50000-spin cadence, so long ops (crypto, batch decode)
 * poll the Zorro completion ring no more often than before. */
uint32_t zz9k_idle_backoff_limit(uint32_t ticks)
{
  if (ticks < 16U) {
    return 500U;
  }
  if (ticks < 22U) {
    return 500U << (ticks - 15U);
  }
  return 50000U;
}

static void zz9k_idle_between_polls_backoff(uint32_t ticks)
{
#if ZZ9K_HOST_AMIGA
  volatile uint32_t spin;
  uint32_t limit;

  limit = zz9k_idle_backoff_limit(ticks);
  for (spin = 0; spin < limit; spin++) {
  }
#else
  (void)ticks;
  if (zz9k_idle_hook_for_test) {
    zz9k_idle_hook_for_test();
  }
#endif
}

#define ZZ9K_SYNC_WAIT_HEARTBEAT_MICROS 4000UL

#define ZZ9K_DOS_TICK_MILLIS 20UL

static uint32_t zz9k_library_timeout_ticks(const ZZ9KContext *ctx,
                                            uint32_t fallback_ticks)
{
  uint32_t timeout_ms;

  if (!ctx || ctx->offload_timeout_ms == 0U) {
    return fallback_ticks;
  }
  timeout_ms = ctx->offload_timeout_ms;
  return timeout_ms / ZZ9K_DOS_TICK_MILLIS +
         (timeout_ms % ZZ9K_DOS_TICK_MILLIS != 0U);
}
#define ZZ9K_SYNC_WAIT_TIMEOUT_MS_DEFAULT 5000UL

static int zz9k_parse_env_u32(const char *text, uint32_t *value)
{
  char *end;
  long parsed;

  if (!text || !*text || !value) {
    return 0;
  }
  errno = 0;
  parsed = strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || parsed <= 0 ||
      (unsigned long)parsed > 0xffffffffUL) {
    return 0;
  }
  *value = (uint32_t)parsed;
  return 1;
}

uint32_t zz9k_env_u32(const char *name, uint32_t fallback)
{
#if ZZ9K_HOST_AMIGA
  struct DosLibrary *previous_dos_base;
  struct Library *dos_base;
  UBYTE buf[16];
  LONG len;
  uint32_t value;

  dos_base = OpenLibrary((CONST_STRPTR)"dos.library", 0);
  if (!dos_base) {
    return fallback;
  }
  previous_dos_base = DOSBase;
  DOSBase = (struct DosLibrary *)dos_base;

  len = GetVar((CONST_STRPTR)name, buf, (LONG)sizeof(buf) - 1, 0);

  DOSBase = previous_dos_base;
  CloseLibrary(dos_base);

  if (len > 0 && len < (LONG)sizeof(buf)) {
    buf[len] = '\0';
    if (zz9k_parse_env_u32((const char *)buf, &value)) {
      return value;
    }
  }
  return fallback;
#else
  const char *env = getenv(name);
  uint32_t value;

  if (zz9k_parse_env_u32(env, &value)) {
    return value;
  }
  return fallback;
#endif
}

static uint32_t zz9k_sync_wait_timeout_ms(void)
{
  return zz9k_env_u32("ZZ9K_SYNC_WAIT_TIMEOUT_MS",
                      ZZ9K_SYNC_WAIT_TIMEOUT_MS_DEFAULT);
}

/* Hard bound for the armed (block-on-IRQ) wait: the wall-clock offload
 * budget takes precedence when set, so the provider's offload-or-fallback
 * posture survives arming; otherwise ENV:ZZ9K_SYNC_WAIT_TIMEOUT_MS, read
 * once per context — never per call. */
static uint32_t zz9k_armed_wait_timeout_ms(ZZ9KContext *ctx)
{
  if (ctx->offload_timeout_ms != 0U) {
    return ctx->offload_timeout_ms;
  }
  if (ctx->sync_wait_timeout_ms == 0U) {
    ctx->sync_wait_timeout_ms = zz9k_sync_wait_timeout_ms();
  }
  return ctx->sync_wait_timeout_ms;
}

/* Monotonic-ish millisecond reading; unsigned deltas absorb wrap.
 * Without timer.device there is no wall clock, and returning a frozen 0
 * would silently disable every hard-timeout check in the armed wait --
 * one freeze mode is exactly that: no timer plus a missing completion
 * IRQ. Derive a coarse heartbeat-scaled count instead;
 * zz9k_wait_block() bumps the counter once per wait cycle. */
static uint32_t zz9k_now_ms(ZZ9KContext *ctx)
{
#if ZZ9K_HOST_AMIGA
  if (!ctx->timer_open || !ctx->timer_request) {
    return ctx->wait_heartbeats *
           (uint32_t)(ZZ9K_SYNC_WAIT_HEARTBEAT_MICROS / 1000UL);
  }
  ctx->timer_request->tr_node.io_Command = TR_GETSYSTIME;
  DoIO((struct IORequest *)ctx->timer_request);
  return (uint32_t)ctx->timer_request->tr_time.tv_secs * 1000UL +
         (uint32_t)(ctx->timer_request->tr_time.tv_micro / 1000UL);
#else
  (void)ctx;
  if (zz9k_now_hook_for_test) {
    return zz9k_now_hook_for_test();
  }
  return 0;
#endif
}

/* Sleep until the SDK completion IRQ, the heartbeat, or Ctrl-C.
 * Returns nonzero iff woken by Ctrl-C. */
static int zz9k_wait_block(ZZ9KContext *ctx)
{
#if ZZ9K_HOST_AMIGA
  unsigned long wait_mask;
  unsigned long signals;
  int have_timer = ctx->timer_open && ctx->timer_request && ctx->timer_port;

  if (!have_timer) {
    /* No timer.device: Wait() below would wake only on the completion
     * IRQ or Ctrl-C, and if the IRQ never arrives (wrong INT2 wiring,
     * arming failure, wedged board) the caller sleeps forever with no
     * deadline. Poll at the flat backoff cadence (~heartbeat scale) so
     * the caller's timeout logic keeps making progress; consume Ctrl-C
     * the same way Wait() would. */
    zz9k_idle_between_polls_backoff(22U);
    ctx->wait_heartbeats++;
    return (SetSignal(0L, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) ? 1 : 0;
  }

  wait_mask = ctx->irq_signal_mask | SIGBREAKF_CTRL_C;
  ctx->timer_request->tr_node.io_Command = TR_ADDREQUEST;
  ctx->timer_request->tr_time.tv_secs = 0;
  ctx->timer_request->tr_time.tv_micro = ZZ9K_SYNC_WAIT_HEARTBEAT_MICROS;
  SendIO((struct IORequest *)ctx->timer_request);
  if (ctx->timer_request->tr_node.io_Error != 0) {
    /* Heartbeat could not be armed: Wait() would then block on a signal
     * that never comes -- the same unbounded-sleep risk as having no
     * timer at all. Reap the failed request first: SendIO may have
     * queued and replied it even on error, and an outstanding request
     * must not be reused by zz9k_now_ms()'s DoIO or a later SendIO.
     * Then fall back to the bounded poll. */
    if (!CheckIO((struct IORequest *)ctx->timer_request)) {
      AbortIO((struct IORequest *)ctx->timer_request);
    }
    WaitIO((struct IORequest *)ctx->timer_request);
    zz9k_idle_between_polls_backoff(22U);
    ctx->wait_heartbeats++;
    return (SetSignal(0L, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) ? 1 : 0;
  }
  wait_mask |= (1UL << ctx->timer_port->mp_SigBit);

  signals = Wait(wait_mask);
  ctx->wait_heartbeats++;

  if (!CheckIO((struct IORequest *)ctx->timer_request)) {
    AbortIO((struct IORequest *)ctx->timer_request);
  }
  WaitIO((struct IORequest *)ctx->timer_request);

  return (signals & SIGBREAKF_CTRL_C) ? 1 : 0;
#else
  (void)ctx;
  if (zz9k_block_hook_for_test) {
    return zz9k_block_hook_for_test();
  }
  return 0;
#endif
}


/* Forward declaration: defined later in this file, needed here because
 * zz9k_await_completion_locked is placed alongside the other wait seams. */
static int zz9k_consume_next_completion_locked(ZZ9KContext *ctx,
                                               ZZ9KMailboxEntry *reply);

static int zz9k_dispatch_foreign_completion(ZZ9KContext *ctx,
                                            const ZZ9KMailboxEntry *reply)
{
  if (ctx->completion_dispatcher) {
    return ctx->completion_dispatcher(ctx->completion_dispatcher_user_data,
                                      reply);
  }
  return ZZ9K_STATUS_OK;
}

/* Armed replacement for the busy-poll loop. Caller holds the mailbox lock and
 * has already enqueued request_id. Polls the ring, sleeping between polls. */
static int zz9k_await_completion_locked(ZZ9KContext *ctx, uint32_t request_id,
                                        uint16_t opcode, uint32_t sync_cookie,
                                        ZZ9KMailboxEntry *reply,
                                        uint32_t hard_timeout_ms)
{
  uint32_t start = zz9k_now_ms(ctx);
  int status;

  for (;;) {
    status = zz9k_consume_next_completion_locked(ctx, reply);
    if (status == ZZ9K_STATUS_OK) {
      if (reply->request_id == request_id &&
          reply->opcode == opcode &&
          reply->user_cookie == sync_cookie) {
        return reply->status;
      }
      status = zz9k_dispatch_foreign_completion(ctx, reply);
      if (status != ZZ9K_STATUS_OK) {
        return status;
      }
      continue;
    }
    if (status != ZZ9K_STATUS_BUSY) {
      return status;   /* hard transport error */
    }
    /* ring empty: sleep, then re-poll */
    if (zz9k_wait_block(ctx) != 0) {
#if ZZ9K_HOST_AMIGA
      /* Ctrl-C: the request is still IN FLIGHT on the board, and the
         caller's failure path will free the buffers the ARM is decoding
         into. Drain bounded (single-member decodes finish in well under
         a second; the cap matches the sync wait default) so the
         completion retires and the firmware is done with the buffers
         before they are released. Never blocks on Wait again -- only
         polls, so further Ctrl-C presses do not extend the drain. The
         iteration cap is belt-and-braces alongside the timer deadline.
         The drain itself owns late_irq_expected: a retired request
         clears it (no assert will come), a timed-out one sets it. */
      uint32_t drain_start = zz9k_now_ms(ctx);
      uint32_t drain_polls = 0U;
      int drain_expired = 0;

      for (;;) {
        status = zz9k_consume_next_completion_locked(ctx, reply);
        if (status == ZZ9K_STATUS_OK &&
            reply->request_id == request_id &&
            reply->opcode == opcode &&
            reply->user_cookie == sync_cookie) {
          /* Retired cleanly: the completion is consumed and no further
             assert will arrive -- clear any stale expectation so disarm
             does not busy-wait the full window for an assert that is
             never coming. */
          ctx->late_irq_expected = 0;
          return ZZ9K_STATUS_CANCELLED;
        }
        if (status == ZZ9K_STATUS_OK) {
          status = zz9k_dispatch_foreign_completion(ctx, reply);
          if (status != ZZ9K_STATUS_OK) {
            break;
          }
        }
        if (status != ZZ9K_STATUS_BUSY && status != ZZ9K_STATUS_OK) {
          break; /* transport error: nothing more to drain, and nothing
                     on a dead transport will ever assert -- leave
                     late_irq_expected untouched rather than arming a
                     15s disarm spin that can never be satisfied */
        }
        if ((uint32_t)(zz9k_now_ms(ctx) - drain_start) >= 2000U ||
            ++drain_polls > 4000000U) {
          drain_expired = 1;
          break;
        }
        zz9k_idle_between_polls_backoff(28U);
        ctx->wait_heartbeats++;
      }
      /* Only a drain that timed out with the request still outstanding
         leaves the ARM working after we return: it will post its
         completion (and assert the IRQ) after the caller frees its
         buffers -- disarm must watch for that. */
      if (drain_expired) {
        ctx->late_irq_expected = 1;
      }
      return ZZ9K_STATUS_CANCELLED;
#endif
      return ZZ9K_STATUS_CANCELLED; /* host stub wake: no drain needed */
    }
    if (zz9k_now_ms(ctx) - start >= hard_timeout_ms) {
      return ZZ9K_STATUS_TIMEOUT;
    }
  }
}

static void zz9k_put_wire_be16(volatile uint8_t *p, uint16_t value)
{
#if ZZ9K_HOST_AMIGA
  *(volatile uint16_t *)p = value;
#else
  zz9k_put_be16(p, value);
#endif
}

static void zz9k_put_wire_be32(volatile uint8_t *p, uint32_t value)
{
#if ZZ9K_HOST_AMIGA
  *(volatile uint16_t *)p = (uint16_t)((value >> 16) & 0xffffU);
  *(volatile uint16_t *)(p + 2) = (uint16_t)(value & 0xffffU);
#else
  zz9k_put_be32(p, value);
#endif
}

static void zz9k_copy_payload_to_wire(volatile uint8_t *dst,
                                      const uint8_t *src, uint32_t length)
{

#if ZZ9K_HOST_AMIGA
  uint32_t i;
  for (i = 0; i + 1U < length; i += 2U) {
    *(volatile uint16_t *)(dst + i) =
        (uint16_t)(((uint16_t)src[i] << 8) | src[i + 1U]);
  }
  if (i < length) {
    dst[i] = src[i];
  }
#else
  zz9k_copy_to_volatile(dst, src, length);
#endif
}

static void zz9k_entry_to_wire(volatile ZZ9KMailboxWireEntry *wire,
                               const ZZ9KMailboxEntry *entry)
{
  uint32_t payload_len;

  payload_len = entry->payload_len;
  if (payload_len > sizeof(entry->payload.inline_data)) {
    payload_len = sizeof(entry->payload.inline_data);
  }

  zz9k_put_wire_be32(wire->request_id, entry->request_id);
  zz9k_put_wire_be16(wire->opcode, entry->opcode);
  zz9k_put_wire_be16(wire->status, entry->status);
  zz9k_put_wire_be16(wire->flags, entry->flags);
  zz9k_put_wire_be16(wire->payload_len, entry->payload_len);
  zz9k_put_wire_be32(wire->user_cookie, entry->user_cookie);
  zz9k_copy_payload_to_wire(wire->payload, entry->payload.inline_data,
                            payload_len);
}

static void zz9k_entry_from_wire(ZZ9KMailboxEntry *entry,
                                 const volatile ZZ9KMailboxWireEntry *wire)
{
  entry->request_id = zz9k_get_be32(wire->request_id);
  entry->opcode = zz9k_get_be16(wire->opcode);
  entry->status = zz9k_get_be16(wire->status);
  entry->flags = zz9k_get_be16(wire->flags);
  entry->payload_len = zz9k_get_be16(wire->payload_len);
  entry->user_cookie = zz9k_get_be32(wire->user_cookie);
  zz9k_copy_from_volatile(entry->payload.inline_data, wire->payload,
                          sizeof(entry->payload.inline_data));
}

static uint32_t zz9k_next_ring_index(uint32_t index, uint32_t entries)
{
  index++;
  if (index >= entries) {
    index = 0;
  }
  return index;
}

static int zz9k_board_range_fits(uint32_t board_size, uint32_t board_offset,
                                 uint32_t length)
{
  if (length == 0 || board_offset > (UINT32_MAX - (length - 1U))) {
    return 0;
  }

  if (board_size != 0) {
    if (board_offset >= board_size || length > (board_size - board_offset)) {
      return 0;
    }
  }

  return 1;
}

static int zz9k_arm_range_to_board_offset(uint32_t board_size,
                                          uint32_t arm_addr, uint32_t length,
                                          uint32_t *board_offset)
{
  uint32_t last_addr;
  uint32_t offset;

  if (!board_offset || length == 0 ||
      arm_addr > (UINT32_MAX - (length - 1U))) {
    return 0;
  }

  last_addr = arm_addr + length - 1U;
  if (arm_addr >= ZZ9K_MAPPED_IO_ARM_START &&
      last_addr < (ZZ9K_MAPPED_IO_ARM_START + ZZ9K_MAPPED_IO_WINDOW_SIZE)) {
    offset =
        ZZ9K_MAPPED_IO_BOARD_OFFSET + (arm_addr - ZZ9K_MAPPED_IO_ARM_START);
    if (!zz9k_board_range_fits(board_size, offset, length)) {
      return 0;
    }
    *board_offset = offset;
    return 1;
  }

  if (arm_addr < ZZ9K_ARM_MEMORY_START ||
      last_addr > ZZ9K_ARM_MEMORY_VISIBLE_END) {
    return 0;
  }

  offset = ZZ9K_AMIGA_MEMORY_OFFSET + (arm_addr - ZZ9K_ARM_MEMORY_START);
  if (!zz9k_board_range_fits(board_size, offset, length)) {
    return 0;
  }

  *board_offset = offset;
  return 1;
}

static volatile void *zz9k_arm_range_to_board_ptr(uint32_t board_addr,
                                                  uint32_t board_size,
                                                  uint32_t arm_addr,
                                                  uint32_t length)
{
  uint32_t board_offset;

  if (board_addr == 0 ||
      !zz9k_arm_range_to_board_offset(board_size, arm_addr, length,
                                      &board_offset)) {
    return 0;
  }

  return (volatile void *)((uintptr_t)board_addr + board_offset);
}

#if ZZ9K_HOST_AMIGA
static volatile uint16_t *zz9k_reg16(uint32_t board_addr, uint32_t offset)
{
  return (volatile uint16_t *)((uint8_t *)board_addr + offset);
}

static int zz9k_board_contains_offset(uint32_t board_size, uint32_t offset,
                                      uint32_t size)
{
  if (size == 0 || offset > (UINT32_MAX - (size - 1U))) {
    return 0;
  }
  if (board_size == 0) {
    return 1;
  }

  return offset < board_size && size <= board_size - offset;
}

static volatile uint16_t *zz9k_z3_register_window_reg16(
    const ZZ9KBoard *board, uint32_t offset)
{
  uint32_t register_offset;

  if (!board || board->zorro_version != 3) {
    return 0;
  }

  register_offset = ZZ9K_Z3_REGISTER_WINDOW_OFFSET + offset;
  if (!zz9k_board_contains_offset(board->board_size, register_offset,
                                  sizeof(uint16_t))) {
    return 0;
  }

  return zz9k_reg16(board->board_addr, register_offset);
}

static uint16_t zz9k_read_reg16(uint32_t board_addr, uint32_t offset)
{
  return *zz9k_reg16(board_addr, offset);
}
#endif

int zz9k_find_board(ZZ9KBoard *board)
{
#if ZZ9K_HOST_AMIGA
  struct Library *expansion_base;
  struct ExpansionBase *previous_expansion_base;
  struct ConfigDev *config_dev;
#endif

  if (!board) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  board->board_addr = 0;
  board->board_size = 0;
  board->product = 0;
  board->zorro_version = 0;
  board->firmware_version = 0;

#if ZZ9K_HOST_AMIGA
  expansion_base = OpenLibrary((CONST_STRPTR)"expansion.library", 0);
  if (!expansion_base) {
    return ZZ9K_STATUS_IO_ERROR;
  }

  previous_expansion_base = ExpansionBase;
  ExpansionBase = (struct ExpansionBase *)expansion_base;

  config_dev = FindConfigDev(0, ZZ9K_MNT_MANUFACTURER, ZZ9K_PRODUCT_Z3);
  if (config_dev) {
    board->board_addr = (uint32_t)config_dev->cd_BoardAddr;
    board->board_size = config_dev->cd_BoardSize;
    board->product = ZZ9K_PRODUCT_Z3;
    board->zorro_version = 3;
  } else {
    config_dev = FindConfigDev(0, ZZ9K_MNT_MANUFACTURER, ZZ9K_PRODUCT_Z2);
    if (config_dev) {
      board->board_addr = (uint32_t)config_dev->cd_BoardAddr;
      board->board_size = config_dev->cd_BoardSize;
      board->product = ZZ9K_PRODUCT_Z2;
      board->zorro_version = 2;
    }
  }

  ExpansionBase = previous_expansion_base;
  CloseLibrary(expansion_base);

  if (!config_dev) {
    return ZZ9K_STATUS_NOT_FOUND;
  }

  board->firmware_version =
      ((uint32_t)zz9k_read_reg16(board->board_addr, ZZ9K_REG_SDK_VERSION)
       << 16);
  return ZZ9K_STATUS_OK;
#else
  return ZZ9K_STATUS_UNSUPPORTED;

#endif
}
#if ZZ9K_HOST_AMIGA && !defined(ZZ9K_HOST_LIBRARY_BUILD)
static int zz9k_library_lvo_open(void *user, ZZ9KBoard *board, void **base,
                                 uint16_t *version, uint16_t *revision)
{
  struct Library *library_base;
  int status;

  (void)user;
  library_base = OpenLibrary((CONST_STRPTR)"zz9k.library", 0);
  if (!library_base) {
    return ZZ9K_STATUS_NOT_FOUND;
  }
  *base = library_base;
  *version = library_base->lib_Version;
  *revision = library_base->lib_Revision;
  status = zz9k_find_board(board);
  if (status != ZZ9K_STATUS_OK) {
    CloseLibrary(library_base);
    *base = 0;
  }
  return status;
}

static void zz9k_library_lvo_close(void *base)
{
  CloseLibrary((struct Library *)base);
}

static int zz9k_library_lvo_call(void *base, ZZ9KRequest *request,
                                 ZZ9KMailboxEntry *reply,
                                 uint32_t timeout_ticks)
{
  return ZZ9KCallAtBase((struct Library *)base, request, reply, timeout_ticks);
}

static int zz9k_library_lvo_call_async(
    void *base, ZZ9KAsyncRequest *async, const ZZ9KRequest *request,
    ZZ9KAsyncCallback callback, void *callback_user_data)
{
  return ZZ9KCallAsyncAtBase((struct Library *)base, async, request, callback,
                             callback_user_data);
}

static int zz9k_library_lvo_poll(void *base, uint32_t max_completions,
                                 uint32_t *completed)
{
  return ZZ9KPollAtBase((struct Library *)base, max_completions, completed);
}

static int zz9k_library_lvo_cancel(void *base, ZZ9KAsyncRequest *async)
{
  return ZZ9KCancelAsyncAtBase((struct Library *)base, async);
}

static const ZZ9KLibraryTransport zz9k_library_lvo_transport = {
  zz9k_library_lvo_open,
  zz9k_library_lvo_close,
  zz9k_library_lvo_call,
  zz9k_library_lvo_call_async,
  zz9k_library_lvo_poll,
  zz9k_library_lvo_cancel,
  0
};
#endif

int zz9k_open(ZZ9KContext **ctx)
{
#if ZZ9K_HOST_AMIGA
  ZZ9KBoard board;
  uint32_t mailbox_addr;
  volatile ZZ9KMailboxDescriptor *mailbox;
  int status;
#endif

  if (!ctx) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  *ctx = 0;

#if ZZ9K_HOST_AMIGA && !defined(ZZ9K_HOST_LIBRARY_BUILD)
  {
    struct Library *library_base;
    ZZ9KContext *new_ctx;

    library_base = OpenLibrary((CONST_STRPTR)"zz9k.library", 0);
    if (library_base) {
      if (!zz9k_library_version_supported(library_base->lib_Version,
                                          library_base->lib_Revision)) {
        CloseLibrary(library_base);
        return ZZ9K_STATUS_LIBRARY_TOO_OLD;
      }
      status = zz9k_find_board(&board);
      if (status == ZZ9K_STATUS_OK) {
        new_ctx = (ZZ9KContext *)zz9k_alloc_host(sizeof(*new_ctx));
        if (new_ctx) {
          ZZ9KCaps caps;

          memset(new_ctx, 0, sizeof(*new_ctx));
          new_ctx->board = board;
          new_ctx->library_transport = 1U;
          new_ctx->library_base = library_base;
          new_ctx->library_transport_ops = &zz9k_library_lvo_transport;
          status = zz9k_query_caps(new_ctx, &caps);
          if (status == ZZ9K_STATUS_OK &&
              (caps.capability_bits & ZZ9K_CAP_APERTURE_LAYOUT) != 0U &&
              board.zorro_version == 2U) {
            status = zz9k_query_aperture_layout(
                new_ctx, &new_ctx->aperture_layout);
          }
          if (status == ZZ9K_STATUS_OK) {
            *ctx = new_ctx;
            return ZZ9K_STATUS_OK;
          }
          zz9k_close(new_ctx);
          return status;
        }
        status = ZZ9K_STATUS_NO_MEMORY;
      }
      CloseLibrary(library_base);
      return status;
    }
  }
#endif

#if ZZ9K_HOST_AMIGA
  status = zz9k_find_board(&board);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  if (zz9k_read_reg16(board.board_addr, ZZ9K_REG_SDK_MAGIC) !=
      ZZ9K_REG_SDK_MAGIC_VALUE) {
    return ZZ9K_STATUS_UNSUPPORTED;
  }

  mailbox_addr =
      ((uint32_t)zz9k_read_reg16(board.board_addr, ZZ9K_REG_SDK_MAILBOX_HI)
       << 16) |
      zz9k_read_reg16(board.board_addr, ZZ9K_REG_SDK_MAILBOX_LO);
  mailbox = (volatile ZZ9KMailboxDescriptor *)zz9k_arm_range_to_board_ptr(
      board.board_addr, board.board_size, mailbox_addr,
      ZZ9K_MAILBOX_DESCRIPTOR_SIZE);
  if (!mailbox) {
    return ZZ9K_STATUS_UNSUPPORTED;
  }

  status = zz9k_attach_mailbox(
      ctx, &board, mailbox,
      zz9k_z3_register_window_reg16(&board, ZZ9K_REG_SDK_DOORBELL), 0);
  if (status == ZZ9K_STATUS_OK && *ctx &&
      ((*ctx)->capability_bits & ZZ9K_CAP_IRQ_COMPLETION) != 0U) {
    (*ctx)->sdk_irq_control = zz9k_reg16(board.board_addr,
                                         ZZ9K_REG_SDK_IRQ_ACK);
  }
  return status;
#else
  if (zz9k_library_transport_for_test) {
    const ZZ9KLibraryTransport *transport = zz9k_library_transport_for_test;
    ZZ9KContext *new_ctx;
    ZZ9KBoard board;
    void *library_base;
    uint16_t version;
    uint16_t revision;
    int status;

    memset(&board, 0, sizeof(board));
    library_base = 0;
    status = transport->open(transport->user, &board, &library_base, &version,
                             &revision);
    if (status != ZZ9K_STATUS_OK) {
      return status;
    }
    if (!zz9k_library_version_supported(version, revision)) {
      transport->close(library_base);
      return ZZ9K_STATUS_LIBRARY_TOO_OLD;
    }
    new_ctx = (ZZ9KContext *)zz9k_alloc_host(sizeof(*new_ctx));
    if (!new_ctx) {
      transport->close(library_base);
      return ZZ9K_STATUS_NO_MEMORY;
    }
    memset(new_ctx, 0, sizeof(*new_ctx));
    new_ctx->board = board;
    new_ctx->library_transport = 1U;
    new_ctx->library_transport_ops = transport;
    new_ctx->library_base = library_base;
    *ctx = new_ctx;
    return ZZ9K_STATUS_OK;
  }
  return ZZ9K_STATUS_UNSUPPORTED;
#endif
}

int zz9k_attach_mailbox(ZZ9KContext **ctx, const ZZ9KBoard *board,
                        volatile ZZ9KMailboxDescriptor *mailbox,
                        volatile uint16_t *doorbell,
                        volatile uint16_t *irq_ack)
{
  ZZ9KContext *new_ctx;
  volatile uint8_t *base;
  uint32_t request_offset;
  uint32_t completion_offset;
  uint32_t request_entries;
  uint32_t completion_entries;
  uint32_t capability_bits;

  if (!ctx || !board || !mailbox) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  *ctx = 0;

  if (zz9k_get_be32(mailbox->magic) != ZZ9K_ABI_MAGIC) {
    return ZZ9K_STATUS_UNSUPPORTED;
  }
  if (zz9k_get_be16(mailbox->abi_major) != ZZ9K_ABI_VERSION_MAJOR) {
    return ZZ9K_STATUS_UNSUPPORTED;
  }

  request_offset = zz9k_get_be32(mailbox->request_ring_offset);
  completion_offset = zz9k_get_be32(mailbox->completion_ring_offset);
  request_entries = zz9k_get_be32(mailbox->request_ring_entries);
  completion_entries = zz9k_get_be32(mailbox->completion_ring_entries);
  capability_bits = zz9k_get_be32(mailbox->capability_bits);

  if (request_entries < 2 || completion_entries < 2) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if ((capability_bits & ZZ9K_CAP_MAILBOX) == 0) {
    return ZZ9K_STATUS_UNSUPPORTED;
  }
  if ((capability_bits & ZZ9K_CAP_APERTURE_LAYOUT) != 0U &&
      board->zorro_version == 2U) {
    ZZ9KApertureLayout expected;

    if (board->board_size == 0U ||
        !zz9k_expected_aperture_layout(board->board_size, &expected)) {
      return ZZ9K_STATUS_UNSUPPORTED;
    }
  }

  new_ctx = (ZZ9KContext *)zz9k_alloc_host(sizeof(*new_ctx));
  if (!new_ctx) {
    return ZZ9K_STATUS_NO_MEMORY;
  }

  memset(new_ctx, 0, sizeof(*new_ctx));
  new_ctx->board = *board;
  new_ctx->mailbox = mailbox;
  new_ctx->doorbell =
      (capability_bits & ZZ9K_CAP_DOORBELL) ? doorbell : 0;
  new_ctx->irq_ack =
      (capability_bits & ZZ9K_CAP_IRQ_COMPLETION) ? irq_ack : 0;
  new_ctx->sdk_irq_control = 0;
  new_ctx->capability_bits = capability_bits;
  new_ctx->request_ring_entries = request_entries;
  new_ctx->completion_ring_entries = completion_entries;
  new_ctx->next_request_id = 1;
#if ZZ9K_HOST_AMIGA
  new_ctx->sync_cookie_mask =
      ZZ9K_SYNC_COOKIE_MASK ^
      (uint32_t)(uintptr_t)new_ctx ^
      (uint32_t)(uintptr_t)FindTask(0);
  if (new_ctx->sync_cookie_mask == 0) {
    new_ctx->sync_cookie_mask = ZZ9K_SYNC_COOKIE_MASK;
  }
#else
  new_ctx->sync_cookie_mask = ZZ9K_SYNC_COOKIE_MASK;
#endif

  base = (volatile uint8_t *)mailbox;
  new_ctx->request_ring =
      (volatile ZZ9KMailboxWireEntry *)(base + request_offset);
  new_ctx->completion_ring =
      (volatile ZZ9KMailboxWireEntry *)(base + completion_offset);

  *ctx = new_ctx;
  if ((capability_bits & ZZ9K_CAP_APERTURE_LAYOUT) != 0U &&
      board->zorro_version == 2U) {
    ZZ9KApertureLayout layout;
    ZZ9KCaps caps;
    int status;

    status = zz9k_query_aperture_layout(new_ctx, &layout);
    if (status == ZZ9K_STATUS_OK) {
      status = zz9k_query_caps(new_ctx, &caps);
    }
    if (status != ZZ9K_STATUS_OK ||
        (new_ctx->capability_bits & ZZ9K_CAP_APERTURE_LAYOUT) == 0U) {
      zz9k_close(new_ctx);
      *ctx = 0;
      return status == ZZ9K_STATUS_OK ? ZZ9K_STATUS_INTERNAL_ERROR : status;
    }
  }
  return ZZ9K_STATUS_OK;
}

void zz9k_close(ZZ9KContext *ctx)
{
  if (!ctx) {
    return;
  }
  zz9k_disarm_completion_irq(ctx);
#if ZZ9K_HOST_AMIGA
  zz9k_timer_close(ctx);
#endif
#if !defined(ZZ9K_HOST_LIBRARY_BUILD)
  while (ctx->library_pending) {
    ZZ9KStaticLibraryPending *pending = ctx->library_pending;
    int status;

    status = ctx->library_transport_ops->cancel(ctx->library_base,
                                                &pending->async);
    if (status != ZZ9K_STATUS_OK &&
        !(status == ZZ9K_STATUS_BAD_REQUEST && pending->async.completed)) {
      return;
    }
    ctx->library_pending = pending->next;
    zz9k_free_host(pending);
  }
  if (ctx->library_transport && ctx->library_transport_ops &&
      ctx->library_base) {
    ctx->library_transport_ops->close(ctx->library_base);
  }
#endif
  zz9k_free_host(ctx);
}

void zz9k_set_offload_timeout_ms(ZZ9KContext *ctx, uint32_t timeout_ms)
{
  if (ctx) {
    ctx->offload_timeout_ms = timeout_ms;
  }
}

void zz9k_set_completion_dispatcher(ZZ9KContext *ctx,
                                    ZZ9KCompletionDispatcher dispatcher,
                                    void *user_data)
{
  if (ctx) {
    ctx->completion_dispatcher = dispatcher;
    ctx->completion_dispatcher_user_data = user_data;
  }
}

int zz9k_set_client_id(ZZ9KContext *ctx, uint16_t client_id)
{
  if (!ctx || client_id > (ZZ9K_ENTRY_CLIENT_ID_MASK >>
                           ZZ9K_ENTRY_CLIENT_ID_SHIFT)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  ctx->client_id = client_id;
  return ZZ9K_STATUS_OK;
}

int zz9k_completion_irq_supported(ZZ9KContext *ctx)
{
  return ctx && !ctx->library_transport &&
         (ctx->capability_bits & ZZ9K_CAP_IRQ_COMPLETION) != 0U;
}

int zz9k_uses_library_transport(const ZZ9KContext *ctx)
{
  return ctx && ctx->library_transport;
}

int zz9k_shared_heap_board_visible(const ZZ9KContext *ctx)
{
	return ctx && ctx->board.zorro_version != 2U;
}

int zz9k_interrupt_status(ZZ9KContext *ctx, uint16_t *status)
{
  if (!ctx || !status) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (!zz9k_completion_irq_supported(ctx)) {
    return ZZ9K_STATUS_UNSUPPORTED;
  }

#if ZZ9K_HOST_AMIGA
  if (ctx->board.board_addr == 0) {
    return ZZ9K_STATUS_UNSUPPORTED;
  }
  *status = zz9k_read_reg16(ctx->board.board_addr, ZZ9K_REG_CONFIG);
  return ZZ9K_STATUS_OK;
#else
  *status = 0;
  return ZZ9K_STATUS_UNSUPPORTED;
#endif
}

int zz9k_completion_irq_ack(ZZ9KContext *ctx)
{
  if (!ctx) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (ctx->library_transport || !zz9k_completion_irq_supported(ctx)) {
    return ZZ9K_STATUS_UNSUPPORTED;
  }

#if ZZ9K_HOST_AMIGA
  if (ctx->board.board_addr == 0) {
    return ZZ9K_STATUS_UNSUPPORTED;
  }
  *zz9k_reg16(ctx->board.board_addr, ZZ9K_REG_CONFIG) =
      ZZ9K_CONFIG_ACK_MODE | ZZ9K_CONFIG_ACK_SDK;
  return ZZ9K_STATUS_OK;
#else
  return ZZ9K_STATUS_UNSUPPORTED;
#endif
}

int zz9k_completion_irq_enable(ZZ9KContext *ctx, int enable)
{
  if (!ctx) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (ctx->library_transport || !zz9k_completion_irq_supported(ctx) ||
      !ctx->sdk_irq_control) {
    return ZZ9K_STATUS_UNSUPPORTED;
  }

  *ctx->sdk_irq_control = enable ? ZZ9K_SDK_IRQ_ENABLE_VALUE :
                                   ZZ9K_SDK_IRQ_DISABLE_VALUE;
  return ZZ9K_STATUS_OK;
}

int zz9k_sdk_use_int2(const ZZ9KContext *ctx)
{
#if ZZ9K_HOST_AMIGA
  BPTR f = Open((CONST_STRPTR)"ENV:ZZ9K_INT2", MODE_OLDFILE);
  if (f) {
    Close(f);
    return 1;
  }
  /* No ENV override: consult the ZZ9000.CFG `int2` key. On firmware
   * older than ABI 2.3 the register group reads as zero, so the key
   * reports absent and INT6 stays the default. */
  if (ctx && ctx->board.board_addr) {
    volatile uint16_t *key =
        zz9k_reg16(ctx->board.board_addr, ZZ9K_REG_CONFIG_KEY);
    volatile uint16_t *present =
        zz9k_reg16(ctx->board.board_addr, ZZ9K_REG_CONFIG_PRESENT);
    *key = ZZ9K_CFG_KEY_INT2;
    if (*present && *key) {
      return 1;
    }
  }
  return 0;
#else
  /* Host stub: no Amiga interrupt lines to choose between. */
  (void)ctx;
  return 0;
#endif
}

#if ZZ9K_HOST_AMIGA

static uint32_t zz9k_sdk_isr(ZZ9KContext *ctx __asm__("a1"))
{
  uint16_t status = 0;

  if (!ctx || zz9k_interrupt_status(ctx, &status) != ZZ9K_STATUS_OK) {
    return 0;
  }
  if ((status & ZZ9K_INTERRUPT_SDK) == 0U) {
    return 0;   /* not our interrupt: let the next server look */
  }
  zz9k_completion_irq_ack(ctx);
  if (ctx->irq_task && ctx->irq_signal_mask) {
    Signal(ctx->irq_task, ctx->irq_signal_mask);
  }
  return 1;
}

static int zz9k_timer_open(ZZ9KContext *ctx)
{
  if (ctx->timer_open) {
    return ZZ9K_STATUS_OK;
  }
  ctx->timer_port = CreateMsgPort();
  if (!ctx->timer_port) {
    return ZZ9K_STATUS_NO_MEMORY;
  }
  ctx->timer_request = (struct timerequest *)CreateIORequest(
      ctx->timer_port, sizeof(struct timerequest));
  if (!ctx->timer_request) {
    DeleteMsgPort(ctx->timer_port);
    ctx->timer_port = 0;
    return ZZ9K_STATUS_NO_MEMORY;
  }
  if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
                 (struct IORequest *)ctx->timer_request, 0) != 0) {
    DeleteIORequest((struct IORequest *)ctx->timer_request);
    ctx->timer_request = 0;
    DeleteMsgPort(ctx->timer_port);
    ctx->timer_port = 0;
    return ZZ9K_STATUS_UNSUPPORTED;
  }
  ctx->timer_open = 1;
  return ZZ9K_STATUS_OK;
}

static void zz9k_timer_close(ZZ9KContext *ctx)
{
  if (ctx->timer_open) {
    CloseDevice((struct IORequest *)ctx->timer_request);
    ctx->timer_open = 0;
  }
  if (ctx->timer_request) {
    DeleteIORequest((struct IORequest *)ctx->timer_request);
    ctx->timer_request = 0;
  }
  if (ctx->timer_port) {
    DeleteMsgPort(ctx->timer_port);
    ctx->timer_port = 0;
  }
}
#endif /* ZZ9K_HOST_AMIGA */

int zz9k_arm_completion_irq(ZZ9KContext *ctx)
{
#if ZZ9K_HOST_AMIGA
  int status;

  if (!ctx) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (ctx->irq_armed) {
    return ZZ9K_STATUS_OK;
  }
  if (!zz9k_completion_irq_supported(ctx) || !ctx->sdk_irq_control) {
    return ZZ9K_STATUS_UNSUPPORTED;
  }

  ctx->irq_signal_bit = AllocSignal(-1);
  if (ctx->irq_signal_bit < 0) {
    return ZZ9K_STATUS_NO_MEMORY;
  }
  ctx->irq_task = FindTask(0);
  ctx->irq_signal_mask = 1UL << ctx->irq_signal_bit;
  ctx->irq_int_bit = zz9k_sdk_use_int2(ctx) ? INTB_PORTS : INTB_EXTER;

  memset(&ctx->irq, 0, sizeof(ctx->irq));
  ctx->irq.is_Node.ln_Type = NT_INTERRUPT;
  ctx->irq.is_Node.ln_Pri = 0;
  ctx->irq.is_Node.ln_Name = (char *)"zz9k SDK IRQ";
  ctx->irq.is_Data = ctx;
  ctx->irq.is_Code = (void (*)())zz9k_sdk_isr;

  status = zz9k_timer_open(ctx);
  if (status != ZZ9K_STATUS_OK) {
    FreeSignal(ctx->irq_signal_bit);
    ctx->irq_signal_bit = -1;
    ctx->irq_task = 0;
    ctx->irq_signal_mask = 0;
    return status;
  }

  Forbid();
  AddIntServer(ctx->irq_int_bit, &ctx->irq);
  Permit();

  (void)SetSignal(0, ctx->irq_signal_mask);
  (void)zz9k_completion_irq_ack(ctx);
  if (zz9k_completion_irq_enable(ctx, 1) != ZZ9K_STATUS_OK) {
    Forbid();
    RemIntServer(ctx->irq_int_bit, &ctx->irq);
    Permit();
    zz9k_timer_close(ctx);
    FreeSignal(ctx->irq_signal_bit);
    ctx->irq_signal_bit = -1;
    ctx->irq_task = 0;
    ctx->irq_signal_mask = 0;
    return ZZ9K_STATUS_UNSUPPORTED;
  }

  ctx->irq_armed = 1;
  return ZZ9K_STATUS_OK;
#else
  (void)ctx;
  return ZZ9K_STATUS_UNSUPPORTED;
#endif
}

void zz9k_expect_late_completion_irq(ZZ9KContext *ctx)
{
  if (ctx) {
    ctx->late_irq_expected = 1;
  }
}

void zz9k_disarm_completion_irq(ZZ9KContext *ctx)
{
#if ZZ9K_HOST_AMIGA
  if (!ctx || !ctx->irq_armed) {
    return;
  }
  (void)zz9k_completion_irq_enable(ctx, 0);
  (void)zz9k_completion_irq_ack(ctx);
  Forbid();
  RemIntServer(ctx->irq_int_bit, &ctx->irq);
  Permit();
  /* A completion posted between the disable above and the server removal
     can leave the board asserting the line with no handler installed --
     and a decode abandoned by a Ctrl-C may post its completion SECONDS
     later, after any single ack. An asserted line with no handler crashes
     the next interrupt-heavy operation (directory deletes, disk
     activity). Watch the board's interrupt status for a grace window and
     ack every late assert until the board has been quiet for half a
     second: the ACK register write needs no handler, and by the time the
     loop exits the abandoned work has actually finished posting. */
  {
    uint32_t quiet_start = zz9k_now_ms(ctx);
    uint32_t deadline = quiet_start;
    uint32_t spins = 0U;
    int retired = !ctx->late_irq_expected;

    for (;;) {
      uint16_t status = 0;

      if (zz9k_interrupt_status(ctx, &status) == ZZ9K_STATUS_OK &&
          (status & ZZ9K_INTERRUPT_SDK) != 0U) {
        (void)zz9k_completion_irq_ack(ctx);
        quiet_start = zz9k_now_ms(ctx);
        retired = 1; /* observed the abandoned op's completion assert */
      }
      if (retired &&
          ((uint32_t)(zz9k_now_ms(ctx) - quiet_start) >= 500U ||
           ++spins > 20000000U)) {
        break;
      }
      if ((uint32_t)(zz9k_now_ms(ctx) - deadline) > 15000U) {
        break; /* give up watching; the disable above is the backstop */
      }
    }
    ctx->late_irq_expected = 0;
  }
  (void)zz9k_completion_irq_ack(ctx);
  zz9k_timer_close(ctx);
  if (ctx->irq_signal_bit >= 0) {
    FreeSignal(ctx->irq_signal_bit);
    ctx->irq_signal_bit = -1;
  }
  ctx->irq_task = 0;
  ctx->irq_signal_mask = 0;
  ctx->irq_armed = 0;
#else
  (void)ctx;
#endif
}

int zz9k_query_caps(ZZ9KContext *ctx, ZZ9KCaps *caps)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !caps) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(caps, 0, sizeof(*caps));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_query_caps(&request);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = zz9k_reply_caps(&reply, caps);
  if (status == ZZ9K_STATUS_OK) {
    ctx->capability_bits = caps->capability_bits;
  }
  return status;
}

int zz9k_module_status(ZZ9KContext *ctx, uint32_t index,
                       ZZ9KModuleStatus *module)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !module) return ZZ9K_STATUS_BAD_REQUEST;
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_module_status(&request, index);
  if (status != ZZ9K_STATUS_OK) return status;
  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) return status;
  return zz9k_reply_module_status(&reply, module);
}

int zz9k_query_aperture_layout(ZZ9KContext *ctx,
                               ZZ9KApertureLayout *layout)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  ZZ9KApertureLayout decoded;
  int status;

  if (!ctx || !layout) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  memset(layout, 0, sizeof(*layout));
  if (ctx->aperture_layout_valid &&
      (ctx->aperture_layout.profile & ZZ9K_APERTURE_FLAG_ACKED) != 0U) {
    *layout = ctx->aperture_layout;
    return ZZ9K_STATUS_OK;
  }
  memset(&ctx->aperture_layout, 0, sizeof(ctx->aperture_layout));
  ctx->aperture_layout_valid = 0U;
  if (ctx->board.zorro_version != 2U ||
      (ctx->capability_bits & ZZ9K_CAP_APERTURE_LAYOUT) == 0U) {
    return ZZ9K_STATUS_UNSUPPORTED;
  }

  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_query_aperture_layout(&request);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = zz9k_reply_aperture_layout(&reply, &decoded);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  if (!zz9k_aperture_layout_matches(&ctx->board, &decoded)) {
    return ZZ9K_STATUS_INTERNAL_ERROR;
  }

  ctx->aperture_layout = decoded;
  ctx->aperture_layout_valid = 1U;
  *layout = decoded;
  return ZZ9K_STATUS_OK;
}

static int zz9k_refresh_z2_aperture(ZZ9KContext *ctx)
{
  ZZ9KApertureLayout layout;
  ZZ9KCaps caps;
  int status;

  status = zz9k_query_caps(ctx, &caps);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  if ((ctx->capability_bits & ZZ9K_CAP_APERTURE_LAYOUT) == 0U) {
    return ZZ9K_STATUS_UNSUPPORTED;
  }
  if ((ctx->capability_bits & ZZ9K_CAP_HOST_WINDOW_HEAP) == 0U) {
    return ZZ9K_STATUS_UNSUPPORTED;
  }
  return zz9k_query_aperture_layout(ctx, &layout);
}

int zz9k_query_service(ZZ9KContext *ctx, uint32_t service_id,
                       ZZ9KServiceInfo *service)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !service) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(service, 0, sizeof(*service));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_query_service(&request, service_id);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return zz9k_reply_service_info(&reply, service_id, service);
}

int zz9k_ping(ZZ9KContext *ctx, const uint8_t *payload,
              uint32_t payload_len, uint8_t *reply_payload,
              uint32_t *reply_len)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  uint32_t capacity;
  int status;

  if (!ctx || payload_len > sizeof(request.entry.payload.inline_data) ||
      (payload_len != 0 && !payload)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (reply_payload && !reply_len) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  if (reply_len) {
    capacity = *reply_len;
    *reply_len = 0;
  } else {
    capacity = 0;
  }

  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_ping(&request, payload, payload_len);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  if (!reply_payload && !reply_len) {
    return zz9k_reply_require(&reply, ZZ9K_OP_PING, 0U);
  }
  if (reply_len) {
    *reply_len = capacity;
  }

  return zz9k_reply_copy_inline_payload(&reply, ZZ9K_OP_PING,
                                        reply_payload, reply_len);
}

int zz9k_alloc_shared(ZZ9KContext *ctx, uint32_t length, uint32_t alignment,
                      uint32_t flags, ZZ9KSharedBuffer *buffer)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  ZZ9KSharedBufferInfo info;
  uint32_t arm_addr;
  uint32_t board_offset;
  int card_only;
  int negotiated_host_window;
  int status;

  if (!ctx || !buffer || length == 0) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  card_only = (flags & ZZ9K_ALLOC_CARD_ONLY) != 0U;
  /* HOST_WINDOW exists so a Zorro 2 host can map the buffer through its
   * 4 MB board window; the shared heap already maps on Zorro 3, so keep
   * the tiny host-window heap free there. A card-only buffer needs no
   * mapping on any bus. */
  if (card_only || ctx->board.zorro_version != 2) {
    flags &= ~(uint32_t)ZZ9K_ALLOC_HOST_WINDOW;
  }
  negotiated_host_window = 0;
  if ((flags & ZZ9K_ALLOC_HOST_WINDOW) != 0U) {
    if ((ctx->capability_bits & ZZ9K_CAP_APERTURE_LAYOUT) != 0U) {
      if ((ctx->capability_bits & ZZ9K_CAP_HOST_WINDOW_HEAP) == 0U ||
          !ctx->aperture_layout_valid ||
          (ctx->aperture_layout.profile & ZZ9K_APERTURE_FLAG_ACKED) == 0U) {
        status = zz9k_refresh_z2_aperture(ctx);
        if (status != ZZ9K_STATUS_OK) {
          memset(buffer, 0, sizeof(*buffer));
          return status;
        }
      }
      if ((ctx->capability_bits & ZZ9K_CAP_HOST_WINDOW_HEAP) == 0U ||
          !ctx->aperture_layout_valid ||
          (ctx->aperture_layout.profile & ZZ9K_APERTURE_FLAG_ACKED) == 0U) {
        memset(buffer, 0, sizeof(*buffer));
        return ZZ9K_STATUS_UNSUPPORTED;
      }
      negotiated_host_window = 1;
    } else if (ctx->board.board_size != ZZ9K_HOST_WINDOW_MIN_BOARD_SIZE) {
      /* With no layout capability, retain only the historical fixed 4 MB
       * mapping. A 2 MB or unknown/extended aperture must never inherit the
       * old absolute heap placement by accident. */
      memset(buffer, 0, sizeof(*buffer));
      return ZZ9K_STATUS_UNSUPPORTED;
    }
  }

  memset(buffer, 0, sizeof(*buffer));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_alloc_shared(&request, length, alignment, flags);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = zz9k_reply_shared_buffer(&reply, &info);
  if (status != ZZ9K_STATUS_OK) {
    if (zz9k_reply_require(&reply, ZZ9K_OP_ALLOC_SHARED, 16U) ==
        ZZ9K_STATUS_OK) {
      uint32_t returned_handle =
          zz9k_get_be32(&reply.payload.inline_data[0]);

      if (returned_handle != ZZ9K_INVALID_HANDLE) {
        (void)zz9k_free_shared(ctx, returned_handle);
      }
    }
    return status;
  }

  buffer->handle = info.handle;
  arm_addr = info.arm_addr;
  buffer->length = info.length;
  if (!card_only) {
    buffer->data = zz9k_arm_range_to_board_ptr(ctx->board.board_addr,
                                               ctx->board.board_size,
                                               arm_addr, buffer->length);
  }

  if (buffer->handle == ZZ9K_INVALID_HANDLE || buffer->length < length ||
      (!card_only && !buffer->data) ||
      (negotiated_host_window &&
       (info.flags & ZZ9K_ALLOC_HOST_WINDOW) == 0U)) {
    /* The card-side allocation exists even when we cannot use it; free
     * it or every failed attempt permanently leaks a shared-heap block
     * (seen on Zorro 2, where the default heap never maps). */
    if (buffer->handle != ZZ9K_INVALID_HANDLE) {
      (void)zz9k_free_shared(ctx, buffer->handle);
    }
    memset(buffer, 0, sizeof(*buffer));
    return ZZ9K_STATUS_INTERNAL_ERROR;
  }

  if (negotiated_host_window) {
    if (!zz9k_arm_range_to_board_offset(ctx->board.board_size, arm_addr,
                                        buffer->length, &board_offset) ||
        board_offset < ctx->aperture_layout.host_base ||
        board_offset - ctx->aperture_layout.host_base >=
            ctx->aperture_layout.host_size ||
        buffer->length > ctx->aperture_layout.host_size -
                             (board_offset -
                              ctx->aperture_layout.host_base)) {
      (void)zz9k_free_shared(ctx, buffer->handle);
      memset(buffer, 0, sizeof(*buffer));
      return ZZ9K_STATUS_INTERNAL_ERROR;
    }
  }

  return ZZ9K_STATUS_OK;
}

int zz9k_free_shared(ZZ9KContext *ctx, uint32_t handle)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || handle == ZZ9K_INVALID_HANDLE) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_free_shared(&request, handle);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
}

int zz9k_mem_fill(ZZ9KContext *ctx, uint32_t handle, uint32_t offset,
                  uint32_t length, uint8_t value)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || handle == ZZ9K_INVALID_HANDLE) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_mem_fill(&request, handle, offset, length, value);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
}

int zz9k_mem_copy(ZZ9KContext *ctx, uint32_t dst_handle, uint32_t dst_offset,
                  uint32_t src_handle, uint32_t src_offset, uint32_t length)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || dst_handle == ZZ9K_INVALID_HANDLE ||
      src_handle == ZZ9K_INVALID_HANDLE) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_mem_copy(&request, dst_handle, dst_offset,
                                 src_handle, src_offset, length);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
}

static int zz9k_surface_from_reply(ZZ9KContext *ctx,
                                   const ZZ9KMailboxEntry *reply,
                                   uint16_t expected_opcode,
                                   ZZ9KSurface *surface)
{
  int is_framebuffer;
  int is_arm_local;
  int status;

  status = zz9k_reply_surface(reply, expected_opcode, surface);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  is_framebuffer = surface->handle == ZZ9K_SURFACE_HANDLE_FRAMEBUFFER;
  is_arm_local = (surface->flags & ZZ9K_SURFACE_FLAG_ARM_LOCAL) != 0U;
  surface->data = zz9k_arm_range_to_board_ptr(ctx->board.board_addr,
                                              ctx->board.board_size,
                                              surface->arm_addr,
                                              surface->length);

  if (surface->handle == ZZ9K_INVALID_HANDLE || surface->width == 0 ||
      surface->height == 0 || surface->pitch == 0 || surface->length == 0 ||
      (!is_framebuffer && !is_arm_local && !surface->data)) {
    memset(surface, 0, sizeof(*surface));
    return ZZ9K_STATUS_INTERNAL_ERROR;
  }

  return ZZ9K_STATUS_OK;
}

int zz9k_alloc_surface_ex(ZZ9KContext *ctx, uint32_t width, uint32_t height,
                          uint32_t format, uint32_t flags, uint32_t pitch,
                          ZZ9KSurface *surface)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !surface || width == 0 || height == 0) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(surface, 0, sizeof(*surface));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_alloc_surface_ex(&request, width, height, format,
                                         flags, pitch);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_surface_from_reply(ctx, &reply, ZZ9K_OP_ALLOC_SURFACE,
                                 surface);
}

int zz9k_alloc_surface(ZZ9KContext *ctx, uint32_t width, uint32_t height,
                       uint32_t format, uint32_t flags, ZZ9KSurface *surface)
{
  return zz9k_alloc_surface_ex(ctx, width, height, format, flags, 0, surface);
}

int zz9k_free_surface(ZZ9KContext *ctx, uint32_t handle)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || handle == ZZ9K_INVALID_HANDLE ||
      handle == ZZ9K_SURFACE_HANDLE_FRAMEBUFFER) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_free_surface(&request, handle);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
}

int zz9k_map_framebuffer_surface(ZZ9KContext *ctx, ZZ9KSurface *surface)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !surface) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(surface, 0, sizeof(*surface));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_map_framebuffer_surface(&request);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_surface_from_reply(ctx, &reply,
                                 ZZ9K_OP_MAP_FRAMEBUFFER_SURFACE,
                                 surface);
}

int zz9k_scale_image(ZZ9KContext *ctx, const ZZ9KScaleImageDesc *desc)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !desc || desc->src_w == 0 || desc->src_h == 0 ||
      desc->dst_w == 0 || desc->dst_h == 0) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_scale_image(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
}

int zz9k_scale_image_clipped(ZZ9KContext *ctx,
                             const ZZ9KScaleImageClippedDesc *desc)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !desc || desc->src_w == 0U || desc->src_h == 0U ||
      desc->dst_w == 0U || desc->dst_h == 0U ||
      desc->clip_w == 0U || desc->clip_h == 0U) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_scale_image_clipped(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
}

int zz9k_fill_surface(ZZ9KContext *ctx, const ZZ9KSurfaceFillDesc *desc)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !desc || desc->surface == ZZ9K_INVALID_HANDLE ||
      desc->width == 0U || desc->height == 0U) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_fill_surface(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
}

int zz9k_copy_surface(ZZ9KContext *ctx, const ZZ9KSurfaceCopyDesc *desc)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !desc || desc->src_surface == ZZ9K_INVALID_HANDLE ||
      desc->dst_surface == ZZ9K_INVALID_HANDLE || desc->width == 0U ||
      desc->height == 0U) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_copy_surface(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
}

int zz9k_query_palette(ZZ9KContext *ctx, const ZZ9KPaletteQueryDesc *desc)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  /* The builder revalidates; checking here too keeps the failure local for
   * a caller that passed a null context. */
  if (!ctx || !desc) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_query_palette(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
}

int zz9k_decode_image(ZZ9KContext *ctx, uint32_t opcode,
                      const ZZ9KImageDecodeDesc *desc,
                      ZZ9KImageDecodeResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !desc || !result || opcode > 0xffffU) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_decode_image(&request, (uint16_t)opcode, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_reply_image_decode_result(&reply, (uint16_t)opcode, result);
}

int zz9k_decode_jpeg(ZZ9KContext *ctx, const ZZ9KImageDecodeDesc *desc,
                     ZZ9KImageDecodeResult *result)
{
  return zz9k_decode_image(ctx, ZZ9K_OP_DECODE_JPEG, desc, result);
}

int zz9k_decode_png(ZZ9KContext *ctx, const ZZ9KImageDecodeDesc *desc,
                    ZZ9KImageDecodeResult *result)
{
  return zz9k_decode_image(ctx, ZZ9K_OP_DECODE_PNG, desc, result);
}

int zz9k_decode_gif(ZZ9KContext *ctx, const ZZ9KImageDecodeDesc *desc,
                    ZZ9KImageDecodeResult *result)
{
  return zz9k_decode_image(ctx, ZZ9K_OP_DECODE_GIF, desc, result);
}

int zz9k_decode_mp3(ZZ9KContext *ctx, const ZZ9KAudioDecodeDesc *desc,
                    ZZ9KAudioDecodeResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !desc || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_decode_mp3(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_reply_audio_decode_result(&reply, result);
}

int zz9k_audio_stream_begin(ZZ9KContext *ctx,
                            const ZZ9KAudioStreamBeginDesc *desc,
                            ZZ9KAudioStreamResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !desc || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_audio_stream_begin(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return zz9k_reply_audio_stream_result(&reply,
                                        ZZ9K_OP_AUDIO_STREAM_BEGIN,
                                        result);
}

int zz9k_audio_stream_begin_ex(ZZ9KContext *ctx,
                               const ZZ9KAudioStreamBeginExDesc *desc,
                               ZZ9KAudioStreamResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !desc || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_audio_stream_begin_ex(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return zz9k_reply_audio_stream_result(&reply,
                                        ZZ9K_OP_AUDIO_STREAM_BEGIN_EX,
                                        result);
}

int zz9k_audio_stream_feed(ZZ9KContext *ctx,
                           const ZZ9KAudioStreamFeedDesc *desc,
                           ZZ9KAudioStreamResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !desc || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_audio_stream_feed(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return zz9k_reply_audio_stream_result(&reply,
                                        ZZ9K_OP_AUDIO_STREAM_FEED,
                                        result);
}

int zz9k_audio_stream_read(ZZ9KContext *ctx, uint32_t session,
                           uint32_t pcm_read, uint32_t flags,
                           ZZ9KAudioStreamResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_audio_stream_read(&request, session, pcm_read,
                                          flags);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return zz9k_reply_audio_stream_result(&reply,
                                        ZZ9K_OP_AUDIO_STREAM_READ,
                                        result);
}

int zz9k_audio_stream_close(ZZ9KContext *ctx, uint32_t session,
                            uint32_t flags,
                            ZZ9KAudioStreamResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_audio_stream_close(&request, session, flags);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return zz9k_reply_audio_stream_result(&reply,
                                        ZZ9K_OP_AUDIO_STREAM_CLOSE,
                                        result);
}

int zz9k_audio_stream_play(ZZ9KContext *ctx, uint32_t session,
                           uint32_t flags,
                           ZZ9KAudioStreamResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_audio_stream_play(&request, session, flags);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return zz9k_reply_audio_stream_result(&reply,
                                        ZZ9K_OP_AUDIO_STREAM_PLAY,
                                        result);
}

int zz9k_audio_stream_set_gain(ZZ9KContext *ctx, uint32_t session,
                               uint32_t gain, uint32_t flags,
                               ZZ9KAudioStreamResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_audio_stream_set_gain(&request, session, gain, flags);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return zz9k_reply_audio_stream_result(&reply,
                                        ZZ9K_OP_AUDIO_STREAM_GAIN,
                                        result);
}
int zz9k_audio_stream_stop(ZZ9KContext *ctx, uint32_t session,
                           uint32_t flags,
                           ZZ9KAudioStreamResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_audio_stream_stop(&request, session, flags);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return zz9k_reply_audio_stream_result(&reply,
                                        ZZ9K_OP_AUDIO_STREAM_STOP,
                                        result);
}

int zz9k_audio_ring_acquire(ZZ9KContext *ctx,
                            const ZZ9KAudioRingAcquireDesc *desc,
                            ZZ9KAudioRingAcquireResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  uint32_t granted_slot = 0U;
  uint32_t granted_generation = 0U;
  int grant_identified;
  int status;

  if (!ctx || !desc || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_audio_ring_acquire(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  /* Preserve generation-bound ownership before the strict decoder can
   * clear an otherwise complete but invalid grant. A malformed opcode,
   * status, or payload never identifies a slot to release. */
  grant_identified =
      zz9k_reply_require(&reply, ZZ9K_OP_AUDIO_RING_ACQUIRE,
                         sizeof(ZZ9KAudioRingAcquireResultPayload));
  if (grant_identified == ZZ9K_STATUS_OK) {
    granted_slot = zz9k_get_be32(&reply.payload.inline_data[0]);
    granted_generation = zz9k_get_be32(&reply.payload.inline_data[4]);
  }
  status = zz9k_reply_audio_ring_acquire_result(&reply, result);
  if (status != ZZ9K_STATUS_OK) {
    if (grant_identified == ZZ9K_STATUS_OK &&
        granted_slot >= 1U &&
        granted_slot <= ZZ9K_AUDIO_RING_SLOT_MAX &&
        granted_generation != 0U) {
      (void)zz9k_audio_ring_release(ctx, granted_slot,
                                    granted_generation, 0U);
    }
    return status;
  }
  /* A valid-but-different contract or rate is a firmware fault
   * (PR #31 review): the grant must be exactly what was asked for.
   * Release the slot so it does not stay BUSY until heartbeat
   * revocation, and fail closed with a zeroed result. */
  if (!zz9k_audio_ring_grant_matches_desc(desc, result)) {
    (void)zz9k_audio_ring_release(ctx, result->slot, result->generation,
                                  0U);
    memset(result, 0, sizeof(*result));
    return ZZ9K_STATUS_INTERNAL_ERROR;
  }
  return ZZ9K_STATUS_OK;
}

int zz9k_audio_ring_release(ZZ9KContext *ctx, uint32_t slot,
                            uint32_t generation, uint32_t flags)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_audio_ring_release(&request, slot, generation,
                                           flags);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  /* RELEASE reserves no result payload; the status word is the whole
   * contract (OK, or BAD_HANDLE for a stale generation). */
  return zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
}

int zz9k_audio_fabric_state_get(
    ZZ9KContext *ctx, const ZZ9KAudioFabricStateDesc *desc,
    ZZ9KAudioFabricStateResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !desc || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_audio_fabric_state_get(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return zz9k_reply_audio_fabric_state_result(&reply, result);
}

int zz9k_audio_output_clock_get(ZZ9KContext *ctx,
                                ZZ9KAudioOutputClockResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  status = zz9k_request_audio_output_clock(&request);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return zz9k_reply_audio_output_clock_result(&reply, result);
}

int zz9k_audio_ring_session_begin(ZZ9KContext *ctx,
                                  const ZZ9KAudioRingAcquireDesc *desc,
                                  ZZ9KAudioRingSession *session)
{
  volatile uint8_t *control;
  int status;

  if (!ctx || !desc || !session) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  /* Fail closed: a refused acquisition (bus policy, occupied slot,
   * unadvertised plane) or any later validation error leaves the
   * session zeroed, so the data-path helpers stay dead and no
   * pointer is ever exposed from an unusable grant. */
  memset(session, 0, sizeof(*session));
  status = zz9k_audio_ring_acquire(ctx, desc, &session->grant);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  /* The reply decoder already validated the grant (R3); mapping is
   * the host's own check: both granted ranges must sit inside the
   * active board aperture (R2) before any pointer is derived. On
   * Zorro II that means the reported aperture layout as well -- a
   * direct-ring grant may only live in unreported firmware-reserved
   * space, never inside the framebuffer, PIP, template, host-window
   * heap, or audio regions. Zorro III has no layout query: the
   * board-window bound is the whole check. */
  if (!zz9k_audio_ring_grant_valid(&session->grant) ||
      !zz9k_board_range_fits(ctx->board.board_size,
                             session->grant.ring_offset,
                             session->grant.ring_capacity) ||
      !zz9k_board_range_fits(ctx->board.board_size,
                             session->grant.control_offset,
                             ZZ9K_AUDIO_RING_CONTROL_SIZE)) {
    /* The slot is already leased firmware-side; release it or the next
     * acquire stays BUSY until heartbeat revocation (PR #29 review). */
    (void)zz9k_audio_ring_release(ctx, session->grant.slot,
                                  session->grant.generation, 0U);
    memset(session, 0, sizeof(*session));
    return ZZ9K_STATUS_INTERNAL_ERROR;
  }
  if (ctx->board.zorro_version == 2U) {
    ZZ9KApertureLayout layout;

    status = zz9k_query_aperture_layout(ctx, &layout);
    if (status != ZZ9K_STATUS_OK ||
        !zz9k_aperture_range_free(&layout, session->grant.ring_offset,
                                  session->grant.ring_capacity) ||
        !zz9k_aperture_range_free(&layout, session->grant.control_offset,
                                  ZZ9K_AUDIO_RING_CONTROL_SIZE)) {
      (void)zz9k_audio_ring_release(ctx, session->grant.slot,
                                    session->grant.generation, 0U);
      memset(session, 0, sizeof(*session));
      return ZZ9K_STATUS_INTERNAL_ERROR;
    }
  }

  session->ring = (volatile uint8_t *)(uintptr_t)ctx->board.board_addr +
                  session->grant.ring_offset;
  control = (volatile uint8_t *)(uintptr_t)ctx->board.board_addr +
            session->grant.control_offset;
  session->producer_line =
      (volatile ZZ9KAudioRingProducerLine *)(void *)control;
  session->firmware_line =
      (volatile ZZ9KAudioRingFirmwareLine *)(void *)(control +
          ZZ9K_AUDIO_RING_CONTROL_LINE_SIZE);
  session->write_cursor = 0U;
  session->consumed_cursor = 0U;
  session->heartbeat = 1U;
  session->flags = 0U;
  session->mapped = 1U;

  /* First publication: cursor 0, fresh heartbeat. The lease is live
   * from acquisition (R11) even before any PCM is staged; firmware
   * consumes complete periods only, so a zero cursor publishes
   * nothing. A torn first credit read is harmless -- it leaves the
   * consumed cursor at firmware's initialized zero. */
  zz9k_audio_ring_publish(session);
  (void)zz9k_audio_ring_take_credits(session, 4U);
  return ZZ9K_STATUS_OK;
}

int zz9k_audio_ring_session_end(ZZ9KContext *ctx,
                                ZZ9KAudioRingSession *session)
{
  uint32_t slot;
  uint32_t generation;
  int status;

  if (!ctx || !session || !session->mapped) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  slot = session->grant.slot;
  generation = session->grant.generation;
  memset(session, 0, sizeof(*session));
  /* Stale generations release idempotently: the slot may already
   * have been revoked (R4, R13). */
  status = zz9k_audio_ring_release(ctx, slot, generation, 0U);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return ZZ9K_STATUS_OK;
}

/* ---- Wavetable voice engine (zz9k/voice.h) ---- */

static int zz9k_voice_alloc_buffer(ZZ9KContext *ctx, uint32_t length,
                                   ZZ9KSharedBuffer *buffer)
{
  int status = zz9k_alloc_shared(ctx, length, ZZ9K_VOICE_BUFFER_ALIGN,
                                 ZZ9K_ALLOC_HOST_WINDOW, buffer);

  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  /* The ring cursor is one 16-bit store and the status words are read as
   * 16-bit halves, so the board pointers must be even. The firmware
   * alignment request already makes them 64-byte aligned. */
  if ((((uintptr_t)buffer->data) & 3U) != 0U) {
    (void)zz9k_free_shared(ctx, buffer->handle);
    memset(buffer, 0, sizeof(*buffer));
    return ZZ9K_STATUS_INTERNAL_ERROR;
  }
  return ZZ9K_STATUS_OK;
}

/* Frees whichever of the three handles is set; returns the first failure. */
static int zz9k_voice_free_buffers(ZZ9KContext *ctx, uint32_t ring_handle,
                                   uint32_t status_handle,
                                   uint32_t staging_handle)
{
  uint32_t handles[3];
  int result = ZZ9K_STATUS_OK;
  uint32_t i;

  handles[0] = ring_handle;
  handles[1] = status_handle;
  handles[2] = staging_handle;
  for (i = 0U; i < 3U; i++) {
    if (handles[i] != ZZ9K_INVALID_HANDLE) {
      int status = zz9k_free_shared(ctx, handles[i]);

      if (result == ZZ9K_STATUS_OK) {
        result = status;
      }
    }
  }
  return result;
}

static int zz9k_voice_session_call(ZZ9KContext *ctx, uint16_t opcode,
                                   uint32_t session)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_audio_voice_session(&request, opcode, session);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
}

int zz9k_voice_open(ZZ9KContext *ctx, const ZZ9KVoiceOpenDesc *desc,
                    ZZ9KVoiceSession *session)
{
  ZZ9KSharedBuffer ring;
  ZZ9KSharedBuffer status_block;
  ZZ9KSharedBuffer staging;
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  ZZ9KVoiceGrant grant;
  uint32_t ring_bytes;
  uint32_t staging_bytes;
  uint32_t raw_session = 0U;
  int status;

  if (!ctx || !desc || !session) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  memset(session, 0, sizeof(*session));

  ring_bytes = desc->ring_bytes != 0U ? desc->ring_bytes
                                      : ZZ9K_VOICE_RING_DEFAULT_BYTES;
  staging_bytes = desc->staging_bytes != 0U
                      ? desc->staging_bytes
                      : ZZ9K_VOICE_STAGING_DEFAULT_BYTES;
  if (ring_bytes < ZZ9K_VOICE_RING_MIN_BYTES ||
      ring_bytes > ZZ9K_VOICE_RING_MAX_BYTES || staging_bytes < 4U) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  /* Validate the descriptor before any allocation: the builder checks the
   * request fields and only needs three distinct handles for that. */
  status = zz9k_request_audio_voice_open(&request, 1U, 2U, 3U, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  memset(&ring, 0, sizeof(ring));
  memset(&status_block, 0, sizeof(status_block));
  memset(&staging, 0, sizeof(staging));
  status = zz9k_voice_alloc_buffer(ctx, ring_bytes, &ring);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = zz9k_voice_alloc_buffer(ctx, ZZ9K_VOICE_STATUS_BYTES,
                                   &status_block);
  if (status != ZZ9K_STATUS_OK) {
    (void)zz9k_voice_free_buffers(ctx, ring.handle, ZZ9K_INVALID_HANDLE,
                                  ZZ9K_INVALID_HANDLE);
    return status;
  }
  status = zz9k_voice_alloc_buffer(ctx, staging_bytes, &staging);
  if (status != ZZ9K_STATUS_OK) {
    (void)zz9k_voice_free_buffers(ctx, ring.handle, status_block.handle,
                                  ZZ9K_INVALID_HANDLE);
    return status;
  }

  /* The firmware reads cursor 0 and a clean status block at open. */
  (void)zz9k_shared_set(&ring, 0U, 0U, ZZ9K_VOICE_RING_HEADER_BYTES);
  zz9k_put_be32((volatile uint8_t *)ring.data +
                    ZZ9K_VOICE_RING_HEARTBEAT_OFFSET,
                1U);
  (void)zz9k_shared_set(&status_block, 0U, 0U, ZZ9K_VOICE_STATUS_BYTES);
  zz9k_audio_ring_cache_flush(ring.data, ZZ9K_VOICE_RING_HEADER_BYTES);
  zz9k_audio_ring_cache_flush(status_block.data, ZZ9K_VOICE_STATUS_BYTES);

  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_audio_voice_open(&request, ring.handle,
                                         status_block.handle,
                                         staging.handle, desc);
  if (status == ZZ9K_STATUS_OK) {
    status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  }
  if (status == ZZ9K_STATUS_OK) {
    /* Remember the session a malformed grant named, so it can be closed
     * instead of leaking the firmware's bank. */
    if (zz9k_reply_require(&reply, ZZ9K_OP_AUDIO_VOICE_OPEN,
                           sizeof(ZZ9KAudioVoiceOpenResultPayload)) ==
        ZZ9K_STATUS_OK) {
      raw_session = zz9k_get_be32(&reply.payload.inline_data[0]);
    }
    status = zz9k_reply_audio_voice_open_result(&reply, &grant);
  }
  if (status == ZZ9K_STATUS_OK &&
      grant.ring_records != zz9k_voice_ring_records_for_bytes(ring_bytes)) {
    status = ZZ9K_STATUS_INTERNAL_ERROR;
  }
  if (status != ZZ9K_STATUS_OK) {
    if (raw_session != 0U) {
      (void)zz9k_voice_session_call(ctx, ZZ9K_OP_AUDIO_VOICE_CLOSE,
                                    raw_session);
    }
    (void)zz9k_voice_free_buffers(ctx, ring.handle, status_block.handle,
                                  staging.handle);
    memset(session, 0, sizeof(*session));
    return status;
  }

  session->grant = grant;
  session->ring_handle = ring.handle;
  session->status_handle = status_block.handle;
  session->staging_handle = staging.handle;
  session->ring_bytes = ring.length;
  session->staging_bytes = staging.length;
  session->ring = (volatile uint8_t *)ring.data;
  session->status = (volatile uint8_t *)status_block.data;
  session->staging = (volatile uint8_t *)staging.data;
  session->write_cursor = 0U;
  session->heartbeat = 1U;
  session->mapped = 1U;
  return ZZ9K_STATUS_OK;
}

int zz9k_voice_close(ZZ9KContext *ctx, ZZ9KVoiceSession *session)
{
  uint32_t ring_handle;
  uint32_t status_handle;
  uint32_t staging_handle;
  int status;
  int free_status;

  if (!ctx || !session || !session->mapped) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  ring_handle = session->ring_handle;
  status_handle = session->status_handle;
  staging_handle = session->staging_handle;
  status = zz9k_voice_session_call(ctx, ZZ9K_OP_AUDIO_VOICE_CLOSE,
                                   session->grant.session);
  /* A stale session (the firmware already closed it on a missed heartbeat)
   * has unpinned the buffers too, so they are freed either way. */
  free_status = zz9k_voice_free_buffers(ctx, ring_handle, status_handle,
                                        staging_handle);
  memset(session, 0, sizeof(*session));
  return status != ZZ9K_STATUS_OK ? status : free_status;
}

int zz9k_voice_reset(ZZ9KContext *ctx, ZZ9KVoiceSession *session)
{
  if (!ctx || !session || !session->mapped) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  return zz9k_voice_session_call(ctx, ZZ9K_OP_AUDIO_VOICE_RESET,
                                 session->grant.session);
}

int zz9k_voice_upload(ZZ9KContext *ctx, ZZ9KVoiceSession *session,
                      const ZZ9KVoiceUploadDesc *desc)
{
  ZZ9KSharedBuffer staging;
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  const uint8_t *src;
  uint32_t done = 0U;
  uint32_t chunk;
  int status;

  if (!ctx || !session || !session->mapped || !desc || !desc->data ||
      desc->length == 0U ||
      desc->sample_id >= session->grant.max_samples ||
      desc->bank_offset > session->grant.bank_bytes ||
      desc->length > session->grant.bank_bytes - desc->bank_offset) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  /* Refuse a sample the DEFINE would reject before any byte is copied. */
  status = zz9k_request_audio_voice_upload(
      &request, session->grant.session, ZZ9K_VOICE_UPLOAD_FLAG_DEFINE, 0U, 0U,
      0U, desc->sample_id, desc->bank_offset, desc->length, desc->format);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  staging.handle = session->staging_handle;
  staging.data = session->staging;
  staging.length = session->staging_bytes;
  src = (const uint8_t *)desc->data;
  while (done < desc->length) {
    chunk = desc->length - done;
    if (chunk > session->staging_bytes) {
      /* Whole longwords per round trip keeps the staging copy aligned. */
      chunk = session->staging_bytes & ~3U;
    }
    if (!zz9k_shared_copy_to(&staging, 0U, src + done, chunk)) {
      return ZZ9K_STATUS_INTERNAL_ERROR;
    }
    zz9k_audio_ring_cache_flush(staging.data, chunk);
    status = zz9k_request_audio_voice_upload(
        &request, session->grant.session, ZZ9K_VOICE_UPLOAD_FLAG_COPY, 0U,
        desc->bank_offset + done, chunk, 0U, 0U, 0U, 0U);
    if (status == ZZ9K_STATUS_OK) {
      memset(&reply, 0, sizeof(reply));
      status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
    }
    if (status != ZZ9K_STATUS_OK) {
      return status;
    }
    done += chunk;
  }

  status = zz9k_request_audio_voice_upload(
      &request, session->grant.session, ZZ9K_VOICE_UPLOAD_FLAG_DEFINE, 0U, 0U,
      0U, desc->sample_id, desc->bank_offset, desc->length, desc->format);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  memset(&reply, 0, sizeof(reply));
  return zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
}

uint32_t zz9k_voice_write(ZZ9KVoiceSession *session,
                          const ZZ9KVoiceRecord *records, uint32_t count)
{
  uint16_t read_cursor;
  uint32_t staged;

  if (!session || !session->mapped || !records || count == 0U) {
    return 0U;
  }
  zz9k_audio_ring_cache_invalidate(session->status, ZZ9K_VOICE_STATUS_BYTES);
  read_cursor = zz9k_voice_load_be16(session->status +
                                     ZZ9K_VOICE_STATUS_READ_CURSOR_OFFSET);
  staged = zz9k_voice_ring_stage(session, read_cursor, records, count);
  if (staged == 0U) {
    return 0U;
  }
  /* Records reach the card before the cursor that announces them. */
  zz9k_audio_ring_cache_flush(session->ring + ZZ9K_VOICE_RING_HEADER_BYTES,
                              session->ring_bytes -
                                  ZZ9K_VOICE_RING_HEADER_BYTES);
  zz9k_voice_ring_publish(session);
  zz9k_audio_ring_cache_flush(session->ring, ZZ9K_VOICE_RING_HEADER_BYTES);
  return staged;
}

int zz9k_voice_status(ZZ9KVoiceSession *session, ZZ9KVoiceStatus *status)
{
  if (!session || !session->mapped || !status) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  zz9k_audio_ring_cache_invalidate(session->status, ZZ9K_VOICE_STATUS_BYTES);
  zz9k_voice_status_read(session->status, status);
  session->heartbeat++;
  zz9k_put_be32(session->ring + ZZ9K_VOICE_RING_HEARTBEAT_OFFSET,
                session->heartbeat);
  zz9k_audio_ring_cache_flush(session->ring, ZZ9K_VOICE_RING_HEADER_BYTES);
  return ZZ9K_STATUS_OK;
}

static int zz9k_video_session_call(ZZ9KContext *ctx,
                                   ZZ9KRequest *request,
                                   uint16_t opcode,
                                   ZZ9KVideoSessionResult *result)
{
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !request || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_call(ctx, request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return zz9k_reply_video_session_result(&reply, opcode, result);
}

int zz9k_video_session_begin(ZZ9KContext *ctx,
                             const ZZ9KVideoSessionBeginDesc *desc,
                             ZZ9KVideoSessionResult *result)
{
  ZZ9KRequest request;
  int status;

  if (!ctx || !desc || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  status = zz9k_request_video_session_begin(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return zz9k_video_session_call(ctx, &request,
                                 ZZ9K_OP_VIDEO_SESSION_BEGIN, result);
}

int zz9k_video_session_write(ZZ9KContext *ctx,
                             const ZZ9KVideoSessionWriteDesc *desc,
                             ZZ9KVideoSessionResult *result)
{
  ZZ9KRequest request;
  int status;

  if (!ctx || !desc || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  status = zz9k_request_video_session_write(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return zz9k_video_session_call(ctx, &request,
                                 ZZ9K_OP_VIDEO_SESSION_WRITE, result);
}

int zz9k_video_session_decode(ZZ9KContext *ctx,
                              const ZZ9KVideoSessionDecodeDesc *desc,
                              ZZ9KVideoSessionResult *result)
{
  ZZ9KRequest request;
  int status;

  if (!ctx || !desc || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  status = zz9k_request_video_session_decode(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return zz9k_video_session_call(ctx, &request,
                                 ZZ9K_OP_VIDEO_SESSION_DECODE, result);
}

int zz9k_video_session_close(ZZ9KContext *ctx, uint32_t session,
                             uint32_t flags,
                             ZZ9KVideoSessionResult *result)
{
  ZZ9KRequest request;
  int status;

  if (!ctx || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  status = zz9k_request_video_session_close(&request, session, flags);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return zz9k_video_session_call(ctx, &request,
                                 ZZ9K_OP_VIDEO_SESSION_CLOSE, result);
}

static int zz9k_media_session_main_call(
    ZZ9KContext *ctx, ZZ9KRequest *request, uint16_t opcode,
    ZZ9KMediaSessionMainResult *result)
{
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !request || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_call(ctx, request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return zz9k_reply_media_session_main(&reply, opcode, result);
}

int zz9k_media_session_begin(ZZ9KContext *ctx,
                             const ZZ9KMediaSessionBeginDesc *desc,
                             ZZ9KMediaSessionMainResult *result)
{
  ZZ9KRequest request;
  int status;

  if (!ctx || !desc || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  status = zz9k_request_media_session_begin(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return zz9k_media_session_main_call(
      ctx, &request, ZZ9K_OP_MEDIA_SESSION_BEGIN, result);
}

int zz9k_media_session_write(ZZ9KContext *ctx,
                             const ZZ9KMediaSessionWriteDesc *desc,
                             ZZ9KMediaSessionMainResult *result)
{
  ZZ9KRequest request;
  int status;

  if (!ctx || !desc || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  status = zz9k_request_media_session_write(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return zz9k_media_session_main_call(
      ctx, &request, ZZ9K_OP_MEDIA_SESSION_WRITE, result);
}

static int zz9k_media_session_simple_main(
    ZZ9KContext *ctx, uint16_t opcode, uint32_t session, uint32_t flags,
    ZZ9KMediaSessionMainResult *result)
{
  ZZ9KRequest request;
  int status;

  if (!ctx || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  status = zz9k_request_media_session_command(
      &request, opcode, session, flags);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return zz9k_media_session_main_call(ctx, &request, opcode, result);
}

int zz9k_media_session_decode(ZZ9KContext *ctx, uint32_t session,
                              uint32_t flags,
                              ZZ9KMediaSessionMainResult *result)
{
  return zz9k_media_session_simple_main(
      ctx, ZZ9K_OP_MEDIA_SESSION_DECODE, session, flags, result);
}

int zz9k_media_session_present(ZZ9KContext *ctx, uint32_t session,
                               uint32_t flags,
                               ZZ9KMediaSessionMainResult *result)
{
  return zz9k_media_session_simple_main(
      ctx, ZZ9K_OP_MEDIA_SESSION_PRESENT, session, flags, result);
}

int zz9k_media_session_discard(ZZ9KContext *ctx, uint32_t session,
                               uint32_t flags,
                               ZZ9KMediaSessionMainResult *result)
{
  return zz9k_media_session_simple_main(
      ctx, ZZ9K_OP_MEDIA_SESSION_DISCARD, session, flags, result);
}

int zz9k_media_session_close(ZZ9KContext *ctx, uint32_t session,
                             uint32_t flags,
                             ZZ9KMediaSessionMainResult *result)
{
  return zz9k_media_session_simple_main(
      ctx, ZZ9K_OP_MEDIA_SESSION_CLOSE, session, flags, result);
}

int zz9k_media_session_status(ZZ9KContext *ctx, uint32_t session,
                              uint32_t page, uint32_t flags,
                              ZZ9KMediaSessionStatusResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  status = zz9k_request_media_session_status(
      &request, session, page, flags);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return zz9k_reply_media_session_status(&reply, result);
}

static int zz9k_media_session_audio_call(
    ZZ9KContext *ctx, uint16_t opcode, uint32_t session,
    uint64_t value, uint32_t flags, ZZ9KMediaSessionAudioResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  status = zz9k_request_media_session_command_value(
      &request, opcode, session, value, flags);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return zz9k_reply_media_session_audio(&reply, opcode, result);
}

int zz9k_media_session_audio_read(ZZ9KContext *ctx, uint32_t session,
                                  uint64_t acknowledged, uint32_t flags,
                                  ZZ9KMediaSessionAudioResult *result)
{
  return zz9k_media_session_audio_call(
      ctx, ZZ9K_OP_MEDIA_SESSION_AUDIO_READ, session,
      acknowledged, flags, result);
}

int zz9k_media_session_audio_bind(ZZ9KContext *ctx, uint32_t session,
                                  uint32_t flags,
                                  ZZ9KMediaSessionAudioResult *result)
{
  return zz9k_media_session_audio_call(
      ctx, ZZ9K_OP_MEDIA_SESSION_AUDIO_BIND, session, 0U, flags, result);
}

int zz9k_media_session_audio_unbind(ZZ9KContext *ctx, uint32_t session,
                                    uint32_t flags,
                                    ZZ9KMediaSessionAudioResult *result)
{
  return zz9k_media_session_audio_call(
      ctx, ZZ9K_OP_MEDIA_SESSION_AUDIO_UNBIND, session, 0U, flags, result);
}

int zz9k_image_session_begin(ZZ9KContext *ctx,
                             const ZZ9KImageSessionBeginDesc *desc,
                             ZZ9KImageSessionResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !desc || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_image_session_begin(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_reply_image_session_result(
      &reply, ZZ9K_OP_IMAGE_SESSION_BEGIN, result);
}

int zz9k_image_session_feed(ZZ9KContext *ctx,
                            const ZZ9KImageSessionFeedDesc *desc,
                            ZZ9KImageSessionResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !desc || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_image_session_feed(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_reply_image_session_result(
      &reply, ZZ9K_OP_IMAGE_SESSION_FEED, result);
}

int zz9k_image_session_close(ZZ9KContext *ctx, uint32_t session,
                             uint32_t flags)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_image_session_close(&request, session, flags);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
}
int zz9k_image_animation_frame_next(ZZ9KContext *ctx, uint32_t session,
                                    uint32_t flags,
                                    ZZ9KImageAnimationFrameResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_image_animation_frame_next(&request, session, flags);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_reply_image_animation_frame_result(
      &reply, ZZ9K_OP_IMAGE_ANIMATION_FRAME_NEXT, result);
}

int zz9k_image_animation_frame_present(ZZ9KContext *ctx, uint32_t session,
                                       uint32_t frame_token, uint32_t flags,
                                       ZZ9KImageAnimationFrameResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_image_animation_frame_present(
      &request, session, frame_token, flags);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_reply_image_animation_frame_result(
      &reply, ZZ9K_OP_IMAGE_ANIMATION_FRAME_PRESENT, result);
}

int zz9k_image_animation_frame_retire(ZZ9KContext *ctx, uint32_t session,
                                      uint32_t frame_token, uint32_t flags,
                                      ZZ9KImageAnimationFrameResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_image_animation_frame_retire(
      &request, session, frame_token, flags);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_reply_image_animation_frame_result(
      &reply, ZZ9K_OP_IMAGE_ANIMATION_FRAME_RETIRE, result);
}

int zz9k_image_animation_restart(ZZ9KContext *ctx, uint32_t session,
                                 uint32_t flags,
                                 ZZ9KImageAnimationFrameResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_image_animation_restart(&request, session, flags);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_reply_image_animation_frame_result(
      &reply, ZZ9K_OP_IMAGE_ANIMATION_RESTART, result);
}


int zz9k_crypto_hash(ZZ9KContext *ctx, const ZZ9KCryptoHashDesc *desc,
                     ZZ9KCryptoResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !desc || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_crypto_hash(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_reply_crypto_result(&reply, ZZ9K_OP_CRYPTO_HASH, result);
}

int zz9k_crypto_stream(ZZ9KContext *ctx,
                       const ZZ9KCryptoStreamDesc *desc,
                       ZZ9KCryptoResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !desc || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_crypto_stream(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_reply_crypto_result(&reply, ZZ9K_OP_CRYPTO_STREAM, result);
}

int zz9k_crypto_aead(ZZ9KContext *ctx, const ZZ9KCryptoAeadDesc *desc,
                     ZZ9KCryptoResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !desc || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_crypto_aead(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_reply_crypto_result(&reply, ZZ9K_OP_CRYPTO_AEAD, result);
}

int zz9k_crypto_kx(ZZ9KContext *ctx, const ZZ9KCryptoKxDesc *desc,
                   ZZ9KCryptoResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !desc || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_crypto_kx(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_reply_crypto_result(&reply, ZZ9K_OP_CRYPTO_KX, result);
}

int zz9k_crypto_verify(ZZ9KContext *ctx, const ZZ9KCryptoVerifyDesc *desc,
                       int *valid)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !desc || !valid) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  *valid = 0;
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_crypto_verify(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_reply_crypto_verify(&reply, valid);
}

int zz9k_decompress(ZZ9KContext *ctx, const ZZ9KDecompressDesc *desc,
                    ZZ9KDecompressResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !desc || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_decompress(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_reply_decompress_result(&reply, result);
}

int zz9k_decompress_batch(ZZ9KContext *ctx,
                          const ZZ9KDecompressBatchDesc *desc,
                          ZZ9KDecompressBatchResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !desc || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_decompress_batch(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_reply_decompress_batch_result(&reply, result);
}

int zz9k_decompress_test(ZZ9KContext *ctx,
                         const ZZ9KDecompressTestDesc *desc,
                         ZZ9KDecompressResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !desc || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_decompress_test(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_reply_decompress_result(&reply, result);
}

int zz9k_decompress_stream_begin(
    ZZ9KContext *ctx,
    const ZZ9KDecompressStreamBeginDesc *desc,
    ZZ9KDecompressStreamResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !desc || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_decompress_stream_begin(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_reply_decompress_stream_result(
      &reply, ZZ9K_OP_DECOMPRESS_STREAM_BEGIN, result);
}

int zz9k_decompress_stream_read(
    ZZ9KContext *ctx,
    const ZZ9KDecompressStreamReadDesc *desc,
    ZZ9KDecompressStreamResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !desc || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_decompress_stream_read(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_reply_decompress_stream_result(
      &reply, ZZ9K_OP_DECOMPRESS_STREAM_READ, result);
}

int zz9k_decompress_stream_feed(
    ZZ9KContext *ctx,
    const ZZ9KDecompressStreamFeedDesc *desc,
    ZZ9KDecompressStreamResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !desc || !result) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(result, 0, sizeof(*result));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_decompress_stream_feed(&request, desc);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_reply_decompress_stream_result(
      &reply, ZZ9K_OP_DECOMPRESS_STREAM_FEED, result);
}

int zz9k_decompress_stream_close(ZZ9KContext *ctx, uint32_t session,
                                 uint32_t flags)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_decompress_stream_close(&request, session, flags);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  return zz9k_reply_require(&reply, ZZ9K_OP_DECOMPRESS_STREAM_CLOSE, 0U);
}

int zz9k_read_diag(ZZ9KContext *ctx, ZZ9KDiagInfo *diag)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !diag) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(diag, 0, sizeof(*diag));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_diag_read(&request);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return zz9k_reply_diag_info(&reply, diag);
}

int zz9k_read_diag_timing(ZZ9KContext *ctx, ZZ9KDiagTimingInfo *timing)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !timing) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(timing, 0, sizeof(*timing));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_diag_timing(&request);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return zz9k_reply_diag_timing(&reply, timing);
}

int zz9k_read_diag_sched(ZZ9KContext *ctx, ZZ9KDiagSchedInfo *sched)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !sched) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(sched, 0, sizeof(*sched));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_diag_sched(&request);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return zz9k_reply_diag_sched(&reply, sched);
}

int zz9k_read_diag_memory(ZZ9KContext *ctx, ZZ9KDiagMemoryInfo *memory)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  if (!ctx || !memory) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(memory, 0, sizeof(*memory));
  memset(&reply, 0, sizeof(reply));
  status = zz9k_request_diag_memory(&request);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return zz9k_reply_diag_memory(&reply, memory);
}

static int zz9k_enqueue_request_locked(ZZ9KContext *ctx, ZZ9KRequest *request,
                                       uint32_t *request_id)
{
  ZZ9KMailboxEntry entry;
  uint32_t head;
  uint32_t tail;
  uint32_t next_tail;

  if (!ctx || !request || !request_id) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (!ctx->mailbox || !ctx->request_ring) {
    return ZZ9K_STATUS_UNSUPPORTED;
  }
  if (request->entry.payload_len > sizeof(request->entry.payload.inline_data)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }


  head = zz9k_get_be32(ctx->mailbox->request_head);
  tail = zz9k_get_be32(ctx->mailbox->request_tail);
  if (head >= ctx->request_ring_entries || tail >= ctx->request_ring_entries) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  next_tail = zz9k_next_ring_index(tail, ctx->request_ring_entries);
  if (next_tail == head) {
    return ZZ9K_STATUS_BUSY;
  }

  entry = request->entry;
  if (entry.request_id == 0) {
    entry.request_id = zz9k_context_next_request_id(ctx);
  }
  entry.status = ZZ9K_STATUS_QUEUED;

  zz9k_entry_to_wire(&ctx->request_ring[tail], &entry);
  zz9k_put_wire_be32(ctx->mailbox->request_tail, next_tail);
  if (ctx->doorbell) {
    *ctx->doorbell = 1;
  }

  *request_id = entry.request_id;
  return ZZ9K_STATUS_QUEUED;
}

#if !defined(ZZ9K_HOST_LIBRARY_BUILD)
static void zz9k_library_pending_complete(ZZ9KAsyncRequest *async,
                                          void *user_data)
{
  (void)async;
  (void)user_data;
}

static int zz9k_library_take_completed(ZZ9KContext *ctx,
                                       ZZ9KMailboxEntry *reply)
{
  ZZ9KStaticLibraryPending **link = &ctx->library_pending;

  while (*link) {
    ZZ9KStaticLibraryPending *pending = *link;
    int status;

    if (!pending->async.completed) {
      link = &pending->next;
      continue;
    }
    status = ctx->library_transport_ops->cancel(ctx->library_base,
                                                &pending->async);
    if (status != ZZ9K_STATUS_OK &&
        !(status == ZZ9K_STATUS_BAD_REQUEST && pending->async.completed)) {
      return status;
    }
    *reply = pending->async.reply;
    *link = pending->next;
    zz9k_free_host(pending);
    return ZZ9K_STATUS_OK;
  }
  return ZZ9K_STATUS_BUSY;
}
#endif

int zz9k_submit(ZZ9KContext *ctx, ZZ9KRequest *request, uint32_t *request_id)
{
  int lock_status;
  int status;

  if (!ctx || !request || !request_id) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (ctx->library_transport) {
#if !defined(ZZ9K_HOST_LIBRARY_BUILD)
    ZZ9KStaticLibraryPending *pending =
        (ZZ9KStaticLibraryPending *)zz9k_alloc_host(sizeof(*pending));

    if (!pending) {
      return ZZ9K_STATUS_NO_MEMORY;
    }
    memset(pending, 0, sizeof(*pending));
    status = ctx->library_transport_ops->call_async(
        ctx->library_base, &pending->async, request,
        zz9k_library_pending_complete, ctx);
    if (status != ZZ9K_STATUS_QUEUED) {
      zz9k_free_host(pending);
      return status;
    }
    pending->next = ctx->library_pending;
    ctx->library_pending = pending;
    *request_id = pending->async.request_id;
    return status;
#else
    return ZZ9K_STATUS_UNSUPPORTED;
#endif
  }

  request->entry.flags &= (uint16_t)~ZZ9K_ENTRY_CLIENT_ID_MASK;
  request->entry.flags |= (uint16_t)(ctx->client_id <<
                                     ZZ9K_ENTRY_CLIENT_ID_SHIFT);

  lock_status = zz9k_mailbox_lock(ctx);
  if (lock_status != ZZ9K_STATUS_OK) {
    return lock_status;
  }
  status = zz9k_enqueue_request_locked(ctx, request, request_id);
  zz9k_mailbox_unlock(ctx);
  return status;
}

int zz9k_submit_batch(ZZ9KContext *ctx, ZZ9KRequest *requests,
                      uint32_t count, uint32_t *request_ids,
                      uint32_t *submitted)
{
  uint32_t done;
  int status;

  if (!ctx || !requests || count == 0 || !submitted) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  *submitted = 0;
  if (request_ids) {
    uint32_t i;

    for (i = 0; i < count; i++) {
      request_ids[i] = 0;
    }
  }

  done = 0;
  while (done < count) {
    uint32_t request_id;

    request_id = 0;
    status = zz9k_submit(ctx, &requests[done], &request_id);
    if (status != ZZ9K_STATUS_QUEUED) {
      if (done != 0) {
        return ZZ9K_STATUS_QUEUED;
      }
      return status;
    }

    if (request_ids) {
      request_ids[done] = request_id;
    }
    done++;
    *submitted = done;
  }

  return ZZ9K_STATUS_QUEUED;
}

static int zz9k_consume_next_completion_locked(ZZ9KContext *ctx,
                                               ZZ9KMailboxEntry *reply)
{
  uint32_t head;
  uint32_t tail;
  uint32_t next_head;

  if (!ctx || !reply) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (!ctx->mailbox || !ctx->completion_ring) {
    return ZZ9K_STATUS_UNSUPPORTED;
  }


  head = zz9k_get_be32(ctx->mailbox->completion_head);
  tail = zz9k_get_be32(ctx->mailbox->completion_tail);
  if (head >= ctx->completion_ring_entries ||
      tail >= ctx->completion_ring_entries) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (head == tail) {
    return ZZ9K_STATUS_BUSY;
  }


  zz9k_entry_from_wire(reply, &ctx->completion_ring[head]);
  next_head = zz9k_next_ring_index(head, ctx->completion_ring_entries);
  zz9k_put_wire_be32(ctx->mailbox->completion_head, next_head);
  if (ctx->irq_ack) {
    *ctx->irq_ack = 1;
  }

  return ZZ9K_STATUS_OK;
}

int zz9k_poll(ZZ9KContext *ctx, ZZ9KMailboxEntry *reply)
{
  int lock_status;
  int status;

  if (!ctx || !reply) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (ctx->library_transport) {
#if !defined(ZZ9K_HOST_LIBRARY_BUILD)
    uint32_t completed = 0U;

    status = zz9k_library_take_completed(ctx, reply);
    if (status == ZZ9K_STATUS_OK) {
      return status;
    }
    status = ctx->library_transport_ops->poll(ctx->library_base, 1U,
                                               &completed);
    if (status != ZZ9K_STATUS_OK) {
      return status;
    }
    return zz9k_library_take_completed(ctx, reply);
#else
    return ZZ9K_STATUS_UNSUPPORTED;
#endif
  }

  lock_status = zz9k_mailbox_lock(ctx);
  if (lock_status != ZZ9K_STATUS_OK) {
    return lock_status;
  }
  status = zz9k_consume_next_completion_locked(ctx, reply);
  zz9k_mailbox_unlock(ctx);
  return status;
}

int zz9k_poll_batch(ZZ9KContext *ctx, ZZ9KMailboxEntry *replies,
                    uint32_t capacity, uint32_t *completed)
{
  uint32_t done;
  int status;

  if (!ctx || !replies || capacity == 0 || !completed) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  *completed = 0;
  done = 0;
  while (done < capacity) {
    status = zz9k_poll(ctx, &replies[done]);
    if (status == ZZ9K_STATUS_BUSY) {
      return done == 0 ? ZZ9K_STATUS_BUSY : ZZ9K_STATUS_OK;
    }
    if (status != ZZ9K_STATUS_OK) {
      return status;
    }

    done++;
    *completed = done;
  }

  return ZZ9K_STATUS_OK;
}

static void zz9k_crypto_track_submissions(
    uint32_t request_ids[ZZ9K_CRYPTO_BATCH_MAX_IN_FLIGHT],
    const uint32_t *submitted_ids, uint32_t count)
{
  uint32_t i;
  uint32_t slot;

  for (i = 0; i < count; i++) {
    for (slot = 0; slot < ZZ9K_CRYPTO_BATCH_MAX_IN_FLIGHT; slot++) {
      if (request_ids[slot] == 0U) {
        request_ids[slot] = submitted_ids[i];
        break;
      }
    }
  }
}

static int zz9k_poll_crypto_batch(
    ZZ9KContext *ctx,
    uint32_t request_ids[ZZ9K_CRYPTO_BATCH_MAX_IN_FLIGHT],
    ZZ9KMailboxEntry *replies, uint32_t capacity, uint32_t *completed)
{
  ZZ9KMailboxEntry reply;
  uint32_t done;
  uint32_t slot;
  int status;

  if (!ctx || !replies || capacity == 0U || !completed) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (!ctx->completion_dispatcher) {
    return zz9k_poll_batch(ctx, replies, capacity, completed);
  }

  *completed = 0;
  done = 0;
  while (done < capacity) {
    status = zz9k_poll(ctx, &reply);
    if (status == ZZ9K_STATUS_BUSY) {
      return done == 0U ? ZZ9K_STATUS_BUSY : ZZ9K_STATUS_OK;
    }
    if (status != ZZ9K_STATUS_OK) {
      return status;
    }
    for (slot = 0; slot < ZZ9K_CRYPTO_BATCH_MAX_IN_FLIGHT; slot++) {
      if (request_ids[slot] == reply.request_id) {
        request_ids[slot] = 0U;
        replies[done++] = reply;
        *completed = done;
        break;
      }
    }
    if (slot == ZZ9K_CRYPTO_BATCH_MAX_IN_FLIGHT) {
      status = zz9k_dispatch_foreign_completion(ctx, &reply);
      if (status != ZZ9K_STATUS_OK) {
        return status;
      }
    }
  }
  return ZZ9K_STATUS_OK;
}

int zz9k_crypto_hash_batch(ZZ9KContext *ctx,
                           const ZZ9KCryptoHashDesc *descs,
                           ZZ9KCryptoResult *results,
                           uint32_t count,
                           uint32_t max_in_flight,
                           uint32_t timeout_ticks)
{
  ZZ9KRequest requests[ZZ9K_CRYPTO_BATCH_MAX_IN_FLIGHT];
  ZZ9KMailboxEntry replies[ZZ9K_CRYPTO_BATCH_MAX_IN_FLIGHT];
  uint32_t in_flight_ids[ZZ9K_CRYPTO_BATCH_MAX_IN_FLIGHT];
  uint32_t submitted_ids[ZZ9K_CRYPTO_BATCH_MAX_IN_FLIGHT];
  uint32_t submitted_total;
  uint32_t completed_total;
  uint32_t in_flight;
  uint32_t wait_ticks;
  int status;

  if (!ctx || !descs || !results || count == 0U) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  if (max_in_flight == 0U ||
      max_in_flight > ZZ9K_CRYPTO_BATCH_MAX_IN_FLIGHT) {
    max_in_flight = ZZ9K_CRYPTO_BATCH_MAX_IN_FLIGHT;
  }

  memset(results, 0, sizeof(results[0]) * count);
  memset(in_flight_ids, 0, sizeof(in_flight_ids));
  submitted_total = 0;
  completed_total = 0;
  in_flight = 0;
  wait_ticks = 0;

  while (completed_total < count) {
    while (submitted_total < count && in_flight < max_in_flight) {
      uint32_t batch_capacity;
      uint32_t submitted_now;
      uint32_t i;

      batch_capacity = count - submitted_total;
      if (batch_capacity > max_in_flight - in_flight) {
        batch_capacity = max_in_flight - in_flight;
      }

      for (i = 0; i < batch_capacity; i++) {
        status = zz9k_request_crypto_hash(&requests[i],
                                           &descs[submitted_total + i]);
        if (status != ZZ9K_STATUS_OK) {
          return status;
        }
        requests[i].entry.user_cookie = submitted_total + i;
      }

      submitted_now = 0;
      status = zz9k_submit_batch(ctx, requests, batch_capacity, submitted_ids,
                                 &submitted_now);
      if (status == ZZ9K_STATUS_QUEUED && submitted_now != 0U) {
        zz9k_crypto_track_submissions(in_flight_ids, submitted_ids,
                                      submitted_now);
        submitted_total += submitted_now;
        in_flight += submitted_now;
        wait_ticks = 0;
        if (submitted_now < batch_capacity) {
          break;
        }
        continue;
      }
      if (status == ZZ9K_STATUS_BUSY) {
        break;
      }
      return status;
    }

    if (in_flight == 0U) {
      zz9k_idle_between_polls_backoff(wait_ticks);
      wait_ticks++;
      if (wait_ticks >= timeout_ticks) {
        return ZZ9K_STATUS_TIMEOUT;
      }
      continue;
    }

    memset(replies, 0, sizeof(replies));
    {
      uint32_t poll_capacity;
      uint32_t just_completed;
      uint32_t i;

      poll_capacity = in_flight;
      if (poll_capacity > max_in_flight) {
        poll_capacity = max_in_flight;
      }

      just_completed = 0;
      status = zz9k_poll_crypto_batch(ctx, in_flight_ids, replies,
                                      poll_capacity, &just_completed);
      if (status == ZZ9K_STATUS_BUSY) {
        zz9k_idle_between_polls_backoff(wait_ticks);
        wait_ticks++;
        if (wait_ticks >= timeout_ticks) {
          return ZZ9K_STATUS_TIMEOUT;
        }
        continue;
      }
      if (status != ZZ9K_STATUS_OK) {
        return status;
      }

      for (i = 0; i < just_completed; i++) {
        uint32_t index;

        index = replies[i].user_cookie;
        if (index >= count || results[index].bytes_written != 0U) {
          return ZZ9K_STATUS_INTERNAL_ERROR;
        }

        status = zz9k_reply_crypto_result(&replies[i],
                                          ZZ9K_OP_CRYPTO_HASH,
                                          &results[index]);
        if (status != ZZ9K_STATUS_OK) {
          return status;
        }
      }

      completed_total += just_completed;
      in_flight -= just_completed;
      wait_ticks = 0;
    }
  }

  return ZZ9K_STATUS_OK;
}

int zz9k_crypto_stream_batch(ZZ9KContext *ctx,
                             const ZZ9KCryptoStreamDesc *descs,
                             ZZ9KCryptoResult *results,
                             uint32_t count,
                             uint32_t max_in_flight,
                             uint32_t timeout_ticks)
{
  ZZ9KRequest requests[ZZ9K_CRYPTO_BATCH_MAX_IN_FLIGHT];
  ZZ9KMailboxEntry replies[ZZ9K_CRYPTO_BATCH_MAX_IN_FLIGHT];
  uint32_t in_flight_ids[ZZ9K_CRYPTO_BATCH_MAX_IN_FLIGHT];
  uint32_t submitted_ids[ZZ9K_CRYPTO_BATCH_MAX_IN_FLIGHT];
  uint32_t submitted_total;
  uint32_t completed_total;
  uint32_t in_flight;
  uint32_t wait_ticks;
  int status;

  if (!ctx || !descs || !results || count == 0U) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  if (max_in_flight == 0U ||
      max_in_flight > ZZ9K_CRYPTO_BATCH_MAX_IN_FLIGHT) {
    max_in_flight = ZZ9K_CRYPTO_BATCH_MAX_IN_FLIGHT;
  }

  memset(results, 0, sizeof(results[0]) * count);
  memset(in_flight_ids, 0, sizeof(in_flight_ids));
  submitted_total = 0;
  completed_total = 0;
  in_flight = 0;
  wait_ticks = 0;

  while (completed_total < count) {
    while (submitted_total < count && in_flight < max_in_flight) {
      uint32_t batch_capacity;
      uint32_t submitted_now;
      uint32_t i;

      batch_capacity = count - submitted_total;
      if (batch_capacity > max_in_flight - in_flight) {
        batch_capacity = max_in_flight - in_flight;
      }

      for (i = 0; i < batch_capacity; i++) {
        status = zz9k_request_crypto_stream(&requests[i],
                                            &descs[submitted_total + i]);
        if (status != ZZ9K_STATUS_OK) {
          return status;
        }
        requests[i].entry.user_cookie = submitted_total + i;
      }

      submitted_now = 0;
      status = zz9k_submit_batch(ctx, requests, batch_capacity, submitted_ids,
                                 &submitted_now);
      if (status == ZZ9K_STATUS_QUEUED && submitted_now != 0U) {
        zz9k_crypto_track_submissions(in_flight_ids, submitted_ids,
                                      submitted_now);
        submitted_total += submitted_now;
        in_flight += submitted_now;
        wait_ticks = 0;
        if (submitted_now < batch_capacity) {
          break;
        }
        continue;
      }
      if (status == ZZ9K_STATUS_BUSY) {
        break;
      }
      return status;
    }

    if (in_flight == 0U) {
      zz9k_idle_between_polls_backoff(wait_ticks);
      wait_ticks++;
      if (wait_ticks >= timeout_ticks) {
        return ZZ9K_STATUS_TIMEOUT;
      }
      continue;
    }

    memset(replies, 0, sizeof(replies));
    {
      uint32_t poll_capacity;
      uint32_t just_completed;
      uint32_t i;

      poll_capacity = in_flight;
      if (poll_capacity > max_in_flight) {
        poll_capacity = max_in_flight;
      }

      just_completed = 0;
      status = zz9k_poll_crypto_batch(ctx, in_flight_ids, replies,
                                      poll_capacity, &just_completed);
      if (status == ZZ9K_STATUS_BUSY) {
        zz9k_idle_between_polls_backoff(wait_ticks);
        wait_ticks++;
        if (wait_ticks >= timeout_ticks) {
          return ZZ9K_STATUS_TIMEOUT;
        }
        continue;
      }
      if (status != ZZ9K_STATUS_OK) {
        return status;
      }

      for (i = 0; i < just_completed; i++) {
        uint32_t index;

        index = replies[i].user_cookie;
        if (index >= count || results[index].bytes_written != 0U) {
          return ZZ9K_STATUS_INTERNAL_ERROR;
        }

        status = zz9k_reply_crypto_result(&replies[i],
                                          ZZ9K_OP_CRYPTO_STREAM,
                                          &results[index]);
        if (status != ZZ9K_STATUS_OK) {
          return status;
        }
      }

      completed_total += just_completed;
      in_flight -= just_completed;
      wait_ticks = 0;
    }
  }

  return ZZ9K_STATUS_OK;
}

int zz9k_crypto_aead_batch(ZZ9KContext *ctx,
                           const ZZ9KCryptoAeadDesc *descs,
                           ZZ9KCryptoResult *results,
                           uint32_t count,
                           uint32_t max_in_flight,
                           uint32_t timeout_ticks)
{
  ZZ9KRequest requests[ZZ9K_CRYPTO_BATCH_MAX_IN_FLIGHT];
  ZZ9KMailboxEntry replies[ZZ9K_CRYPTO_BATCH_MAX_IN_FLIGHT];
  uint32_t in_flight_ids[ZZ9K_CRYPTO_BATCH_MAX_IN_FLIGHT];
  uint32_t submitted_ids[ZZ9K_CRYPTO_BATCH_MAX_IN_FLIGHT];
  uint32_t submitted_total;
  uint32_t completed_total;
  uint32_t in_flight;
  uint32_t wait_ticks;
  int status;

  if (!ctx || !descs || !results || count == 0U) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  if (max_in_flight == 0U ||
      max_in_flight > ZZ9K_CRYPTO_BATCH_MAX_IN_FLIGHT) {
    max_in_flight = ZZ9K_CRYPTO_BATCH_MAX_IN_FLIGHT;
  }

  memset(results, 0, sizeof(results[0]) * count);
  memset(in_flight_ids, 0, sizeof(in_flight_ids));
  submitted_total = 0;
  completed_total = 0;
  in_flight = 0;
  wait_ticks = 0;

  while (completed_total < count) {
    while (submitted_total < count && in_flight < max_in_flight) {
      uint32_t batch_capacity;
      uint32_t submitted_now;
      uint32_t i;

      batch_capacity = count - submitted_total;
      if (batch_capacity > max_in_flight - in_flight) {
        batch_capacity = max_in_flight - in_flight;
      }

      for (i = 0; i < batch_capacity; i++) {
        status = zz9k_request_crypto_aead(&requests[i],
                                          &descs[submitted_total + i]);
        if (status != ZZ9K_STATUS_OK) {
          return status;
        }
        requests[i].entry.user_cookie = submitted_total + i;
      }

      submitted_now = 0;
      status = zz9k_submit_batch(ctx, requests, batch_capacity, submitted_ids,
                                 &submitted_now);
      if (status == ZZ9K_STATUS_QUEUED && submitted_now != 0U) {
        zz9k_crypto_track_submissions(in_flight_ids, submitted_ids,
                                      submitted_now);
        submitted_total += submitted_now;
        in_flight += submitted_now;
        wait_ticks = 0;
        if (submitted_now < batch_capacity) {
          break;
        }
        continue;
      }
      if (status == ZZ9K_STATUS_BUSY) {
        break;
      }
      return status;
    }

    if (in_flight == 0U) {
      zz9k_idle_between_polls_backoff(wait_ticks);
      wait_ticks++;
      if (wait_ticks >= timeout_ticks) {
        return ZZ9K_STATUS_TIMEOUT;
      }
      continue;
    }

    memset(replies, 0, sizeof(replies));
    {
      uint32_t poll_capacity;
      uint32_t just_completed;
      uint32_t i;

      poll_capacity = in_flight;
      if (poll_capacity > max_in_flight) {
        poll_capacity = max_in_flight;
      }

      just_completed = 0;
      status = zz9k_poll_crypto_batch(ctx, in_flight_ids, replies,
                                      poll_capacity, &just_completed);
      if (status == ZZ9K_STATUS_BUSY) {
        zz9k_idle_between_polls_backoff(wait_ticks);
        wait_ticks++;
        if (wait_ticks >= timeout_ticks) {
          return ZZ9K_STATUS_TIMEOUT;
        }
        continue;
      }
      if (status != ZZ9K_STATUS_OK) {
        return status;
      }

      for (i = 0; i < just_completed; i++) {
        uint32_t index;

        index = replies[i].user_cookie;
        if (index >= count || results[index].bytes_written != 0U) {
          return ZZ9K_STATUS_INTERNAL_ERROR;
        }

        status = zz9k_reply_crypto_result(&replies[i],
                                          ZZ9K_OP_CRYPTO_AEAD,
                                          &results[index]);
        if (status != ZZ9K_STATUS_OK) {
          return status;
        }
      }

      completed_total += just_completed;
      in_flight -= just_completed;
      wait_ticks = 0;
    }
  }

  return ZZ9K_STATUS_OK;
}

int zz9k_call(ZZ9KContext *ctx, ZZ9KRequest *request, ZZ9KMailboxEntry *reply,
              uint32_t timeout_ticks)
{
  uint32_t request_id;
  uint32_t sync_cookie;
  uint32_t ticks;
  uint32_t start_ms = 0U;
  int use_wall_clock = 0;
  int lock_status;
  int status;

  if (!ctx || !request || !reply) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (ctx->library_transport) {
#if !defined(ZZ9K_HOST_LIBRARY_BUILD)
    return ctx->library_transport_ops->call(
        ctx->library_base, request, reply,
        zz9k_library_timeout_ticks(ctx, timeout_ticks));
#else
    return ZZ9K_STATUS_UNSUPPORTED;
#endif
  }

  lock_status = zz9k_mailbox_lock(ctx);
  if (lock_status != ZZ9K_STATUS_OK) {
    return lock_status;
  }
  if (request->entry.request_id == 0) {
    request->entry.request_id = zz9k_context_next_request_id(ctx);
  }
  sync_cookie = request->entry.request_id ^ ctx->sync_cookie_mask;
  request->entry.user_cookie = sync_cookie;

  status = zz9k_enqueue_request_locked(ctx, request, &request_id);
  if (status != ZZ9K_STATUS_QUEUED) {
    goto done;
  }

  if (ctx->irq_armed) {
    status = zz9k_await_completion_locked(ctx, request_id,
                                          request->entry.opcode, sync_cookie,
                                          reply,
                                          zz9k_armed_wait_timeout_ms(ctx));
    goto done;
  }

#if ZZ9K_HOST_AMIGA
  if (ctx->offload_timeout_ms != 0U &&
      zz9k_timer_open(ctx) == ZZ9K_STATUS_OK) {
    use_wall_clock = 1;
    start_ms = zz9k_now_ms(ctx);
  }
#else
  if (ctx->offload_timeout_ms != 0U && zz9k_now_hook_for_test) {
    use_wall_clock = 1;
    start_ms = zz9k_now_ms(ctx);
  }
#endif

  if (!use_wall_clock && timeout_ticks == 0U) {
    status = ZZ9K_STATUS_TIMEOUT;
    goto done;
  }

  for (ticks = 0;;) {
    status = zz9k_consume_next_completion_locked(ctx, reply);
    if (status == ZZ9K_STATUS_OK) {
      if (reply->request_id == request_id &&
          reply->opcode == request->entry.opcode &&
          reply->user_cookie == sync_cookie) {
        status = reply->status;
        goto done;
      }
      status = zz9k_dispatch_foreign_completion(ctx, reply);
      if (status != ZZ9K_STATUS_OK) {
        goto done;
      }
      continue;
    }
    if (status != ZZ9K_STATUS_BUSY) {
      goto done;
    }

    if (use_wall_clock) {
      if ((uint32_t)(zz9k_now_ms(ctx) - start_ms) >= ctx->offload_timeout_ms ||
          ticks >= (uint32_t)ZZ9K_OFFLOAD_ITER_BACKSTOP) {
        status = ZZ9K_STATUS_TIMEOUT;
        goto done;
      }
    } else if (ticks + 1U >= timeout_ticks) {
      status = ZZ9K_STATUS_TIMEOUT;
      goto done;
    }
    zz9k_idle_between_polls_backoff(ticks + 1U);
    ticks++;
  }

done:
  zz9k_mailbox_unlock(ctx);
  return status;
}
