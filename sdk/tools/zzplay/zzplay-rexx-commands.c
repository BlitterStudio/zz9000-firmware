/* ARexx command language for ZZPlay; see zzplay-rexx-commands.h.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-rexx-commands.h"

#include <stdio.h>
#include <string.h>

/* Command word plus arguments. ADD and OPEN take one argument per path. */
#define ZZPLAY_REXX_MAX_ARGS 64U

typedef void (*ZZPlayRexxHandler)(const ZZPlayRexxHost *host,
                                  uint32_t argc, char **argv,
                                  ZZPlayRexxReply *reply);

typedef struct ZZPlayRexxCommand {
  const char *name;
  ZZPlayRexxHandler run;
  /* Argument count bounds, command word excluded. */
  uint8_t min_args;
  uint8_t max_args;
} ZZPlayRexxCommand;

/* The port runs on the player task only, one command at a time. */
static char *zzplay_rexx_argv[ZZPLAY_REXX_MAX_ARGS];

static char zzplay_rexx_upper(char c)
{
  return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static int zzplay_rexx_blank(char c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Case-insensitive match of `word` (ending at NUL or a blank) against an
 * upper-case keyword. */
static int zzplay_rexx_word_is(const char *word, const char *keyword)
{
  while (*keyword != '\0') {
    if (zzplay_rexx_upper(*word) != *keyword) {
      return 0;
    }
    word++;
    keyword++;
  }
  return *word == '\0' || zzplay_rexx_blank(*word);
}

/* Split `line` in place. Returns the number of words, or -1 for an
 * unterminated quote, a quote glued to the next word, or too many words. */
static int zzplay_rexx_split(char *line, char **argv)
{
  uint32_t argc = 0U;
  char *p = line;

  for (;;) {
    while (zzplay_rexx_blank(*p)) {
      p++;
    }
    if (*p == '\0') {
      return (int)argc;
    }
    if (argc == ZZPLAY_REXX_MAX_ARGS) {
      return -1;
    }
    if (*p == '"') {
      argv[argc++] = ++p;
      while (*p != '\0' && *p != '"') {
        p++;
      }
      if (*p != '"') {
        return -1;
      }
      *p++ = '\0';
      if (*p != '\0' && !zzplay_rexx_blank(*p)) {
        return -1;
      }
    } else {
      argv[argc++] = p;
      while (*p != '\0' && !zzplay_rexx_blank(*p)) {
        p++;
      }
      if (*p == '\0') {
        return (int)argc;
      }
      *p++ = '\0';
    }
  }
}

/* Decimal number with an optional '+' or '-'. `sign` is 0 without one.
 * Values past 999999999 are refused rather than wrapped. */
static int zzplay_rexx_number(const char *text, int *sign, uint32_t *value)
{
  uint32_t n = 0U;

  *sign = 0;
  if (*text == '+') {
    *sign = 1;
    text++;
  } else if (*text == '-') {
    *sign = -1;
    text++;
  }
  if (*text == '\0') {
    return 0;
  }
  for (; *text != '\0'; text++) {
    if (*text < '0' || *text > '9' || n > 99999999U) {
      return 0;
    }
    n = n * 10U + (uint32_t)(*text - '0');
  }
  *value = n;
  return 1;
}

/* A 1-based playlist position naming an existing entry, as an entry index. */
static int zzplay_rexx_position(const ZZPlayRexxHost *host, const char *text,
                                uint32_t *index)
{
  int sign;
  uint32_t n;

  if (!zzplay_rexx_number(text, &sign, &n) || sign != 0 || n == 0U ||
      n > host->playlist->count) {
    return 0;
  }
  *index = n - 1U;
  return 1;
}

static void zzplay_rexx_text(ZZPlayRexxReply *reply, const char *text)
{
  size_t length = text ? strlen(text) : 0U;

  if (length >= sizeof(reply->result)) {
    length = sizeof(reply->result) - 1U;
  }
  if (length != 0U) {
    memcpy(reply->result, text, length);
  }
  reply->result[length] = '\0';
  reply->has_result = 1;
}

static void zzplay_rexx_u32(ZZPlayRexxReply *reply, uint32_t value)
{
  sprintf(reply->result, "%lu", (unsigned long)value);
  reply->has_result = 1;
}

static void zzplay_rexx_flag(ZZPlayRexxReply *reply, int on)
{
  zzplay_rexx_text(reply, on ? "ON" : "OFF");
}

/* ON, OFF or TOGGLE applied to `current`. Returns -1 for anything else. */
static int zzplay_rexx_switch(const char *word, int current)
{
  if (zzplay_rexx_word_is(word, "ON")) {
    return 1;
  }
  if (zzplay_rexx_word_is(word, "OFF")) {
    return 0;
  }
  if (zzplay_rexx_word_is(word, "TOGGLE")) {
    return !current;
  }
  return -1;
}

static void zzplay_rexx_warn_unless(ZZPlayRexxReply *reply, int done)
{
  if (!done) {
    reply->rc = ZZPLAY_REXX_RC_WARN;
  }
}

/* --- Transport --- */

/* Pause or resume the playing item. The controller refuses while the modal
 * gate owns the pause. */
static void zzplay_rexx_set_paused(const ZZPlayRexxHost *host, int paused,
                                   ZZPlayRexxReply *reply)
{
  ZZPlayController *ctl = host->ctl;

  if (!ctl->item_active) {
    reply->rc = ZZPLAY_REXX_RC_WARN;
    return;
  }
  if ((ctl->paused != 0) != (paused != 0)) {
    zzplay_rexx_warn_unless(
        reply, zzplay_controller_apply(ctl, ZZPLAY_CONTROL_TOGGLE_PAUSE));
  }
}

static void zzplay_rexx_play(const ZZPlayRexxHost *host, uint32_t argc,
                             char **argv, ZZPlayRexxReply *reply)
{
  ZZPlayController *ctl = host->ctl;
  uint32_t index;

  if (argc == 1U) {
    if (!zzplay_rexx_position(host, argv[0], &index)) {
      reply->rc = ZZPLAY_REXX_RC_ERROR;
      return;
    }
    zzplay_rexx_warn_unless(reply,
                            zzplay_controller_jump(ctl, (int32_t)index));
    return;
  }
  if (ctl->item_active) {
    zzplay_rexx_set_paused(host, 0, reply);
    return;
  }
  if (host->playlist->count == 0U) {
    reply->rc = ZZPLAY_REXX_RC_WARN;
    return;
  }
  zzplay_rexx_warn_unless(
      reply, zzplay_controller_request(ctl, ZZPLAY_REQUEST_PLAY));
}

static void zzplay_rexx_pause(const ZZPlayRexxHost *host, uint32_t argc,
                              char **argv, ZZPlayRexxReply *reply)
{
  (void)argc;
  (void)argv;
  zzplay_rexx_set_paused(host, 1, reply);
}

static void zzplay_rexx_resume(const ZZPlayRexxHost *host, uint32_t argc,
                               char **argv, ZZPlayRexxReply *reply)
{
  (void)argc;
  (void)argv;
  zzplay_rexx_set_paused(host, 0, reply);
}

static void zzplay_rexx_playpause(const ZZPlayRexxHost *host, uint32_t argc,
                                  char **argv, ZZPlayRexxReply *reply)
{
  (void)argc;
  (void)argv;
  if (!host->ctl->item_active && host->playlist->count == 0U) {
    reply->rc = ZZPLAY_REXX_RC_WARN;
    return;
  }
  zzplay_rexx_warn_unless(
      reply,
      zzplay_controller_apply(host->ctl, ZZPLAY_CONTROL_TOGGLE_PAUSE));
}

static void zzplay_rexx_stop(const ZZPlayRexxHost *host, uint32_t argc,
                             char **argv, ZZPlayRexxReply *reply)
{
  (void)argc;
  (void)argv;
  zzplay_rexx_warn_unless(
      reply, zzplay_controller_request(host->ctl, ZZPLAY_REQUEST_STOP));
}

static void zzplay_rexx_next(const ZZPlayRexxHost *host, uint32_t argc,
                             char **argv, ZZPlayRexxReply *reply)
{
  (void)argc;
  (void)argv;
  zzplay_rexx_warn_unless(
      reply, zzplay_controller_apply(host->ctl, ZZPLAY_CONTROL_NEXT));
}

static void zzplay_rexx_previous(const ZZPlayRexxHost *host, uint32_t argc,
                                 char **argv, ZZPlayRexxReply *reply)
{
  (void)argc;
  (void)argv;
  zzplay_rexx_warn_unless(
      reply, zzplay_controller_apply(host->ctl, ZZPLAY_CONTROL_PREVIOUS));
}

/* SEEK 90 goes to 1:30; SEEK +10 / SEEK -10 move relative to now. */
static void zzplay_rexx_seek(const ZZPlayRexxHost *host, uint32_t argc,
                             char **argv, ZZPlayRexxReply *reply)
{
  ZZPlayController *ctl = host->ctl;
  int sign;
  uint32_t seconds;
  uint64_t target;
  uint64_t delta;

  (void)argc;
  if (!zzplay_rexx_number(argv[0], &sign, &seconds)) {
    reply->rc = ZZPLAY_REXX_RC_ERROR;
    return;
  }
  if (!ctl->item_active || !ctl->now.seekable) {
    reply->rc = ZZPLAY_REXX_RC_WARN;
    return;
  }
  delta = (uint64_t)seconds * 1000U;
  if (sign > 0) {
    target = (uint64_t)ctl->now.elapsed_ms + delta;
  } else if (sign < 0) {
    target = delta < ctl->now.elapsed_ms ? ctl->now.elapsed_ms - delta : 0U;
  } else {
    target = delta;
  }
  if (target > 0xFFFFFFFFU) {
    target = 0xFFFFFFFFU;
  }
  zzplay_rexx_warn_unless(
      reply, zzplay_controller_seek_to(ctl, (uint32_t)target));
}

/* --- Settings --- */

/* VOLUME returns the level; VOLUME 50 sets it; VOLUME +5 / -5 step it. */
static void zzplay_rexx_volume(const ZZPlayRexxHost *host, uint32_t argc,
                               char **argv, ZZPlayRexxReply *reply)
{
  ZZPlayController *ctl = host->ctl;
  int sign;
  uint32_t value;
  uint32_t volume;

  if (argc == 0U) {
    zzplay_rexx_u32(reply, ctl->volume);
    return;
  }
  if (!zzplay_rexx_number(argv[0], &sign, &value)) {
    reply->rc = ZZPLAY_REXX_RC_ERROR;
    return;
  }
  if (sign > 0) {
    volume = ctl->volume + (value > ZZPLAY_VOLUME_MAX ? ZZPLAY_VOLUME_MAX
                                                      : value);
  } else if (sign < 0) {
    volume = value < ctl->volume ? ctl->volume - value : 0U;
  } else {
    volume = value;
  }
  zzplay_controller_set_volume(ctl, volume);
}

static void zzplay_rexx_repeat(const ZZPlayRexxHost *host, uint32_t argc,
                               char **argv, ZZPlayRexxReply *reply)
{
  static const char *const names[] = { "OFF", "TRACK", "ALL" };
  uint32_t i;

  if (argc == 0U) {
    zzplay_rexx_text(reply, (uint32_t)host->ctl->repeat < 3U
                                ? names[host->ctl->repeat]
                                : "OFF");
    return;
  }
  for (i = 0U; i < 3U; i++) {
    if (zzplay_rexx_word_is(argv[0], names[i])) {
      zzplay_controller_set_repeat(host->ctl, (ZZPlayRepeat)i);
      return;
    }
  }
  reply->rc = ZZPLAY_REXX_RC_ERROR;
}

static void zzplay_rexx_shuffle(const ZZPlayRexxHost *host, uint32_t argc,
                                char **argv, ZZPlayRexxReply *reply)
{
  int on;

  if (argc == 0U) {
    zzplay_rexx_flag(reply, host->ctl->shuffle);
    return;
  }
  on = zzplay_rexx_switch(argv[0], host->ctl->shuffle);
  if (on < 0) {
    reply->rc = ZZPLAY_REXX_RC_ERROR;
    return;
  }
  zzplay_controller_set_shuffle(host->ctl, on);
}

static void zzplay_rexx_fullscreen(const ZZPlayRexxHost *host, uint32_t argc,
                                   char **argv, ZZPlayRexxReply *reply)
{
  int on;

  if (argc == 0U) {
    zzplay_rexx_flag(reply, host->ctl->fullscreen);
    return;
  }
  on = zzplay_rexx_switch(argv[0], host->ctl->fullscreen);
  if (on < 0) {
    reply->rc = ZZPLAY_REXX_RC_ERROR;
    return;
  }
  zzplay_controller_set_fullscreen(host->ctl, on);
}

/* --- Playlist --- */

static uint32_t zzplay_rexx_add_all(const ZZPlayRexxHost *host,
                                    ZZPlayPlaylist *list, uint32_t argc,
                                    char **argv)
{
  uint32_t added = 0U;
  uint32_t i;

  for (i = 0U; i < argc; i++) {
    added += host->add_path(host->user, list, argv[i]);
  }
  return added;
}

/* Appends; RESULT is the number of entries added. Playback is not
 * started: that is what OPEN is for. */
static void zzplay_rexx_add(const ZZPlayRexxHost *host, uint32_t argc,
                            char **argv, ZZPlayRexxReply *reply)
{
  uint32_t added;

  if (!host->add_path) {
    reply->rc = ZZPLAY_REXX_RC_ERROR;
    return;
  }
  added = zzplay_rexx_add_all(host, host->playlist, argc, argv);
  if (added == 0U) {
    reply->rc = ZZPLAY_REXX_RC_ERROR;
    return;
  }
  host->ctl->dirty |= ZZPLAY_DIRTY_PLAYLIST;
  zzplay_rexx_u32(reply, added);
}

/* Replaces the playlist and plays its first entry. When the paths add
 * nothing, the playlist is left as it was. */
static void zzplay_rexx_open(const ZZPlayRexxHost *host, uint32_t argc,
                             char **argv, ZZPlayRexxReply *reply)
{
  ZZPlayPlaylist *list = host->playlist;
  ZZPlayPlaylist next;
  uint32_t added;

  if (!host->add_path) {
    reply->rc = ZZPLAY_REXX_RC_ERROR;
    return;
  }
  zzplay_playlist_init(&next);
  next.repeat = list->repeat;
  next.shuffle = list->shuffle;
  next.rng = list->rng;
  added = zzplay_rexx_add_all(host, &next, argc, argv);
  if (added == 0U) {
    zzplay_playlist_free(&next);
    reply->rc = ZZPLAY_REXX_RC_ERROR;
    return;
  }
  zzplay_playlist_free(list);
  *list = next;
  host->ctl->dirty |= ZZPLAY_DIRTY_PLAYLIST;
  zzplay_rexx_warn_unless(reply, zzplay_controller_jump(host->ctl, 0));
  zzplay_rexx_u32(reply, added);
}

static void zzplay_rexx_clear(const ZZPlayRexxHost *host, uint32_t argc,
                              char **argv, ZZPlayRexxReply *reply)
{
  (void)argc;
  (void)argv;
  (void)reply;
  zzplay_playlist_clear(host->playlist);
  host->ctl->dirty |= ZZPLAY_DIRTY_PLAYLIST;
}

static void zzplay_rexx_remove(const ZZPlayRexxHost *host, uint32_t argc,
                               char **argv, ZZPlayRexxReply *reply)
{
  uint32_t index;

  (void)argc;
  if (!zzplay_rexx_position(host, argv[0], &index)) {
    reply->rc = ZZPLAY_REXX_RC_ERROR;
    return;
  }
  (void)zzplay_playlist_remove(host->playlist, index);
  host->ctl->dirty |= ZZPLAY_DIRTY_PLAYLIST;
}

static void zzplay_rexx_saveplaylist(const ZZPlayRexxHost *host,
                                     uint32_t argc, char **argv,
                                     ZZPlayRexxReply *reply)
{
  (void)argc;
  if (!host->save_playlist ||
      !host->save_playlist(host->user, host->playlist, argv[0])) {
    reply->rc = ZZPLAY_REXX_RC_ERROR;
  }
}

/* --- Queries --- */

static void zzplay_rexx_status(const ZZPlayRexxHost *host, uint32_t argc,
                               char **argv, ZZPlayRexxReply *reply)
{
  (void)argc;
  (void)argv;
  if (!host->ctl->item_active) {
    zzplay_rexx_text(reply, "STOPPED");
  } else {
    zzplay_rexx_text(reply, host->ctl->paused ? "PAUSED" : "PLAYING");
  }
}

/* While stopped, TITLE and FILE describe the entry PLAY would start. */
static void zzplay_rexx_title(const ZZPlayRexxHost *host, uint32_t argc,
                              char **argv, ZZPlayRexxReply *reply)
{
  const ZZPlayPlaylist *list = host->playlist;

  (void)argc;
  (void)argv;
  if (host->ctl->item_active) {
    zzplay_rexx_text(reply, host->ctl->now.title);
  } else if (list->current >= 0 && (uint32_t)list->current < list->count) {
    zzplay_rexx_text(reply, zzplay_playlist_display_name(
                                list, (uint32_t)list->current));
  } else {
    zzplay_rexx_text(reply, "");
  }
}

static void zzplay_rexx_file(const ZZPlayRexxHost *host, uint32_t argc,
                             char **argv, ZZPlayRexxReply *reply)
{
  const ZZPlayPlaylist *list = host->playlist;

  (void)argc;
  (void)argv;
  if (host->ctl->item_active && host->item_path &&
      host->item_path[0] != '\0') {
    zzplay_rexx_text(reply, host->item_path);
  } else if (list->current >= 0 && (uint32_t)list->current < list->count) {
    zzplay_rexx_text(reply, list->entries[list->current].path);
  } else {
    zzplay_rexx_text(reply, "");
  }
}

static void zzplay_rexx_format(const ZZPlayRexxHost *host, uint32_t argc,
                               char **argv, ZZPlayRexxReply *reply)
{
  (void)argc;
  (void)argv;
  zzplay_rexx_text(reply,
                   host->ctl->item_active ? host->ctl->now.format : "");
}

static void zzplay_rexx_position_query(const ZZPlayRexxHost *host,
                                       uint32_t argc, char **argv,
                                       ZZPlayRexxReply *reply)
{
  (void)argc;
  (void)argv;
  zzplay_rexx_u32(reply, host->ctl->item_active
                             ? host->ctl->now.elapsed_ms / 1000U
                             : 0U);
}

/* 0 when stopped or when the length is not known. */
static void zzplay_rexx_duration(const ZZPlayRexxHost *host, uint32_t argc,
                                 char **argv, ZZPlayRexxReply *reply)
{
  (void)argc;
  (void)argv;
  zzplay_rexx_u32(reply, host->ctl->item_active
                             ? host->ctl->now.total_ms / 1000U
                             : 0U);
}

static void zzplay_rexx_count(const ZZPlayRexxHost *host, uint32_t argc,
                              char **argv, ZZPlayRexxReply *reply)
{
  (void)argc;
  (void)argv;
  zzplay_rexx_u32(reply, host->playlist->count);
}

/* Position of the current entry, 0 when there is none. */
static void zzplay_rexx_index(const ZZPlayRexxHost *host, uint32_t argc,
                              char **argv, ZZPlayRexxReply *reply)
{
  int32_t current = host->playlist->current;

  (void)argc;
  (void)argv;
  zzplay_rexx_u32(reply, current >= 0 ? (uint32_t)current + 1U : 0U);
}

static void zzplay_rexx_entry(const ZZPlayRexxHost *host, uint32_t argc,
                              char **argv, ZZPlayRexxReply *reply)
{
  uint32_t index;

  (void)argc;
  if (!zzplay_rexx_position(host, argv[0], &index)) {
    reply->rc = ZZPLAY_REXX_RC_ERROR;
    return;
  }
  zzplay_rexx_text(reply, host->playlist->entries[index].path);
}

static void zzplay_rexx_version(const ZZPlayRexxHost *host, uint32_t argc,
                                char **argv, ZZPlayRexxReply *reply)
{
  (void)argc;
  (void)argv;
  zzplay_rexx_text(reply, host->version ? host->version : "ZZPlay");
}

/* --- Program --- */

static void zzplay_rexx_show(const ZZPlayRexxHost *host, uint32_t argc,
                             char **argv, ZZPlayRexxReply *reply)
{
  (void)argc;
  (void)argv;
  zzplay_rexx_warn_unless(reply, host->show && host->show(host->user));
}

static void zzplay_rexx_quit(const ZZPlayRexxHost *host, uint32_t argc,
                             char **argv, ZZPlayRexxReply *reply)
{
  (void)argc;
  (void)argv;
  if (host->quit_blocked && host->quit_blocked(host->user)) {
    reply->rc = ZZPLAY_REXX_RC_WARN;
    return;
  }
  (void)zzplay_controller_request(host->ctl, ZZPLAY_REQUEST_QUIT);
}

static void zzplay_rexx_help(const ZZPlayRexxHost *host, uint32_t argc,
                             char **argv, ZZPlayRexxReply *reply);

static const ZZPlayRexxCommand zzplay_rexx_commands[] = {
  { "PLAY", zzplay_rexx_play, 0U, 1U },
  { "PAUSE", zzplay_rexx_pause, 0U, 0U },
  { "RESUME", zzplay_rexx_resume, 0U, 0U },
  { "PLAYPAUSE", zzplay_rexx_playpause, 0U, 0U },
  { "STOP", zzplay_rexx_stop, 0U, 0U },
  { "NEXT", zzplay_rexx_next, 0U, 0U },
  { "PREVIOUS", zzplay_rexx_previous, 0U, 0U },
  { "SEEK", zzplay_rexx_seek, 1U, 1U },
  { "VOLUME", zzplay_rexx_volume, 0U, 1U },
  { "REPEAT", zzplay_rexx_repeat, 0U, 1U },
  { "SHUFFLE", zzplay_rexx_shuffle, 0U, 1U },
  { "FULLSCREEN", zzplay_rexx_fullscreen, 0U, 1U },
  { "ADD", zzplay_rexx_add, 1U, ZZPLAY_REXX_MAX_ARGS - 1U },
  { "OPEN", zzplay_rexx_open, 1U, ZZPLAY_REXX_MAX_ARGS - 1U },
  { "CLEAR", zzplay_rexx_clear, 0U, 0U },
  { "REMOVE", zzplay_rexx_remove, 1U, 1U },
  { "SAVEPLAYLIST", zzplay_rexx_saveplaylist, 1U, 1U },
  { "STATUS", zzplay_rexx_status, 0U, 0U },
  { "TITLE", zzplay_rexx_title, 0U, 0U },
  { "FILE", zzplay_rexx_file, 0U, 0U },
  { "FORMAT", zzplay_rexx_format, 0U, 0U },
  { "POSITION", zzplay_rexx_position_query, 0U, 0U },
  { "DURATION", zzplay_rexx_duration, 0U, 0U },
  { "COUNT", zzplay_rexx_count, 0U, 0U },
  { "INDEX", zzplay_rexx_index, 0U, 0U },
  { "ENTRY", zzplay_rexx_entry, 1U, 1U },
  { "VERSION", zzplay_rexx_version, 0U, 0U },
  { "SHOW", zzplay_rexx_show, 0U, 0U },
  { "HELP", zzplay_rexx_help, 0U, 0U },
  { "QUIT", zzplay_rexx_quit, 0U, 0U }
};

#define ZZPLAY_REXX_COMMAND_COUNT \
  (sizeof(zzplay_rexx_commands) / sizeof(zzplay_rexx_commands[0]))

static void zzplay_rexx_help(const ZZPlayRexxHost *host, uint32_t argc,
                             char **argv, ZZPlayRexxReply *reply)
{
  size_t length = 0U;
  uint32_t i;

  (void)host;
  (void)argc;
  (void)argv;
  for (i = 0U; i < ZZPLAY_REXX_COMMAND_COUNT; i++) {
    size_t name_length = strlen(zzplay_rexx_commands[i].name);

    if (length + name_length + 2U > sizeof(reply->result)) {
      break;
    }
    if (length != 0U) {
      reply->result[length++] = ' ';
    }
    memcpy(reply->result + length, zzplay_rexx_commands[i].name,
           name_length);
    length += name_length;
  }
  reply->result[length] = '\0';
  reply->has_result = 1;
}

int zzplay_rexx_needs_gate(const char *line)
{
  if (!line) {
    return 0;
  }
  while (zzplay_rexx_blank(*line)) {
    line++;
  }
  return zzplay_rexx_word_is(line, "ADD") ||
         zzplay_rexx_word_is(line, "OPEN");
}

void zzplay_rexx_execute(const ZZPlayRexxHost *host, char *line,
                         ZZPlayRexxReply *reply)
{
  uint32_t i;
  int argc;

  reply->rc = ZZPLAY_REXX_RC_ERROR;
  reply->has_result = 0;
  reply->result[0] = '\0';
  if (!host || !host->ctl || !host->playlist || !line) {
    return;
  }
  argc = zzplay_rexx_split(line, zzplay_rexx_argv);
  if (argc <= 0) {
    return;
  }
  for (i = 0U; i < ZZPLAY_REXX_COMMAND_COUNT; i++) {
    const ZZPlayRexxCommand *command = &zzplay_rexx_commands[i];
    uint32_t args = (uint32_t)argc - 1U;

    if (!zzplay_rexx_word_is(zzplay_rexx_argv[0], command->name)) {
      continue;
    }
    if (args < command->min_args || args > command->max_args) {
      return;
    }
    reply->rc = ZZPLAY_REXX_RC_OK;
    command->run(host, args, zzplay_rexx_argv + 1, reply);
    if (reply->rc != ZZPLAY_REXX_RC_OK) {
      reply->has_result = 0;
      reply->result[0] = '\0';
    }
    return;
  }
}
