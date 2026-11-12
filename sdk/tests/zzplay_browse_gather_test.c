/*
 * Tests for ZZPlay collection gathering (Browse window Play/Add whole).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../tools/zzplay/zzplay-browse-gather.h"

#define FAKE_ROWS_MAX 200U
#define FAKE_FOLDERS_MAX 8U
#define LISTED_MAX 32U
#define OUT_MAX 512U

typedef struct FakeFolder {
  char id[32];
  ZZPlayProviderItem rows[FAKE_ROWS_MAX];
  char row_ids[FAKE_ROWS_MAX][16];
  uint32_t count;
} FakeFolder;

static FakeFolder g_folders[FAKE_FOLDERS_MAX];
static uint32_t g_folder_count;
static char g_listed[LISTED_MAX][32];
static uint32_t g_listed_count;
static uint32_t g_list_calls;
static const char *g_cancel_at; /* listing this node trips the cancel flag */
static int g_cancelled;
static uint32_t g_progress_last;
static uint32_t g_progress_calls;
static char g_out[OUT_MAX][16];
static uint32_t g_out_count;
static uint32_t g_out_stop_after; /* out sink stops after this many, 0 never */

static FakeFolder *fake_find(const char *id)
{
  uint32_t i;
  for (i = 0U; i < g_folder_count; i++) {
    if (strcmp(g_folders[i].id, id) == 0) {
      return &g_folders[i];
    }
  }
  return NULL;
}

static FakeFolder *fake_folder(const char *id)
{
  FakeFolder *f = &g_folders[g_folder_count++];
  memset(f, 0, sizeof(*f));
  snprintf(f->id, sizeof(f->id), "%s", id);
  return f;
}

static void fake_row(FakeFolder *f, const char *id, uint32_t kind,
                     uint32_t playable)
{
  ZZPlayProviderItem *row = &f->rows[f->count];

  snprintf(f->row_ids[f->count], sizeof(f->row_ids[0]), "%s", id);
  memset(row, 0, sizeof(*row));
  row->id = f->row_ids[f->count];
  row->title = f->row_ids[f->count];
  row->subtitle = "";
  row->kind = kind;
  row->playable = playable;
  f->count++;
}

/* art:1 holds a direct playable track, two playable album folders, an
 * unplayable letter folder and an unplayable track. alb:12 spans two
 * pages and holds a playable folder that must not be walked (depth 2). */
static void fake_build(void)
{
  FakeFolder *art = fake_folder("art:1");
  FakeFolder *alb11 = fake_folder("alb:11");
  FakeFolder *alb12 = fake_folder("alb:12");
  FakeFolder *ltr = fake_folder("ltr:X");
  FakeFolder *alb99 = fake_folder("alb:99");
  uint32_t i;

  fake_row(art, "t0", ZZPLAY_ITEM_TRACK, 1U);
  fake_row(art, "alb:11", ZZPLAY_ITEM_FOLDER, 1U);
  fake_row(art, "ltr:X", ZZPLAY_ITEM_FOLDER, 0U);
  fake_row(art, "t9x", ZZPLAY_ITEM_TRACK, 0U);
  fake_row(art, "alb:12", ZZPLAY_ITEM_FOLDER, 1U);

  fake_row(alb11, "t1", ZZPLAY_ITEM_TRACK, 1U);
  fake_row(alb11, "t2", ZZPLAY_ITEM_TRACK, 0U);
  fake_row(alb11, "t3", ZZPLAY_ITEM_TRACK, 1U);

  fake_row(alb12, "alb:99", ZZPLAY_ITEM_FOLDER, 1U);
  for (i = 0U; i < 150U; i++) {
    char id[16];
    snprintf(id, sizeof(id), "m%lu", (unsigned long)i);
    fake_row(alb12, id, ZZPLAY_ITEM_TRACK, 1U);
  }

  fake_row(ltr, "leaked", ZZPLAY_ITEM_TRACK, 1U);
  fake_row(alb99, "deep", ZZPLAY_ITEM_TRACK, 1U);
}

static void fake_reset(void)
{
  memset(g_folders, 0, sizeof(g_folders));
  g_folder_count = 0U;
  memset(g_listed, 0, sizeof(g_listed));
  g_listed_count = 0U;
  g_list_calls = 0U;
  g_cancel_at = NULL;
  g_cancelled = 0;
  g_progress_last = 0U;
  g_progress_calls = 0U;
  memset(g_out, 0, sizeof(g_out));
  g_out_count = 0U;
  g_out_stop_after = 0U;
  fake_build();
}

static int fake_list(void *user, const char *node, uint32_t start,
                     uint32_t count, const ZZPlayItemSink *sink,
                     char *err, uint32_t err_size)
{
  FakeFolder *f;
  uint32_t i;
  uint32_t end;

  (void)user;
  g_list_calls++;
  if (g_listed_count < LISTED_MAX) {
    snprintf(g_listed[g_listed_count], sizeof(g_listed[0]), "%s",
             node ? node : "");
    g_listed_count++;
  }
  if (g_cancel_at && strcmp(node, g_cancel_at) == 0) {
    g_cancelled = 1;
  }
  if (count != ZZPLAY_GATHER_PAGE_SIZE) {
    snprintf(err, err_size, "bad page size %lu", (unsigned long)count);
    return 0;
  }
  f = fake_find(node);
  if (!f) {
    snprintf(err, err_size, "unknown folder");
    return 0;
  }
  if (sink->total) {
    sink->total(sink->user, f->count);
  }
  end = start + count < f->count ? start + count : f->count;
  for (i = start; i < end; i++) {
    if (sink->item(sink->user, &f->rows[i]) != 0) {
      return 1;
    }
  }
  return 1;
}

static void fake_progress(void *user, uint32_t collected)
{
  (void)user;
  g_progress_last = collected;
  g_progress_calls++;
}

static int out_item(void *user, const ZZPlayProviderItem *item)
{
  (void)user;
  if (g_out_count >= OUT_MAX) {
    return 1;
  }
  if (g_out_stop_after != 0U && g_out_count >= g_out_stop_after) {
    return 1;
  }
  snprintf(g_out[g_out_count], sizeof(g_out[0]), "%s",
           item->id ? item->id : "");
  g_out_count++;
  return 0;
}

static char g_err[128];

static int run_gather(const char *node, uint32_t limit, int *truncated)
{
  ZZPlayGather g;
  ZZPlayItemSink out;

  memset(&g, 0, sizeof(g));
  g.list = fake_list;
  g.progress = fake_progress;
  g.cancelled = &g_cancelled;
  memset(&out, 0, sizeof(out));
  out.item = out_item;
  g_err[0] = '\0';
  return zzplay_gather_collection(&g, node, limit, &out, truncated,
                                  g_err, sizeof(g_err));
}

static int listed_index(const char *id)
{
  uint32_t i;
  for (i = 0U; i < g_listed_count; i++) {
    if (strcmp(g_listed[i], id) == 0) {
      return (int)i;
    }
  }
  return -1;
}

static uint32_t listed_count(const char *id)
{
  uint32_t i;
  uint32_t n = 0U;
  for (i = 0U; i < g_listed_count; i++) {
    if (strcmp(g_listed[i], id) == 0) {
      n++;
    }
  }
  return n;
}

/* The collection's direct tracks come first, then each playable folder's
 * tracks in folder order; unplayable rows are left out, folders deeper
 * than two levels are not walked and unplayable folders not listed. */
static int test_full_walk(void)
{
  int truncated = -1;

  fake_reset();
  assert(run_gather("art:1", 4096U, &truncated) == 1);
  assert(truncated == 0);
  assert(g_err[0] == '\0');
  assert(g_out_count == 153U);
  assert(strcmp(g_out[0], "t0") == 0);
  assert(strcmp(g_out[1], "t1") == 0);
  assert(strcmp(g_out[2], "t3") == 0);
  assert(strcmp(g_out[3], "m0") == 0);
  assert(strcmp(g_out[152], "m149") == 0);

  assert(listed_index("art:1") == 0);
  assert(listed_index("alb:11") == 1);
  assert(listed_index("alb:12") == 2);
  assert(listed_index("ltr:X") == -1);
  assert(listed_index("alb:99") == -1);
  /* alb:12 spans two pages of 100 rows */
  assert(listed_count("alb:12") == 2U);
  assert(g_progress_last == 153U);
  assert(g_progress_calls >= 4U);
  return 0;
}

/* The limit keeps the first items in walk order; truncated reports that
 * more existed. An exact fit is not truncated. */
static int test_limit(void)
{
  int truncated = -1;

  fake_reset();
  assert(run_gather("art:1", 4U, &truncated) == 1);
  assert(truncated == 1);
  assert(g_out_count == 4U);
  assert(strcmp(g_out[0], "t0") == 0);
  assert(strcmp(g_out[1], "t1") == 0);
  assert(strcmp(g_out[2], "t3") == 0);
  assert(strcmp(g_out[3], "m0") == 0);

  fake_reset();
  assert(run_gather("art:1", 153U, &truncated) == 1);
  assert(truncated == 0);
  assert(g_out_count == 153U);

  fake_reset();
  assert(run_gather("art:1", 152U, &truncated) == 1);
  assert(truncated == 1);
  assert(g_out_count == 152U);
  assert(strcmp(g_out[151], "m148") == 0);
  return 0;
}

/* Stop aborts the walk: nothing after the cancelled listing runs. */
static int test_cancel(void)
{
  int truncated = -1;

  fake_reset();
  g_cancel_at = "alb:11";
  assert(run_gather("art:1", 4096U, &truncated) == 0);
  assert(strcmp(g_err, "cancelled") == 0);
  assert(listed_index("alb:11") == 1);
  assert(listed_index("alb:12") == -1);
  assert(listed_count("alb:11") == 1U);
  assert(g_out_count <= 1U);
  return 0;
}

/* A listing failure ends the walk with the module's reason. */
static int test_list_failure(void)
{
  int truncated = -1;

  fake_reset();
  assert(run_gather("nope", 4096U, &truncated) == 0);
  assert(strcmp(g_err, "unknown folder") == 0);
  assert(g_out_count == 0U);
  return 0;
}

/* When the caller's sink stops early, the walk ends without truncation. */
static int test_out_stops(void)
{
  int truncated = -1;

  fake_reset();
  g_out_stop_after = 2U;
  assert(run_gather("art:1", 4096U, &truncated) == 1);
  assert(truncated == 0);
  assert(g_out_count == 2U);
  assert(strcmp(g_out[0], "t0") == 0);
  assert(strcmp(g_out[1], "t1") == 0);
  return 0;
}

int main(void)
{
  assert(test_full_walk() == 0);
  assert(test_limit() == 0);
  assert(test_cancel() == 0);
  assert(test_list_failure() == 0);
  assert(test_out_stops() == 0);
  printf("zzplay_browse_gather_test: all tests passed\n");
  return 0;
}
