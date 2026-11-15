/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <stdio.h>
#include <string.h>

#include "audio_stream_close.h"
#include "scheduler.h"
#include "sdk_core1_reclaim.h"

static int failures;
static int decoder_live;
static int pool_reclaimed;
static int release_after_reclaim;

#define CHECK(condition, message) do { \
  if (!(condition)) { \
    printf("FAIL: %s\n", message); \
    failures++; \
  } \
} while (0)

/* The seam's mailbox collaborator represents a core-1 FLAC stream whose
 * deferred CLOSE task disappeared when the CPU halted. */
void sdk_mailbox_poison_core1_audio_streams(void)
{
  decoder_live = 0;
}

unsigned sdk_compression_reclaim_core1_decode(void)
{
  if (decoder_live) {
    release_after_reclaim++;
  }
  pool_reclaimed = 1;
  return 1U;
}

static void retry_deferred_close(void)
{
  enum audio_stream_close_action action = audio_stream_close_step(
      1, 0, 0, 1, 1, decoder_live);

  if (action == AUDIO_STREAM_CLOSE_QUEUE) {
    /* This would call sdk_flac_release() through the internal CLOSE task. */
    if (pool_reclaimed) {
      release_after_reclaim++;
    }
    return;
  }
  CHECK(action == AUDIO_STREAM_CLOSE_FREE,
        "poisoned deferred close retires its stream slot");
}

int main(void)
{
  taskq_t queue;

  memset(&queue, 0, sizeof(queue));
  taskq_init(&queue);
  queue.descs[0].state = TASK_CLAIMED; /* deferred CLOSE in flight */
  decoder_live = 1;
  pool_reclaimed = 0;
  release_after_reclaim = 0;

  sdk_core1_reclaim_after_halt(&queue);
  CHECK(pool_reclaimed, "core-1 decode pool is reclaimed");
  CHECK(queue.descs[0].state == TASK_FAILED,
        "halted deferred CLOSE becomes harvestable failure");
  retry_deferred_close();
  CHECK(release_after_reclaim == 0,
        "retry never releases a decoder after its pool was reclaimed");

  if (failures) {
    printf("core1_reclaim_test: %d failure(s)\n", failures);
    return 1;
  }
  puts("core1_reclaim_test: all passed");
  return 0;
}
