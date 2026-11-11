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
  ZZPlayBrowseItem items[4];

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

  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = items;
  reply.item_count = 4U;

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

  /* Select playable track */
  zzplay_browse_model_set_selection(&model, 2);
  assert(zzplay_browse_model_can_play(&model));
  assert(zzplay_browse_model_can_add(&model));

  /* Select playable station */
  zzplay_browse_model_set_selection(&model, 3);
  assert(zzplay_browse_model_can_play(&model));
  assert(zzplay_browse_model_can_add(&model));

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


int main(void)
{
  assert(test_enter_folder_and_back() == 0);
  assert(test_page_two_appends() == 0);
  assert(test_new_search_drops_previous_query() == 0);
  assert(test_reply_after_stop_dropped() == 0);
  assert(test_play_add_unavailable_for_folders_and_unplayable() == 0);
  assert(test_activation_folder_enters_station_plays() == 0);
  assert(test_favourite_action() == 0);
  printf("zzplay_browse_test: all tests passed\n");
  return 0;
}
