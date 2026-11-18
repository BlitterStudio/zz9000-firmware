/*
 * Message ring conformance test for the ARM app runtime. It loads
 * ringflood.elf (templates/ in BlitterStudio/aar) and runs three phases: idle, where
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
 * The messages travel through the client transport module
 * (aar_xport_zz9000): the app's messages are taken from the submission queue
 * into the module's inbox, pings and mode changes are sent under the message
 * credit of the control block, and the lease is renewed from the polling
 * loop.
 *
 * usage: zz9k-app-ringtest <ringflood.elf> [seconds] [MAXLAT=<ms>]
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <devices/timer.h>
#include <exec/ports.h>
#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aar_xport_zz9000.h"
#include "zz9k-app-ringtest.h"

struct Device *TimerBase;

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
/* The lease is renewed this often; it is 2 s while the app runs. */
#define LEASE_INTERVAL_US 100000U
/* Outcome of a Ctrl-C, apart from the AAR_E_* codes. */
#define RT_CANCELLED (-1000)

static uint32_t eclock_hz;
static aar_xport app;

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

static const char *aar_text(int rc)
{
  switch (rc) {
  case AAR_OK: return "ok";
  case AAR_E_IO: return "io error";
  case AAR_E_INVAL: return "invalid";
  case AAR_E_NOMEM: return "no memory";
  case AAR_E_BUSY: return "busy";
  case AAR_E_NOENT: return "not found";
  case AAR_E_NOSYS: return "unsupported";
  case AAR_E_TIMEDOUT: return "timed out";
  case AAR_E_NORESOURCE: return "refused by the card";
  case AAR_E_STALE: return "claim gone";
  case RT_CANCELLED: return "cancelled";
  default: return "error";
  }
}

static uint32_t read_counter(volatile uint32_t *word, int flush)
{
  if (flush) CacheClearE((APTR)word, 4, CACRF_ClearD);
  return *word;
}

typedef struct Session {
  ZZ9KRingTest test;
  volatile uint32_t *counter;
  int flush;
  int sample;
  int last_status;
  uint32_t taken;
  uint32_t ping_id;
  uint32_t ctrl_id;
  uint64_t last_lease;
} Session;

/* Renews the lease about every LEASE_INTERVAL_US, from whichever loop is
 * running. */
static void lease(Session *s)
{
  uint64_t now = now_us();

  if (now - s->last_lease >= LEASE_INTERVAL_US) {
    s->last_lease = now;
    (void)aar_xport_lease_renew(&app);
  }
}

static int io_get(void *ctx, uint8_t message[32])
{
  Session *s = (Session *)ctx;

  s->last_status = aar_xport_get_message(&app, message);
  if (s->last_status == 1) return 0;
  if (s->last_status == 0) return 1;
  return -1;
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
 * break checks. Returns an AAR_E_* code, AAR_OK when no error occurred. */
static int drain(Session *s)
{
  ZZ9KRtIo io;

  lease(s);
  io.get = io_get;
  io.now = io_now;
  io.sample = io_sample;
  io.ctx = s;
  if (zz9k_rt_drain(&s->test, &io, ZZ9K_RT_DRAIN_BATCH, &s->taken) < 0) {
    return s->last_status < 0 ? s->last_status : AAR_E_IO;
  }
  return AAR_OK;
}

static int app_alive(aar_xport_status *info)
{
  if (aar_xport_status_get(&app, info) != AAR_OK) return 0;
  return info->state == AAR_STATE_RUNNING ||
         info->state == AAR_STATE_STARTING;
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
 * second, and watches for Ctrl-C. Returns AAR_OK or an error; *died is set
 * when the app stopped by itself. */
static int run_phase(Session *s, uint64_t duration_us, int pinging, int *died)
{
  aar_xport_status info;
  uint64_t start = now_us();
  uint64_t end = start + duration_us;
  uint64_t next_ping = start;
  uint64_t next_status = start + STATUS_INTERVAL_US;
  int status;

  *died = 0;
  for (;;) {
    uint64_t now;

    status = drain(s);
    if (status != AAR_OK) return status;
    now = now_us();
    if (now >= end) return AAR_OK;
    if (pinging && now >= next_ping) {
      uint8_t ping[AAR_MSG_BYTES];
      uint32_t id = s->ping_id + 1U;

      zz9k_rt_build_ping(ping, id, now);
      /* BUSY means no credit or a full queue: not sent, so not recorded. */
      if (aar_xport_send_message(&app, ping) == AAR_OK) {
        zz9k_rt_note_ping(&s->test, id, now);
        s->ping_id = id;
      }
      next_ping = now + PING_INTERVAL_US;
    }
    if (now >= next_status) {
      next_status = now + STATUS_INTERVAL_US;
      if (!app_alive(&info)) {
        *died = 1;
        return AAR_OK;
      }
    }
    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
      return RT_CANCELLED;
    }
  }
}

/* Sends a mode change and records it. */
static int send_ctrl(Session *s, uint32_t mode)
{
  uint8_t ctrl[AAR_MSG_BYTES];
  uint32_t id = s->ctrl_id + 1U;
  uint64_t start = now_us();
  int status;

  zz9k_rt_build_ctrl(ctrl, id, mode);
  do {
    status = aar_xport_send_message(&app, ctrl);
    if (status == AAR_OK) {
      zz9k_rt_note_ctrl(&s->test, id, mode);
      s->ctrl_id = id;
      return AAR_OK;
    }
    /* The credit comes back as the app takes messages; keep draining ours. */
    if (status == AAR_E_BUSY) {
      int drained = drain(s);

      if (drained != AAR_OK) return drained;
    }
  } while (status == AAR_E_BUSY && now_us() - start < CTRL_TIMEOUT_US);
  return status;
}

int main(int argc, char **argv)
{
  static Session session;
  aar_xport_board board;
  struct MsgPort *timer_port = 0;
  struct timerequest *timer_request = 0;
  const char *app_argv[1] = { "ringflood" };
  aar_xport_status info;
  uint32_t seconds = 10U;
  uint64_t max_rtt_us = 0U;
  uint64_t start;
  int died = 0;
  uint64_t elapsed = 0U;
  uint64_t grace_end;
  const char *mapping = "none";
  const char *reason = 0;
  const char *step = "open";
  uint32_t refusal = 0U;
  int board_open = 0;
  int claimed = 0;
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
    return 20;
  }
  TimerBase = timer_request->tr_node.io_Device;
  if (aar_xport_amiga_board_open(&board) != AAR_OK) {
    puts("zz9k-app-ringtest: no Zorro III board, zz9k.library or mmu.library");
    status = AAR_E_NOSYS;
    reason = "no Zorro III board";
    goto finish;
  }
  board_open = 1;

  trace("open");
  status = aar_xport_open(&app, &board, &refusal);
  if (status != AAR_OK) goto fail;
  claimed = 1;
  session.last_lease = now_us();
  step = "load";
  trace(step);
  status = aar_xport_load_file(&app, argv[1], 0U);
  if (status != AAR_OK) goto fail;
  step = "start";
  trace(step);
  status = aar_xport_start(&app, app_argv, 1U);
  if (status != AAR_OK) goto fail;

  /* The app's first message is INFO. */
  step = "info";
  start = now_us();
  while (!t->have_info) {
    status = drain(&session);
    if (status != AAR_OK) goto fail;
    if (now_us() - start > START_TIMEOUT_US) {
      reason = "no INFO message from the app";
      goto stop;
    }
    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
      status = RT_CANCELLED;
      goto fail;
    }
  }
  if (t->block_offset == ZZ9K_RT_NO_COUNTER || t->block_bytes < 4U) {
    reason = "the app has no usable counter block";
    goto stop;
  }
  /* The offset the app reports is a board offset, the token of its block. */
  {
    uint32_t block_off = t->block_offset & 0xfffU;
    uint32_t block_token = t->block_offset - block_off;
    volatile uint8_t *word = aar_xport_map(&app, block_token, block_off, 4U);
    uint32_t board_size = aar_xport_amiga_board_size(&board);

    if (!word && board_size && (t->block_offset >= board_size ||
                                board_size - t->block_offset < 4U)) {
      reason = "the app has no usable counter block";
      goto stop;
    }
    if (word) {
      session.counter = (volatile uint32_t *)word;
      mapping = "cache-inhibited (mmu.library)";
    } else {
      session.counter = (volatile uint32_t *)(uintptr_t)(
          aar_xport_amiga_board_base(&board) + t->block_offset);
      mapping = "CacheClearE before each read (no mmu.library mapping)";
      session.flush = 1;
    }
  }
  printf("zz9k-app-ringtest: counter block at board offset %08lx, %s\n",
         (unsigned long)t->block_offset, mapping);
  fflush(stdout);

  /* Phase 1, idle: the app is quiet and only answers pings, so the round
   * trip is the ring and the app's wake-up. MAXLAT applies to this phase. */
  step = "idle";
  trace(step);
  t->phase = ZZ9K_RT_PHASE_IDLE;
  status = run_phase(&session, IDLE_PHASE_US, 1, &died);
  if (status != AAR_OK) goto fail;
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
  if (status != AAR_OK) goto fail;
  t->phase = ZZ9K_RT_PHASE_FLOOD;
  start = now_us();
  status = run_phase(&session, (uint64_t)seconds * 1000000U, 1, &died);
  if (status != AAR_OK) goto fail;
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
  if (status != AAR_OK) goto fail;
  start = now_us();
  while (zz9k_rt_ctrls_outstanding(t) &&
         now_us() - start < SETTLE_TIMEOUT_US) {
    status = drain(&session);
    if (status != AAR_OK) goto fail;
    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
      status = RT_CANCELLED;
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
  if (status != AAR_OK) goto fail;
  if (died) {
    reason = "the app stopped during the run";
    goto stop;
  }

  /* No new pings now. Every ping that was sent must be answered, so wait a
   * moment for replies still in flight before stopping the app. */
  grace_end = now_us() + PING_GRACE_US;
  while (zz9k_rt_pings_outstanding(t) && now_us() < grace_end) {
    status = drain(&session);
    if (status != AAR_OK) goto fail;
    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
      status = RT_CANCELLED;
      goto fail;
    }
  }

stop:
  /* Stop sampling: the app's block goes away when it exits. */
  session.sample = 0;
  step = "stop";
  trace(step);
  status = aar_xport_stop(&app);
  if (status != AAR_OK) goto fail;
  start = now_us();
  for (;;) {
    status = drain(&session);
    if (status != AAR_OK) goto fail;
    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
      status = RT_CANCELLED;
      goto fail;
    }
    status = aar_xport_status_get(&app, &info);
    if (status != AAR_OK) goto fail;
    if (info.state == AAR_STATE_IDLE ||
        info.state == AAR_STATE_FAULTED) break;
    if (now_us() - start > STOP_TIMEOUT_US) {
      reason = reason ? reason : "the app did not stop";
      goto finish;
    }
  }
  /* The app has exited, so the ring only shrinks: read it dry. */
  do {
    status = drain(&session);
    if (status != AAR_OK) goto fail;
  } while (session.taken != 0U);
  if (!reason && (info.fault_class != AAR_FAULT_NONE ||
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
  {
    const aar_xport_stats *xs = aar_xport_stats_get(&app);

    printf("  transport: %lu messages sent, %lu taken, %lu lost to a full "
           "inbox, %lu doorbells\n", (unsigned long)xs->msgs_sent,
           (unsigned long)xs->msgs_taken, (unsigned long)xs->msgs_overflow,
           (unsigned long)xs->kicks);
  }
  goto finish;

fail:
  printf("zz9k-app-ringtest: FAIL at %s: %s (%d)\n", step,
         aar_text(status), status);
  if (status == AAR_E_NORESOURCE) {
    printf("the card refused the claim, reason %lu\n",
           (unsigned long)(refusal ? refusal : aar_xport_reason(&app)));
  }
  if (claimed && aar_xport_status_get(&app, &info) == AAR_OK) {
    printf("app state %lu fault %lu exit %ld handle %08lx pc %08lx\n",
           (unsigned long)info.state, (unsigned long)info.fault_class,
           (long)info.exit_code, (unsigned long)info.handle,
           (unsigned long)info.fault_pc);
  }
  reason = reason ? reason : "see above";
  status = AAR_E_IO;

finish:
  session.counter = 0;
  if (claimed) (void)aar_xport_release(&app);
  if (board_open) aar_xport_amiga_board_close(&board);
  CloseDevice((struct IORequest *)timer_request);
  DeleteIORequest((struct IORequest *)timer_request);
  DeleteMsgPort(timer_port);
  if (reason) {
    printf("zz9k-app-ringtest: FAIL: %s\n", reason);
    return 20;
  }
  puts("zz9k-app-ringtest: PASS");
  return 0;
}
