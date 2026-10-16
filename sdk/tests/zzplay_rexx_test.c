/* Behavioural tests for the ARexx command language
 * (zzplay-rexx-commands.h).
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <stdio.h>
#include <string.h>

#include "../tools/zzplay/zzplay-rexx-commands.h"

static int failures;

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__,  \
              #cond);                                                   \
      failures++;                                                       \
    }                                                                   \
  } while (0)

typedef struct Fixture {
  ZZPlayController ctl;
  ZZPlayPlaylist playlist;
  ZZPlayRexxHost host;
  ZZPlayRexxReply reply;
  char item_path[ZZPLAY_PLAYLIST_PATH_MAX];
  char saved_path[ZZPLAY_PLAYLIST_PATH_MAX];
  char added[4][ZZPLAY_PLAYLIST_PATH_MAX];
  uint32_t add_calls;
  int quit_blocked;
  int window_open;
} Fixture;

/* A path naming a drawer "Empty:" adds nothing; any other path adds one
 * entry, as a single file would. */
static uint32_t fake_add_path(void *user, ZZPlayPlaylist *list,
                              const char *path)
{
  Fixture *f = (Fixture *)user;

  if (f->add_calls < 4U) {
    strncpy(f->added[f->add_calls], path, ZZPLAY_PLAYLIST_PATH_MAX - 1U);
  }
  f->add_calls++;
  if (strncmp(path, "Empty:", 6) == 0) {
    return 0U;
  }
  return zzplay_playlist_add(list, path) >= 0 ? 1U : 0U;
}

static int fake_save(void *user, const ZZPlayPlaylist *list, const char *path)
{
  Fixture *f = (Fixture *)user;

  (void)list;
  strncpy(f->saved_path, path, sizeof(f->saved_path) - 1U);
  return strncmp(path, "RAM:", 4) == 0;
}

static int fake_quit_blocked(void *user)
{
  return ((Fixture *)user)->quit_blocked;
}

static int fake_show(void *user)
{
  return ((Fixture *)user)->window_open;
}

static void fixture_init(Fixture *f, uint32_t entries)
{
  static const char *const paths[] = { "Work:a.mp3", "Work:b.mp3",
                                       "Work:c.mp3" };
  uint32_t i;

  memset(f, 0, sizeof(*f));
  zzplay_controller_init(&f->ctl, 50U, ZZPLAY_REPEAT_OFF, 0);
  zzplay_playlist_init(&f->playlist);
  for (i = 0U; i < entries && i < 3U; i++) {
    (void)zzplay_playlist_add(&f->playlist, paths[i]);
  }
  f->host.ctl = &f->ctl;
  f->host.playlist = &f->playlist;
  f->host.item_path = f->item_path;
  f->host.version = "ZZPlay 0.6 (07.10.2026)";
  f->host.user = f;
  f->host.add_path = fake_add_path;
  f->host.save_playlist = fake_save;
  f->host.quit_blocked = fake_quit_blocked;
  f->host.show = fake_show;
}

static void fixture_free(Fixture *f)
{
  zzplay_playlist_free(&f->playlist);
}

/* Run one command; returns its rc. */
static int run(Fixture *f, const char *command)
{
  char line[ZZPLAY_REXX_LINE_MAX];

  strncpy(line, command, sizeof(line) - 1U);
  line[sizeof(line) - 1U] = '\0';
  zzplay_rexx_execute(&f->host, line, &f->reply);
  return f->reply.rc;
}

static int result_is(const Fixture *f, const char *text)
{
  return f->reply.has_result && strcmp(f->reply.result, text) == 0;
}

/* An item is playing: what the engine reports after item_begin. */
static void start_item(Fixture *f, int seekable, uint32_t elapsed_ms,
                       uint32_t total_ms)
{
  zzplay_controller_item_begin(&f->ctl);
  zzplay_controller_set_capabilities(&f->ctl, seekable, 1, 0);
  zzplay_controller_set_position(&f->ctl, elapsed_ms, 1);
  zzplay_controller_set_duration(&f->ctl, total_ms);
}

static void test_parsing(void)
{
  Fixture f;

  fixture_init(&f, 0U);
  /* A quoted path keeps its blanks; commands ignore case. */
  CHECK(run(&f, "add \"Work:My Music/a b.mp3\" Work:c.mp3") ==
        ZZPLAY_REXX_RC_OK);
  CHECK(strcmp(f.added[0], "Work:My Music/a b.mp3") == 0);
  CHECK(strcmp(f.added[1], "Work:c.mp3") == 0);
  CHECK(f.playlist.count == 2U);
  CHECK(result_is(&f, "2"));

  /* A malformed line runs nothing at all. */
  f.add_calls = 0U;
  CHECK(run(&f, "ADD Work:x.mp3 \"Work:unterminated") ==
        ZZPLAY_REXX_RC_ERROR);
  CHECK(run(&f, "ADD \"Work:a\"b") == ZZPLAY_REXX_RC_ERROR);
  CHECK(f.add_calls == 0U);
  CHECK(f.playlist.count == 2U);

  CHECK(run(&f, "") == ZZPLAY_REXX_RC_ERROR);
  CHECK(run(&f, "REWIND") == ZZPLAY_REXX_RC_ERROR);
  /* A prefix of a command is not that command. */
  CHECK(run(&f, "PLAYP") == ZZPLAY_REXX_RC_ERROR);
  /* Unexpected arguments are refused, not ignored. */
  CHECK(run(&f, "STOP now") == ZZPLAY_REXX_RC_ERROR);
  CHECK(f.ctl.request == ZZPLAY_REQUEST_NONE);
  CHECK(run(&f, "SEEK") == ZZPLAY_REXX_RC_ERROR);

  /* Only commands that read the file system wait for the gate. */
  CHECK(zzplay_rexx_needs_gate("  add Work:x"));
  CHECK(zzplay_rexx_needs_gate("OPEN Work:x"));
  CHECK(!zzplay_rexx_needs_gate("ADDX"));
  CHECK(!zzplay_rexx_needs_gate("PLAY 1"));
  CHECK(!zzplay_rexx_needs_gate("\"ADD\"x"));
  /* A quoted command word runs as that command, so it is gated too. */
  CHECK(zzplay_rexx_needs_gate("\"ADD\" Work:x"));
  CHECK(zzplay_rexx_needs_gate("\t\"open\""));
  /* A quoted word with blanks inside is not a command, gated or not. */
  f.add_calls = 0U;
  CHECK(!zzplay_rexx_needs_gate("\"ADD \" Work:x"));
  CHECK(run(&f, "\"ADD \" Work:x") == ZZPLAY_REXX_RC_ERROR);
  CHECK(f.add_calls == 0U);
  fixture_free(&f);
}

static void test_play_and_pause(void)
{
  Fixture f;

  fixture_init(&f, 0U);
  /* Nothing to play. */
  CHECK(run(&f, "PLAY") == ZZPLAY_REXX_RC_WARN);
  CHECK(run(&f, "PLAYPAUSE") == ZZPLAY_REXX_RC_WARN);
  CHECK(f.ctl.request == ZZPLAY_REQUEST_NONE);
  fixture_free(&f);

  fixture_init(&f, 3U);
  CHECK(run(&f, "PLAY") == ZZPLAY_REXX_RC_OK);
  CHECK(f.ctl.request == ZZPLAY_REQUEST_PLAY);
  f.ctl.request = ZZPLAY_REQUEST_NONE;

  /* Positions are 1-based and must exist. */
  CHECK(run(&f, "PLAY 2") == ZZPLAY_REXX_RC_OK);
  CHECK(f.ctl.request == ZZPLAY_REQUEST_JUMP && f.ctl.jump_index == 1);
  f.ctl.request = ZZPLAY_REQUEST_NONE;
  CHECK(run(&f, "PLAY 0") == ZZPLAY_REXX_RC_ERROR);
  CHECK(run(&f, "PLAY 4") == ZZPLAY_REXX_RC_ERROR);
  CHECK(run(&f, "PLAY -1") == ZZPLAY_REXX_RC_ERROR);
  CHECK(f.ctl.request == ZZPLAY_REQUEST_NONE);

  /* Pause and resume need an item and are idempotent. */
  CHECK(run(&f, "PAUSE") == ZZPLAY_REXX_RC_WARN);
  CHECK(run(&f, "RESUME") == ZZPLAY_REXX_RC_WARN);
  start_item(&f, 1, 0U, 0U);
  CHECK(run(&f, "STATUS") == ZZPLAY_REXX_RC_OK && result_is(&f, "PLAYING"));
  CHECK(run(&f, "PAUSE") == ZZPLAY_REXX_RC_OK && f.ctl.paused);
  CHECK(run(&f, "PAUSE") == ZZPLAY_REXX_RC_OK && f.ctl.paused);
  CHECK(run(&f, "STATUS") == ZZPLAY_REXX_RC_OK && result_is(&f, "PAUSED"));
  /* PLAY on a paused item resumes it rather than restarting it. */
  CHECK(run(&f, "PLAY") == ZZPLAY_REXX_RC_OK && !f.ctl.paused);
  CHECK(f.ctl.request == ZZPLAY_REQUEST_NONE);
  CHECK(run(&f, "RESUME") == ZZPLAY_REXX_RC_OK && !f.ctl.paused);
  CHECK(run(&f, "PLAYPAUSE") == ZZPLAY_REXX_RC_OK && f.ctl.paused);
  CHECK(run(&f, "PLAYPAUSE") == ZZPLAY_REXX_RC_OK && !f.ctl.paused);

  /* While the gate owns the pause, a script cannot undo it. */
  CHECK(zzplay_controller_begin_modal(&f.ctl, ZZPLAY_MODAL_ADD_DROPPED));
  CHECK(run(&f, "RESUME") == ZZPLAY_REXX_RC_WARN && f.ctl.paused);
  zzplay_controller_end_modal(&f.ctl);

  CHECK(run(&f, "STOP") == ZZPLAY_REXX_RC_OK);
  CHECK(f.ctl.request == ZZPLAY_REQUEST_STOP);
  /* A pending stop outranks a later next. */
  CHECK(run(&f, "NEXT") == ZZPLAY_REXX_RC_WARN);
  CHECK(f.ctl.request == ZZPLAY_REQUEST_STOP);
  fixture_free(&f);
}

static void test_seek(void)
{
  Fixture f;

  fixture_init(&f, 1U);
  CHECK(run(&f, "SEEK 10") == ZZPLAY_REXX_RC_WARN);  /* nothing playing */
  start_item(&f, 0, 5000U, 60000U);
  CHECK(run(&f, "SEEK 10") == ZZPLAY_REXX_RC_WARN);  /* not seekable */
  zzplay_controller_item_end(&f.ctl);

  start_item(&f, 1, 30500U, 180000U);
  CHECK(run(&f, "SEEK 90") == ZZPLAY_REXX_RC_OK);
  CHECK(f.ctl.request == ZZPLAY_REQUEST_SEEK && f.ctl.seek_ms == 90000U);
  f.ctl.request = ZZPLAY_REQUEST_NONE;
  CHECK(run(&f, "SEEK +10") == ZZPLAY_REXX_RC_OK);
  CHECK(f.ctl.seek_ms == 40500U);
  f.ctl.request = ZZPLAY_REQUEST_NONE;
  CHECK(run(&f, "SEEK -10") == ZZPLAY_REXX_RC_OK);
  CHECK(f.ctl.seek_ms == 20500U);
  f.ctl.request = ZZPLAY_REQUEST_NONE;
  /* Back past the start lands on the start. */
  CHECK(run(&f, "SEEK -60") == ZZPLAY_REXX_RC_OK);
  CHECK(f.ctl.request == ZZPLAY_REQUEST_SEEK && f.ctl.seek_ms == 0U);
  f.ctl.request = ZZPLAY_REQUEST_NONE;
  /* At or past the end moves on, as the seek key does. */
  CHECK(run(&f, "SEEK 180") == ZZPLAY_REXX_RC_OK);
  CHECK(f.ctl.request == ZZPLAY_REQUEST_NEXT);
  f.ctl.request = ZZPLAY_REQUEST_NONE;

  CHECK(run(&f, "SEEK 1:30") == ZZPLAY_REXX_RC_ERROR);
  CHECK(run(&f, "SEEK +") == ZZPLAY_REXX_RC_ERROR);
  CHECK(run(&f, "SEEK 99999999999") == ZZPLAY_REXX_RC_ERROR);
  CHECK(f.ctl.request == ZZPLAY_REQUEST_NONE);

  CHECK(run(&f, "POSITION") == ZZPLAY_REXX_RC_OK && result_is(&f, "30"));
  CHECK(run(&f, "DURATION") == ZZPLAY_REXX_RC_OK && result_is(&f, "180"));
  zzplay_controller_item_end(&f.ctl);
  CHECK(run(&f, "POSITION") == ZZPLAY_REXX_RC_OK && result_is(&f, "0"));
  CHECK(run(&f, "DURATION") == ZZPLAY_REXX_RC_OK && result_is(&f, "0"));
  fixture_free(&f);
}

static void test_settings(void)
{
  Fixture f;

  fixture_init(&f, 0U);
  CHECK(run(&f, "VOLUME") == ZZPLAY_REXX_RC_OK && result_is(&f, "50"));
  CHECK(run(&f, "VOLUME 150") == ZZPLAY_REXX_RC_OK && f.ctl.volume == 100U);
  CHECK(f.ctl.volume_changed);
  CHECK(run(&f, "VOLUME -15") == ZZPLAY_REXX_RC_OK && f.ctl.volume == 85U);
  CHECK(run(&f, "VOLUME +5") == ZZPLAY_REXX_RC_OK && f.ctl.volume == 90U);
  CHECK(run(&f, "VOLUME -200") == ZZPLAY_REXX_RC_OK && f.ctl.volume == 0U);
  CHECK(run(&f, "VOLUME loud") == ZZPLAY_REXX_RC_ERROR);
  CHECK(f.ctl.volume == 0U);

  CHECK(run(&f, "REPEAT") == ZZPLAY_REXX_RC_OK && result_is(&f, "OFF"));
  CHECK(run(&f, "REPEAT track") == ZZPLAY_REXX_RC_OK);
  CHECK(f.ctl.repeat == ZZPLAY_REPEAT_ONE);
  CHECK(run(&f, "REPEAT") == ZZPLAY_REXX_RC_OK && result_is(&f, "TRACK"));
  CHECK(run(&f, "REPEAT ALL") == ZZPLAY_REXX_RC_OK);
  CHECK(f.ctl.repeat == ZZPLAY_REPEAT_ALL);
  CHECK(run(&f, "REPEAT ONCE") == ZZPLAY_REXX_RC_ERROR);
  CHECK(f.ctl.repeat == ZZPLAY_REPEAT_ALL);

  CHECK(run(&f, "SHUFFLE") == ZZPLAY_REXX_RC_OK && result_is(&f, "OFF"));
  CHECK(run(&f, "SHUFFLE TOGGLE") == ZZPLAY_REXX_RC_OK && f.ctl.shuffle);
  CHECK(run(&f, "SHUFFLE ON") == ZZPLAY_REXX_RC_OK && f.ctl.shuffle);
  CHECK(run(&f, "SHUFFLE OFF") == ZZPLAY_REXX_RC_OK && !f.ctl.shuffle);
  CHECK(run(&f, "SHUFFLE YES") == ZZPLAY_REXX_RC_ERROR);

  CHECK(run(&f, "FULLSCREEN ON") == ZZPLAY_REXX_RC_OK && f.ctl.fullscreen);
  CHECK(run(&f, "FULLSCREEN") == ZZPLAY_REXX_RC_OK && result_is(&f, "ON"));
  CHECK(run(&f, "FULLSCREEN TOGGLE") == ZZPLAY_REXX_RC_OK &&
        !f.ctl.fullscreen);
  fixture_free(&f);
}

static void test_playlist(void)
{
  Fixture f;

  fixture_init(&f, 2U);
  f.ctl.dirty = 0U;
  /* Paths that add nothing fail and leave the list alone. */
  CHECK(run(&f, "ADD Empty:") == ZZPLAY_REXX_RC_ERROR);
  CHECK(!f.reply.has_result);
  CHECK(f.playlist.count == 2U && f.ctl.dirty == 0U);
  CHECK(run(&f, "OPEN Empty:") == ZZPLAY_REXX_RC_ERROR);
  CHECK(f.playlist.count == 2U && f.ctl.request == ZZPLAY_REQUEST_NONE);

  /* ADD appends without starting anything. */
  CHECK(run(&f, "ADD Empty: Work:c.mp3") == ZZPLAY_REXX_RC_OK);
  CHECK(result_is(&f, "1"));
  CHECK(f.playlist.count == 3U);
  CHECK((f.ctl.dirty & ZZPLAY_DIRTY_PLAYLIST) != 0U);
  CHECK(f.ctl.request == ZZPLAY_REQUEST_NONE);
  CHECK(run(&f, "COUNT") == ZZPLAY_REXX_RC_OK && result_is(&f, "3"));
  CHECK(run(&f, "ENTRY 3") == ZZPLAY_REXX_RC_OK &&
        result_is(&f, "Work:c.mp3"));
  CHECK(run(&f, "ENTRY 4") == ZZPLAY_REXX_RC_ERROR);

  /* OPEN replaces the list and plays its first entry. */
  CHECK(run(&f, "OPEN Work:x.mp3 Work:y.mp3") == ZZPLAY_REXX_RC_OK);
  CHECK(result_is(&f, "2"));
  CHECK(f.playlist.count == 2U);
  CHECK(strcmp(f.playlist.entries[0].path, "Work:x.mp3") == 0);
  CHECK(f.ctl.request == ZZPLAY_REQUEST_JUMP && f.ctl.jump_index == 0);
  f.ctl.request = ZZPLAY_REQUEST_NONE;

  /* The playing item's path survives removal of its entry. */
  (void)zzplay_playlist_set_current(&f.playlist, 1);
  strcpy(f.item_path, "Work:y.mp3");
  start_item(&f, 1, 0U, 0U);
  CHECK(run(&f, "INDEX") == ZZPLAY_REXX_RC_OK && result_is(&f, "2"));
  CHECK(run(&f, "REMOVE 2") == ZZPLAY_REXX_RC_OK);
  CHECK(f.playlist.count == 1U);
  CHECK(run(&f, "INDEX") == ZZPLAY_REXX_RC_OK && result_is(&f, "0"));
  CHECK(run(&f, "FILE") == ZZPLAY_REXX_RC_OK &&
        result_is(&f, "Work:y.mp3"));
  zzplay_controller_item_end(&f.ctl);
  /* Stopped with no current entry there is no file. */
  CHECK(run(&f, "FILE") == ZZPLAY_REXX_RC_OK && result_is(&f, ""));
  CHECK(run(&f, "REMOVE 2") == ZZPLAY_REXX_RC_ERROR);

  CHECK(run(&f, "SAVEPLAYLIST \"RAM:my list.m3u\"") == ZZPLAY_REXX_RC_OK);
  CHECK(strcmp(f.saved_path, "RAM:my list.m3u") == 0);
  CHECK(run(&f, "SAVEPLAYLIST Locked:list.m3u") == ZZPLAY_REXX_RC_ERROR);

  CHECK(run(&f, "CLEAR") == ZZPLAY_REXX_RC_OK);
  CHECK(f.playlist.count == 0U);
  fixture_free(&f);
}

static void test_program(void)
{
  Fixture f;

  fixture_init(&f, 0U);
  CHECK(run(&f, "VERSION") == ZZPLAY_REXX_RC_OK &&
        result_is(&f, "ZZPlay 0.6 (07.10.2026)"));
  CHECK(run(&f, "SHOW") == ZZPLAY_REXX_RC_WARN);
  f.window_open = 1;
  CHECK(run(&f, "SHOW") == ZZPLAY_REXX_RC_OK);

  /* An open file requester keeps ZZPlay running, as the window does. */
  f.quit_blocked = 1;
  CHECK(run(&f, "QUIT") == ZZPLAY_REXX_RC_WARN);
  CHECK(f.ctl.request == ZZPLAY_REQUEST_NONE);
  f.quit_blocked = 0;
  CHECK(run(&f, "QUIT") == ZZPLAY_REXX_RC_OK);
  CHECK(f.ctl.request == ZZPLAY_REQUEST_QUIT);
  fixture_free(&f);
}

int main(void)
{
  test_parsing();
  test_play_and_pause();
  test_seek();
  test_settings();
  test_playlist();
  test_program();
  if (failures != 0) {
    fprintf(stderr, "zzplay_rexx_test: %d check(s) failed\n", failures);
    return 1;
  }
  printf("zzplay_rexx_test: all tests passed\n");
  return 0;
}
