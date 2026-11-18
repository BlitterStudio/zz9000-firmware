/*
 * Message ring conformance test for the ARM app runtime. It loads
 * ringflood.elf (sdk/zzapp/templates) and runs three phases: idle, where
 * the app only answers pings; flood, for the given number of seconds, where
 * the app keeps the app-to-host ring full; and idle again. Every message is
 * checked: sequence gaps, wrong bytes, ordering. A counter word that the app
 * updates in a host-visible block is read through a cache-inhibited mapping,
 * counting reads that run backwards or lag the messages. Ping round trips are
 * kept per phase: MAXLAT applies to the idle phases, where the figure times
 * the transport; during the flood a ping waits behind a full ring of DATA,
 * so that figure is reported as queueing delay without a limit. The
 * accounting is in zz9k-app-ringtest.h, which has a host test.
 *
 * usage: zz9k-app-ringtest <ringflood.elf> [seconds] [MAXLAT=<ms>]
 *
 * The ring is polled, not signalled: the library signal comes from its
 * VBlank server, so it can add up to a frame to the latency this test
 * measures.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <devices/timer.h>
#include <exec/ports.h>
#include <exec/types.h>
#include <libraries/configvars.h>
#include <libraries/expansion.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/timer.h>
#include <proto/zz9k.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zz9k/abi.h>
#include <zz9k/text.h>

#include "zz9k-app-ringtest.h"

struct Library *ZZ9KBase;
struct Device *TimerBase;

#define ZZ9K_MMU_MAPP_CACHEINHIBIT 0x00000040UL
#define ZZ9K_MMU_MAPP_COPYBACK 0x00002000UL
#define ZZ9K_MMU_MAPP_IMPRECISE 0x00200000UL
#define ZZ9K_MMU_MAPP_NONSERIALIZED 0x20000000UL
#define ZZ9K_MMU_CACHE_MASK (ZZ9K_MMU_MAPP_CACHEINHIBIT | \
                             ZZ9K_MMU_MAPP_COPYBACK | \
                             ZZ9K_MMU_MAPP_IMPRECISE | \
                             ZZ9K_MMU_MAPP_NONSERIALIZED)

#define PING_INTERVAL_US 20000U
#define STATUS_INTERVAL_US 500000U
#define START_TIMEOUT_US 5000000U
#define STOP_TIMEOUT_US 10000000U
/* After the last ping, how long replies in flight may take to arrive. */
#define PING_GRACE_US 500000U
/* Each idle phase, before and after the flood. */
#define IDLE_PHASE_US 1500000U
/* How long the ring may take to drain to the QUIET acknowledgement. */
#define SETTLE_TIMEOUT_US 10000000U
#define CTRL_TIMEOUT_US 2000000U

struct mmu_context;

static uint32_t eclock_hz;

static uint64_t now_us(void)
{
  struct EClockVal ev;

  eclock_hz = ReadEClock(&ev);
  return zz9k_rt_ticks_to_us(((uint64_t)ev.ev_hi << 32) | ev.ev_lo,
                             eclock_hz);
}

static void trace(const char *step)
{
  printf("zz9k-app-ringtest: %s\n", step);
  fflush(stdout);
}

static void usage(const char *name)
{
  printf("usage: %s <ringflood.elf> [seconds] [MAXLAT=<ms>]\n", name);
}

static void print_ms(const char *label, uint64_t us)
{
  printf("%s%lu.%03lu ms", label, (unsigned long)(us / 1000U),
         (unsigned long)(us % 1000U));
}

/* Cache-inhibits the pages holding [address, address + 4) with
 * mmu.library, the way the library does for the app window. */
static int mmu_inhibit(uint32_t address)
{
  struct Library *mmu = OpenLibrary((CONST_STRPTR)"mmu.library", 41);
  struct mmu_context *context;
  uint32_t page_bytes;
  uint32_t first;
  uint32_t last;
  uint32_t page;
  int ok = 1;

  if (!mmu) return 0;
  {
    register struct mmu_context *d0 __asm("d0");
    register struct Library *a6 __asm("a6") = mmu;

    __asm volatile("jsr -150(a6)" : "=r"(d0) : "r"(a6) :
                   "cc", "memory", "d1", "a0", "a1");
    context = d0;
  }
  if (!context) {
    CloseLibrary(mmu);
    return 0;
  }
  {
    register uint32_t d0 __asm("d0");
    register struct mmu_context *a0 __asm("a0") = context;
    register struct Library *a6 __asm("a6") = mmu;

    __asm volatile("jsr -48(a6)" : "=r"(d0), "+r"(a0) : "r"(a6) :
                   "cc", "memory", "d1", "a1");
    page_bytes = d0;
  }
  if (page_bytes == 0U || (page_bytes & (page_bytes - 1U)) != 0U) {
    CloseLibrary(mmu);
    return 0;
  }
  first = address & ~(page_bytes - 1U);
  last = (address + 4U + page_bytes - 1U) & ~(page_bytes - 1U);
  for (page = first; page < last && ok; page += page_bytes) {
    register uint32_t d0 __asm("d0") = page_bytes;
    register uint32_t d1 __asm("d1") = ZZ9K_MMU_MAPP_CACHEINHIBIT;
    register uint32_t d2 __asm("d2") = ZZ9K_MMU_CACHE_MASK;
    register struct mmu_context *a0 __asm("a0") = context;
    register void *a1 __asm("a1") = (void *)(uintptr_t)page;
    register void *a2 __asm("a2") = 0;
    register struct Library *a6 __asm("a6") = mmu;

    __asm volatile("jsr -84(a6)" : "+r"(d0), "+r"(d1), "+r"(d2), "+r"(a0),
                   "+r"(a1), "+r"(a2) : "r"(a6) : "cc", "memory");
    ok = d0 != 0U;
  }
  CloseLibrary(mmu);
  return ok;
}

static uint32_t read_counter(volatile uint32_t *word, int flush)
{
  if (flush) CacheClearE((APTR)word, 4, CACRF_ClearD);
  return *word;
}

typedef struct Session {
  ZZ9KAppHandle *app;
  ZZ9KRingTest test;
  volatile uint32_t *counter;
  int flush;
  int sample;
  int last_status;
  uint32_t taken;
  uint32_t ping_id;
  uint32_t ctrl_id;
} Session;

static int io_get(void *ctx, uint8_t message[32])
{
  Session *s = (Session *)ctx;
  ZZ9KAppMessage got;

  s->last_status = ZZ9KAppGetMessage(s->app, &got);
  if (s->last_status == ZZ9K_STATUS_OK) {
    memcpy(message, got.bytes, sizeof(got.bytes));
    return 0;
  }
  return s->last_status == ZZ9K_STATUS_BUSY ? 1 : -1;
}

static uint64_t io_now(void *ctx)
{
  (void)ctx;
  return now_us();
}

static void io_sample(void *ctx, ZZ9KRingTest *t)
{
  Session *s = (Session *)ctx;

  if (s->sample && s->counter) {
    zz9k_rt_on_counter(t, read_counter(s->counter, s->flush));
  }
}

/* One bounded pass: at most ZZ9K_RT_DRAIN_BATCH messages, so a producer that
 * keeps the ring full cannot starve the caller's timeout, ping, status and
 * break checks. Returns a ZZ9K_STATUS code, OK when no error occurred. */
static int drain(Session *s)
{
  ZZ9KRtIo io;

  io.get = io_get;
  io.now = io_now;
  io.sample = io_sample;
  io.ctx = s;
  if (zz9k_rt_drain(&s->test, &io, ZZ9K_RT_DRAIN_BATCH, &s->taken) < 0) {
    return s->last_status;
  }
  return ZZ9K_STATUS_OK;
}

static int app_alive(Session *s, ZZ9KAppStatusInfo *info)
{
  if (ZZ9KAppStatus(s->app, info) != ZZ9K_STATUS_OK) return 0;
  return info->state == ZZ9K_APP_STATE_RUNNING ||
         info->state == ZZ9K_APP_STATE_STARTING;
}

static void print_rtt(const char *label, const ZZ9KRtRtt *r)
{
  fputs(label, stdout);
  if (!r->n) {
    puts("no ping answered");
    return;
  }
  printf("%lu pings, ", (unsigned long)r->n);
  print_ms("min ", r->min);
  print_ms(", avg ", zz9k_rt_rtt_avg(r));
  print_ms(", max ", r->max);
  putchar('\n');
}

/* Runs one phase for duration_us: drains the ring in bounded passes, sends a
 * ping every PING_INTERVAL_US when pinging, checks the app is alive twice a
 * second, and watches for Ctrl-C. Returns a ZZ9K_STATUS code; *died is set
 * when the app stopped by itself. */
static int run_phase(Session *s, uint64_t duration_us, int pinging, int *died)
{
  ZZ9KAppStatusInfo info;
  uint64_t start = now_us();
  uint64_t end = start + duration_us;
  uint64_t next_ping = start;
  uint64_t next_status = start + STATUS_INTERVAL_US;
  int status;

  *died = 0;
  for (;;) {
    uint64_t now;

    status = drain(s);
    if (status != ZZ9K_STATUS_OK) return status;
    now = now_us();
    if (now >= end) return ZZ9K_STATUS_OK;
    if (pinging && now >= next_ping) {
      ZZ9KAppMessage ping;
      uint32_t id = s->ping_id + 1U;

      zz9k_rt_build_ping(ping.bytes, id, now);
      /* BUSY means the ring is full: not sent, so not recorded. */
      if (ZZ9KAppSendMessage(s->app, &ping) == ZZ9K_STATUS_OK) {
        zz9k_rt_note_ping(&s->test, id, now);
        s->ping_id = id;
      }
      next_ping = now + PING_INTERVAL_US;
    }
    if (now >= next_status) {
      next_status = now + STATUS_INTERVAL_US;
      if (!app_alive(s, &info)) {
        *died = 1;
        return ZZ9K_STATUS_OK;
      }
    }
    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
      return ZZ9K_STATUS_CANCELLED;
    }
  }
}

/* Sends a mode change and records it. */
static int send_ctrl(Session *s, uint32_t mode)
{
  ZZ9KAppMessage ctrl;
  uint32_t id = s->ctrl_id + 1U;
  uint64_t start = now_us();
  int status;

  zz9k_rt_build_ctrl(ctrl.bytes, id, mode);
  do {
    status = ZZ9KAppSendMessage(s->app, &ctrl);
    if (status == ZZ9K_STATUS_OK) {
      zz9k_rt_note_ctrl(&s->test, id, mode);
      s->ctrl_id = id;
      return ZZ9K_STATUS_OK;
    }
  } while (status == ZZ9K_STATUS_BUSY && now_us() - start < CTRL_TIMEOUT_US);
  return status;
}

int main(int argc, char **argv)
{
  static Session session;
  ZZ9KAppHandle *app = 0;
  struct MsgPort *port = 0;
  struct MsgPort *timer_port = 0;
  struct timerequest *timer_request = 0;
  struct Library *expansion = 0;
  struct ConfigDev *config_dev;
  const char *app_argv[1] = { "ringflood" };
  ZZ9KAppStatusInfo info;
  uint32_t seconds = 10U;
  uint64_t max_rtt_us = 0U;
  uint64_t start;
  int died = 0;
  uint64_t elapsed = 0U;
  uint64_t grace_end;
  uint32_t board_addr = 0U;
  uint32_t board_size = 0U;
  const char *mapping = "none";
  const char *reason = 0;
  const char *step = "open";
  int status;
  int i;
  ZZ9KRingTest *t;

  if (argc < 2) {
    usage(argv[0]);
    return 10;
  }
  for (i = 2; i < argc; i++) {
    char *end_ptr = 0;
    unsigned long value;

    if (strncmp(argv[i], "MAXLAT=", 7) == 0) {
      value = strtoul(argv[i] + 7, &end_ptr, 10);
      if (!end_ptr || *end_ptr || value == 0UL || value > 60000UL) {
        usage(argv[0]);
        return 10;
      }
      max_rtt_us = (uint64_t)value * 1000U;
    } else {
      value = strtoul(argv[i], &end_ptr, 10);
      if (!end_ptr || *end_ptr || value == 0UL || value > 3600UL) {
        usage(argv[0]);
        return 10;
      }
      seconds = (uint32_t)value;
    }
  }

  memset(&session, 0, sizeof(session));
  zz9k_rt_init(&session.test);
  t = &session.test;
  ZZ9KBase = OpenLibrary((CONST_STRPTR)ZZ9K_LIBRARY_NAME,
                         ZZ9K_LIBRARY_VERSION);
  if (!ZZ9KBase) {
    puts("zz9k-app-ringtest: zz9k.library is not installed");
    return 20;
  }
  if (ZZ9KBase->lib_Revision < ZZ9K_LIBRARY_MIN_REVISION_APP_CLIENT) {
    puts("zz9k-app-ringtest: zz9k.library app client is too old");
    CloseLibrary(ZZ9KBase);
    return 20;
  }
  timer_port = CreateMsgPort();
  if (timer_port) {
    timer_request = (struct timerequest *)CreateIORequest(
        timer_port, sizeof(struct timerequest));
  }
  if (!timer_request ||
      OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
                 (struct IORequest *)timer_request, 0) != 0) {
    puts("zz9k-app-ringtest: timer.device is not available");
    if (timer_request) DeleteIORequest((struct IORequest *)timer_request);
    if (timer_port) DeleteMsgPort(timer_port);
    CloseLibrary(ZZ9KBase);
    return 20;
  }
  TimerBase = timer_request->tr_node.io_Device;
  expansion = OpenLibrary((CONST_STRPTR)"expansion.library", 0);
  config_dev = expansion ? FindConfigDev(0, ZZ9K_MNT_MANUFACTURER,
                                         ZZ9K_PRODUCT_Z3) : 0;
  if (!config_dev) {
    puts("zz9k-app-ringtest: no Zorro III ZZ9000 found");
    status = ZZ9K_STATUS_UNSUPPORTED;
    reason = "no Zorro III board";
    goto finish;
  }
  board_addr = (uint32_t)config_dev->cd_BoardAddr;
  board_size = config_dev->cd_BoardSize;

  port = CreateMsgPort();
  if (!port) {
    reason = "no message port";
    goto finish;
  }
  trace("open");
  status = ZZ9KAppOpen(&app, port);
  if (status != ZZ9K_STATUS_OK) goto fail;
  session.app = app;
  step = "load";
  trace(step);
  status = ZZ9KAppLoadFile(app, argv[1]);
  if (status != ZZ9K_STATUS_OK) goto fail;
  step = "start";
  trace(step);
  status = ZZ9KAppStart(app, app_argv, 1U);
  if (status != ZZ9K_STATUS_OK) goto fail;

  /* The app's first message is INFO. */
  step = "info";
  start = now_us();
  while (!t->have_info) {
    status = drain(&session);
    if (status != ZZ9K_STATUS_OK) goto fail;
    if (now_us() - start > START_TIMEOUT_US) {
      reason = "no INFO message from the app";
      goto stop;
    }
    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
      status = ZZ9K_STATUS_CANCELLED;
      goto fail;
    }
  }
  if (t->block_offset == ZZ9K_RT_NO_COUNTER || t->block_bytes < 4U ||
      (board_size && (t->block_offset >= board_size ||
                      board_size - t->block_offset < 4U))) {
    reason = "the app has no usable counter block";
    goto stop;
  }
  session.counter = (volatile uint32_t *)(uintptr_t)(board_addr +
                                                    t->block_offset);
  if (mmu_inhibit(board_addr + t->block_offset)) {
    mapping = "cache-inhibited (mmu.library)";
  } else {
    mapping = "CacheClearE before each read (no mmu.library mapping)";
    session.flush = 1;
  }
  printf("zz9k-app-ringtest: counter block at %08lx, %s\n",
         (unsigned long)(board_addr + t->block_offset), mapping);
  fflush(stdout);

  /* Phase 1, idle: the app is quiet and only answers pings, so the round
   * trip is the ring and the app's wake-up. MAXLAT applies to this phase. */
  step = "idle";
  trace(step);
  t->phase = ZZ9K_RT_PHASE_IDLE;
  status = run_phase(&session, IDLE_PHASE_US, 1, &died);
  if (status != ZZ9K_STATUS_OK) goto fail;
  if (died) {
    reason = "the app stopped during the run";
    goto stop;
  }

  /* Phase 2, flood: the app fills the ring. Pings queue behind DATA here;
   * their round trip is reported and has no limit. */
  step = "flood";
  trace(step);
  session.sample = 1;
  status = send_ctrl(&session, ZZ9K_RT_MODE_FLOOD);
  if (status != ZZ9K_STATUS_OK) goto fail;
  t->phase = ZZ9K_RT_PHASE_FLOOD;
  start = now_us();
  status = run_phase(&session, (uint64_t)seconds * 1000000U, 1, &died);
  if (status != ZZ9K_STATUS_OK) goto fail;
  elapsed = now_us() - start;
  if (died) {
    reason = "the app stopped during the run";
    goto stop;
  }

  /* Back to quiet. No ping goes out while the ring drains: it would queue
   * behind the flood. The ACK follows the last DATA message. */
  step = "quiet";
  trace(step);
  status = send_ctrl(&session, ZZ9K_RT_MODE_QUIET);
  if (status != ZZ9K_STATUS_OK) goto fail;
  start = now_us();
  while (zz9k_rt_ctrls_outstanding(t) &&
         now_us() - start < SETTLE_TIMEOUT_US) {
    status = drain(&session);
    if (status != ZZ9K_STATUS_OK) goto fail;
    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
      status = ZZ9K_STATUS_CANCELLED;
      goto fail;
    }
  }
  session.sample = 0;
  if (zz9k_rt_ctrls_outstanding(t)) {
    reason = "the app did not acknowledge QUIET";
    goto stop;
  }

  /* Phase 3, idle again: the transport after the flood. */
  step = "idle again";
  trace(step);
  t->phase = ZZ9K_RT_PHASE_IDLE;
  status = run_phase(&session, IDLE_PHASE_US, 1, &died);
  if (status != ZZ9K_STATUS_OK) goto fail;
  if (died) {
    reason = "the app stopped during the run";
    goto stop;
  }

  /* No new pings now. Every ping that was sent must be answered, so wait a
   * moment for replies still in flight before stopping the app. */
  grace_end = now_us() + PING_GRACE_US;
  while (zz9k_rt_pings_outstanding(t) && now_us() < grace_end) {
    status = drain(&session);
    if (status != ZZ9K_STATUS_OK) goto fail;
    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
      status = ZZ9K_STATUS_CANCELLED;
      goto fail;
    }
  }

stop:
  /* Stop sampling: the app's block goes away when it exits. */
  session.sample = 0;
  step = "stop";
  trace(step);
  status = ZZ9KAppStop(app);
  if (status != ZZ9K_STATUS_OK) goto fail;
  start = now_us();
  for (;;) {
    status = drain(&session);
    if (status != ZZ9K_STATUS_OK) goto fail;
    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
      status = ZZ9K_STATUS_CANCELLED;
      goto fail;
    }
    status = ZZ9KAppStatus(app, &info);
    if (status != ZZ9K_STATUS_OK) goto fail;
    if (info.state == ZZ9K_APP_STATE_IDLE ||
        info.state == ZZ9K_APP_STATE_FAULTED) break;
    if (now_us() - start > STOP_TIMEOUT_US) {
      reason = reason ? reason : "the app did not stop";
      goto finish;
    }
  }
  /* The app has exited, so the ring only shrinks: read it dry. */
  do {
    status = drain(&session);
    if (status != ZZ9K_STATUS_OK) goto fail;
  } while (session.taken != 0U);
  if (!reason && (info.fault_code != ZZ9K_APP_FAULT_NONE ||
                  info.exit_code != 0)) {
    reason = "the app faulted or returned an error";
  }
  if (!reason) {
    reason = zz9k_rt_verdict(t, max_rtt_us);
  }
  if (!elapsed) elapsed = 1U;
  printf("zz9k-app-ringtest: %lu s, %lu data messages (%lu msg/s)\n",
         (unsigned long)(elapsed / 1000000U), (unsigned long)t->messages,
         (unsigned long)((uint64_t)t->messages * 1000000U / elapsed));
  printf("  gaps %lu (lost %lu), reordered %lu, wrong-byte messages %lu "
         "(%lu bytes), unexpected %lu\n",
         (unsigned long)t->gaps, (unsigned long)t->lost,
         (unsigned long)t->reordered, (unsigned long)t->bad_messages,
         (unsigned long)t->bad_bytes, (unsigned long)t->unknown);
  if (t->have_done) {
    printf("  DONE: app sent %lu, ring-full retries %lu, pongs %lu, "
           "lost at the tail %lu\n",
           (unsigned long)t->done_total, (unsigned long)t->done_busy,
           (unsigned long)t->done_pongs, (unsigned long)t->tail_lost);
  } else {
    puts("  DONE: not received");
  }
  printf("  counter (%s): %lu samples (need %lu), backwards %lu, older "
         "than the message %lu, ahead of the app %lu\n", mapping,
         (unsigned long)t->counter_samples,
         (unsigned long)ZZ9K_RT_MIN_COUNTER_SAMPLES,
         (unsigned long)t->counter_nonmono, (unsigned long)t->counter_stale,
         (unsigned long)t->counter_ahead);
  printf("  pings: sent %lu, answered %lu (need %lu), unanswered %lu, "
         "PONGs received %lu, unmatched %lu, duplicate %lu, count mismatch "
         "%lu\n",
         (unsigned long)t->pings_sent, (unsigned long)t->pings_answered,
         (unsigned long)ZZ9K_RT_MIN_PONGS,
         (unsigned long)zz9k_rt_pings_outstanding(t),
         (unsigned long)t->pongs_received, (unsigned long)t->pong_unmatched,
         (unsigned long)t->pong_duplicate, (unsigned long)t->pong_count_bad);
  print_rtt("  idle ping round trip (app not flooding): ",
            &t->rtt[ZZ9K_RT_PHASE_IDLE]);
  print_rtt("  flood ping round trip (ring kept full, queueing, no limit): ",
            &t->rtt[ZZ9K_RT_PHASE_FLOOD]);
  printf("  mode changes: sent %lu, acknowledged %lu, unmatched ACKs %lu, "
         "wrong counts %lu, data after QUIET %lu\n",
         (unsigned long)t->ctrls_sent, (unsigned long)t->ctrls_acked,
         (unsigned long)t->ack_unmatched, (unsigned long)t->ack_count_bad,
         (unsigned long)t->data_in_quiet);
  if (t->ow_n) {
    printf("  one-way delay above the best message: ");
    print_ms("avg ", zz9k_rt_ow_excess_avg(t));
    print_ms(", max ", zz9k_rt_ow_excess_max(t));
    putchar('\n');
  }
  goto finish;

fail:
  printf("zz9k-app-ringtest: FAIL at %s: %s (%d)\n", step,
         zz9k_status_text(status), status);
  if (app && ZZ9KAppStatus(app, &info) == ZZ9K_STATUS_OK) {
    printf("app state %lu fault %lu exit %ld generation %lu pc %08lx\n",
           (unsigned long)info.state, (unsigned long)info.fault_code,
           (long)info.exit_code, (unsigned long)info.generation,
           (unsigned long)info.pc);
  }
  reason = reason ? reason : "see above";
  status = ZZ9K_STATUS_INTERNAL_ERROR;

finish:
  if (port) {
    while (GetMsg(port)) {}
  }
  if (app) {
    uint32_t guard;
    ZZ9KAppMessage drop;

    /* The library wants the ring empty before Close. */
    for (guard = 0; guard < 4096U; guard++) {
      if (ZZ9KAppGetMessage(app, &drop) != ZZ9K_STATUS_OK) break;
    }
    ZZ9KAppClose(app);
  }
  if (port) DeleteMsgPort(port);
  if (expansion) CloseLibrary(expansion);
  CloseDevice((struct IORequest *)timer_request);
  DeleteIORequest((struct IORequest *)timer_request);
  DeleteMsgPort(timer_port);
  CloseLibrary(ZZ9KBase);
  if (reason) {
    printf("zz9k-app-ringtest: FAIL: %s\n", reason);
    return 20;
  }
  puts("zz9k-app-ringtest: PASS");
  return 0;
}
