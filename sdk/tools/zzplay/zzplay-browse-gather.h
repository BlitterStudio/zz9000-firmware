/* Collection gathering for ZZPlay's Browse window: walk a folder a module
 * marks playable and collect its playable tracks and stations.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_BROWSE_GATHER_H
#define ZZPLAY_BROWSE_GATHER_H

#include <stdint.h>

#include "zzplay-provider.h"

/* Rows requested per page while gathering. */
#define ZZPLAY_GATHER_PAGE_SIZE 100U

typedef struct ZZPlayGather {
  void *user;
  /* List one page of a folder, like ZZPlayProvider.list. */
  int (*list)(void *user, const char *node, uint32_t start, uint32_t count,
              const ZZPlayItemSink *sink, char *err, uint32_t err_size);
  /* Optional: the playable items collected so far, reported after every
   * listed page. */
  void (*progress)(void *user, uint32_t collected);
  /* Stop: set from outside the walk to abort it. */
  volatile int *cancelled;
} ZZPlayGather;

/* Collect the playable tracks and stations of the collection `node`:
 * every page of it, then every page of each playable folder directly
 * inside it (so the walk is at most two folders deep), in listing order,
 * at most `limit` in total, through out->item(). *truncated reports that
 * more playable items existed than `limit` allowed. Returns 1 when the
 * walk ran to its end, 0 when a listing failed (reason in err) or the
 * walk was cancelled. */
int zzplay_gather_collection(const ZZPlayGather *g, const char *node,
                             uint32_t limit, const ZZPlayItemSink *out,
                             int *truncated, char *err, uint32_t err_size);

#endif /* ZZPLAY_BROWSE_GATHER_H */
