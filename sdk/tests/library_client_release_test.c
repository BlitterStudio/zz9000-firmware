/*
 * Client ownership release behavior for zz9k.library.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/library.h"
#include "zz9k/request.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define TEST_RING_ENTRIES 4U

struct TestMailbox {
  ZZ9KMailboxDescriptor descriptor;
  ZZ9KMailboxWireEntry request_ring[TEST_RING_ENTRIES];
  ZZ9KMailboxWireEntry completion_ring[TEST_RING_ENTRIES];
};

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
  zz9k_put_be32(mailbox->descriptor.request_ring_entries, TEST_RING_ENTRIES);
  zz9k_put_be32(mailbox->descriptor.completion_ring_offset,
                (uint32_t)offsetof(struct TestMailbox, completion_ring));
  zz9k_put_be32(mailbox->descriptor.completion_ring_entries,
                TEST_RING_ENTRIES);
  zz9k_put_be32(mailbox->descriptor.capability_bits,
                ZZ9K_CAP_MAILBOX | ZZ9K_CAP_IRQ_COMPLETION);
}

static int attach_library(ZZ9KLibrary *library, struct TestMailbox *mailbox)
{
  ZZ9KBoard board;
  ZZ9KContext *ctx;
  int status;

  memset(&board, 0, sizeof(board));
  ctx = 0;
  status = zz9k_attach_mailbox(&ctx, &board, &mailbox->descriptor, 0, 0);
  if (status != ZZ9K_STATUS_OK) return status;
  ZZ9KInit(library);
  return ZZ9KAttachContext(library, ctx, 1);
}

void zz9k_set_busy_poll_clock_for_test(uint32_t (*clock_fn)(void));

static int test_release_is_sent_once_after_last_reference(void)
{
  struct TestMailbox mailbox;
  ZZ9KLibrary library;
  ZZ9KClientLease lease;
  uint32_t tail;
  int status;

  init_mailbox(&mailbox);
  if (attach_library(&library, &mailbox) != ZZ9K_STATUS_OK) return 1;
  ZZ9KClientLeaseInit(&lease, 37U);
  if (ZZ9KClientLeaseRetain(&lease) != ZZ9K_STATUS_OK) return 2;

  status = ZZ9KClientLeaseClose(&library, &lease, 0U);
  if (status != ZZ9K_STATUS_OK || lease.references != 1U) return 3;
  tail = zz9k_get_be32(mailbox.descriptor.request_tail);
  if (tail != 0U) return 4;

  status = ZZ9KClientLeaseClose(&library, &lease, 0U);
  if (status != ZZ9K_STATUS_TIMEOUT || lease.references != 0U) return 5;
  tail = zz9k_get_be32(mailbox.descriptor.request_tail);
  if (tail != 1U ||
      zz9k_get_be16(mailbox.request_ring[0].opcode) != ZZ9K_OP_CLIENT_RELEASE ||
      (zz9k_get_be16(mailbox.request_ring[0].flags) &
       ZZ9K_ENTRY_CLIENT_ID_MASK) !=
          (37U << ZZ9K_ENTRY_CLIENT_ID_SHIFT)) return 6;

  status = ZZ9KClientLeaseClose(&library, &lease, 0U);
  if (status != ZZ9K_STATUS_BAD_REQUEST ||
      zz9k_get_be32(mailbox.descriptor.request_tail) != 1U) return 7;

  ZZ9KClose(&library);
  return 0;
}

/* The firmware answers CLIENT_RELEASE with BUSY while the client still has
 * worker activity. Production resubmits -- it does not assume one shot.
 * The busy-poll clock hook stands in for the firmware: each newly
 * submitted release gets one reply, BUSY twice, then OK. */
struct BusyReleaseState {
  struct TestMailbox *mailbox;
  uint32_t busy_left;
  uint32_t posted_tail;
  uint32_t deliveries;
  uint32_t clock;
};

static struct BusyReleaseState *busy_release_state;

/* Each newly submitted release gets one completion in the next ring slot,
 * as the firmware would post it. The clock advances on every poll so a
 * library that stops consuming completions times out instead of hanging. */
static uint32_t deliver_release_completion(void)
{
  struct BusyReleaseState *state = busy_release_state;
  ZZ9KMailboxWireEntry *completion;
  uint32_t tail;
  uint32_t request_id;
  uint16_t status;

  tail = zz9k_get_be32(state->mailbox->descriptor.request_tail);
  if (tail != state->posted_tail && tail != 0U) {
    request_id = zz9k_get_be32(
        state->mailbox->request_ring[(tail - 1U) % TEST_RING_ENTRIES]
            .request_id);
    status = state->busy_left != 0U ? ZZ9K_STATUS_BUSY : ZZ9K_STATUS_OK;
    if (state->busy_left != 0U) state->busy_left--;
    state->posted_tail = tail;
    completion = &state->mailbox->completion_ring[
        state->deliveries % TEST_RING_ENTRIES];
    zz9k_put_be32(completion->request_id, request_id);
    zz9k_put_be16(completion->opcode, ZZ9K_OP_CLIENT_RELEASE);
    zz9k_put_be16(completion->status, status);
    zz9k_put_be16(completion->payload_len, 0);
    zz9k_put_be32(completion->user_cookie, request_id ^ 0x5aa55aa5UL);
    state->deliveries++;
    zz9k_put_be32(state->mailbox->descriptor.completion_tail,
                  state->deliveries % TEST_RING_ENTRIES);
  }
  return ++state->clock;
}

static int test_release_resubmits_on_busy(void)
{
  struct TestMailbox mailbox;
  struct BusyReleaseState state;
  ZZ9KLibrary library;
  ZZ9KClientLease lease;
  uint32_t tail;
  uint32_t i;
  int status;

  init_mailbox(&mailbox);
  if (attach_library(&library, &mailbox) != ZZ9K_STATUS_OK) return 1;
  ZZ9KClientLeaseInit(&lease, 41U);

  state.mailbox = &mailbox;
  state.busy_left = 2U;
  state.posted_tail = 0U;
  state.deliveries = 0U;
  state.clock = 0U;
  busy_release_state = &state;
  zz9k_set_busy_poll_clock_for_test(deliver_release_completion);

  status = ZZ9KClientLeaseClose(&library, &lease, 50U);
  if (status != ZZ9K_STATUS_OK) return 2;
  /* Two BUSY replies then OK: three submissions, all client-tagged. */
  if (state.deliveries != 3U) return 3;
  tail = zz9k_get_be32(mailbox.descriptor.request_tail);
  if (tail != 3U) return 4;
  for (i = 0U; i < tail; i++) {
    if (zz9k_get_be16(mailbox.request_ring[i].opcode) !=
            ZZ9K_OP_CLIENT_RELEASE ||
        (zz9k_get_be16(mailbox.request_ring[i].flags) &
         ZZ9K_ENTRY_CLIENT_ID_MASK) != (41U << ZZ9K_ENTRY_CLIENT_ID_SHIFT))
      return 5;
  }

  zz9k_set_busy_poll_clock_for_test(0);
  busy_release_state = 0;
  ZZ9KClose(&library);
  return 0;
}

/* Client tagging happens at the locked submit point: clearing the context
 * id right after a submission (the resident's enter/leave pairing) must
 * not untag the queued request, and an untagged context submits unowned. */
static int test_client_tag_is_set_at_submit(void)
{
  struct TestMailbox mailbox;
  ZZ9KLibrary library;
  ZZ9KRequest request;
  ZZ9KAsyncRequest asyncs[2];
  uint16_t flags0;
  uint16_t flags1;

  init_mailbox(&mailbox);
  if (attach_library(&library, &mailbox) != ZZ9K_STATUS_OK) return 1;

  memset(&request, 0, sizeof(request));
  request.entry.opcode = ZZ9K_OP_PING;
  request.entry.flags = ZZ9K_ENTRY_INLINE_PAYLOAD;
  memset(asyncs, 0, sizeof(asyncs));

  if (ZZ9KSetClientId(&library, 43U) != ZZ9K_STATUS_OK) return 2;
  if (ZZ9KCallAsync(&library, &asyncs[0], &request, 0, 0) !=
      ZZ9K_STATUS_QUEUED)
    return 3;
  ZZ9KSetClientId(&library, 0U);

  if (ZZ9KCallAsync(&library, &asyncs[1], &request, 0, 0) !=
      ZZ9K_STATUS_QUEUED)
    return 4;

  flags0 = zz9k_get_be16(mailbox.request_ring[0].flags);
  flags1 = zz9k_get_be16(mailbox.request_ring[1].flags);
  if ((flags0 & ZZ9K_ENTRY_CLIENT_ID_MASK) !=
      (43U << ZZ9K_ENTRY_CLIENT_ID_SHIFT))
    return 5;
  if ((flags1 & ZZ9K_ENTRY_CLIENT_ID_MASK) != 0U) return 6;

  (void)ZZ9KCancelAsync(&library, &asyncs[0]);
  (void)ZZ9KCancelAsync(&library, &asyncs[1]);
  ZZ9KClose(&library);
  return 0;
}

int main(void)
{
  int status = test_release_is_sent_once_after_last_reference();

  if (status == 0) {
    status = test_release_resubmits_on_busy();
    if (status != 0) status += 10;
  }
  if (status == 0) {
    status = test_client_tag_is_set_at_submit();
    if (status != 0) status += 100;
  }
  if (status != 0) {
    printf("library client release test failed: %d\n", status);
  }
  return status;
}
