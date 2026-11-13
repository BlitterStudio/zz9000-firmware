/*
 * Tests for ZZPlay service browser model.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../tools/zzplay/zzplay-browse-model.h"
#include "../tools/zzplay/zzplay-browse-gather.h"
#include "../tools/zzplay/zzplay-sys.h"
#include <stdlib.h>

static int test_enter_folder_and_back(void)
{
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseReply reply;
  ZZPlayBrowseItem root_items[2];
  ZZPlayBrowseItem folder_items[1];
  ZZPlayBrowseAction action;

  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "radio", "Internet Radio", NULL));
  assert(zzplay_browse_model_select_service(&model, 0, &req));
  assert(req.kind == ZZPLAY_BROWSE_REQ_LIST);
  assert(req.generation == 1U);

  /* Page 1 reply with 2 folder items */
  memset(root_items, 0, sizeof(root_items));
  snprintf(root_items[0].id, sizeof(root_items[0].id), "countries");
  snprintf(root_items[0].title, sizeof(root_items[0].title), "By Country");
  root_items[0].kind = ZZPLAY_ITEM_FOLDER;

  snprintf(root_items[1].id, sizeof(root_items[1].id), "favs");
  snprintf(root_items[1].title, sizeof(root_items[1].title), "Favourites");
  root_items[1].kind = ZZPLAY_ITEM_FOLDER;

  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = root_items;
  reply.item_count = 2U;
  reply.start = 0U;

  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);
  assert(model.item_count == 2U);

  /* Set selection to 1 (Favourites) */
  zzplay_browse_model_set_selection(&model, 1);
  assert(model.selection == 1);

  /* Activate folder (Favourites) */
  action = zzplay_browse_model_activate(&model, 1, &req);
  assert(action == ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  assert(req.kind == ZZPLAY_BROWSE_REQ_LIST);
  assert(req.generation == 2U);
  assert(strcmp(req.node_id, "favs") == 0);
  assert(model.stack_depth == 1U);
  assert(model.item_count == 0U);
  assert(model.selection == -1);

  /* Reply for folder */
  memset(folder_items, 0, sizeof(folder_items));
  snprintf(folder_items[0].id, sizeof(folder_items[0].id), "station_1");
  snprintf(folder_items[0].title, sizeof(folder_items[0].title), "Radio One");
  folder_items[0].kind = ZZPLAY_ITEM_STATION;
  folder_items[0].playable = 1U;

  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = folder_items;
  reply.item_count = 1U;
  reply.start = 0U;

  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);
  assert(model.item_count == 1U);
  assert(strcmp(model.items[0].title, "Radio One") == 0);

  /* Back restores previous list and selection */
  assert(zzplay_browse_model_can_back(&model));
  assert(zzplay_browse_model_back(&model, &req));
  assert(model.stack_depth == 0U);
  assert(model.item_count == 2U);
  assert(strcmp(model.items[0].id, "countries") == 0);
  assert(strcmp(model.items[1].id, "favs") == 0);
  assert(model.selection == 1);

  zzplay_browse_model_free(&model);
  return 0;
}

static int test_page_two_appends(void)
{
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseReply reply;
  ZZPlayBrowseItem page1[2];
  ZZPlayBrowseItem page2[2];

  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "plex", "Plex Server", NULL));
  assert(zzplay_browse_model_select_service(&model, 0, &req));

  /* Page 1: 2 items, total reported 4 */
  memset(page1, 0, sizeof(page1));
  snprintf(page1[0].id, sizeof(page1[0].id), "track_1");
  snprintf(page1[0].title, sizeof(page1[0].title), "Track 1");
  page1[0].kind = ZZPLAY_ITEM_TRACK;
  page1[0].playable = 1U;

  snprintf(page1[1].id, sizeof(page1[1].id), "track_2");
  snprintf(page1[1].title, sizeof(page1[1].title), "Track 2");
  page1[1].kind = ZZPLAY_ITEM_TRACK;
  page1[1].playable = 1U;

  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = page1;
  reply.item_count = 2U;
  reply.start = 0U;
  reply.has_total = 1;
  reply.total = 4U;

  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);
  assert(model.item_count == 2U);
  assert(model.has_more == 1);

  /* Request page 2 */
  assert(zzplay_browse_model_request_more(&model, &req));
  assert(req.start == 2U);
  assert(req.count == ZZPLAY_BROWSE_PAGE_SIZE);

  /* Page 2: 2 items */
  memset(page2, 0, sizeof(page2));
  snprintf(page2[0].id, sizeof(page2[0].id), "track_3");
  snprintf(page2[0].title, sizeof(page2[0].title), "Track 3");
  page2[0].kind = ZZPLAY_ITEM_TRACK;
  page2[0].playable = 1U;

  snprintf(page2[1].id, sizeof(page2[1].id), "track_4");
  snprintf(page2[1].title, sizeof(page2[1].title), "Track 4");
  page2[1].kind = ZZPLAY_ITEM_TRACK;
  page2[1].playable = 1U;

  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = page2;
  reply.item_count = 2U;
  reply.start = 2U;
  reply.has_total = 1;
  reply.total = 4U;

  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);
  assert(model.item_count == 4U);
  assert(strcmp(model.items[0].id, "track_1") == 0);
  assert(strcmp(model.items[1].id, "track_2") == 0);
  assert(strcmp(model.items[2].id, "track_3") == 0);
  assert(strcmp(model.items[3].id, "track_4") == 0);
  assert(model.has_more == 0);

  zzplay_browse_model_free(&model);
  return 0;
}

/* A folder longer than one page, opened again from the cache after Back,
 * still offers More. */
static int test_cached_folder_keeps_more(void)
{
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseReply reply;
  ZZPlayBrowseItem root_items[1];
  ZZPlayBrowseItem page1[2];

  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "plex", "Plex Server", NULL));
  assert(zzplay_browse_model_select_service(&model, 0, &req));

  memset(root_items, 0, sizeof(root_items));
  snprintf(root_items[0].id, sizeof(root_items[0].id), "big");
  snprintf(root_items[0].title, sizeof(root_items[0].title), "Big folder");
  root_items[0].kind = ZZPLAY_ITEM_FOLDER;
  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = root_items;
  reply.item_count = 1U;
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);

  assert(zzplay_browse_model_activate(&model, 0, &req) ==
         ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  memset(page1, 0, sizeof(page1));
  snprintf(page1[0].id, sizeof(page1[0].id), "track_1");
  page1[0].kind = ZZPLAY_ITEM_TRACK;
  page1[0].playable = 1U;
  snprintf(page1[1].id, sizeof(page1[1].id), "track_2");
  page1[1].kind = ZZPLAY_ITEM_TRACK;
  page1[1].playable = 1U;
  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = page1;
  reply.item_count = 2U;
  reply.has_total = 1;
  reply.total = 4U;
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);
  assert(model.has_more == 1);

  assert(zzplay_browse_model_back(&model, &req));
  memset(&req, 0, sizeof(req));
  assert(zzplay_browse_model_activate(&model, 0, &req) ==
         ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  assert(req.kind == ZZPLAY_BROWSE_REQ_NONE); /* shown from the cache */
  assert(model.item_count == 2U);
  assert(model.has_more == 1);
  assert(model.has_total == 1 && model.total == 4U);
  assert(zzplay_browse_model_request_more(&model, &req));
  assert(req.start == 2U);

  zzplay_browse_model_free(&model);
  return 0;
}

static int test_new_search_drops_previous_query(void)
{
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseReply reply;
  ZZPlayBrowseItem jazz_items[1];
  ZZPlayBrowseItem rock_items[1];
  uint32_t jazz_gen;
  uint32_t rock_gen;

  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "radio", "Radio", NULL));
  assert(zzplay_browse_model_select_service(&model, 0, &req));

  /* User searches for "jazz" */
  assert(zzplay_browse_model_search(&model, "jazz", &req));
  jazz_gen = req.generation;
  assert(req.kind == ZZPLAY_BROWSE_REQ_SEARCH);

  /* User starts new search for "rock" before "jazz" reply arrives */
  assert(zzplay_browse_model_search(&model, "rock", &req));
  rock_gen = req.generation;
  assert(rock_gen > jazz_gen);

  /* Late reply for "jazz" arrives */
  memset(jazz_items, 0, sizeof(jazz_items));
  snprintf(jazz_items[0].id, sizeof(jazz_items[0].id), "jazz_st");
  snprintf(jazz_items[0].title, sizeof(jazz_items[0].title), "Jazz Radio");
  jazz_items[0].kind = ZZPLAY_ITEM_STATION;
  jazz_items[0].playable = 1U;

  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_SEARCH;
  reply.generation = jazz_gen;
  reply.success = 1;
  reply.items = jazz_items;
  reply.item_count = 1U;

  /* Dropped! */
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 0);
  assert(model.item_count == 0U);

  /* Reply for "rock" arrives */
  memset(rock_items, 0, sizeof(rock_items));
  snprintf(rock_items[0].id, sizeof(rock_items[0].id), "rock_st");
  snprintf(rock_items[0].title, sizeof(rock_items[0].title), "Rock Radio");
  rock_items[0].kind = ZZPLAY_ITEM_STATION;
  rock_items[0].playable = 1U;

  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_SEARCH;
  reply.generation = rock_gen;
  reply.success = 1;
  reply.items = rock_items;
  reply.item_count = 1U;

  /* Accepted! */
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);
  assert(model.item_count == 1U);
  assert(strcmp(model.items[0].title, "Rock Radio") == 0);

  zzplay_browse_model_free(&model);
  return 0;
}

static int test_reply_after_stop_dropped(void)
{
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseReply reply;
  ZZPlayBrowseItem items[1];
  uint32_t req_gen;

  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "radio", "Radio", NULL));
  assert(zzplay_browse_model_select_service(&model, 0, &req));
  req_gen = req.generation;
  assert(model.busy == 1);

  /* Stop is pressed */
  zzplay_browse_model_stop(&model);
  assert(model.busy == 0);
  assert(model.generation > req_gen);

  /* Late reply arrives */
  memset(items, 0, sizeof(items));
  snprintf(items[0].id, sizeof(items[0].id), "late_item");
  snprintf(items[0].title, sizeof(items[0].title), "Late Item");
  items[0].kind = ZZPLAY_ITEM_TRACK;
  items[0].playable = 1U;

  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = req_gen;
  reply.success = 1;
  reply.items = items;
  reply.item_count = 1U;

  /* Dropped! */
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 0);
  assert(model.item_count == 0U);

  zzplay_browse_model_free(&model);
  return 0;
}

static int test_play_add_unavailable_for_folders_and_unplayable(void)
{
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseReply reply;
  ZZPlayBrowseItem items[5];

  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "plex", "Plex", NULL));
  assert(zzplay_browse_model_select_service(&model, 0, &req));

  memset(items, 0, sizeof(items));
  /* 0: Folder */
  snprintf(items[0].id, sizeof(items[0].id), "dir_1");
  snprintf(items[0].title, sizeof(items[0].title), "Music Folder");
  items[0].kind = ZZPLAY_ITEM_FOLDER;
  items[0].playable = 0U;

  /* 1: Unplayable track */
  snprintf(items[1].id, sizeof(items[1].id), "track_unplayable");
  snprintf(items[1].title, sizeof(items[1].title), "Corrupted Track");
  items[1].kind = ZZPLAY_ITEM_TRACK;
  items[1].playable = 0U;

  /* 2: Playable track */
  snprintf(items[2].id, sizeof(items[2].id), "track_ok");
  snprintf(items[2].title, sizeof(items[2].title), "Song FLAC");
  items[2].kind = ZZPLAY_ITEM_TRACK;
  items[2].playable = 1U;

  /* 3: Playable station */
  snprintf(items[3].id, sizeof(items[3].id), "station_live");
  snprintf(items[3].title, sizeof(items[3].title), "Live Stream");
  items[3].kind = ZZPLAY_ITEM_STATION;
  items[3].playable = 1U;

  /* 4: Playable folder, a collection */
  snprintf(items[4].id, sizeof(items[4].id), "alb:201");
  snprintf(items[4].title, sizeof(items[4].title), "First Light");
  items[4].kind = ZZPLAY_ITEM_FOLDER;
  items[4].playable = 1U;

  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = items;
  reply.item_count = 5U;

  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);

  /* No selection */
  assert(model.selection == -1);
  assert(!zzplay_browse_model_can_play(&model));
  assert(!zzplay_browse_model_can_add(&model));

  /* Select folder */
  zzplay_browse_model_set_selection(&model, 0);
  assert(!zzplay_browse_model_can_play(&model));
  assert(!zzplay_browse_model_can_add(&model));

  /* Select unplayable item */
  zzplay_browse_model_set_selection(&model, 1);
  assert(!zzplay_browse_model_can_play(&model));
  assert(!zzplay_browse_model_can_add(&model));
  assert(zzplay_browse_model_play_target(&model) == ZZPLAY_BROWSE_TARGET_NONE);

  /* Select playable track */
  zzplay_browse_model_set_selection(&model, 2);
  assert(zzplay_browse_model_can_play(&model));
  assert(zzplay_browse_model_can_add(&model));
  assert(zzplay_browse_model_play_target(&model) == ZZPLAY_BROWSE_TARGET_ITEM);

  /* Select playable station */
  zzplay_browse_model_set_selection(&model, 3);
  assert(zzplay_browse_model_can_play(&model));
  assert(zzplay_browse_model_can_add(&model));
  assert(zzplay_browse_model_play_target(&model) == ZZPLAY_BROWSE_TARGET_ITEM);

  /* Select a playable folder: a collection Play and Add gather */
  zzplay_browse_model_set_selection(&model, 4);
  assert(zzplay_browse_model_can_play(&model));
  assert(zzplay_browse_model_can_add(&model));
  assert(zzplay_browse_model_play_target(&model) ==
         ZZPLAY_BROWSE_TARGET_COLLECTION);

  /* A collection is not a favourite */
  assert(!zzplay_browse_model_can_favourite(&model));

  zzplay_browse_model_free(&model);
  return 0;
}

static int test_activation_folder_enters_station_plays(void)
{
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseReply reply;
  ZZPlayBrowseItem items[2];
  ZZPlayBrowseAction action;
  char uri[128];
  char title[128];

  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "radio", "Internet Radio", NULL));
  assert(zzplay_browse_model_select_service(&model, 0, &req));

  memset(items, 0, sizeof(items));
  snprintf(items[0].id, sizeof(items[0].id), "rock_folder");
  snprintf(items[0].title, sizeof(items[0].title), "Classic Rock");
  items[0].kind = ZZPLAY_ITEM_FOLDER;
  items[0].playable = 0U;

  snprintf(items[1].id, sizeof(items[1].id), "station_def_leppard");
  snprintf(items[1].title, sizeof(items[1].title), "Def Leppard Radio");
  items[1].kind = ZZPLAY_ITEM_STATION;
  items[1].playable = 1U;

  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = items;
  reply.item_count = 2U;

  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);

  /* Activate folder -> enters it */
  action = zzplay_browse_model_activate(&model, 0, &req);
  assert(action == ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  assert(req.kind == ZZPLAY_BROWSE_REQ_LIST);
  assert(strcmp(req.node_id, "rock_folder") == 0);
  assert(model.stack_depth == 1U);

  /* Back to root */
  assert(zzplay_browse_model_back(&model, &req));

  /* Activate station -> plays it */
  action = zzplay_browse_model_activate(&model, 1, &req);
  assert(action == ZZPLAY_BROWSE_ACTION_PLAY);
  assert(model.selection == 1);

  /* Format URI */
  assert(zzplay_browse_model_format_uri(&model, 1, uri, sizeof(uri),
                                        title, sizeof(title)) == 1);
  assert(strcmp(uri, "radio://station_def_leppard") == 0);
  assert(strcmp(title, "Def Leppard Radio") == 0);

  zzplay_browse_model_free(&model);
  return 0;
}

static int test_settings_row_opens_settings(void)
{
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseReply reply;
  ZZPlayBrowseItem item;

  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "plex", "Plex", NULL));
  assert(zzplay_browse_model_select_service(&model, 0, &req));
  memset(&item, 0, sizeof(item));
  snprintf(item.id, sizeof(item.id), "%s", ZZPLAY_PROVIDER_SETTINGS_ID);
  snprintf(item.title, sizeof(item.title),
           "Not signed in. Open Settings to sign in.");
  item.kind = ZZPLAY_ITEM_TRACK;
  item.playable = 0U;
  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = &item;
  reply.item_count = 1U;
  assert(zzplay_browse_model_apply_reply(&model, &reply));
  assert(zzplay_browse_model_activate(&model, 0, &req) ==
         ZZPLAY_BROWSE_ACTION_OPEN_SETTINGS);
  assert(model.selection == 0);
  zzplay_browse_model_free(&model);
  return 0;
}


static int test_gather_request_for_collection(void)
{
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseReply reply;
  ZZPlayBrowseItem items[3];
  uint32_t gen;

  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "plex", "Plex", NULL));
  assert(zzplay_browse_model_select_service(&model, 0, &req));

  memset(items, 0, sizeof(items));
  snprintf(items[0].id, sizeof(items[0].id), "alb:201");
  snprintf(items[0].title, sizeof(items[0].title), "First Light");
  items[0].kind = ZZPLAY_ITEM_FOLDER;
  items[0].playable = 1U;

  snprintf(items[1].id, sizeof(items[1].id), "ltr:3:9:A");
  snprintf(items[1].title, sizeof(items[1].title), "A (2)");
  items[1].kind = ZZPLAY_ITEM_FOLDER;
  items[1].playable = 0U;

  snprintf(items[2].id, sizeof(items[2].id), "301");
  snprintf(items[2].title, sizeof(items[2].title), "Opening");
  items[2].kind = ZZPLAY_ITEM_TRACK;
  items[2].playable = 1U;

  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = items;
  reply.item_count = 3U;
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);

  /* Only a playable folder starts a gather */
  assert(model.selection == -1);
  assert(!zzplay_browse_model_gather(&model, 4096U, &req));
  zzplay_browse_model_set_selection(&model, 2);
  assert(!zzplay_browse_model_gather(&model, 4096U, &req));
  zzplay_browse_model_set_selection(&model, 1);
  assert(!zzplay_browse_model_gather(&model, 4096U, &req));

  zzplay_browse_model_set_selection(&model, 0);
  assert(zzplay_browse_model_gather(&model, 4096U, &req));
  gen = model.generation;
  assert(req.kind == ZZPLAY_BROWSE_REQ_GATHER);
  assert(req.generation == gen);
  assert(model.busy == 1);
  assert(strcmp(req.scheme, "plex") == 0);
  assert(strcmp(req.node_id, "alb:201") == 0);
  assert(req.count == 4096U);

  /* A second gather supersedes the first */
  assert(zzplay_browse_model_gather(&model, 12U, &req));
  assert(req.generation == gen + 1U);
  assert(req.count == 12U);
  assert(model.busy == 1);

  zzplay_browse_model_free(&model);
  return 0;
}
static int fake_fav(void *ctx, const ZZPlayProviderItem *item, int add,
                    char *err, uint32_t err_size)
{
  (void)ctx; (void)item; (void)add; (void)err; (void)err_size;
  return 1;
}

static int test_favourite_action(void)
{
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseReply reply;
  ZZPlayBrowseItem items[4];
  ZZPlayProvider prov_with_fav;
  ZZPlayProvider prov_no_fav;
  ZZPlayProvider prov_old_size;

  memset(&prov_with_fav, 0, sizeof(prov_with_fav));
  prov_with_fav.size = sizeof(ZZPlayProvider);
  prov_with_fav.scheme = "radio";
  prov_with_fav.favourite = fake_fav;

  memset(&prov_no_fav, 0, sizeof(prov_no_fav));
  prov_no_fav.size = sizeof(ZZPlayProvider);
  prov_no_fav.scheme = "plex";
  prov_no_fav.favourite = NULL;

  memset(&prov_old_size, 0, sizeof(prov_old_size));
  prov_old_size.size = offsetof(ZZPlayProvider, favourite);
  prov_old_size.scheme = "old";
  prov_old_size.favourite = fake_fav;

  /* 1. Provider capability checks */
  assert(zzplay_browse_model_provider_has_favourite(&prov_with_fav) == 1);
  assert(zzplay_browse_model_provider_has_favourite(&prov_no_fav) == 0);
  assert(zzplay_browse_model_provider_has_favourite(&prov_old_size) == 0);
  assert(zzplay_browse_model_provider_has_favourite(NULL) == 0);

  /* 2. Availability with service without favourite call */
  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "plex", "Plex", &prov_no_fav));
  assert(zzplay_browse_model_select_service(&model, 0, &req));

  memset(items, 0, sizeof(items));
  snprintf(items[0].id, sizeof(items[0].id), "tr1");
  items[0].kind = ZZPLAY_ITEM_TRACK;
  items[0].playable = 1U;

  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = items;
  reply.item_count = 1U;
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);

  zzplay_browse_model_set_selection(&model, 0);
  assert(zzplay_browse_model_can_favourite(&model) == 0); /* no call */
  zzplay_browse_model_free(&model);

  /* 3. Availability and operation choice with service having favourite call */
  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "radio", "Radio", &prov_with_fav));
  assert(zzplay_browse_model_select_service(&model, 0, &req));

  memset(items, 0, sizeof(items));
  /* items[0]: folder */
  snprintf(items[0].id, sizeof(items[0].id), "fld");
  items[0].kind = ZZPLAY_ITEM_FOLDER;
  items[0].playable = 0U;

  /* items[1]: unplayable track */
  snprintf(items[1].id, sizeof(items[1].id), "unplay");
  items[1].kind = ZZPLAY_ITEM_TRACK;
  items[1].playable = 0U;

  /* items[2]: playable unflagged station (favourite = 0) */
  snprintf(items[2].id, sizeof(items[2].id), "st_unfav");
  items[2].kind = ZZPLAY_ITEM_STATION;
  items[2].playable = 1U;
  items[2].favourite = 0U;

  /* items[3]: playable flagged station (favourite = 1) */
  snprintf(items[3].id, sizeof(items[3].id), "st_fav");
  items[3].kind = ZZPLAY_ITEM_STATION;
  items[3].playable = 1U;
  items[3].favourite = 1U;

  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = items;
  reply.item_count = 4U;
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);

  /* Availability: folder -> 0 */
  zzplay_browse_model_set_selection(&model, 0);
  assert(zzplay_browse_model_can_favourite(&model) == 0);

  /* Availability: unplayable -> 0 */
  zzplay_browse_model_set_selection(&model, 1);
  assert(zzplay_browse_model_can_favourite(&model) == 0);

  /* Availability & operation: playable unflagged item -> can=1, chooses add (1) */
  zzplay_browse_model_set_selection(&model, 2);
  assert(zzplay_browse_model_can_favourite(&model) == 1);
  assert(zzplay_browse_model_favourite(&model, &req) == 1);
  assert(req.kind == ZZPLAY_BROWSE_REQ_FAVOURITE);
  assert(req.fav_add == 1); /* add */
  assert(strcmp(req.item.id, "st_unfav") == 0);

  /* Availability & operation: playable flagged item -> can=1, chooses remove (0) */
  zzplay_browse_model_set_selection(&model, 3);
  assert(zzplay_browse_model_can_favourite(&model) == 1);
  assert(zzplay_browse_model_favourite(&model, &req) == 1);
  assert(req.kind == ZZPLAY_BROWSE_REQ_FAVOURITE);
  assert(req.fav_add == 0); /* remove */
  assert(strcmp(req.item.id, "st_fav") == 0);

  zzplay_browse_model_free(&model);
  return 0;
}
/* Build a reply for `gen` carrying `n` items titled t0..t(n-1). */
static ZZPlayBrowseReply make_list_reply(uint32_t gen, uint32_t start,
                                         ZZPlayBrowseItem *items, uint32_t n,
                                         int partial)
{
  ZZPlayBrowseReply r;
  uint32_t i;

  memset(items, 0, (size_t)n * sizeof(*items));
  for (i = 0U; i < n; i++) {
    snprintf(items[i].id, sizeof(items[i].id), "id_%lu_%lu",
             (unsigned long)start, (unsigned long)i);
    snprintf(items[i].title, sizeof(items[i].title), "Item %lu_%lu",
             (unsigned long)start, (unsigned long)i);
    items[i].kind = ZZPLAY_ITEM_STATION;
    items[i].playable = 1U;
  }
  memset(&r, 0, sizeof(r));
  r.kind = ZZPLAY_BROWSE_REQ_LIST;
  r.generation = gen;
  r.success = 1;
  r.items = items;
  r.item_count = n;
  r.start = start;
  r.is_partial = partial;
  return r;
}

static int test_remembered_folder_reopens_without_request(void)
{
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseReply reply;
  ZZPlayBrowseItem items[2];
  ZZPlayBrowseAction act;

  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "plex", "Plex", NULL));

  /* Root: one folder */
  reply = make_list_reply(1U, 0U, items, 1U, 0);
  snprintf(items[0].id, sizeof(items[0].id), "artist1");
  snprintf(items[0].title, sizeof(items[0].title), "Artist One");
  items[0].kind = ZZPLAY_ITEM_FOLDER;
  items[0].playable = 0U;
  assert(zzplay_browse_model_select_service(&model, 0U, &req) == 1);
  assert(req.kind == ZZPLAY_BROWSE_REQ_LIST);
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);

  /* Enter the folder, list completes */
  act = zzplay_browse_model_activate(&model, 0, &req);
  assert(act == ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  assert(req.kind == ZZPLAY_BROWSE_REQ_LIST);
  reply = make_list_reply(req.generation, 0U, items, 2U, 0);
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);
  assert(model.item_count == 2U);
  zzplay_browse_model_set_selection(&model, 1);
  assert(model.selection == 1);

  /* Back to the root, then re-enter: remembered, no request */
  assert(zzplay_browse_model_back(&model, &req) == 1);
  act = zzplay_browse_model_activate(&model, 0, &req);
  assert(act == ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  assert(req.kind == ZZPLAY_BROWSE_REQ_NONE);
  assert(model.busy == 0);
  assert(model.item_count == 2U);
  assert(model.selection == 1);
  assert(strcmp(model.items[1].title, "Item 0_1") == 0);

  /* A remembered search repeats without a request too */
  assert(zzplay_browse_model_back(&model, &req) == 1);
  assert(zzplay_browse_model_search(&model, "beatles", &req) == 1);
  assert(req.kind == ZZPLAY_BROWSE_REQ_SEARCH);
  reply = make_list_reply(req.generation, 0U, items, 1U, 0);
  reply.kind = ZZPLAY_BROWSE_REQ_SEARCH;
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);
  assert(zzplay_browse_model_search(&model, "beatles", &req) == 1);
  assert(req.kind == ZZPLAY_BROWSE_REQ_NONE);
  assert(model.item_count == 1U);

  zzplay_browse_model_free(&model);
  return 0;
}

static int test_reload_sends_request_and_replaces_copy(void)
{
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseReply reply;
  ZZPlayBrowseItem items[1];

  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "radio", "Radio", NULL));
  assert(zzplay_browse_model_select_service(&model, 0U, &req) == 1);
  reply = make_list_reply(req.generation, 0U, items, 1U, 0);
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);

  /* Disabled while nothing loads; enabled with a list shown */
  assert(zzplay_browse_model_can_reload(&model) == 1);

  /* Reload sends a request from start 0 */
  assert(zzplay_browse_model_reload(&model, &req) == 1);
  assert(req.kind == ZZPLAY_BROWSE_REQ_LIST);
  assert(req.start == 0U);
  assert(model.busy == 1);
  assert(zzplay_browse_model_can_reload(&model) == 0);

  /* The new list replaces the remembered copy */
  reply = make_list_reply(req.generation, 0U, items, 1U, 0);
  snprintf(items[0].title, sizeof(items[0].title), "Fresh");
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);
  assert(strcmp(model.items[0].title, "Fresh") == 0);

  /* Back into the folder state: switching service away and back uses cache */
  /* (root here) — the cached root now holds the fresh row */
  assert(zzplay_browse_model_select_service(&model, 0U, &req) == 1);
  assert(req.kind == ZZPLAY_BROWSE_REQ_NONE);
  assert(strcmp(model.items[0].title, "Fresh") == 0);

  zzplay_browse_model_free(&model);
  return 0;
}

/* Rows numbered from `first`, as the worker groups them for a request. */
static ZZPlayBrowseReply make_rows(uint32_t gen, uint32_t start,
                                   ZZPlayBrowseItem *items, uint32_t first,
                                   uint32_t n, int partial)
{
  ZZPlayBrowseReply r = make_list_reply(gen, start, items, n, partial);
  uint32_t i;

  for (i = 0U; i < n; i++) {
    snprintf(items[i].title, sizeof(items[i].title), "Row %lu",
             (unsigned long)(first + i));
  }
  return r;
}

/* The worker sends every group of a request with the request's start and
 * then a final reply carrying the whole page again. */
static int test_partial_replies_append_and_stale_dropped(void)
{
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseReply reply;
  ZZPlayBrowseItem items[6];

  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "radio", "Radio", NULL));
  assert(zzplay_browse_model_select_service(&model, 0U, &req) == 1);

  /* First group, then a second group of the same request: appended. */
  reply = make_rows(req.generation, 0U, items, 0U, 2U, 1);
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);
  assert(model.item_count == 2U);
  assert(model.busy == 1);
  reply = make_rows(req.generation, 0U, items, 2U, 2U, 1);
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);
  assert(model.item_count == 4U);
  assert(strcmp(model.items[0].title, "Row 0") == 0);
  assert(strcmp(model.items[3].title, "Row 3") == 0);

  /* A stale partial (older generation) is dropped */
  reply = make_rows(req.generation - 1U, 0U, items, 0U, 1U, 1);
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 0);
  assert(model.item_count == 4U);

  /* The final reply carries the whole page: the rows are not doubled. */
  reply = make_rows(req.generation, 0U, items, 0U, 5U, 0);
  reply.has_total = 1;
  reply.total = 8U;
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);
  assert(model.busy == 0);
  assert(model.item_count == 5U);
  assert(strcmp(model.items[4].title, "Row 4") == 0);

  /* Page two: its groups follow the first page, its final reply
   * replaces only what its groups added. */
  assert(zzplay_browse_model_request_more(&model, &req) == 1);
  reply = make_rows(req.generation, 5U, items, 5U, 2U, 1);
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);
  assert(model.item_count == 7U);
  reply = make_rows(req.generation, 5U, items, 5U, 3U, 0);
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);
  assert(model.item_count == 8U);
  assert(strcmp(model.items[0].title, "Row 0") == 0);
  assert(strcmp(model.items[7].title, "Row 7") == 0);

  zzplay_browse_model_free(&model);
  return 0;
}

static int test_stop_keeps_arrived_rows(void)
{
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseReply reply;
  ZZPlayBrowseItem items[2];

  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "radio", "Radio", NULL));
  assert(zzplay_browse_model_select_service(&model, 0U, &req) == 1);

  reply = make_list_reply(req.generation, 0U, items, 2U, 1);
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);

  zzplay_browse_model_stop(&model);
  assert(model.busy == 0);
  assert(model.item_count == 2U);

  /* Late rows for the stopped request are dropped */
  reply = make_list_reply(req.generation - 1U, 2U, items, 2U, 1);
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 0);
  assert(model.item_count == 2U);

  zzplay_browse_model_free(&model);
  return 0;
}

static int test_cache_bound_drops_oldest(void)
{
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseReply reply;
  ZZPlayBrowseItem items[1];
  uint32_t i;
  char title[ZZPLAY_PROVIDER_TEXT_MAX];

  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "plex", "Plex", NULL));

  /* Root list with folders f0..fN; each opened list completes (cached) */
  for (i = 0U; i <= ZZPLAY_BROWSE_CACHE_MAX; i++) {
    snprintf(title, sizeof(title), "folder_%lu", (unsigned long)i);
    if (i == 0U) {
      assert(zzplay_browse_model_select_service(&model, 0U, &req) == 1);
      reply = make_list_reply(req.generation, 0U, items, 1U, 0);
      snprintf(items[0].id, sizeof(items[0].id), "%s", title);
      items[0].kind = ZZPLAY_ITEM_FOLDER;
      items[0].playable = 0U;
      assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);
    } else {
      assert(zzplay_browse_model_activate(&model, 0, &req) ==
             ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
      /* The first folder's list was dropped once the cache filled */
      if (i == 1U) {
        assert(req.kind == ZZPLAY_BROWSE_REQ_LIST);
      }
      if (req.kind == ZZPLAY_BROWSE_REQ_LIST) {
        reply = make_list_reply(req.generation, 0U, items, 1U, 0);
        snprintf(items[0].id, sizeof(items[0].id), "station_%lu",
                 (unsigned long)i);
        assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);
      }
      assert(zzplay_browse_model_back(&model, &req) == 1);
      /* Touch the root again so folder_1 ages fastest after the root */
      assert(zzplay_browse_model_activate(&model, 0, &req) ==
             ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
      if (req.kind == ZZPLAY_BROWSE_REQ_LIST) {
        reply = make_list_reply(req.generation, 0U, items, 1U, 0);
        snprintf(items[0].id, sizeof(items[0].id), "station_%lu",
                 (unsigned long)i);
        assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);
      }
      assert(zzplay_browse_model_back(&model, &req) == 1);
    }
  }

  /* The cache held the root plus folder lists; the oldest folder
   * (folder_1's list) was dropped. Root survives (touched last). */
  assert(zzplay_browse_model_select_service(&model, 0U, &req) == 1);
  assert(req.kind == ZZPLAY_BROWSE_REQ_NONE);

  zzplay_browse_model_free(&model);
  return 0;
}

/* Play all / Add all act on the folder being shown: availability and
 * the gather request they start. */
static int test_play_all_add_all_on_shown_folder(void)
{
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseReply reply;
  ZZPlayBrowseItem root[2];
  ZZPlayBrowseItem artist_albums[1];
  ZZPlayBrowseItem album_tracks[2];
  uint32_t gen;

  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "plex", "Plex", NULL));
  assert(zzplay_browse_model_select_service(&model, 0, &req));

  /* Top level: an artist (a playable collection) and a plain folder */
  memset(root, 0, sizeof(root));
  snprintf(root[0].id, sizeof(root[0].id), "art:7");
  snprintf(root[0].title, sizeof(root[0].title), "An Artist");
  root[0].kind = ZZPLAY_ITEM_FOLDER;
  root[0].playable = 1U;

  snprintf(root[1].id, sizeof(root[1].id), "ltr:A");
  snprintf(root[1].title, sizeof(root[1].title), "A");
  root[1].kind = ZZPLAY_ITEM_FOLDER;
  root[1].playable = 0U;

  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = root;
  reply.item_count = 2U;
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);

  /* The top level is not a collection */
  assert(!zzplay_browse_model_can_play_all(&model));
  assert(!zzplay_browse_model_can_add_all(&model));
  assert(!zzplay_browse_model_gather_shown(&model, 4096U, &req));

  /* Enter the artist: its list is shown and it is a collection, but not
   * while the list is still loading. */
  assert(zzplay_browse_model_activate(&model, 0, &req) ==
         ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  assert(strcmp(req.node_id, "art:7") == 0);
  assert(model.busy == 1);
  assert(!zzplay_browse_model_can_play_all(&model));

  memset(artist_albums, 0, sizeof(artist_albums));
  snprintf(artist_albums[0].id, sizeof(artist_albums[0].id), "alb:9");
  snprintf(artist_albums[0].title, sizeof(artist_albums[0].title), "An Album");
  artist_albums[0].kind = ZZPLAY_ITEM_FOLDER;
  artist_albums[0].playable = 1U;

  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = artist_albums;
  reply.item_count = 1U;
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);
  assert(zzplay_browse_model_can_play_all(&model));
  assert(zzplay_browse_model_can_add_all(&model));

  /* A collection view with no rows yet (its list arrived empty, or is
   * still loading) offers neither button: Play all on nothing must not
   * be possible. */
  {
    ZZPlayBrowseModel empty_model;
    ZZPlayBrowseRequest empty_req;
    ZZPlayBrowseReply empty_reply;
    ZZPlayBrowseItem root[1];

    zzplay_browse_model_init(&empty_model);
    assert(zzplay_browse_model_add_service(&empty_model, "plex", "Plex",
                                           NULL));
    assert(zzplay_browse_model_select_service(&empty_model, 0, &empty_req));
    memset(root, 0, sizeof(root));
    snprintf(root[0].id, sizeof(root[0].id), "art:1");
    snprintf(root[0].title, sizeof(root[0].title), "Empty Artist");
    root[0].kind = ZZPLAY_ITEM_FOLDER;
    root[0].playable = 1U;

    memset(&empty_reply, 0, sizeof(empty_reply));
    empty_reply.kind = ZZPLAY_BROWSE_REQ_LIST;
    empty_reply.generation = empty_req.generation;
    empty_reply.success = 1;
    empty_reply.items = root;
    empty_reply.item_count = 1U;
    assert(zzplay_browse_model_apply_reply(&empty_model, &empty_reply) == 1);

    assert(zzplay_browse_model_activate(&empty_model, 0, &empty_req) ==
           ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
    memset(&empty_reply, 0, sizeof(empty_reply));
    empty_reply.kind = ZZPLAY_BROWSE_REQ_LIST;
    empty_reply.generation = empty_req.generation;
    empty_reply.success = 1;
    empty_reply.item_count = 0U;
    assert(zzplay_browse_model_apply_reply(&empty_model, &empty_reply) == 1);
    assert(empty_model.busy == 0);
    assert(empty_model.folder_is_collection == 1);
    assert(empty_model.item_count == 0U);
    assert(!zzplay_browse_model_can_play_all(&empty_model));
    assert(!zzplay_browse_model_gather_shown(&empty_model, 4096U,
                                             &empty_req));
    zzplay_browse_model_free(&empty_model);
  }

  /* The gather asks for the shown folder, not a selected row */
  assert(zzplay_browse_model_gather_shown(&model, 4096U, &req));
  gen = model.generation;
  assert(req.kind == ZZPLAY_BROWSE_REQ_GATHER);
  assert(req.generation == gen);
  assert(strcmp(req.scheme, "plex") == 0);
  assert(strcmp(req.node_id, "art:7") == 0);
  assert(req.count == 4096U);
  assert(model.busy == 1);
  /* While it runs the buttons are unavailable */
  assert(!zzplay_browse_model_can_play_all(&model));
  assert(!zzplay_browse_model_gather_shown(&model, 4096U, &req));

  /* Enter the album inside; Back returns to the artist, still a
   * collection. */
  assert(zzplay_browse_model_activate(&model, 0, &req) ==
         ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  memset(album_tracks, 0, sizeof(album_tracks));
  snprintf(album_tracks[0].id, sizeof(album_tracks[0].id), "t1");
  snprintf(album_tracks[0].title, sizeof(album_tracks[0].title), "One");
  album_tracks[0].kind = ZZPLAY_ITEM_TRACK;
  album_tracks[0].playable = 1U;
  snprintf(album_tracks[1].id, sizeof(album_tracks[1].id), "t2");
  snprintf(album_tracks[1].title, sizeof(album_tracks[1].title), "Two");
  album_tracks[1].kind = ZZPLAY_ITEM_TRACK;
  album_tracks[1].playable = 1U;

  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = album_tracks;
  reply.item_count = 2U;
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);
  assert(zzplay_browse_model_can_play_all(&model));

  assert(zzplay_browse_model_back(&model, &req));
  assert(zzplay_browse_model_can_play_all(&model));
  assert(zzplay_browse_model_back(&model, &req));
  assert(!zzplay_browse_model_can_play_all(&model));

  /* A plain folder is no collection */
  assert(zzplay_browse_model_activate(&model, 1, &req) ==
         ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = root;
  reply.item_count = 2U;
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);
  assert(!zzplay_browse_model_can_play_all(&model));

  /* Search results are not a collection either */
  assert(zzplay_browse_model_search(&model, "album", &req));
  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_SEARCH;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = album_tracks;
  reply.item_count = 2U;
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);
  assert(!zzplay_browse_model_can_play_all(&model));

  /* A remembered collection folder reopens at once with its buttons
   * available, without a request. */
  assert(zzplay_browse_model_back(&model, &req));
  assert(zzplay_browse_model_activate(&model, 0, &req) ==
         ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  assert(req.kind == ZZPLAY_BROWSE_REQ_NONE);
  assert(model.busy == 0);
  assert(zzplay_browse_model_can_play_all(&model));

  zzplay_browse_model_free(&model);
  return 0;
}

/* The reported Amiberry sequence: section -> letter -> artist -> Play
 * all -> Back must show the letter's artists again with Play all off.
 * The gather reply is handled like zzplay-browse.c handles it: the model
 * only gets its busy flag cleared, no apply_reply runs for it. */
static int test_back_after_play_all_shows_letter_rows(void)
{
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseReply reply;
  ZZPlayBrowseItem sections[1];
  ZZPlayBrowseItem letters[1];
  ZZPlayBrowseItem artists[2];
  ZZPlayBrowseItem albums[1];

  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "plex", "Plex", NULL));
  assert(zzplay_browse_model_select_service(&model, 0, &req));

  memset(sections, 0, sizeof(sections));
  snprintf(sections[0].id, sizeof(sections[0].id), "sec:1");
  snprintf(sections[0].title, sizeof(sections[0].title), "Artists");
  sections[0].kind = ZZPLAY_ITEM_FOLDER;
  sections[0].playable = 0U;

  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = sections;
  reply.item_count = 1U;
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);

  /* Enter the Artists section */
  assert(zzplay_browse_model_activate(&model, 0, &req) ==
         ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  assert(strcmp(req.node_id, "sec:1") == 0);

  memset(letters, 0, sizeof(letters));
  snprintf(letters[0].id, sizeof(letters[0].id), "ltr:1:9:A");
  snprintf(letters[0].title, sizeof(letters[0].title), "A (72)");
  letters[0].kind = ZZPLAY_ITEM_FOLDER;
  letters[0].playable = 0U;

  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = letters;
  reply.item_count = 1U;
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);

  /* Enter letter A */
  assert(zzplay_browse_model_activate(&model, 0, &req) ==
         ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  assert(strcmp(req.node_id, "ltr:1:9:A") == 0);

  memset(artists, 0, sizeof(artists));
  snprintf(artists[0].id, sizeof(artists[0].id), "art:17");
  snprintf(artists[0].title, sizeof(artists[0].title), "ABBA");
  artists[0].kind = ZZPLAY_ITEM_FOLDER;
  artists[0].playable = 1U;

  snprintf(artists[1].id, sizeof(artists[1].id), "art:42");
  snprintf(artists[1].title, sizeof(artists[1].title), "Ace of Base");
  artists[1].kind = ZZPLAY_ITEM_FOLDER;
  artists[1].playable = 1U;

  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = artists;
  reply.item_count = 2U;
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);

  /* Enter the artist: its list shows one playable album */
  assert(zzplay_browse_model_activate(&model, 0, &req) ==
         ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  assert(strcmp(req.node_id, "art:17") == 0);

  memset(albums, 0, sizeof(albums));
  snprintf(albums[0].id, sizeof(albums[0].id), "alb:9");
  snprintf(albums[0].title, sizeof(albums[0].title), "Happy New Year");
  albums[0].kind = ZZPLAY_ITEM_FOLDER;
  albums[0].playable = 1U;

  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = albums;
  reply.item_count = 1U;
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);
  assert(model.folder_is_collection == 1);
  assert(zzplay_browse_model_can_play_all(&model));

  /* Play all: a GATHER for the shown artist */
  assert(zzplay_browse_model_gather_shown(&model, 4096U, &req));
  assert(req.kind == ZZPLAY_BROWSE_REQ_GATHER);
  assert(strcmp(req.node_id, "art:17") == 0);
  assert(model.busy == 1);

  /* The gather's reply never reaches apply_reply; the GUI only clears
   * the busy flag once the tracks were added. */
  model.busy = 0;

  /* Back: the letter's artists again, not a collection */
  assert(zzplay_browse_model_back(&model, &req));
  assert(strcmp(model.current_node, "ltr:1:9:A") == 0);
  assert(model.item_count == 2U);
  assert(model.items != NULL);
  assert(strcmp(model.items[0].id, "art:17") == 0);
  assert(model.folder_is_collection == 0);
  assert(!zzplay_browse_model_can_play_all(&model));
  assert(!zzplay_browse_model_gather_shown(&model, 4096U, &req));

  zzplay_browse_model_free(&model);
  return 0;
}


/* ------------------------------------------------------------------ *
 * A faithful replay of zzplay-browse.c's worker-reply loop over a
 * scripted service, so click timing (pending, cancel, partials) can be
 * varied against the real model. */

typedef struct ReplayNode {
  const char *id;
  const char *titles[4];
  uint32_t kinds[4];
  uint32_t playable[4];
  const char *child_ids[4];
  uint32_t count;
} ReplayNode;

/* "" -> lib:1:art -> ltr:1:8:A (artists) -> art:N (one album each) */
static ReplayNode g_replay_tree[] = {
    {"", {"Artists"}, {0}, {0}, {"lib:1:art"}, 1},
    {"lib:1:art", {"A"}, {0}, {0}, {"ltr:1:8:A"}, 1},
    {"ltr:1:8:A",
     {"Artist One", "Artist Two", "Artist Three", "Artist Four"},
     {0, 0, 0, 0},
     {1, 1, 1, 1},
     {"art:1", "art:2", "art:3", "art:4"},
     4},
    {"art:1", {"One Album"}, {0}, {1}, {"alb:1"}, 1},
    {"art:2", {"Two Album"}, {0}, {1}, {"alb:2"}, 1},
    {"art:3", {"Three Album"}, {0}, {1}, {"alb:3"}, 1},
    {"art:4", {"Four Album"}, {0}, {1}, {"alb:4"}, 1},
    {"alb:1", {"Track A"}, {1}, {1}, {"301"}, 1},
    {"alb:2", {"Track B"}, {1}, {1}, {"302"}, 1},
    {"alb:3", {"Track C"}, {1}, {1}, {"303"}, 1},
    {"alb:4", {"Track D"}, {1}, {1}, {"304"}, 1},
};

static ReplayNode *replay_find(const char *id)
{
  uint32_t i;
  for (i = 0U; i < sizeof(g_replay_tree) / sizeof(g_replay_tree[0]); i++) {
    if (strcmp(g_replay_tree[i].id, id) == 0) {
      return &g_replay_tree[i];
    }
  }
  return NULL;
}

typedef struct ReplayState {
  ZZPlayBrowseModel *model;
  ZZPlayBrowseReply queue[16];
  uint32_t queued;
  uint32_t taken;
  int worker_busy;
  ZZPlayBrowseRequest pending;
  int have_pending;
  int gather_active;
  uint32_t gather_generation;
  uint32_t gather_seen;
  uint32_t gather_added;
} ReplayState;

static ReplayState g_replay;

static void replay_queue_reply(const ZZPlayBrowseReply *reply)
{
  if (g_replay.queued < sizeof(g_replay.queue) / sizeof(g_replay.queue[0])) {
    g_replay.queue[g_replay.queued++] = *reply;
  }
}

/* The worker runs one request to completion, queueing its replies. */
static void replay_run(const ZZPlayBrowseRequest *req)
{
  ZZPlayBrowseItem *items;
  ReplayNode *node;
  ZZPlayBrowseReply reply;
  uint32_t i;

  memset(&reply, 0, sizeof(reply));
  reply.kind = req->kind;
  reply.generation = req->generation;
  reply.start = req->start;
  reply.success = 1;

  if (req->kind == ZZPLAY_BROWSE_REQ_LIST) {
    node = replay_find(req->node_id);
    if (!node) {
      reply.success = 0;
      snprintf(reply.error, sizeof(reply.error), "unknown node");
      replay_queue_reply(&reply);
      return;
    }
    items = NULL;
    if (node->count > 0U) {
      items = (ZZPlayBrowseItem *)calloc((size_t)node->count,
                                         sizeof(ZZPlayBrowseItem));
      assert(items != NULL);
      for (i = 0U; i < node->count; i++) {
        snprintf(items[i].id, sizeof(items[i].id), "%s",
                 node->child_ids[i]);
        snprintf(items[i].title, sizeof(items[i].title), "%s",
                 node->titles[i]);
        items[i].kind = node->kinds[i];
        items[i].playable = node->playable[i];
      }
    }
    reply.items = items;
    reply.item_count = node->count;
    reply.has_total = 1;
    reply.total = node->count;
    reply.is_partial = 0;
    replay_queue_reply(&reply);
    return;
  }

  if (req->kind == ZZPLAY_BROWSE_REQ_GATHER) {
    ReplayNode *root = replay_find(req->node_id);
    uint32_t collected = 0U;
    assert(root != NULL);
    items = (ZZPlayBrowseItem *)calloc(8U, sizeof(ZZPlayBrowseItem));
    assert(items != NULL);
    for (i = 0U; i < root->count; i++) {
      ReplayNode *child = replay_find(root->child_ids[i]);
      uint32_t t;
      assert(child != NULL);
      for (t = 0U; t < child->count; t++) {
        snprintf(items[collected].id, sizeof(items[collected].id), "%s",
                 child->child_ids[t]);
        snprintf(items[collected].title, sizeof(items[collected].title),
                 "%s", child->titles[t]);
        items[collected].kind = child->kinds[t];
        items[collected].playable = child->playable[t];
        collected++;
      }
    }
    reply.items = items;
    reply.item_count = collected;
    reply.is_partial = 0;
    replay_queue_reply(&reply);
    return;
  }

  replay_queue_reply(&reply);
}

/* browse_send: send now, or hold as pending while one runs */
static void replay_send(const ZZPlayBrowseRequest *req)
{
  if (g_replay.worker_busy) {
    g_replay.pending = *req;
    g_replay.have_pending = 1;
    return;
  }
  g_replay.worker_busy = 1;
  replay_run(req);
}

static void replay_apply_gather(const ZZPlayBrowseReply *reply)
{
  g_replay.gather_active = 0;
  g_replay.model->busy = 0;
  if (reply->success) {
    g_replay.gather_added = reply->item_count;
  }
}

/* zzplay_browse_poll's worker-reply loop, replayed */
static void replay_drain(uint32_t max_replies)
{
  uint32_t processed = 0U;

  while (g_replay.taken < g_replay.queued &&
         (max_replies == 0U || processed < max_replies)) {
    ZZPlayBrowseReply reply = g_replay.queue[g_replay.taken++];
    processed++;

    if (!reply.is_partial) {
      g_replay.worker_busy = 0;
    }
    if (reply.kind == ZZPLAY_BROWSE_REQ_GATHER) {
      if (g_replay.gather_active &&
          reply.generation == g_replay.gather_generation) {
        if (reply.is_partial) {
          g_replay.gather_seen += reply.item_count;
        } else {
          replay_apply_gather(&reply);
        }
      }
      free((void *)reply.items);
      continue;
    }
    {
      int ok = zzplay_browse_model_apply_reply(g_replay.model, &reply);
      (void)ok;
      free((void *)reply.items);
    }
    if (g_replay.have_pending && !g_replay.worker_busy) {
      g_replay.have_pending = 0;
      g_replay.worker_busy = 1;
      replay_run(&g_replay.pending);
    }
  }
  if (g_replay.taken == g_replay.queued) {
    g_replay.queued = 0U;
    g_replay.taken = 0U;
  }
}

static void replay_click_folder(int32_t index)
{
  ZZPlayBrowseRequest req;

  if (zzplay_browse_model_activate(g_replay.model, index, &req) ==
      ZZPLAY_BROWSE_ACTION_ENTER_FOLDER) {
    if (req.kind != ZZPLAY_BROWSE_REQ_NONE) {
      replay_send(&req);
    }
  }
}

static void replay_play_all(void)
{
  ZZPlayBrowseRequest req;

  if (zzplay_browse_model_gather_shown(g_replay.model, 4096U, &req)) {
    g_replay.gather_active = 1;
    g_replay.gather_generation = req.generation;
    g_replay.gather_seen = 0U;
    g_replay.gather_added = 0U;
    replay_send(&req);
  }
}

static void replay_back(void)
{
  ZZPlayBrowseRequest req;
  (void)zzplay_browse_model_back(g_replay.model, &req);
}

/* The reported Amiberry sequence with the artist clicked while the
 * letter's rows were still arriving: the list is clickable then, so the
 * artist's list becomes the pending request while the letter's finishes
 * as a stale reply. */
static int test_replay_click_during_letter_load(void)
{
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;

  memset(&g_replay, 0, sizeof(g_replay));
  g_replay.model = &model;

  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "plex", "Plex", NULL));
  assert(zzplay_browse_model_select_service(&model, 0, &req));
  replay_send(&req);
  replay_drain(0);
  assert(model.item_count == 1U);

  replay_click_folder(0); /* "Artists" */
  replay_drain(0);
  assert(strcmp(model.current_node, "lib:1:art") == 0);
  assert(model.item_count == 1U);

  replay_click_folder(0); /* letter A */
  replay_drain(0);
  assert(strcmp(model.current_node, "ltr:1:8:A") == 0);
  assert(model.item_count == 4U);

  replay_click_folder(1); /* Artist Two */
  replay_drain(0);
  assert(strcmp(model.current_node, "art:2") == 0);
  assert(model.item_count == 1U);
  assert(model.folder_is_collection == 1);
  assert(zzplay_browse_model_can_play_all(&model));

  replay_play_all();
  replay_drain(0);
  assert(g_replay.gather_added == 1U);
  assert(model.busy == 0);

  replay_back();
  assert(strcmp(model.current_node, "ltr:1:8:A") == 0);
  assert(model.item_count == 4U);
  assert(model.items != NULL);
  assert(model.folder_is_collection == 0);
  assert(!zzplay_browse_model_can_play_all(&model));

  zzplay_browse_model_free(&model);
  return 0;
}

/* The Amiberry dead frame: a search started while a folder's list was
 * still loading used to push that never-shown view (no rows, the
 * collection flag already set); Back then landed on an empty view with
 * Play all enabled. The search field uses GA_RelVerify, so clicking out
 * of it fires the search with whatever text it holds. */
static int test_search_during_load_pushes_no_dead_frame(void)
{
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseReply reply;
  ZZPlayBrowseItem letters[1];
  ZZPlayBrowseItem artists[2];
  ZZPlayBrowseItem results[1];

  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "plex", "Plex", NULL));
  assert(zzplay_browse_model_select_service(&model, 0, &req));

  memset(letters, 0, sizeof(letters));
  snprintf(letters[0].id, sizeof(letters[0].id), "ltr:1:8:A");
  snprintf(letters[0].title, sizeof(letters[0].title), "A");
  letters[0].kind = ZZPLAY_ITEM_FOLDER;
  letters[0].playable = 0U;

  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = letters;
  reply.item_count = 1U;
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);

  /* Enter the letter, load its artists */
  assert(zzplay_browse_model_activate(&model, 0, &req) ==
         ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  memset(artists, 0, sizeof(artists));
  snprintf(artists[0].id, sizeof(artists[0].id), "art:1");
  snprintf(artists[0].title, sizeof(artists[0].title), "ABBA");
  artists[0].kind = ZZPLAY_ITEM_FOLDER;
  artists[0].playable = 1U;
  snprintf(artists[1].id, sizeof(artists[1].id), "art:2");
  snprintf(artists[1].title, sizeof(artists[1].title), "Ace");
  artists[1].kind = ZZPLAY_ITEM_FOLDER;
  artists[1].playable = 1U;

  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = artists;
  reply.item_count = 2U;
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);

  /* Enter the artist: its list (one album) is requested but has not
   * arrived when the search fires. */
  assert(zzplay_browse_model_activate(&model, 0, &req) ==
         ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  assert(strcmp(req.node_id, "art:1") == 0);
  assert(model.busy == 1);
  assert(model.item_count == 0U);
  assert(model.folder_is_collection == 1);

  assert(zzplay_browse_model_search(&model, "album", &req));
  assert(req.kind == ZZPLAY_BROWSE_REQ_SEARCH);
  assert(model.stack_depth == 2U);

  memset(results, 0, sizeof(results));
  snprintf(results[0].id, sizeof(results[0].id), "alb:9");
  snprintf(results[0].title, sizeof(results[0].title), "Christmas");
  results[0].kind = ZZPLAY_ITEM_FOLDER;
  results[0].playable = 1U;

  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_SEARCH;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = results;
  reply.item_count = 1U;
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);

  /* Leaving the search returns to the letter's artists, not to a dead
   * empty view of the artist. */
  assert(zzplay_browse_model_back(&model, &req));
  assert(strcmp(model.current_node, "ltr:1:8:A") == 0);
  assert(model.item_count == 2U);
  assert(model.folder_is_collection == 0);
  assert(!zzplay_browse_model_can_play_all(&model));

  zzplay_browse_model_free(&model);
  return 0;
}

/* Back steps past a frame whose view never got rows (its list was
 * superseded before anything arrived) instead of stranding the user on
 * an empty view that Play all would accept. */
static int test_back_skips_dead_frames(void)
{
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseReply reply;
  ZZPlayBrowseItem top[1];
  ZZPlayBrowseItem artists[1];

  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "plex", "Plex", NULL));
  assert(zzplay_browse_model_select_service(&model, 0, &req));

  memset(top, 0, sizeof(top));
  snprintf(top[0].id, sizeof(top[0].id), "ltr:1:8:A");
  snprintf(top[0].title, sizeof(top[0].title), "A");
  top[0].kind = ZZPLAY_ITEM_FOLDER;
  top[0].playable = 0U;

  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = top;
  reply.item_count = 1U;
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);

  /* Enter the letter; its artists load */
  assert(zzplay_browse_model_activate(&model, 0, &req) ==
         ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  memset(artists, 0, sizeof(artists));
  snprintf(artists[0].id, sizeof(artists[0].id), "art:1");
  snprintf(artists[0].title, sizeof(artists[0].title), "ABBA");
  artists[0].kind = ZZPLAY_ITEM_FOLDER;
  artists[0].playable = 1U;

  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = artists;
  reply.item_count = 1U;
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);

  /* Enter the artist, then plant its frame (the letter view) as one
   * whose rows were never shown, the way a superseded navigation left
   * it: the frame's items are gone and its count is zero. */
  assert(zzplay_browse_model_activate(&model, 0, &req) ==
         ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  {
    ZZPlayBrowseFrame *dead = &model.stack[model.stack_depth - 1U];
    zzplay_free(dead->items);
    dead->items = NULL;
    dead->item_count = 0U;
    dead->item_capacity = 0U;
    dead->folder_is_collection = 1;
  }
  assert(model.stack_depth == 2U);

  /* Back steps past the dead frame to the top view */
  assert(zzplay_browse_model_back(&model, &req));
  assert(model.stack_depth == 0U);
  assert(strcmp(model.current_node, "") == 0);
  assert(model.item_count == 1U);
  assert(model.folder_is_collection == 0);

  zzplay_browse_model_free(&model);
  return 0;
}

static void set_item(ZZPlayBrowseItem *it, const char *id, const char *title,
                     const char *subtitle, uint32_t kind, uint32_t playable)
{
  memset(it, 0, sizeof(*it));
  snprintf(it->id, sizeof(it->id), "%s", id);
  snprintf(it->title, sizeof(it->title), "%s", title);
  snprintf(it->subtitle, sizeof(it->subtitle), "%s", subtitle);
  it->kind = kind;
  it->playable = playable;
}

static void apply_list(ZZPlayBrowseModel *model, uint32_t gen, uint32_t start,
                       const ZZPlayBrowseItem *items, uint32_t n,
                       uint32_t total, int partial)
{
  ZZPlayBrowseReply reply;

  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = gen;
  reply.success = 1;
  reply.items = items;
  reply.item_count = n;
  reply.start = start;
  reply.has_total = total > 0U;
  reply.total = total;
  reply.is_partial = partial;
  assert(zzplay_browse_model_apply_reply(model, &reply) == 1);
}

/* Cursor keys move the selection and stop at the ends; letters jump to
 * the next row starting with them, in either case, wrapping round. */
static int test_keyboard_selection(void)
{
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseItem rows[6];

  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "radio", "Radio", NULL));
  assert(zzplay_browse_model_select_service(&model, 0, &req));
  assert(zzplay_browse_model_move_selection(&model, 1) == -1);
  assert(zzplay_browse_model_jump_to_letter(&model, 'a') == -1);

  set_item(&rows[0], "1", "Alpha", "", ZZPLAY_ITEM_STATION, 1U);
  set_item(&rows[1], "2", "beta", "", ZZPLAY_ITEM_STATION, 1U);
  set_item(&rows[2], "3", "Bravo", "", ZZPLAY_ITEM_FOLDER, 0U);
  set_item(&rows[3], "4", "Charlie", "", ZZPLAY_ITEM_STATION, 1U);
  set_item(&rows[4], "5", "bob", "", ZZPLAY_ITEM_TRACK, 1U);
  set_item(&rows[5], "6", "\xE9" "cole", "", ZZPLAY_ITEM_TRACK, 1U);
  apply_list(&model, req.generation, 0U, rows, 6U, 0U, 0);

  /* Nothing selected: down picks the first row, up the last. */
  assert(zzplay_browse_model_move_selection(&model, 1) == 0);
  assert(zzplay_browse_model_move_selection(&model, 1) == 1);
  assert(zzplay_browse_model_move_selection(&model, 10) == 5);
  assert(model.selection == 5);
  assert(zzplay_browse_model_move_selection(&model, -2) == 3);
  assert(zzplay_browse_model_move_selection(&model, -100) == 0);
  assert(zzplay_browse_model_move_selection(&model, 0x7FFFFFFF) == 5);
  assert(zzplay_browse_model_move_selection(&model, -0x7FFFFFFF) == 0);
  zzplay_browse_model_set_selection(&model, -1);
  assert(zzplay_browse_model_move_selection(&model, -1) == 5);
  zzplay_browse_model_set_selection(&model, -1);
  assert(zzplay_browse_model_move_selection(&model, 4) == 0);

  /* Letters */
  zzplay_browse_model_set_selection(&model, -1);
  assert(zzplay_browse_model_jump_to_letter(&model, 'b') == 1);
  assert(zzplay_browse_model_jump_to_letter(&model, 'B') == 2);
  assert(zzplay_browse_model_jump_to_letter(&model, 'b') == 4);
  assert(zzplay_browse_model_jump_to_letter(&model, 'b') == 1);
  assert(model.selection == 1);
  assert(zzplay_browse_model_jump_to_letter(&model, 'z') == -1);
  assert(model.selection == 1);
  assert(zzplay_browse_model_jump_to_letter(&model, ' ') == -1);
  assert(zzplay_browse_model_jump_to_letter(&model, 'A') == 0);
  /* The only C row, from itself, is found again. */
  assert(zzplay_browse_model_jump_to_letter(&model, 'c') == 3);
  assert(zzplay_browse_model_jump_to_letter(&model, 'C') == 3);
  /* Latin-1 capitals match their small letters. */
  assert(zzplay_browse_model_jump_to_letter(&model, 0xC9) == 5);

  zzplay_browse_model_free(&model);
  return 0;
}

/* The list keeps 25 entries, newest first; a repeat moves to the top. */
static int test_recent_list_add_dedupe_cap(void)
{
  static ZZPlayBrowseRecent recent;
  ZZPlayBrowseItem it;
  char id[16];
  uint32_t i;
  uint32_t seen;

  zzplay_browse_recent_clear(&recent);
  for (i = 0U; i < 30U; i++) {
    snprintf(id, sizeof(id), "t%lu", (unsigned long)i);
    set_item(&it, id, "Title", "", ZZPLAY_ITEM_TRACK, 1U);
    assert(zzplay_browse_recent_add(&recent, &it) == 1);
  }
  assert(recent.count == ZZPLAY_BROWSE_RECENT_MAX);
  assert(strcmp(recent.items[0].id, "t29") == 0);
  assert(strcmp(recent.items[24].id, "t5") == 0);

  /* A repeat moves to the top instead of appearing twice. */
  set_item(&it, "t10", "Again", "", ZZPLAY_ITEM_TRACK, 1U);
  assert(zzplay_browse_recent_add(&recent, &it) == 1);
  assert(recent.count == ZZPLAY_BROWSE_RECENT_MAX);
  assert(strcmp(recent.items[0].id, "t10") == 0);
  assert(strcmp(recent.items[0].title, "Again") == 0);
  assert(strcmp(recent.items[1].id, "t29") == 0);
  assert(strcmp(recent.items[24].id, "t5") == 0);
  for (i = 0U, seen = 0U; i < recent.count; i++) {
    seen += strcmp(recent.items[i].id, "t10") == 0 ? 1U : 0U;
  }
  assert(seen == 1U);

  /* The same id of another kind is another entry; the oldest drops. */
  set_item(&it, "t29", "Folder", "", ZZPLAY_ITEM_FOLDER, 1U);
  assert(zzplay_browse_recent_add(&recent, &it) == 1);
  assert(recent.count == ZZPLAY_BROWSE_RECENT_MAX);
  assert(recent.items[0].kind == ZZPLAY_ITEM_FOLDER);
  assert(strcmp(recent.items[2].id, "t29") == 0);
  assert(recent.items[2].kind == ZZPLAY_ITEM_TRACK);
  assert(strcmp(recent.items[24].id, "t6") == 0);

  /* Refused: no id, an unknown kind, a control character in the id,
   * the Recently played row itself. */
  set_item(&it, "", "x", "", ZZPLAY_ITEM_TRACK, 1U);
  assert(zzplay_browse_recent_add(&recent, &it) == 0);
  set_item(&it, "k", "x", "", 7U, 1U);
  assert(zzplay_browse_recent_add(&recent, &it) == 0);
  set_item(&it, "a\tb", "x", "", ZZPLAY_ITEM_TRACK, 1U);
  assert(zzplay_browse_recent_add(&recent, &it) == 0);
  set_item(&it, ZZPLAY_BROWSE_RECENT_ID, "x", "", ZZPLAY_ITEM_FOLDER, 0U);
  assert(zzplay_browse_recent_add(&recent, &it) == 0);
  assert(strcmp(recent.items[0].id, "t29") == 0);

  /* Line ends and tabs in the text become spaces; playable is 0 or 1; the
   * favourite flag is kept so Favourite in the list removes it. */
  set_item(&it, "s1", "A\tB\nC", "x\ry", ZZPLAY_ITEM_STATION, 5U);
  it.duration_ms = 1234U;
  it.favourite = 1U;
  assert(zzplay_browse_recent_add(&recent, &it) == 1);
  assert(strcmp(recent.items[0].title, "A B C") == 0);
  assert(strcmp(recent.items[0].subtitle, "x y") == 0);
  assert(recent.items[0].playable == 1U);
  assert(recent.items[0].duration_ms == 0U);
  assert(recent.items[0].favourite == 1U);
  return 0;
}

/* A favourite played from Browse can be removed from Recently played, and
 * the list follows each change after that. */
static int test_recent_favourite_toggle(void)
{
  static const char saved[] = "station\ts1\tRadio One\tPop\t3\n";
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseItem top[1];
  ZZPlayProvider prov;
  char text[128];

  memset(&prov, 0, sizeof(prov));
  prov.size = sizeof(ZZPlayProvider);
  prov.scheme = "radio";
  prov.favourite = fake_fav;
  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "radio", "Radio", &prov));
  assert(zzplay_browse_model_select_service(&model, 0, &req));
  zzplay_browse_model_load_recent(&model, saved, strlen(saved));
  set_item(&top[0], "fav", "Favourites", "", ZZPLAY_ITEM_FOLDER, 0U);
  apply_list(&model, req.generation, 0U, top, 1U, 1U, 0);
  assert(zzplay_browse_model_activate(&model, 0, &req) ==
         ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  assert(zzplay_browse_model_in_recent(&model));
  zzplay_browse_model_set_selection(&model, 0);
  assert(zzplay_browse_model_favourite(&model, &req));
  assert(req.fav_add == 0); /* a favourite: Favourite removes it */
  model.busy = 0;

  /* The removal was saved: the row and the list follow. */
  assert(zzplay_browse_model_favourite_done(&model, "s1", 0) == 1);
  assert(model.items[0].favourite == 0U);
  assert(model.recent.items[0].favourite == 0U);
  (void)zzplay_browse_recent_format(&model.recent, text, sizeof(text));
  assert(strcmp(text, "station\ts1\tRadio One\tPop\t1\n") == 0);
  assert(zzplay_browse_model_favourite(&model, &req));
  assert(req.fav_add == 1);
  zzplay_browse_model_free(&model);
  return 0;
}

/* A folder opened while its parent's rows were still arriving: Back shows
 * those rows and asks for the list again, so the rest can be reached. */
static int test_back_to_list_still_loading(void)
{
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseItem rows[3];

  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "radio", "Radio", NULL));
  assert(zzplay_browse_model_select_service(&model, 0, &req));
  set_item(&rows[0], "country:A", "A", "", ZZPLAY_ITEM_FOLDER, 0U);
  set_item(&rows[1], "country:B", "B", "", ZZPLAY_ITEM_FOLDER, 0U);
  set_item(&rows[2], "country:C", "C", "", ZZPLAY_ITEM_FOLDER, 0U);
  apply_list(&model, req.generation, 0U, rows, 3U, 0U, 1); /* partial */
  assert(model.busy == 1);
  assert(zzplay_browse_model_activate(&model, 0, &req) ==
         ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  assert(zzplay_browse_model_back(&model, &req));
  assert(model.item_count == 3U);
  assert(req.kind == ZZPLAY_BROWSE_REQ_LIST);
  assert(req.node_id[0] == '\0');
  assert(model.busy == 1);
  zzplay_browse_model_free(&model);
  return 0;
}

/* After a favourite change, a list remembered with the old state is not
 * shown again: it is fetched afresh, so Favourite toggles the right way. */
static int test_favourite_change_drops_cached_lists(void)
{
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseItem top[2];
  ZZPlayBrowseItem station[1];
  ZZPlayProvider prov;

  memset(&prov, 0, sizeof(prov));
  prov.size = sizeof(ZZPlayProvider);
  prov.scheme = "radio";
  prov.favourite = fake_fav;
  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "radio", "Radio", &prov));
  assert(zzplay_browse_model_select_service(&model, 0, &req));
  set_item(&top[0], "top", "Top voted", "", ZZPLAY_ITEM_FOLDER, 0U);
  set_item(&top[1], "fav", "Favourites", "", ZZPLAY_ITEM_FOLDER, 0U);
  apply_list(&model, req.generation, 0U, top, 2U, 2U, 0);
  set_item(&station[0], "s1", "Radio One", "", ZZPLAY_ITEM_STATION, 1U);
  station[0].favourite = 1U;

  /* Top voted, then Favourites, both remembered */
  assert(zzplay_browse_model_activate(&model, 0, &req) ==
         ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  apply_list(&model, req.generation, 0U, station, 1U, 1U, 0);
  assert(zzplay_browse_model_back(&model, &req));
  assert(zzplay_browse_model_activate(&model, 1, &req) ==
         ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  apply_list(&model, req.generation, 0U, station, 1U, 1U, 0);

  /* Removed in Favourites */
  zzplay_browse_model_set_selection(&model, 0);
  assert(zzplay_browse_model_favourite(&model, &req));
  assert(req.fav_add == 0);
  model.busy = 0;
  (void)zzplay_browse_model_favourite_done(&model, "s1", 0);
  assert(model.items[0].favourite == 0U);

  /* Top voted is asked for again rather than shown with the old flag */
  assert(zzplay_browse_model_back(&model, &req));
  assert(zzplay_browse_model_activate(&model, 0, &req) ==
         ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  assert(req.kind == ZZPLAY_BROWSE_REQ_LIST);
  zzplay_browse_model_free(&model);
  return 0;
}

/* The saved form reads back what was written; damaged lines are skipped. */
static int test_recent_parse_format(void)
{
  static ZZPlayBrowseRecent recent;
  static ZZPlayBrowseRecent back;
  static char text[8192];
  static char damaged[4096];
  static const char nul_line[] = "track\tnu\0l\ta\tb\t1\n";
  char small[10];
  char longf[160];
  size_t need;
  size_t used;
  uint32_t i;
  ZZPlayBrowseItem it;

  zzplay_browse_recent_clear(&recent);
  set_item(&it, "t:2", "Song", "", ZZPLAY_ITEM_TRACK, 1U);
  assert(zzplay_browse_recent_add(&recent, &it));
  set_item(&it, "alb:1", "Abbey Road", "The Beatles", ZZPLAY_ITEM_FOLDER, 1U);
  assert(zzplay_browse_recent_add(&recent, &it));
  set_item(&it, "s1", "Radio One", "Pop", ZZPLAY_ITEM_STATION, 1U);
  it.favourite = 1U;
  assert(zzplay_browse_recent_add(&recent, &it));

  need = zzplay_browse_recent_format(&recent, NULL, 0U);
  assert(zzplay_browse_recent_format(&recent, text, sizeof(text)) == need);
  assert(strlen(text) == need);
  /* A favourite adds 2 to the last field. */
  assert(strcmp(text, "station\ts1\tRadio One\tPop\t3\n"
                      "folder\talb:1\tAbbey Road\tThe Beatles\t1\n"
                      "track\tt:2\tSong\t\t1\n") == 0);
  assert(zzplay_browse_recent_parse(&back, text, need) == 3U);
  for (i = 0U; i < 3U; i++) {
    assert(strcmp(back.items[i].id, recent.items[i].id) == 0);
    assert(strcmp(back.items[i].title, recent.items[i].title) == 0);
    assert(strcmp(back.items[i].subtitle, recent.items[i].subtitle) == 0);
    assert(back.items[i].kind == recent.items[i].kind);
    assert(back.items[i].playable == recent.items[i].playable);
    assert(back.items[i].favourite == recent.items[i].favourite);
  }

  /* A short buffer gets the start, still terminated. */
  assert(zzplay_browse_recent_format(&recent, small, sizeof(small)) == need);
  assert(strlen(small) == sizeof(small) - 1U);
  assert(memcmp(small, text, sizeof(small) - 1U) == 0);

  /* Damaged lines */
  memset(longf, 'x', sizeof(longf) - 1U);
  longf[sizeof(longf) - 1U] = '\0';
  used = (size_t)snprintf(
      damaged, sizeof(damaged),
      "# a comment\n"
      "\n"
      "track\tgood1\tGood one\tsub\t1\r\n"
      "track\tmissing\tfields\n"
      "track\textra\ta\tb\t1\tzz\n"
      "album\tbadkind\ta\tb\t1\n"
      "track\t\tempty id\tb\t1\n"
      "track\tbadplay\ta\tb\t4\n"
      "track\tbadplay2\ta\tb\t10\n"
      "folder\tgood2\tGood two\t\t0\n"
      "track\tgood1\tRepeat\tsub\t1\n"
      "folder\t\037recent\tsneaky\t\t0\n"
      "track\t%s\tlong id\t\t1\n"
      "station\tgood3\t%s\t\t1",
      longf, longf);
  assert(used < sizeof(damaged) - sizeof(nul_line));
  /* A NUL inside a line damages it too. */
  memmove(damaged + sizeof(nul_line) - 1U, damaged, used);
  memcpy(damaged, nul_line, sizeof(nul_line) - 1U);
  used += sizeof(nul_line) - 1U;

  assert(zzplay_browse_recent_parse(&back, damaged, used) == 3U);
  assert(strcmp(back.items[0].id, "good1") == 0);
  assert(strcmp(back.items[0].title, "Good one") == 0);
  assert(strcmp(back.items[0].subtitle, "sub") == 0);
  assert(back.items[0].kind == ZZPLAY_ITEM_TRACK);
  assert(strcmp(back.items[1].id, "good2") == 0);
  assert(back.items[1].kind == ZZPLAY_ITEM_FOLDER);
  assert(back.items[1].playable == 0U);
  assert(strcmp(back.items[2].id, "good3") == 0);
  assert(strlen(back.items[2].title) == ZZPLAY_PROVIDER_TEXT_MAX - 1U);
  assert(back.items[2].kind == ZZPLAY_ITEM_STATION);

  /* More lines than the list holds: the first 25 count. */
  used = 0U;
  for (i = 0U; i < 30U; i++) {
    used += (size_t)snprintf(damaged + used, sizeof(damaged) - used,
                             "track\tid%lu\tT\t\t1\n", (unsigned long)i);
  }
  assert(zzplay_browse_recent_parse(&back, damaged, used) ==
         ZZPLAY_BROWSE_RECENT_MAX);
  assert(strcmp(back.items[24].id, "id24") == 0);

  /* No file */
  assert(zzplay_browse_recent_parse(&back, NULL, 0U) == 0U);
  assert(back.count == 0U);
  return 0;
}

/* The Recently played folder: first at the top level while the list has
 * entries, opened without a request, its entries playable as usual. */
static int test_recently_played_folder(void)
{
  static const char saved[] =
      "station\ts1\tRadio One\tPop\t1\n"
      "folder\talb:1\tAbbey Road\tThe Beatles\t1\n";
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseReply reply;
  ZZPlayBrowseItem top[2];
  ZZPlayBrowseItem found[1];
  ZZPlayBrowseItem it;
  char uri[64];

  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "plex", "Plex", NULL));
  assert(zzplay_browse_model_select_service(&model, 0, &req));
  assert(req.kind == ZZPLAY_BROWSE_REQ_LIST);
  zzplay_browse_model_load_recent(&model, saved, strlen(saved));
  assert(model.recent.count == 2U);
  /* The top level is still loading: no row yet. */
  assert(model.item_count == 0U);

  set_item(&top[0], "lib:1", "Music", "", ZZPLAY_ITEM_FOLDER, 0U);
  set_item(&top[1], "lib:2", "Films", "", ZZPLAY_ITEM_FOLDER, 0U);
  apply_list(&model, req.generation, 0U, top, 2U, 2U, 0);
  assert(model.item_count == 3U);
  assert(strcmp(model.items[0].id, ZZPLAY_BROWSE_RECENT_ID) == 0);
  assert(strcmp(model.items[0].title, ZZPLAY_BROWSE_RECENT_TITLE) == 0);
  assert(model.items[0].kind == ZZPLAY_ITEM_FOLDER);
  assert(strcmp(model.items[1].id, "lib:1") == 0);
  assert(zzplay_browse_model_service_rows(&model) == 2U);
  assert(model.has_more == 0);
  zzplay_browse_model_set_selection(&model, 0);
  assert(!zzplay_browse_model_can_play(&model));

  /* Open it: shown at once, nothing asked of the service. */
  assert(zzplay_browse_model_activate(&model, 0, &req) ==
         ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  assert(req.kind == ZZPLAY_BROWSE_REQ_NONE);
  assert(zzplay_browse_model_in_recent(&model));
  assert(model.busy == 0);
  assert(model.item_count == 2U);
  assert(strcmp(model.items[0].id, "s1") == 0);
  assert(!zzplay_browse_model_can_play_all(&model));
  assert(zzplay_browse_model_can_reload(&model));

  /* A station plays; a collection is gathered whole. */
  zzplay_browse_model_set_selection(&model, 0);
  assert(zzplay_browse_model_play_target(&model) == ZZPLAY_BROWSE_TARGET_ITEM);
  assert(zzplay_browse_model_format_uri(&model, 0, uri, sizeof(uri), NULL, 0U));
  assert(strcmp(uri, "plex://s1") == 0);
  zzplay_browse_model_set_selection(&model, 1);
  assert(zzplay_browse_model_play_target(&model) ==
         ZZPLAY_BROWSE_TARGET_COLLECTION);
  assert(zzplay_browse_model_gather(&model, 100U, &req));
  assert(req.kind == ZZPLAY_BROWSE_REQ_GATHER);
  assert(strcmp(req.node_id, "alb:1") == 0);
  model.busy = 0; /* the gather's reply arrived */

  /* Something started moves to the top of the list; the open view
   * stays as it is until it is reloaded. */
  set_item(&it, "t9", "Song", "", ZZPLAY_ITEM_TRACK, 1U);
  assert(zzplay_browse_model_note_started(&model, &it) == 1);
  assert(model.recent.count == 3U);
  assert(model.item_count == 2U);
  assert(zzplay_browse_model_reload(&model, &req));
  assert(req.kind == ZZPLAY_BROWSE_REQ_NONE);
  assert(model.item_count == 3U);
  assert(strcmp(model.items[0].id, "t9") == 0);

  /* Back to the top level, the row still first. */
  assert(zzplay_browse_model_back(&model, &req));
  assert(model.current_node[0] == '\0');
  assert(model.item_count == 3U);
  assert(strcmp(model.items[0].id, ZZPLAY_BROWSE_RECENT_ID) == 0);
  assert(model.selection == 0);

  /* Search results never show it. */
  assert(zzplay_browse_model_search(&model, "song", &req));
  set_item(&found[0], "t9", "Song", "", ZZPLAY_ITEM_TRACK, 1U);
  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_SEARCH;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = found;
  reply.item_count = 1U;
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);
  assert(model.item_count == 1U);
  assert(strcmp(model.items[0].id, "t9") == 0);
  assert(zzplay_browse_model_back(&model, &req));
  assert(model.item_count == 3U);

  /* An empty list hides the row; the selection stays on its row. */
  zzplay_browse_model_set_selection(&model, 2);
  zzplay_browse_model_load_recent(&model, NULL, 0U);
  assert(model.item_count == 2U);
  assert(model.selection == 1);
  assert(strcmp(model.items[1].id, "lib:2") == 0);
  /* Starting something brings it back. */
  assert(zzplay_browse_model_note_started(&model, &it) == 1);
  assert(model.item_count == 3U);
  assert(model.selection == 2);
  assert(strcmp(model.items[0].id, ZZPLAY_BROWSE_RECENT_ID) == 0);

  /* Selecting the service again drops the list until the caller loads
   * the service's own. */
  assert(zzplay_browse_model_select_service(&model, 0, &req));
  assert(req.kind == ZZPLAY_BROWSE_REQ_NONE);
  assert(model.recent.count == 0U);
  assert(model.item_count == 2U);
  assert(strcmp(model.items[0].id, "lib:1") == 0);

  zzplay_browse_model_free(&model);
  return 0;
}

/* With the row shown, paging and partial rows still count service rows
 * only; a failed or stopped top level still offers the folder. */
static int test_recent_row_with_paging(void)
{
  static const char saved[] = "track\tt1\tOne\t\t1\n";
  static ZZPlayBrowseItem page[ZZPLAY_BROWSE_PAGE_SIZE];
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseReply reply;
  uint32_t i;

  for (i = 0U; i < ZZPLAY_BROWSE_PAGE_SIZE; i++) {
    char id[16];
    snprintf(id, sizeof(id), "r%lu", (unsigned long)i);
    set_item(&page[i], id, "Row", "", ZZPLAY_ITEM_FOLDER, 0U);
  }

  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "radio", "Radio", NULL));
  assert(zzplay_browse_model_select_service(&model, 0, &req));
  zzplay_browse_model_load_recent(&model, saved, strlen(saved));
  apply_list(&model, req.generation, 0U, page, ZZPLAY_BROWSE_PAGE_SIZE, 150U,
             0);
  assert(model.item_count == ZZPLAY_BROWSE_PAGE_SIZE + 1U);
  assert(zzplay_browse_model_service_rows(&model) == ZZPLAY_BROWSE_PAGE_SIZE);
  assert(model.has_more == 1);

  assert(zzplay_browse_model_request_more(&model, &req));
  assert(req.start == ZZPLAY_BROWSE_PAGE_SIZE);
  for (i = 0U; i < 50U; i++) {
    char id[16];
    snprintf(id, sizeof(id), "r%lu", (unsigned long)(100U + i));
    set_item(&page[i], id, "Row", "", ZZPLAY_ITEM_FOLDER, 0U);
  }
  apply_list(&model, req.generation, 100U, page, 50U, 150U, 0);
  assert(model.item_count == 151U);
  assert(zzplay_browse_model_service_rows(&model) == 150U);
  assert(model.has_more == 0);
  assert(strcmp(model.items[0].id, ZZPLAY_BROWSE_RECENT_ID) == 0);
  assert(strcmp(model.items[100].id, "r99") == 0);
  assert(strcmp(model.items[101].id, "r100") == 0);

  /* A reload's first group replaces the rows, not the folder row. */
  for (i = 0U; i < ZZPLAY_BROWSE_PAGE_SIZE; i++) {
    char id[16];
    snprintf(id, sizeof(id), "n%lu", (unsigned long)i);
    set_item(&page[i], id, "Row", "", ZZPLAY_ITEM_FOLDER, 0U);
  }
  assert(zzplay_browse_model_reload(&model, &req));
  apply_list(&model, req.generation, 0U, page, 10U, 0U, 1);
  assert(model.item_count == 11U);
  assert(strcmp(model.items[0].id, ZZPLAY_BROWSE_RECENT_ID) == 0);
  assert(strcmp(model.items[1].id, "n0") == 0);
  apply_list(&model, req.generation, 0U, page, 20U, 0U, 0);
  assert(model.item_count == 21U);
  assert(strcmp(model.items[20].id, "n19") == 0);
  zzplay_browse_model_free(&model);

  /* The service fails: the folder is still offered. */
  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "radio", "Radio", NULL));
  assert(zzplay_browse_model_select_service(&model, 0, &req));
  zzplay_browse_model_load_recent(&model, saved, strlen(saved));
  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = req.generation;
  reply.success = 0;
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);
  assert(model.item_count == 1U);
  assert(strcmp(model.items[0].id, ZZPLAY_BROWSE_RECENT_ID) == 0);
  zzplay_browse_model_free(&model);

  /* Stopped before any row came: likewise. */
  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "radio", "Radio", NULL));
  assert(zzplay_browse_model_select_service(&model, 0, &req));
  zzplay_browse_model_load_recent(&model, saved, strlen(saved));
  zzplay_browse_model_stop(&model);
  assert(model.item_count == 1U);
  zzplay_browse_model_free(&model);
  return 0;
}

/* Play all on a shown collection records it with its subtitle. */
static int test_shown_collection(void)
{
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseItem top[1];
  ZZPlayBrowseItem tracks[1];
  ZZPlayBrowseItem found[1];
  ZZPlayBrowseItem shown;
  ZZPlayBrowseReply reply;

  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "plex", "Plex", NULL));
  assert(zzplay_browse_model_select_service(&model, 0, &req));
  set_item(&top[0], "alb:7", "Revolver", "The Beatles", ZZPLAY_ITEM_FOLDER,
           1U);
  apply_list(&model, req.generation, 0U, top, 1U, 0U, 0);
  assert(!zzplay_browse_model_shown_collection(&model, &shown));

  assert(zzplay_browse_model_activate(&model, 0, &req) ==
         ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  set_item(&tracks[0], "t1", "Taxman", "", ZZPLAY_ITEM_TRACK, 1U);
  apply_list(&model, req.generation, 0U, tracks, 1U, 0U, 0);
  assert(zzplay_browse_model_shown_collection(&model, &shown));
  assert(strcmp(shown.id, "alb:7") == 0);
  assert(strcmp(shown.title, "Revolver") == 0);
  assert(strcmp(shown.subtitle, "The Beatles") == 0);
  assert(shown.kind == ZZPLAY_ITEM_FOLDER && shown.playable == 1U);

  /* A search is no collection; Back brings the folder's details back. */
  assert(zzplay_browse_model_search(&model, "tax", &req));
  set_item(&found[0], "t1", "Taxman", "", ZZPLAY_ITEM_TRACK, 1U);
  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_SEARCH;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = found;
  reply.item_count = 1U;
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);
  assert(!zzplay_browse_model_shown_collection(&model, &shown));
  assert(zzplay_browse_model_back(&model, &req));
  assert(zzplay_browse_model_shown_collection(&model, &shown));
  assert(strcmp(shown.subtitle, "The Beatles") == 0);

  zzplay_browse_model_free(&model);
  return 0;
}

/* A gather's replies count only while it is the latest request. */
static int test_gather_liveness(void)
{
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseItem top[2];
  uint32_t g;

  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "plex", "Plex", NULL));
  assert(zzplay_browse_model_add_service(&model, "radio", "Radio", NULL));
  assert(zzplay_browse_model_select_service(&model, 0, &req));
  set_item(&top[0], "alb:1", "Album", "", ZZPLAY_ITEM_FOLDER, 1U);
  set_item(&top[1], "lib:2", "Library", "", ZZPLAY_ITEM_FOLDER, 0U);
  apply_list(&model, req.generation, 0U, top, 2U, 0U, 0);

  /* Live until its final reply is handled. */
  zzplay_browse_model_set_selection(&model, 0);
  assert(zzplay_browse_model_gather(&model, 100U, &req));
  g = req.generation;
  assert(zzplay_browse_model_gather_live(&model, g));
  assert(!zzplay_browse_model_gather_live(&model, g - 1U));
  zzplay_browse_model_gather_done(&model);
  assert(!zzplay_browse_model_gather_live(&model, g));
  assert(model.busy == 0);

  /* Opening another folder supersedes it; its end leaves the folder's
   * request busy. */
  assert(zzplay_browse_model_gather(&model, 100U, &req));
  g = req.generation;
  assert(zzplay_browse_model_activate(&model, 1, &req) ==
         ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  assert(!zzplay_browse_model_gather_live(&model, g));
  zzplay_browse_model_gather_done(&model);
  assert(model.busy == 1);
  assert(zzplay_browse_model_back(&model, &req));

  /* Back, a search, Reload, Stop, a service switch and a newer gather
   * supersede it too. */
  zzplay_browse_model_set_selection(&model, 0);
  assert(zzplay_browse_model_gather(&model, 100U, &req));
  g = req.generation;
  assert(zzplay_browse_model_activate(&model, 1, &req) ==
         ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  assert(zzplay_browse_model_back(&model, &req));
  assert(!zzplay_browse_model_gather_live(&model, g));

  zzplay_browse_model_set_selection(&model, 0);
  assert(zzplay_browse_model_gather(&model, 100U, &req));
  g = req.generation;
  assert(zzplay_browse_model_search(&model, "x", &req));
  assert(!zzplay_browse_model_gather_live(&model, g));
  assert(zzplay_browse_model_back(&model, &req));

  zzplay_browse_model_set_selection(&model, 0);
  assert(zzplay_browse_model_gather(&model, 100U, &req));
  g = req.generation;
  model.busy = 0; /* Reload waits for an idle view */
  assert(zzplay_browse_model_reload(&model, &req));
  assert(!zzplay_browse_model_gather_live(&model, g));

  assert(zzplay_browse_model_gather(&model, 100U, &req));
  g = req.generation;
  zzplay_browse_model_stop(&model);
  assert(!zzplay_browse_model_gather_live(&model, g));
  assert(model.gathering == 0);

  assert(zzplay_browse_model_gather(&model, 100U, &req));
  g = req.generation;
  assert(zzplay_browse_model_gather(&model, 100U, &req));
  assert(!zzplay_browse_model_gather_live(&model, g));
  assert(zzplay_browse_model_gather_live(&model, req.generation));
  g = req.generation;
  assert(zzplay_browse_model_select_service(&model, 1, &req));
  assert(!zzplay_browse_model_gather_live(&model, g));

  zzplay_browse_model_free(&model);
  return 0;
}

/* Saved settings drop the service's remembered lists and reopen it. */
static int test_reopen_service(void)
{
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseItem top[1];
  ZZPlayBrowseItem inner[1];

  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "plex", "Plex", NULL));
  assert(zzplay_browse_model_add_service(&model, "radio", "Radio", NULL));
  /* Radio's top level is remembered. */
  assert(zzplay_browse_model_select_service(&model, 1, &req));
  set_item(&top[0], "st", "Stations", "", ZZPLAY_ITEM_FOLDER, 0U);
  apply_list(&model, req.generation, 0U, top, 1U, 0U, 0);

  assert(zzplay_browse_model_select_service(&model, 0, &req));
  assert(req.reopen == 0);
  set_item(&top[0], "lib:1", "Music", "", ZZPLAY_ITEM_FOLDER, 0U);
  apply_list(&model, req.generation, 0U, top, 1U, 0U, 0);
  assert(zzplay_browse_model_activate(&model, 0, &req) ==
         ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  set_item(&inner[0], "t1", "Track", "", ZZPLAY_ITEM_TRACK, 1U);
  apply_list(&model, req.generation, 0U, inner, 1U, 0U, 0);
  assert(zzplay_browse_model_back(&model, &req));
  assert(zzplay_browse_model_select_service(&model, 0, &req));
  assert(req.kind == ZZPLAY_BROWSE_REQ_NONE);

  /* Reopen: a real request that reopens the module, nothing remembered. */
  assert(zzplay_browse_model_reopen_service(&model, &req));
  assert(req.kind == ZZPLAY_BROWSE_REQ_LIST);
  assert(req.reopen == 1);
  assert(req.node_id[0] == '\0' && req.start == 0U);
  assert(strcmp(req.scheme, "plex") == 0);
  assert(model.busy == 1);
  apply_list(&model, req.generation, 0U, top, 1U, 0U, 0);
  assert(zzplay_browse_model_activate(&model, 0, &req) ==
         ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  assert(req.kind == ZZPLAY_BROWSE_REQ_LIST);
  assert(req.reopen == 0);

  /* Other services keep theirs. */
  assert(zzplay_browse_model_select_service(&model, 1, &req));
  assert(req.kind == ZZPLAY_BROWSE_REQ_NONE);

  zzplay_browse_model_free(&model);
  return 0;
}

typedef struct FakeWriter {
  int fail;
  int calls;
  char text[1024];
  uint32_t length;
} FakeWriter;

static int fake_write(void *user, const char *text, uint32_t length)
{
  FakeWriter *w = (FakeWriter *)user;

  w->calls++;
  if (w->fail || length >= sizeof(w->text)) {
    return 0;
  }
  memcpy(w->text, text, length);
  w->text[length] = '\0';
  w->length = length;
  return 1;
}

/* A failed save is reported once; the list stays, and the next save
 * writes all of it. */
static int test_recent_save_failure(void)
{
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseItem it;
  FakeWriter w;
  int saved;

  memset(&w, 0, sizeof(w));
  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "radio", "Radio", NULL));
  assert(zzplay_browse_model_select_service(&model, 0, &req));

  w.fail = 1;
  set_item(&it, "a", "First", "", ZZPLAY_ITEM_STATION, 1U);
  assert(zzplay_browse_model_note_started(&model, &it));
  saved = zzplay_browse_recent_save(&model.recent, fake_write, &w);
  assert(saved == 0 && w.calls == 1);
  assert(zzplay_browse_model_note_recent_save(&model, saved) == 1);

  set_item(&it, "b", "Second", "", ZZPLAY_ITEM_STATION, 1U);
  assert(zzplay_browse_model_note_started(&model, &it));
  saved = zzplay_browse_recent_save(&model.recent, fake_write, &w);
  assert(saved == 0);
  /* Already reported */
  assert(zzplay_browse_model_note_recent_save(&model, saved) == 0);
  assert(model.recent.count == 2U);

  /* The writer works again: everything is written. */
  w.fail = 0;
  set_item(&it, "c", "Third", "", ZZPLAY_ITEM_STATION, 1U);
  assert(zzplay_browse_model_note_started(&model, &it));
  saved = zzplay_browse_recent_save(&model.recent, fake_write, &w);
  assert(saved == 1);
  assert(zzplay_browse_model_note_recent_save(&model, saved) == 0);
  assert(strcmp(w.text, "station\tc\tThird\t\t1\n"
                        "station\tb\tSecond\t\t1\n"
                        "station\ta\tFirst\t\t1\n") == 0);

  /* A new failure after a success is reported again. */
  w.fail = 1;
  saved = zzplay_browse_recent_save(&model.recent, fake_write, &w);
  assert(zzplay_browse_model_note_recent_save(&model, saved) == 1);
  assert(zzplay_browse_recent_save(&model.recent, NULL, &w) == 0);

  zzplay_browse_model_free(&model);
  return 0;
}


/* View row count text: counts what is shown ("1 item", "6 items", or
 * "X of Y items"). */
static int test_format_count(void)
{
  static const char saved[] = "folder\talb:1\tAlbum\tArtist\t1\n";
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseItem items[6];
  char status[64];
  uint32_t i;

  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_format_count(&model, status, sizeof(status)) == 0U);
  assert(status[0] == '\0');

  assert(zzplay_browse_model_add_service(&model, "plex", "Plex", NULL));
  assert(zzplay_browse_model_select_service(&model, 0, &req));

  /* 1 item: singular */
  set_item(&items[0], "1", "One", "", ZZPLAY_ITEM_TRACK, 1U);
  apply_list(&model, req.generation, 0U, items, 1U, 0U, 0);
  assert(zzplay_browse_model_format_count(&model, status, sizeof(status)) > 0U);
  assert(strcmp(status, "1 item") == 0);

  /* 2 items: plural */
  set_item(&items[1], "2", "Two", "", ZZPLAY_ITEM_TRACK, 1U);
  apply_list(&model, req.generation, 0U, items, 2U, 0U, 0);
  assert(zzplay_browse_model_format_count(&model, status, sizeof(status)) > 0U);
  assert(strcmp(status, "2 items") == 0);

  /* Total larger than loaded rows: "X of Y items" */
  apply_list(&model, req.generation, 0U, items, 2U, 10U, 0);
  assert(zzplay_browse_model_format_count(&model, status, sizeof(status)) > 0U);
  assert(strcmp(status, "2 of 10 items") == 0);
  /* Top level with 5 service rows and Recently played folder shown: */
  zzplay_browse_model_free(&model);
  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "plex", "Plex", NULL));
  assert(zzplay_browse_model_select_service(&model, 0, &req));
  zzplay_browse_model_load_recent(&model, saved, strlen(saved));
  for (i = 0U; i < 5U; i++) {
    char id[16];
    snprintf(id, sizeof(id), "lib:%lu", (unsigned long)i);
    set_item(&items[i], id, "Library", "", ZZPLAY_ITEM_FOLDER, 0U);
  }
  apply_list(&model, req.generation, 0U, items, 5U, 5U, 0);
  assert(model.item_count == 6U);
  assert(strcmp(model.items[0].id, ZZPLAY_BROWSE_RECENT_ID) == 0);
  assert(zzplay_browse_model_format_count(&model, status, sizeof(status)) > 0U);
  assert(strcmp(status, "6 items") == 0);

  /* (b) Opening Recently played (1 album row) shows "1 item". */
  assert(zzplay_browse_model_activate(&model, 0, &req) ==
         ZZPLAY_BROWSE_ACTION_ENTER_FOLDER);
  assert(zzplay_browse_model_in_recent(&model));
  assert(model.item_count == 1U);
  assert(zzplay_browse_model_format_count(&model, status, sizeof(status)) > 0U);
  assert(strcmp(status, "1 item") == 0);

  /* (a) Backspace back to top level shows "6 items" again. */
  assert(zzplay_browse_model_back(&model, &req));
  assert(model.item_count == 6U);
  assert(zzplay_browse_model_format_count(&model, status, sizeof(status)) > 0U);
  assert(strcmp(status, "6 items") == 0);

  zzplay_browse_model_free(&model);
  return 0;
}

int main(void)
{
  assert(test_enter_folder_and_back() == 0);
  assert(test_page_two_appends() == 0);
  assert(test_cached_folder_keeps_more() == 0);
  assert(test_new_search_drops_previous_query() == 0);
  assert(test_reply_after_stop_dropped() == 0);
  assert(test_play_add_unavailable_for_folders_and_unplayable() == 0);
  assert(test_gather_request_for_collection() == 0);
  assert(test_activation_folder_enters_station_plays() == 0);
  assert(test_settings_row_opens_settings() == 0);
  assert(test_favourite_action() == 0);
  assert(test_play_all_add_all_on_shown_folder() == 0);
  assert(test_back_after_play_all_shows_letter_rows() == 0);
  assert(test_remembered_folder_reopens_without_request() == 0);
  assert(test_reload_sends_request_and_replaces_copy() == 0);
  assert(test_partial_replies_append_and_stale_dropped() == 0);
  assert(test_stop_keeps_arrived_rows() == 0);
  assert(test_cache_bound_drops_oldest() == 0);
  assert(test_replay_click_during_letter_load() == 0);
  assert(test_search_during_load_pushes_no_dead_frame() == 0);
  assert(test_back_skips_dead_frames() == 0);
  assert(test_keyboard_selection() == 0);
  assert(test_recent_list_add_dedupe_cap() == 0);
  assert(test_recent_favourite_toggle() == 0);
  assert(test_back_to_list_still_loading() == 0);
  assert(test_recent_parse_format() == 0);
  assert(test_recently_played_folder() == 0);
  assert(test_recent_row_with_paging() == 0);
  assert(test_favourite_change_drops_cached_lists() == 0);
  assert(test_shown_collection() == 0);
  assert(test_gather_liveness() == 0);
  assert(test_reopen_service() == 0);
  assert(test_recent_save_failure() == 0);
  assert(test_format_count() == 0);
  printf("zzplay_browse_test: all tests passed\n");
  return 0;
}
