/* Persistent ZZPlay settings.
 *
 * Stored as human-readable KEY=VALUE text in ENV:ZZPlay.prefs (current
 * session, "Use") and ENVARC:ZZPlay.prefs (survives reboot, "Save"), the
 * standard AmigaOS preferences split. Unknown keys are ignored and a bad
 * value only resets that key, so a hand-edited or newer file never makes
 * the player refuse to start.
 *
 *   # ZZPlay settings
 *   MP3OUTPUT=AUTO        AUTO | MHI | AHI
 *   VIDEOAUDIO=AUTO       AUTO | AHI | AX | NONE
 *   AHIUNIT=0             0..3
 *   MHIDRIVER=mhizz9000.library   file name inside LIBS:MHI/
 *   VOLUME=100            0..100
 *   REPEAT=OFF            OFF | ONE | ALL
 *   SHUFFLE=NO            YES | NO
 *   WINDOW=-1,-1          player window left,top; -1,-1 = default
 *   DRAWER=Work:Music     last drawer used in a file requester
 *
 * Precedence, lowest first: built-in defaults, the saved file, then
 * ToolTypes / CLI options for this launch only. Device preferences are
 * preferences: a backend chosen here falls back like AUTO when it cannot
 * play an item. Only a backend named explicitly for this launch (AUDIO=)
 * fails instead of falling back.
 *
 * Host-testable (stdio only). SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_PREFS_H
#define ZZPLAY_PREFS_H

#include <stddef.h>
#include <stdint.h>

#include "zzplay-audio.h"
#include "zzplay-options.h"
#include "zzplay-playlist.h"

#define ZZPLAY_PREFS_ENV_PATH "ENV:ZZPlay.prefs"
#define ZZPLAY_PREFS_ENVARC_PATH "ENVARC:ZZPlay.prefs"
#define ZZPLAY_PREFS_DEFAULT_MHI_DRIVER "mhizz9000.library"
#define ZZPLAY_PREFS_AHI_UNITS 4U
#define ZZPLAY_PREFS_DRIVER_MAX 32U
#define ZZPLAY_PREFS_DRAWER_MAX 256U
#define ZZPLAY_PREFS_TEXT_MAX 1024U

typedef struct ZZPlayPrefs {
  /* Device settings (Settings window). */
  ZZPlayAudioBackend mp3_output;   /* AUTO, MHI or AHI */
  ZZPlayAudioBackend video_audio;  /* AUTO, AHI, AX or NONE */
  uint32_t ahi_unit;               /* 0..3 */
  char mhi_driver[ZZPLAY_PREFS_DRIVER_MAX];
  /* Session state, remembered automatically by the player window. */
  uint32_t volume;                 /* 0..100 */
  ZZPlayRepeat repeat;
  int shuffle;
  int16_t window_left;             /* -1 = default placement */
  int16_t window_top;
  char last_drawer[ZZPLAY_PREFS_DRAWER_MAX];
} ZZPlayPrefs;

void zzplay_prefs_defaults(ZZPlayPrefs *prefs);

/* Apply settings text over `prefs` (call defaults first for a fresh read).
 * Returns the number of keys accepted. */
uint32_t zzplay_prefs_parse(ZZPlayPrefs *prefs, const char *text,
                            size_t length);

/* Format all keys. Returns bytes required excluding the NUL; truncates
 * (still NUL-terminated) when `capacity` is too small. */
size_t zzplay_prefs_format(const ZZPlayPrefs *prefs, char *out,
                           size_t capacity);

/* defaults + parse of the file; a missing file leaves the defaults and
 * returns 0, a readable file returns 1. */
int zzplay_prefs_load(ZZPlayPrefs *prefs, const char *path);
int zzplay_prefs_save(const ZZPlayPrefs *prefs, const char *path);

/* Copy only the session-state fields (volume, repeat, shuffle, window,
 * drawer) from `from` into `to`. */
void zzplay_prefs_copy_session(ZZPlayPrefs *to, const ZZPlayPrefs *from);

/* Read-modify-write: load `path` (defaults when missing), replace its
 * session-state fields with `current`'s and write it back, so remembering
 * the volume never overwrites device settings the user chose to only
 * "Use". Returns 1 on success. */
int zzplay_prefs_save_session(const ZZPlayPrefs *current, const char *path);

/* Keep launch-only overrides out of a remembered session: a volume, repeat
 * or shuffle value in `session` still where this launch `started` it goes
 * back to the `stored` setting, so VOLUME=N or LOOP never become the saved
 * default; a value the user changed during the session is kept. */
void zzplay_prefs_settle_session(ZZPlayPrefs *session,
                                 const ZZPlayPrefs *stored,
                                 const ZZPlayPrefs *started);

/* A driver name is a plain file name: non-empty, shorter than
 * ZZPLAY_PREFS_DRIVER_MAX, no ':' or '/'. */
int zzplay_prefs_valid_driver(const char *name);

/* Lay this launch's ToolType/CLI overrides (AHIUNIT, MHIDRIVER, VOLUME)
 * over the loaded settings. */
void zzplay_prefs_apply_options(ZZPlayPrefs *prefs,
                                const ZZPlayOptions *options);

/* The backend to try first for `media`, and whether failing to get it is an
 * error (`*strict` = 1) rather than a reason to fall back as AUTO would.
 * An explicit AUDIO= option or --benchmark wins; it is strict unless it is
 * AUTO (AUDIO=AUTO, with or without --benchmark), which overrides the saved
 * output but may fall back. Otherwise
 * the saved preference for that media type applies, non-strict. A saved
 * preference that cannot apply to the media type (none can for
 * ZZPLAY_MEDIA_AUDIO_NONE, which is how FLAC and Ogg Vorbis ask) resolves
 * to AUTO; on-card versus AHI for those files is decided after the card
 * answers, not here. */
ZZPlayAudioBackend zzplay_prefs_requested_backend(
    const ZZPlayPrefs *prefs, const ZZPlayOptions *options,
    ZZPlayMediaAudio media, int *strict);

#endif /* ZZPLAY_PREFS_H */
