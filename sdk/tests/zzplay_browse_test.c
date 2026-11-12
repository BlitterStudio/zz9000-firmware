/*
 * Tests for ZZPlay service browser model.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../tools/zzplay/zzplay-browse-model.h"

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


int main(void)
{
  assert(test_enter_folder_and_back() == 0);
  assert(test_page_two_appends() == 0);
  assert(test_new_search_drops_previous_query() == 0);
  assert(test_reply_after_stop_dropped() == 0);
  assert(test_play_add_unavailable_for_folders_and_unplayable() == 0);
  assert(test_gather_request_for_collection() == 0);
  assert(test_play_all_add_all_on_shown_folder() == 0);
  assert(test_favourite_action() == 0);
  assert(test_remembered_folder_reopens_without_request() == 0);
  assert(test_reload_sends_request_and_replaces_copy() == 0);
  assert(test_partial_replies_append_and_stale_dropped() == 0);
  assert(test_stop_keeps_arrived_rows() == 0);
  assert(test_cache_bound_drops_oldest() == 0);
  printf("zzplay_browse_test: all tests passed\n");
  return 0;
}
