/*
 * Queue coherence probe, Amiga side (AAR cutover, slice 2). A temporary
 * diagnostic: it is removed when the AAR transport replaces it.
 *
 * It shares control blocks and queue pairs with firmware code on both ARM
 * cores through SDK shared buffers (the host-visible heap, a normal
 * write-back shareable mapping on the ARM side). The Amiga reads and writes
 * the same memory through a cache-inhibited mapping and checks what the
 * firmware wrote: sequence numbers, torn entries, pattern words and
 * index-before-data. The wire contract is in zz9k-qprobe.h, the accounting
 * and verdict in zz9k-qprobe-run.h, which has a host test.
 *
 * usage:
 *   zz9k-qprobe stream [QUEUES=n] [OWNER=core0|core1|mix] [SECONDS=s]
 *   zz9k-qprobe doorbell [QUEUES=1,8,32] [ITERATIONS=n]
 *   zz9k-qprobe all [QUEUES=n] [SECONDS=s] [ITERATIONS=n] [DOORBELL=list]
 *
 * Result lines are "QPROBE <part> PASS|FAIL key=value ...". Exit code 0
 * only when every part passed; 20 when a part failed, 10 for bad arguments.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <devices/timer.h>
#include <dos/dos.h>
#include <exec/execbase.h>
#include <exec/ports.h>
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zz9k/host.h"
#include "zz9k/reply.h"
#include "zz9k/request.h"

#include "zz9k-qprobe-run.h"

#define ZZ9K_MMU_MAPP_CACHEINHIBIT 0x00000040UL
#define ZZ9K_MMU_MAPP_COPYBACK 0x00002000UL
#define ZZ9K_MMU_MAPP_IMPRECISE 0x00200000UL
#define ZZ9K_MMU_MAPP_NONSERIALIZED 0x20000000UL
#define ZZ9K_MMU_CACHE_MASK (ZZ9K_MMU_MAPP_CACHEINHIBIT | \
                             ZZ9K_MMU_MAPP_COPYBACK | \
                             ZZ9K_MMU_MAPP_IMPRECISE | \
                             ZZ9K_MMU_MAPP_NONSERIALIZED)

#define DEFAULT_QUEUES 2U
#define DEFAULT_SECONDS 30U
#define DEFAULT_ITERATIONS 500U
#define MAX_ITERATIONS 100000U
#define PING_INTERVAL_US 1000000U
#define STATUS_INTERVAL_US 1000000U
#define PROGRESS_INTERVAL_US 5000000U
/* How long the firmware may take to take the last entries after the mode
 * goes back to IDLE, and how often STATUS is asked meanwhile. */
#define SETTLE_US 5000000U
#define SETTLE_POLL_US 20000U
/* How long a PONG may stay invisible after the KICK reply. */
#define PONG_TIMEOUT_US 50000U
/* Down entries taken from one queue per pass, so a producer that keeps the
 * ring full cannot starve the other queues or the checks around it. */
#define STREAM_BATCH 32U
#define DOORBELL_COUNTS_DEFAULT "1,8,32"

#define EXIT_PASS 0
#define EXIT_USAGE 10
#define EXIT_FAIL 20

struct mmu_context;

struct Device *TimerBase;

/* No automatic break handling: the probe checks Ctrl-C itself so it can
 * detach and free its queues. */
void __chkabort(void)
{
}

typedef struct Queue {
  uint32_t index;
  uint32_t owner;
  uint32_t origin_mask;
  ZZ9KSharedBuffer buf;
  volatile uint8_t *pair;
  int allocated;
  int attached;
  int dead;
  int settled;
  uint32_t down_tail; /* down entries consumed: the Amiga writes down_tail */
  uint32_t up_head;   /* up entries produced: the Amiga writes up_head */
  uint64_t next_ping;
  uint64_t next_poll;
  ZZ9KQrPings pings;
  ZZ9KQrQueue res;
} Queue;

static struct MsgPort *timer_port;
static struct timerequest *timer_request;
static uint32_t eclock_hz;
static ZZ9KContext *ctx;
static int flush_mode; /* no cache-inhibited mapping: CacheClearE instead */
static int user_break;
static Queue queues[ZZ9K_QP_MAX_QUEUES];
static ZZ9KQrTally tally;

static uint64_t now_ticks(void)
{
  struct EClockVal ev;

  ReadEClock(&ev);
  return ((uint64_t)ev.ev_hi << 32) | ev.ev_lo;
}

static uint64_t us_to_ticks(uint32_t us)
{
  return (uint64_t)us * eclock_hz / 1000000U;
}

static int broken(void)
{
  if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) user_break = 1;
  return user_break;
}

/* Cache-inhibits the pages holding [address, address + bytes) with
 * mmu.library, the way zz9k-app-ringtest does for its counter block. */
static int mmu_inhibit(uint32_t address, uint32_t bytes)
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
  last = (address + bytes + page_bytes - 1U) & ~(page_bytes - 1U);
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

/* Ordering. On a cache-inhibited, serialized mapping the 68k keeps its
 * accesses in program order, so only the compiler needs holding back. Without
 * the mapping the data cache is flushed after writes and before reads. */
static void compiler_barrier(void)
{
  __asm volatile("" : : : "memory");
}

static void acquire(const volatile void *p, uint32_t bytes)
{
  if (flush_mode) {
    CacheClearE((APTR)(uintptr_t)p, bytes, CACRF_ClearD);
  }
  compiler_barrier();
}

static void release(const volatile void *p, uint32_t bytes)
{
  compiler_barrier();
  if (flush_mode) {
    CacheClearE((APTR)(uintptr_t)p, bytes, CACRF_ClearD);
  }
}

static uint32_t read_index(const Queue *q, uint32_t offset)
{
  acquire(q->pair + offset, 4U);
  return zz9k_qp_get(q->pair, offset);
}

static void write_index(Queue *q, uint32_t offset, uint32_t value)
{
  compiler_barrier();
  zz9k_qp_set(q->pair, offset, value);
  release(q->pair + offset, 4U);
}

/* Reads the 16 words of a slot once, into `words` as native values, and
 * checks that copy. */
static uint32_t read_entry(volatile uint32_t *slot, uint32_t seq,
                           uint32_t queue, uint32_t origin_mask,
                           ZZ9KQpEntry *entry, uint32_t words[16])
{
  uint32_t raw[ZZ9K_QP_ENTRY_WORDS];
  uint32_t code;
  uint32_t i;

  acquire(slot, ZZ9K_QP_ENTRY_BYTES);
  for (i = 0U; i < ZZ9K_QP_ENTRY_WORDS; i++) raw[i] = slot[i];
  code = zz9k_qp_entry_check(raw, seq, queue, origin_mask, entry);
  for (i = 0U; i < ZZ9K_QP_ENTRY_WORDS; i++) words[i] = zz9k_qp_swap(raw[i]);
  return code;
}

/* Writes an up entry at the Amiga's up_head and moves the shadow head. The
 * head is published separately, after the entries. */
static void write_up_entry(Queue *q, uint32_t kind, uint32_t stamp,
                           uint32_t arg)
{
  volatile uint32_t *slot = zz9k_qp_slot(q->pair, ZZ9K_QP_UP_RING_OFFSET,
                                         q->up_head);

  zz9k_qp_entry_write(slot, q->up_head + 1U, kind, q->index,
                      ZZ9K_QP_ORIGIN_AMIGA, stamp, arg);
  release(slot, ZZ9K_QP_ENTRY_BYTES);
  q->up_head++;
}

static void publish_up(Queue *q)
{
  write_index(q, ZZ9K_QP_UP_HEAD, q->up_head);
}

static void set_mode(Queue *q, uint32_t mode)
{
  write_index(q, ZZ9K_QP_HDR_MODE, mode);
}

static uint32_t up_used(const Queue *q)
{
  return q->up_head - read_index(q, ZZ9K_QP_UP_TAIL);
}

/* Sends one PING if the up ring has room for it and `reserve` more entries.
 * Returns 1 when sent. */
static int send_ping(Queue *q, uint32_t reserve)
{
  uint32_t id;

  if (up_used(q) + 1U + reserve > ZZ9K_QP_ENTRIES) return 0;
  id = zz9k_qr_pings_push(&q->pings);
  if (id == 0U) return 0;
  write_up_entry(q, ZZ9K_QP_KIND_PING, id, 0U);
  publish_up(q);
  q->res.pings_sent++;
  return 1;
}

/* Takes the down entries that are there: checks each, writes one COMPLETE
 * per entry while the up ring has room, then publishes up_head and
 * down_tail. Returns how many it took. */
static uint32_t service_queue(Queue *q, uint32_t budget)
{
  uint32_t head;
  uint32_t up_tail;
  int32_t available;
  uint32_t taken = 0U;

  if (q->dead) return 0U;
  head = read_index(q, ZZ9K_QP_DOWN_HEAD);
  available = zz9k_qp_available(head, q->down_tail);
  if (available < 0) {
    zz9k_qr_note_error(&tally, q->index, head, ZZ9K_QP_ERR_INDEX, 0);
    q->dead = 1;
    return 0U;
  }
  if (available == 0) return 0U;
  up_tail = read_index(q, ZZ9K_QP_UP_TAIL);
  if (q->up_head - up_tail > ZZ9K_QP_ENTRIES) {
    zz9k_qr_note_error(&tally, q->index, up_tail, ZZ9K_QP_ERR_INDEX, 0);
    q->dead = 1;
    return 0U;
  }
  compiler_barrier();
  while (available > 0 && taken < budget &&
         q->up_head - up_tail < ZZ9K_QP_ENTRIES) {
    uint32_t seq = q->down_tail + 1U;
    uint32_t words[ZZ9K_QP_ENTRY_WORDS];
    ZZ9KQpEntry entry;
    uint32_t code;

    code = read_entry(zz9k_qp_slot(q->pair, ZZ9K_QP_DOWN_RING_OFFSET,
                                   q->down_tail),
                      seq, q->index, q->origin_mask, &entry, words);
    if (code == ZZ9K_QP_OK) {
      if (entry.kind == ZZ9K_QP_KIND_STREAM) {
        q->res.stream_entries++;
      } else if (entry.kind == ZZ9K_QP_KIND_PONG) {
        if (zz9k_qr_pings_match(&q->pings, entry.arg)) {
          q->res.pings_answered++;
        } else {
          q->res.pong_bad++;
          code = ZZ9K_QP_ERR_ORDER;
        }
      } else {
        code = ZZ9K_QP_ERR_KIND;
      }
    }
    if (code != ZZ9K_QP_OK) {
      zz9k_qr_note_error(&tally, q->index, seq, code, words);
    }
    /* The entry is consumed either way; the COMPLETE names it. */
    write_up_entry(q, ZZ9K_QP_KIND_COMPLETE, 0U, seq);
    q->down_tail++;
    available--;
    taken++;
  }
  if (taken) {
    publish_up(q);
    write_index(q, ZZ9K_QP_DOWN_TAIL, q->down_tail);
  }
  return taken;
}

/* Submits a request and polls for its completion without sleeping.
 * zz9k_call waits for the completion signal between polls, which adds up to
 * a frame to every round trip; the doorbell timing must not include that.
 * Returns the reply's status, or a transport error. */
static int call_polled(ZZ9KRequest *request, ZZ9KMailboxEntry *reply)
{
  uint64_t limit = now_ticks() + us_to_ticks(2000000U);
  uint32_t request_id = 0U;
  int status;

  status = zz9k_submit(ctx, request, &request_id);
  if (status != ZZ9K_STATUS_QUEUED) return status;
  for (;;) {
    memset(reply, 0, sizeof(*reply));
    status = zz9k_poll(ctx, reply);
    if (status == ZZ9K_STATUS_OK && reply->request_id == request_id)
      return reply->status;
    if (status != ZZ9K_STATUS_OK && status != ZZ9K_STATUS_BUSY) return status;
    if (now_ticks() > limit) return ZZ9K_STATUS_TIMEOUT;
  }
}

/* One mailbox command: request words in, reply words out. t_start and t_end
 * (optional) take the clock around the call itself. Returns a ZZ9K_STATUS
 * code. */
static int qp_cmd(const uint32_t *words, uint32_t count, uint32_t *out,
                  uint32_t out_words, uint64_t *t_start, uint64_t *t_end)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  uint32_t i;
  int status;

  zz9k_request_init(&request, ZZ9K_OP_DIAG_QPROBE);
  request.entry.payload_len = (uint16_t)(count * 4U);
  for (i = 0U; i < count; i++)
    zz9k_put_be32(&request.entry.payload.inline_data[i * 4U], words[i]);
  if (t_start) *t_start = now_ticks();
  status = call_polled(&request, &reply);
  if (t_end) *t_end = now_ticks();
  if (status != ZZ9K_STATUS_OK) return status;
  status = zz9k_reply_require(&reply, ZZ9K_OP_DIAG_QPROBE, out_words * 4U);
  if (status != ZZ9K_STATUS_OK) return status;
  for (i = 0U; i < out_words; i++)
    out[i] = zz9k_get_be32(&reply.payload.inline_data[i * 4U]);
  return ZZ9K_STATUS_OK;
}

static int cmd_failed(const char *what, int status)
{
  printf("zz9k-qprobe: %s failed: %s (%d)\n", what, zz9k_status_name(status),
         status);
  return status;
}

static int read_status(const Queue *q, uint32_t status[ZZ9K_QP_STATUS_WORDS])
{
  uint32_t cmd[2];

  cmd[0] = ZZ9K_QP_CMD_STATUS;
  cmd[1] = q->index;
  return qp_cmd(cmd, 2U, status, ZZ9K_QP_STATUS_WORDS, 0, 0);
}

static int release_queue(Queue *q, uint32_t status[ZZ9K_QP_STATUS_WORDS]);

/* Allocates the pair, maps it, attaches it and checks the header the
 * firmware wrote. Returns 0, or -1 with the buffer released. */
static int attach_queue(Queue *q, uint32_t index, uint32_t owner)
{
  uint32_t cmd[4];
  uint32_t out[ZZ9K_QP_ATTACH_WORDS];
  uint32_t header[5];
  int status;

  memset(q, 0, sizeof(*q));
  q->index = index;
  q->owner = owner;
  q->origin_mask = zz9k_qr_origin_mask(owner);
  zz9k_qr_pings_init(&q->pings);
  q->res.queue = index;
  q->res.owner = owner;
  status = zz9k_alloc_shared(ctx, ZZ9K_QP_PAIR_BYTES, ZZ9K_QP_PAIR_ALIGN, 0U,
                             &q->buf);
  if (status != ZZ9K_STATUS_OK) {
    cmd_failed("alloc shared buffer", status);
    return -1;
  }
  q->allocated = 1;
  q->pair = (volatile uint8_t *)q->buf.data;
  if (!q->pair || q->buf.length < ZZ9K_QP_PAIR_BYTES ||
      ((uint32_t)(uintptr_t)q->pair & (ZZ9K_QP_PAIR_ALIGN - 1U)) != 0U) {
    printf("zz9k-qprobe: shared buffer at %08lx, %lu bytes is not a usable "
           "pair\n", (unsigned long)(uintptr_t)q->pair,
           (unsigned long)q->buf.length);
    release_queue(q, 0);
    return -1;
  }
  if (!mmu_inhibit((uint32_t)(uintptr_t)q->pair, ZZ9K_QP_PAIR_BYTES)) {
    flush_mode = 1;
  }
  cmd[0] = ZZ9K_QP_CMD_ATTACH;
  cmd[1] = q->buf.handle;
  cmd[2] = index;
  cmd[3] = owner;
  status = qp_cmd(cmd, 4U, out, ZZ9K_QP_ATTACH_WORDS, 0, 0);
  if (status != ZZ9K_STATUS_OK) {
    cmd_failed("ATTACH", status);
    printf("zz9k-qprobe: queue %lu owner %lu: handle %08lx at %08lx, %lu bytes"
           "; MEM_FILL on it: %d\n", (unsigned long)index,
           (unsigned long)owner, (unsigned long)q->buf.handle,
           (unsigned long)(uintptr_t)q->pair, (unsigned long)q->buf.length,
           zz9k_mem_fill(ctx, q->buf.handle, 0U, 64U, 0U));
    release_queue(q, 0);
    return -1;
  }
  q->attached = 1;
  header[0] = read_index(q, ZZ9K_QP_HDR_MAGIC);
  header[1] = zz9k_qp_get(q->pair, ZZ9K_QP_HDR_VERSION);
  header[2] = zz9k_qp_get(q->pair, ZZ9K_QP_HDR_QUEUE);
  header[3] = zz9k_qp_get(q->pair, ZZ9K_QP_HDR_OWNER);
  header[4] = zz9k_qp_get(q->pair, ZZ9K_QP_HDR_MODE);
  if (out[ZZ9K_QP_ATTACH_PAIR_BYTES] < ZZ9K_QP_PAIR_BYTES ||
      out[ZZ9K_QP_ATTACH_VERSION] != ZZ9K_QP_VERSION ||
      header[0] != ZZ9K_QP_MAGIC || header[1] != ZZ9K_QP_VERSION ||
      header[2] != index || header[3] != owner ||
      header[4] != ZZ9K_QP_MODE_IDLE) {
    printf("zz9k-qprobe: queue %lu: bad header after ATTACH: pair %lu "
           "version %lu; magic %08lx version %lu queue %lu owner %lu mode "
           "%lu\n", (unsigned long)index,
           (unsigned long)out[ZZ9K_QP_ATTACH_PAIR_BYTES],
           (unsigned long)out[ZZ9K_QP_ATTACH_VERSION],
           (unsigned long)header[0], (unsigned long)header[1],
           (unsigned long)header[2], (unsigned long)header[3],
           (unsigned long)header[4]);
    release_queue(q, 0);
    return -1;
  }
  return 0;
}

/* Detaches and frees. `status` (optional) takes the final STATUS from the
 * DETACH reply; the return is 1 when it did. */
static int release_queue(Queue *q, uint32_t status[ZZ9K_QP_STATUS_WORDS])
{
  uint32_t cmd[2];
  uint32_t out[ZZ9K_QP_STATUS_WORDS];
  int ok = 0;

  if (q->attached) {
    cmd[0] = ZZ9K_QP_CMD_DETACH;
    cmd[1] = q->index;
    if (qp_cmd(cmd, 2U, out, ZZ9K_QP_STATUS_WORDS, 0, 0) ==
        ZZ9K_STATUS_OK) {
      ok = 1;
      if (status) memcpy(status, out, sizeof(out));
    }
    q->attached = 0;
  }
  if (q->allocated) {
    zz9k_free_shared(ctx, q->buf.handle);
    q->allocated = 0;
  }
  q->pair = 0;
  return ok;
}

static void print_us(uint64_t ns)
{
  printf("%lu.%02lu", (unsigned long)(ns / 1000U),
         (unsigned long)((ns % 1000U) / 10U));
}

/* " key=min/avg/max" in microseconds. */
static void print_stats(const char *key, const ZZ9KQrStats *s, uint32_t hz)
{
  printf(" %s=", key);
  print_us(zz9k_qr_ticks_to_ns(s->min, hz));
  putchar('/');
  print_us(zz9k_qr_ticks_to_ns(zz9k_qr_stats_avg(s), hz));
  putchar('/');
  print_us(zz9k_qr_ticks_to_ns(s->max, hz));
}

static void print_words(const uint32_t *w)
{
  uint32_t i;

  for (i = 0U; i < ZZ9K_QP_ENTRY_WORDS; i += 8U) {
    uint32_t j;

    printf("    w%02lu-%02lu:", (unsigned long)i, (unsigned long)(i + 7U));
    for (j = 0U; j < 8U; j++) printf(" %08lx", (unsigned long)w[i + j]);
    putchar('\n');
  }
}

/* The evidence of a failed run: the first Amiga-side errors with their raw
 * entries, the error counts by code, and the firmware's own first error. */
static void print_errors(const ZZ9KQrTally *t)
{
  uint32_t i;

  for (i = 0U; i < t->logged; i++) {
    const ZZ9KQrError *e = &t->log[i];

    printf("  error: queue %lu seq %lu code %s (%lu)\n",
           (unsigned long)e->queue, (unsigned long)e->seq,
           zz9k_qp_error_name(e->code), (unsigned long)e->code);
    print_words(e->words);
  }
  if (t->errors > t->logged) {
    printf("  ... %lu more errors not shown\n",
           (unsigned long)(t->errors - t->logged));
  }
  if (t->errors) {
    printf("  errors by code:");
    for (i = 0U; i < ZZ9K_QP_ERR_COUNT; i++) {
      if (t->by_code[i])
        printf(" %s=%lu", zz9k_qp_error_name(i),
               (unsigned long)t->by_code[i]);
    }
    putchar('\n');
  }
  if (t->fw_up_errors) {
    printf("  firmware: %lu up errors, first on queue %lu: %s (%lu) at seq "
           "%lu\n", (unsigned long)t->fw_up_errors,
           (unsigned long)t->fw_first_error_queue,
           zz9k_qp_error_name(t->fw_first_error),
           (unsigned long)t->fw_first_error,
           (unsigned long)t->fw_first_error_seq);
  }
}

static void print_queue(const ZZ9KQrQueue *r)
{
  printf("  queue %lu %s: down %lu (stream %lu), up %lu, pings %lu/%lu",
         (unsigned long)r->queue, zz9k_qr_owner_name(r->owner),
         (unsigned long)r->down_consumed, (unsigned long)r->stream_entries,
         (unsigned long)r->up_produced, (unsigned long)r->pings_answered,
         (unsigned long)r->pings_sent);
  if (r->have_status) {
    printf("; firmware: down %lu, up checked %lu, errors %lu, completed "
           "%lu, pongs %lu, ring full %lu, passes %lu\n",
           (unsigned long)r->fw_down_produced,
           (unsigned long)r->fw_up_checked, (unsigned long)r->fw_up_errors,
           (unsigned long)r->fw_completed, (unsigned long)r->fw_pongs,
           (unsigned long)r->fw_ring_full, (unsigned long)r->fw_passes);
  } else {
    puts("; no firmware status");
  }
}

/* Ends a part: details, then the one result line. Returns 1 on a pass. */
static int finish_part(const char *part, const ZZ9KQrTally *t,
                       const char *detail)
{
  const char *reason = zz9k_qr_verdict(t);

  print_errors(t);
  printf("QPROBE %s %s", part, reason ? "FAIL" : "PASS");
  if (reason) printf(" reason=%s", reason);
  printf(" queues=%lu%s mapping=%s down=%lu stream=%lu up=%lu "
         "pings=%lu/%lu min_queue_stream=%lu errors=%lu fw_errors=%lu\n",
         (unsigned long)t->queues, detail,
         flush_mode ? "cacheclear" : "ci",
         (unsigned long)t->down_entries, (unsigned long)t->stream_entries,
         (unsigned long)t->up_entries, (unsigned long)t->pings_answered,
         (unsigned long)t->pings_sent, (unsigned long)t->min_queue_stream,
         (unsigned long)t->errors, (unsigned long)t->fw_up_errors);
  fflush(stdout);
  return reason == 0;
}

/* Releases `count` queues, folding each one's final firmware STATUS and the
 * Amiga's own counts into the tally. */
static void close_queues(uint32_t count, int verbose)
{
  uint32_t i;

  for (i = 0U; i < count; i++) {
    Queue *q = &queues[i];
    uint32_t status[ZZ9K_QP_STATUS_WORDS];

    q->res.down_consumed = q->down_tail;
    q->res.up_produced = q->up_head;
    if (release_queue(q, status)) zz9k_qr_queue_set_status(&q->res, status);
    zz9k_qr_queue_done(&tally, &q->res);
    if (verbose || !q->res.have_status || q->res.fw_up_errors != 0U ||
        q->res.fw_down_produced != q->res.down_consumed ||
        q->res.fw_up_checked != q->res.up_produced ||
        q->res.pings_answered != q->res.pings_sent)
      print_queue(&q->res);
  }
}

static void release_all(uint32_t count)
{
  uint32_t i;

  for (i = 0U; i < count; i++) release_queue(&queues[i], 0);
}

/* After the mode went back to IDLE: keeps taking entries until the firmware
 * says it produced nothing the Amiga has not taken and consumed every up
 * entry, or SETTLE_US passes. */
static void settle(uint32_t count)
{
  uint64_t end = now_ticks() + us_to_ticks(SETTLE_US);
  uint64_t poll = us_to_ticks(SETTLE_POLL_US);

  for (;;) {
    uint64_t now;
    uint32_t pending = 0U;
    uint32_t i;

    for (i = 0U; i < count; i++) {
      Queue *q = &queues[i];
      uint32_t status[ZZ9K_QP_STATUS_WORDS];

      service_queue(q, STREAM_BATCH);
      if (q->dead || q->settled) continue;
      now = now_ticks();
      if (now >= q->next_poll) {
        q->next_poll = now + poll;
        if (read_status(q, status) == ZZ9K_STATUS_OK &&
            status[ZZ9K_QP_STATUS_DOWN_PRODUCED] == q->down_tail &&
            status[ZZ9K_QP_STATUS_UP_CHECKED] == q->up_head &&
            q->pings.count == 0U) {
          q->settled = 1;
          continue;
        }
      }
      pending++;
    }
    if (pending == 0U) return;
    if (now_ticks() >= end) return;
    /* A new Ctrl-C gives up waiting. */
    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) return;
  }
}

static int run_stream(uint32_t choice, uint32_t count, uint32_t seconds)
{
  char part[24];
  char detail[64];
  uint32_t attached = 0U;
  uint32_t i;
  uint64_t start;
  uint64_t end;
  uint64_t second;
  uint64_t next_status;
  uint64_t next_progress;
  const char *stop = 0;

  sprintf(part, "stream-%s", zz9k_qr_owner_name(choice));
  sprintf(detail, " owner=%s seconds=%lu", zz9k_qr_owner_name(choice),
          (unsigned long)seconds);
  zz9k_qr_tally_init(&tally, 1, ZZ9K_QR_MIN_STREAM_ENTRIES, 1U);
  flush_mode = 0;
  for (i = 0U; i < count; i++) {
    if (attach_queue(&queues[i], i, zz9k_qr_queue_owner(choice, i)) != 0) {
      tally.setup_failed = 1U;
      break;
    }
    attached++;
  }
  if (tally.setup_failed) {
    release_all(attached);
    return finish_part(part, &tally, detail);
  }
  printf("zz9k-qprobe: %s: %lu queues, %lu s, %s\n", part,
         (unsigned long)count, (unsigned long)seconds,
         flush_mode ? "no cache-inhibited mapping, CacheClearE before "
                      "reads and after writes"
                    : "cache-inhibited mapping (mmu.library)");
  for (i = 0U; i < count; i++) {
    printf("  queue %lu %s at %08lx\n", (unsigned long)i,
           zz9k_qr_owner_name(queues[i].owner),
           (unsigned long)(uintptr_t)queues[i].pair);
  }
  fflush(stdout);

  second = us_to_ticks(1000000U);
  start = now_ticks();
  end = start + (uint64_t)seconds * second;
  next_status = start + us_to_ticks(STATUS_INTERVAL_US);
  next_progress = start + us_to_ticks(PROGRESS_INTERVAL_US);
  for (i = 0U; i < count; i++) {
    queues[i].next_ping = start;
    set_mode(&queues[i], ZZ9K_QP_MODE_STREAM);
  }

  for (;;) {
    uint64_t now;

    for (i = 0U; i < count; i++) {
      Queue *q = &queues[i];

      service_queue(q, STREAM_BATCH);
      now = now_ticks();
      if (!q->dead && q->pings.count == 0U && now >= q->next_ping &&
          send_ping(q, 0U)) {
        q->next_ping = now + us_to_ticks(PING_INTERVAL_US);
      }
    }
    now = now_ticks();
    if (broken()) {
      tally.interrupted = 1U;
      stop = "Ctrl-C";
      break;
    }
    if (now >= end) break;
    if (tally.errors >= ZZ9K_QR_ABORT_ERRORS) {
      stop = "too many Amiga-side errors";
      break;
    }
    if (now >= next_status) {
      uint32_t failed = 0U;

      next_status = now + us_to_ticks(STATUS_INTERVAL_US);
      for (i = 0U; i < count && !failed; i++) {
        uint32_t status[ZZ9K_QP_STATUS_WORDS];

        if (read_status(&queues[i], status) != ZZ9K_STATUS_OK) {
          tally.call_failures++;
          stop = "STATUS failed";
          failed = 1U;
        } else if (status[ZZ9K_QP_STATUS_UP_ERRORS] != 0U) {
          stop = "the firmware reports up errors";
          failed = 1U;
        }
      }
      if (failed) break;
    }
    if (now >= next_progress) {
      uint32_t entries = 0U;

      next_progress = now + us_to_ticks(PROGRESS_INTERVAL_US);
      for (i = 0U; i < count; i++) entries += queues[i].res.stream_entries;
      printf("  %lu s: %lu stream entries, %lu errors\n",
             (unsigned long)((now - start) / second),
             (unsigned long)entries, (unsigned long)tally.errors);
      fflush(stdout);
    }
  }
  if (stop) printf("zz9k-qprobe: stopping early: %s\n", stop);

  for (i = 0U; i < count; i++) set_mode(&queues[i], ZZ9K_QP_MODE_IDLE);
  settle(count);
  if (user_break) tally.interrupted = 1U;
  close_queues(count, 1);
  return finish_part(part, &tally, detail);
}

/* One doorbell iteration on queue `q`: a PING, the KICK, then the wait for
 * the PONG in the down ring. Returns 0 to go on, 1 to stop the count (a
 * failed call or a PONG that never showed; both are in the tally). */
static int doorbell_once(Queue *q, uint32_t nq, ZZ9KQrStats *kick,
                         ZZ9KQrStats *visible, ZZ9KQrStats *lag,
                         ZZ9KQrStats *handler, uint32_t *handler_hz)
{
  uint32_t cmd = ZZ9K_QP_CMD_KICK;
  uint32_t out[ZZ9K_QP_KICK_WORDS];
  uint64_t t0;
  uint64_t t1;
  uint64_t t2;
  uint64_t limit = us_to_ticks(PONG_TIMEOUT_US);
  uint32_t answered = q->res.pings_answered;
  int status;

  /* The PING and the COMPLETE for its PONG need room; the firmware only
   * takes entries on a KICK, so make some if the ring is filling. */
  if (up_used(q) + 2U > ZZ9K_QP_ENTRIES) {
    status = qp_cmd(&cmd, 1U, out, ZZ9K_QP_KICK_WORDS, 0, 0);
    if (status != ZZ9K_STATUS_OK) {
      cmd_failed("KICK", status);
      tally.call_failures++;
      return 1;
    }
    if (up_used(q) + 2U > ZZ9K_QP_ENTRIES) {
      printf("zz9k-qprobe: queue %lu: up ring stays full after a KICK\n",
             (unsigned long)q->index);
      tally.call_failures++;
      return 1;
    }
  }
  if (!send_ping(q, 1U)) {
    tally.call_failures++;
    return 1;
  }
  status = qp_cmd(&cmd, 1U, out, ZZ9K_QP_KICK_WORDS, &t0, &t1);
  if (status != ZZ9K_STATUS_OK) {
    cmd_failed("KICK", status);
    tally.call_failures++;
    return 1;
  }
  if (out[ZZ9K_QP_KICK_QUEUES] < nq) {
    printf("zz9k-qprobe: KICK scanned %lu queues, %lu are attached\n",
           (unsigned long)out[ZZ9K_QP_KICK_QUEUES], (unsigned long)nq);
    tally.call_failures++;
    return 1;
  }
  zz9k_qr_stats_add(kick, t1 - t0);
  zz9k_qr_stats_add(handler, out[ZZ9K_QP_KICK_TICKS]);
  *handler_hz = out[ZZ9K_QP_KICK_TIMER_HZ];

  for (;;) {
    uint32_t head = read_index(q, ZZ9K_QP_DOWN_HEAD);

    t2 = now_ticks();
    if (head != q->down_tail) break;
    if (t2 - t0 > limit) {
      printf("zz9k-qprobe: queue %lu: no PONG after %lu ms\n",
             (unsigned long)q->index, (unsigned long)(PONG_TIMEOUT_US / 1000U));
      return 1;
    }
  }
  zz9k_qr_stats_add(visible, t2 - t0);
  zz9k_qr_stats_add(lag, t2 - t1);
  service_queue(q, STREAM_BATCH);
  /* The PONG must have been the entry just taken and matched. */
  return q->res.pings_answered == answered + 1U ? 0 : 1;
}

/* After the iterations: KICKs until the firmware has consumed every up entry
 * of every queue, bounded. */
static void doorbell_settle(uint32_t nq)
{
  uint32_t cmd = ZZ9K_QP_CMD_KICK;
  uint32_t out[ZZ9K_QP_KICK_WORDS];
  uint32_t round;

  for (round = 0U; round < 20U; round++) {
    uint32_t pending = 0U;
    uint32_t i;

    if (qp_cmd(&cmd, 1U, out, ZZ9K_QP_KICK_WORDS, 0, 0) != ZZ9K_STATUS_OK) {
      tally.call_failures++;
      return;
    }
    for (i = 0U; i < nq; i++) {
      uint32_t status[ZZ9K_QP_STATUS_WORDS];

      if (queues[i].dead) continue;
      service_queue(&queues[i], STREAM_BATCH);
      if (read_status(&queues[i], status) != ZZ9K_STATUS_OK ||
          status[ZZ9K_QP_STATUS_UP_CHECKED] != queues[i].up_head)
        pending++;
    }
    if (pending == 0U) return;
  }
}

static int run_doorbell(const uint32_t *counts, uint32_t ncounts,
                        uint32_t iterations)
{
  char detail[96];
  static const uint8_t payload[4] = { 'z', 'z', '9', 'k' };
  ZZ9KQrStats base;
  uint32_t i;
  uint32_t c;

  sprintf(detail, " iterations=%lu counts=", (unsigned long)iterations);
  for (c = 0U; c < ncounts; c++)
    sprintf(detail + strlen(detail), "%s%lu", c ? "," : "",
            (unsigned long)counts[c]);
  zz9k_qr_tally_init(&tally, 0, 0U, 1U);
  flush_mode = 0;

  /* The mailbox alone: what a KICK costs before any queue work. */
  zz9k_qr_stats_init(&base);
  for (i = 0U; i < iterations && !tally.call_failures && !broken(); i++) {
    ZZ9KRequest request;
    ZZ9KMailboxEntry reply;
    uint64_t t0;
    uint64_t t1;
    int status;

    memset(&request, 0, sizeof(request));
    (void)zz9k_request_ping(&request, payload, sizeof(payload));
    t0 = now_ticks();
    status = call_polled(&request, &reply);
    t1 = now_ticks();
    if (status != ZZ9K_STATUS_OK) {
      cmd_failed("baseline ping", status);
      tally.call_failures++;
    } else if (reply.payload_len != sizeof(payload) ||
               memcmp(reply.payload.inline_data, payload,
                      sizeof(payload)) != 0) {
      puts("zz9k-qprobe: baseline ping echo mismatch");
      tally.call_failures++;
    } else {
      zz9k_qr_stats_add(&base, t1 - t0);
    }
  }
  printf("QPROBE-STAT doorbell baseline=ping n=%lu", (unsigned long)base.n);
  print_stats("ping_us", &base, eclock_hz);
  putchar('\n');

  for (c = 0U; c < ncounts && !tally.call_failures && !tally.setup_failed &&
               !broken(); c++) {
    uint32_t nq = counts[c];
    uint32_t attached = 0U;
    ZZ9KQrStats kick;
    ZZ9KQrStats visible;
    ZZ9KQrStats lag;
    ZZ9KQrStats handler;
    uint32_t handler_hz = 0U;
    uint32_t done = 0U;

    zz9k_qr_stats_init(&kick);
    zz9k_qr_stats_init(&visible);
    zz9k_qr_stats_init(&lag);
    zz9k_qr_stats_init(&handler);
    for (i = 0U; i < nq; i++) {
      if (attach_queue(&queues[i], i, ZZ9K_QP_OWNER_DOORBELL) != 0) {
        tally.setup_failed = 1U;
        break;
      }
      attached++;
    }
    if (tally.setup_failed) {
      release_all(attached);
      break;
    }
    for (i = 0U; i < iterations; i++) {
      if (doorbell_once(&queues[i % nq], nq, &kick, &visible, &lag, &handler,
                        &handler_hz) != 0)
        break;
      done++;
      if (broken()) {
        tally.interrupted = 1U;
        break;
      }
    }
    if (!tally.call_failures && !tally.interrupted && done == iterations)
      doorbell_settle(nq);
    printf("QPROBE-STAT doorbell queues=%lu n=%lu", (unsigned long)nq,
           (unsigned long)done);
    print_stats("kick_us", &kick, eclock_hz);
    print_stats("pong_visible_us", &visible, eclock_hz);
    print_stats("pong_after_kick_us", &lag, eclock_hz);
    print_stats("handler_us", &handler, handler_hz);
    putchar('\n');
    close_queues(nq, 0);
    /* A hard failure ends the run; later counts would only repeat it. */
    if (tally.errors != 0U || tally.pings_answered < tally.pings_sent) break;
  }
  if (user_break) tally.interrupted = 1U;
  return finish_part("doorbell", &tally, detail);
}

static int open_timer(void)
{
  timer_port = CreateMsgPort();
  if (timer_port) {
    timer_request = (struct timerequest *)CreateIORequest(
        timer_port, sizeof(struct timerequest));
  }
  if (timer_request &&
      OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
                 (struct IORequest *)timer_request, 0) == 0) {
    struct EClockVal ev;

    TimerBase = timer_request->tr_node.io_Device;
    eclock_hz = ReadEClock(&ev);
    if (eclock_hz != 0U) return 1;
    CloseDevice((struct IORequest *)timer_request);
  }
  if (timer_request) DeleteIORequest((struct IORequest *)timer_request);
  if (timer_port) DeleteMsgPort(timer_port);
  timer_request = 0;
  timer_port = 0;
  return 0;
}

static void close_timer(void)
{
  CloseDevice((struct IORequest *)timer_request);
  DeleteIORequest((struct IORequest *)timer_request);
  DeleteMsgPort(timer_port);
}

static void usage(void)
{
  puts("usage: zz9k-qprobe stream [QUEUES=n] [OWNER=core0|core1|mix] "
       "[SECONDS=s]\n"
       "       zz9k-qprobe doorbell [QUEUES=1,8,32] [ITERATIONS=n]\n"
       "       zz9k-qprobe all [QUEUES=n] [SECONDS=s] [ITERATIONS=n] "
       "[DOORBELL=1,8,32]\n"
       "defaults: 2 queues, OWNER=mix, 30 s, 500 iterations");
}

int main(int argc, char **argv)
{
  uint32_t stream_queues = DEFAULT_QUEUES;
  uint32_t owner = ZZ9K_QR_OWNER_MIX;
  uint32_t seconds = DEFAULT_SECONDS;
  uint32_t iterations = DEFAULT_ITERATIONS;
  uint32_t counts[ZZ9K_QR_MAX_COUNTS];
  uint32_t ncounts;
  uint32_t max_count = 0U;
  uint32_t i;
  int do_stream = 0;
  int do_doorbell = 0;
  int all = 0;
  int passed = 0;
  int parts = 0;
  int status;

  ncounts = zz9k_qr_parse_counts(DOORBELL_COUNTS_DEFAULT, counts,
                                 ZZ9K_QR_MAX_COUNTS);
  if (argc < 2) {
    usage();
    return EXIT_USAGE;
  }
  if (zz9k_qr_equals(argv[1], "stream")) do_stream = 1;
  else if (zz9k_qr_equals(argv[1], "doorbell")) do_doorbell = 1;
  else if (zz9k_qr_equals(argv[1], "all")) do_stream = do_doorbell = all = 1;
  else {
    usage();
    return EXIT_USAGE;
  }
  for (i = 2U; i < (uint32_t)argc; i++) {
    const char *v;
    int ok = 0;

    if ((v = zz9k_qr_arg_value(argv[i], "SECONDS")) != 0 &&
        (do_stream || all)) {
      ok = zz9k_qr_parse_u32(v, 1U, 3600U, &seconds);
    } else if ((v = zz9k_qr_arg_value(argv[i], "ITERATIONS")) != 0 &&
               do_doorbell) {
      ok = zz9k_qr_parse_u32(v, 1U, MAX_ITERATIONS, &iterations);
    } else if ((v = zz9k_qr_arg_value(argv[i], "OWNER")) != 0 &&
               do_stream && !all) {
      ok = zz9k_qr_parse_owner(v, &owner);
    } else if ((v = zz9k_qr_arg_value(argv[i], "QUEUES")) != 0) {
      if (do_doorbell && !all) {
        ncounts = zz9k_qr_parse_counts(v, counts, ZZ9K_QR_MAX_COUNTS);
        ok = ncounts != 0U;
      } else {
        ok = zz9k_qr_parse_u32(v, 1U, ZZ9K_QP_MAX_QUEUES, &stream_queues);
      }
    } else if ((v = zz9k_qr_arg_value(argv[i], "DOORBELL")) != 0 && all) {
      ncounts = zz9k_qr_parse_counts(v, counts, ZZ9K_QR_MAX_COUNTS);
      ok = ncounts != 0U;
    }
    if (!ok) {
      printf("zz9k-qprobe: bad argument %s\n", argv[i]);
      usage();
      return EXIT_USAGE;
    }
  }
  for (i = 0U; i < ncounts; i++)
    if (counts[i] > max_count) max_count = counts[i];
  if (do_doorbell && iterations < max_count) {
    printf("zz9k-qprobe: ITERATIONS must be at least the largest queue "
           "count (%lu)\n", (unsigned long)max_count);
    return EXIT_USAGE;
  }

  if (!open_timer()) {
    puts("zz9k-qprobe: timer.device is not available");
    return EXIT_FAIL;
  }
  status = zz9k_open(&ctx);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-qprobe: open failed: %s (%d)\n", zz9k_status_name(status),
           status);
    close_timer();
    return EXIT_FAIL;
  }
  if (!zz9k_shared_heap_board_visible(ctx)) {
    puts("zz9k-qprobe: the shared heap is not host-visible on this board "
         "(Zorro II)");
    zz9k_close(ctx);
    close_timer();
    return EXIT_FAIL;
  }

  if (all) {
    static const uint32_t choices[3] = { ZZ9K_QP_OWNER_CORE0,
                                         ZZ9K_QP_OWNER_CORE1,
                                         ZZ9K_QR_OWNER_MIX };

    for (i = 0U; i < 3U && !broken(); i++) {
      parts++;
      passed += run_stream(choices[i], stream_queues, seconds);
    }
  } else if (do_stream) {
    parts++;
    passed += run_stream(owner, stream_queues, seconds);
  }
  if (do_doorbell && !broken()) {
    parts++;
    passed += run_doorbell(counts, ncounts, iterations);
  }
  if (broken()) puts("zz9k-qprobe: interrupted");
  if (all) {
    /* A part skipped by Ctrl-C counts as failed. */
    int expected = 4;

    printf("QPROBE all %s parts=%d passed=%d failed=%d\n",
           passed == expected ? "PASS" : "FAIL", expected, passed,
           expected - passed);
  }

  zz9k_close(ctx);
  close_timer();
  return passed == (all ? 4 : parts) && parts != 0 ? EXIT_PASS : EXIT_FAIL;
}
