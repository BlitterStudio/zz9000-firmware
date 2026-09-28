/*
 * Host tests for the armed (block-on-IRQ) wait loop in zz9k_call.
 *
 * The Amiga Wait/interrupt-server code is compiled out on the host; these
 * tests drive the loop's DECISION logic through the now/block seams, using the
 * same in-memory TestMailbox + completion-injection idiom as host_mailbox_test.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200112L   /* setenv for the ENV-cache test */
#endif

#include "zz9k/host.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TEST_SYNC_COOKIE_MASK 0x5aa55aa5UL
#define REQ_ID 0x1234UL

struct TestMailbox {
  ZZ9KMailboxDescriptor descriptor;
  ZZ9KMailboxWireEntry request_ring[4];
  ZZ9KMailboxWireEntry completion_ring[4];
};

/* Test-only seams defined in zz9k_host.c under !ZZ9K_HOST_AMIGA. */
void zz9k_set_idle_hook_for_test(void (*hook)(void));
void zz9k_set_now_hook_for_test(uint32_t (*hook)(void));
void zz9k_set_block_hook_for_test(int (*hook)(void));
void zz9k_set_armed_for_test(ZZ9KContext *ctx, int armed);

static struct TestMailbox *g_mbox;
static uint32_t g_now_ms;
static int g_block_calls;
static int g_block_called_flag;
static int failures;

static void check(int cond, const char *msg)
{
  if (cond) {
    printf("ok   %s\n", msg);
  } else {
    printf("FAIL %s\n", msg);
    failures++;
  }
}

static void init_mailbox(struct TestMailbox *mailbox)
{
  memset(mailbox, 0, sizeof(*mailbox));
  zz9k_put_be32(mailbox->descriptor.magic, ZZ9K_ABI_MAGIC);
  zz9k_put_be16(mailbox->descriptor.abi_major, ZZ9K_ABI_VERSION_MAJOR);
  zz9k_put_be16(mailbox->descriptor.abi_minor, ZZ9K_ABI_VERSION_MINOR);
  zz9k_put_be32(mailbox->descriptor.descriptor_size,
                (uint32_t)sizeof(mailbox->descriptor));
  zz9k_put_be32(mailbox->descriptor.request_ring_offset,
                (uint32_t)offsetof(struct TestMailbox, request_ring));
  zz9k_put_be32(mailbox->descriptor.request_ring_entries, 4);
  zz9k_put_be32(mailbox->descriptor.completion_ring_offset,
                (uint32_t)offsetof(struct TestMailbox, completion_ring));
  zz9k_put_be32(mailbox->descriptor.completion_ring_entries, 4);
  zz9k_put_be32(mailbox->descriptor.capability_bits,
                ZZ9K_CAP_MAILBOX | ZZ9K_CAP_IRQ_COMPLETION);
}

/* Publish a matching PING completion at index 0 (head stays 0, tail -> 1). */
static void inject_reply(struct TestMailbox *m)
{
  zz9k_put_be32(m->completion_ring[0].request_id, REQ_ID);
  zz9k_put_be16(m->completion_ring[0].opcode, ZZ9K_OP_PING);
  zz9k_put_be16(m->completion_ring[0].status, ZZ9K_STATUS_OK);
  zz9k_put_be16(m->completion_ring[0].payload_len, 0);
  zz9k_put_be32(m->completion_ring[0].user_cookie,
                (uint32_t)(REQ_ID ^ TEST_SYNC_COOKIE_MASK));
  zz9k_put_be32(m->descriptor.completion_tail, 1);
}

static uint32_t now_hook(void) { return g_now_ms; }

static int block_never_i(void) { g_block_called_flag = 1; return 0; }
static void idle_inject(void) { inject_reply(g_mbox); }

static int block_inject_on_3rd(void)
{
  g_block_calls++;
  g_now_ms += 4;
  if (g_block_calls == 3) {
    inject_reply(g_mbox);
  }
  return 0;
}

static int block_timeout(void)
{
  g_block_calls++;
  g_now_ms = 6000;   /* jump past the 5000 ms hard bound */
  return 0;
}

static int block_ctrlc(void)
{
  g_block_calls++;
  return 1;          /* nonzero == Ctrl-C */
}

static int block_advance_20(void)
{
  g_block_calls++;
  g_now_ms += 20;
  return 0;
}

static int block_advance_50(void)
{
  g_block_calls++;
  g_now_ms += 50;
  return 0;
}

static int block_inject_on_1st(void)
{
  g_block_calls++;
  g_now_ms += 4;
  inject_reply(g_mbox);
  return 0;
}

static void set_env(const char *name, const char *value)
{
#if defined(_WIN32)
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}

static void reset_hooks(void)
{
  zz9k_set_idle_hook_for_test(0);
  zz9k_set_now_hook_for_test(0);
  zz9k_set_block_hook_for_test(0);
  g_now_ms = 0;
  g_block_calls = 0;
  g_block_called_flag = 0;
}

static ZZ9KContext *open_ctx(struct TestMailbox *mailbox, uint16_t *doorbell)
{
  ZZ9KContext *ctx = 0;
  ZZ9KBoard board;

  init_mailbox(mailbox);
  memset(&board, 0, sizeof(board));
  g_mbox = mailbox;
  if (zz9k_attach_mailbox(&ctx, &board, &mailbox->descriptor, doorbell, 0) !=
      ZZ9K_STATUS_OK) {
    return 0;
  }
  return ctx;
}

static void build_ping(ZZ9KRequest *request)
{
  memset(request, 0, sizeof(*request));
  request->entry.opcode = ZZ9K_OP_PING;
  request->entry.flags = ZZ9K_ENTRY_INLINE_PAYLOAD;
  request->entry.payload_len = 0;
  request->entry.request_id = REQ_ID;   /* fixed id so injection can match */
}

static int test_reply_present_before_first_block(void)
{
  struct TestMailbox mailbox;
  uint16_t doorbell = 0;
  ZZ9KContext *ctx;
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  reset_hooks();
  ctx = open_ctx(&mailbox, &doorbell);
  if (!ctx) return 1;
  zz9k_set_armed_for_test(ctx, 1);
  zz9k_set_block_hook_for_test(block_never_i);
  build_ping(&request);
  inject_reply(&mailbox);                 /* already present at first poll */
  memset(&reply, 0, sizeof(reply));

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  check(status == ZZ9K_STATUS_OK, "immediate reply returns OK");
  check(g_block_called_flag == 0, "immediate reply never blocks");
  zz9k_close(ctx);
  return 0;
}

static int test_reply_on_third_block(void)
{
  struct TestMailbox mailbox;
  uint16_t doorbell = 0;
  ZZ9KContext *ctx;
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  reset_hooks();
  ctx = open_ctx(&mailbox, &doorbell);
  if (!ctx) return 1;
  zz9k_set_armed_for_test(ctx, 1);
  zz9k_set_now_hook_for_test(now_hook);
  zz9k_set_block_hook_for_test(block_inject_on_3rd);
  build_ping(&request);
  memset(&reply, 0, sizeof(reply));

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  check(status == ZZ9K_STATUS_OK, "reply after 3 blocks returns OK");
  check(g_block_calls == 3, "loops exactly 3 times (heartbeat recovery)");
  zz9k_close(ctx);
  return 0;
}

static int test_hard_timeout(void)
{
  struct TestMailbox mailbox;
  uint16_t doorbell = 0;
  ZZ9KContext *ctx;
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  reset_hooks();
  ctx = open_ctx(&mailbox, &doorbell);
  if (!ctx) return 1;
  zz9k_set_armed_for_test(ctx, 1);
  zz9k_set_now_hook_for_test(now_hook);
  zz9k_set_block_hook_for_test(block_timeout);   /* never injects */
  build_ping(&request);
  memset(&reply, 0, sizeof(reply));

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  check(status == ZZ9K_STATUS_TIMEOUT, "no reply past hard bound -> TIMEOUT");
  zz9k_close(ctx);
  return 0;
}

static int test_ctrl_c(void)
{
  struct TestMailbox mailbox;
  uint16_t doorbell = 0;
  ZZ9KContext *ctx;
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  reset_hooks();
  ctx = open_ctx(&mailbox, &doorbell);
  if (!ctx) return 1;
  zz9k_set_armed_for_test(ctx, 1);
  zz9k_set_now_hook_for_test(now_hook);
  zz9k_set_block_hook_for_test(block_ctrlc);
  build_ping(&request);
  memset(&reply, 0, sizeof(reply));

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  check(status == ZZ9K_STATUS_CANCELLED, "Ctrl-C wake -> CANCELLED");
  zz9k_close(ctx);
  return 0;
}

static int test_unarmed_uses_spin_path(void)
{
  struct TestMailbox mailbox;
  uint16_t doorbell = 0;
  ZZ9KContext *ctx;
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  reset_hooks();
  ctx = open_ctx(&mailbox, &doorbell);
  if (!ctx) return 1;
  /* do NOT arm */
  zz9k_set_idle_hook_for_test(idle_inject);      /* existing spin-path seam */
  zz9k_set_block_hook_for_test(block_never_i);   /* must NOT be used */
  build_ping(&request);
  memset(&reply, 0, sizeof(reply));

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  check(status == ZZ9K_STATUS_OK, "unarmed path still returns OK");
  check(g_block_called_flag == 0, "unarmed path never touches block seam");
  zz9k_close(ctx);
  return 0;
}

static int test_hard_transport_error(void)
{
  struct TestMailbox mailbox;
  uint16_t doorbell = 0;
  ZZ9KContext *ctx;
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  reset_hooks();
  ctx = open_ctx(&mailbox, &doorbell);
  if (!ctx) return 1;
  zz9k_set_armed_for_test(ctx, 1);
  zz9k_set_block_hook_for_test(block_never_i);   /* must NOT block on a hard error */
  build_ping(&request);
  /* Corrupt the completion head so consume returns a hard (non-BUSY) error. */
  zz9k_put_be32(mailbox.descriptor.completion_head, 99);
  memset(&reply, 0, sizeof(reply));

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  check(status == ZZ9K_STATUS_BAD_REQUEST, "hard consume error returns immediately");
  check(g_block_called_flag == 0, "hard error never blocks");
  zz9k_close(ctx);
  return 0;
}

static int test_host_arm_is_noop(void)
{
  struct TestMailbox mailbox;
  uint16_t doorbell = 0;
  ZZ9KContext *ctx;
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  reset_hooks();
  ctx = open_ctx(&mailbox, &doorbell);
  if (!ctx) return 1;

  status = zz9k_arm_completion_irq(ctx);          /* no Amiga -> UNSUPPORTED */
  check(status == ZZ9K_STATUS_UNSUPPORTED, "host arm returns UNSUPPORTED");

  zz9k_set_idle_hook_for_test(idle_inject);       /* spin path completes it */
  zz9k_set_block_hook_for_test(block_never_i);    /* must stay unused */
  build_ping(&request);
  memset(&reply, 0, sizeof(reply));

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  check(status == ZZ9K_STATUS_OK, "host stays on spin path after failed arm");
  check(g_block_called_flag == 0, "failed arm never engages block seam");

  zz9k_disarm_completion_irq(ctx);                /* idempotent no-op */
  zz9k_close(ctx);
  return 0;
}

static int test_armed_offload_timeout_precedence(void)
{
  struct TestMailbox mailbox;
  uint16_t doorbell = 0;
  ZZ9KContext *ctx;
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  reset_hooks();
  ctx = open_ctx(&mailbox, &doorbell);
  if (!ctx) return 1;
  zz9k_set_armed_for_test(ctx, 1);
  zz9k_set_now_hook_for_test(now_hook);
  zz9k_set_block_hook_for_test(block_advance_20);
  zz9k_set_offload_timeout_ms(ctx, 30U);
  build_ping(&request);
  memset(&reply, 0, sizeof(reply));

  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  check(status == ZZ9K_STATUS_TIMEOUT, "armed wait honors offload budget");
  check(g_block_calls == 2, "offload budget bounds the wait (2 blocks)");
  zz9k_close(ctx);
  return 0;
}

static int test_armed_sync_timeout_cached_once(void)
{
  struct TestMailbox mailbox;
  uint16_t doorbell = 0;
  ZZ9KContext *ctx;
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  reset_hooks();
  set_env("ZZ9K_SYNC_WAIT_TIMEOUT_MS", "100");
  ctx = open_ctx(&mailbox, &doorbell);
  if (!ctx) return 1;
  zz9k_set_armed_for_test(ctx, 1);
  zz9k_set_now_hook_for_test(now_hook);

  /* First armed call reads (and caches) the 100 ms bound, then completes. */
  zz9k_set_block_hook_for_test(block_inject_on_1st);
  build_ping(&request);
  memset(&reply, 0, sizeof(reply));
  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  check(status == ZZ9K_STATUS_OK, "first armed call completes");

  /* ENV now says 10 ms. A per-call re-read would time out after ONE 50 ms
   * block; the cached 100 ms bound needs TWO. */
  set_env("ZZ9K_SYNC_WAIT_TIMEOUT_MS", "10");
  g_block_calls = 0;
  zz9k_set_block_hook_for_test(block_advance_50);
  build_ping(&request);
  memset(&reply, 0, sizeof(reply));
  status = zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  check(status == ZZ9K_STATUS_TIMEOUT, "second armed call times out");
  check(g_block_calls == 2, "ENV bound read once per context, not per call");

  set_env("ZZ9K_SYNC_WAIT_TIMEOUT_MS", "");
  zz9k_close(ctx);
  return 0;
}

int main(void)
{
  test_reply_present_before_first_block();
  test_reply_on_third_block();
  test_hard_timeout();
  test_ctrl_c();
  test_unarmed_uses_spin_path();
  test_hard_transport_error();
  test_host_arm_is_noop();
  test_armed_offload_timeout_precedence();
  test_armed_sync_timeout_cached_once();

  if (failures) {
    printf("call_irq_wait_test: %d failure(s)\n", failures);
    return 1;
  }
  printf("call_irq_wait_test: all passed\n");
  return 0;
}
