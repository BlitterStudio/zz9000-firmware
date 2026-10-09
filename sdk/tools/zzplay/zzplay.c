/*
 * zzplay: desktop multi-format player for the ZZ9000 streaming media
 * service.
 *
 * The application loop owns the playlist, settings and the playback
 * controller; every media format is one row in zzplay_engines[] (see
 * zzplay-formats.h). MPEG-1 Program Stream playback lives here; standalone
 * MP3 is zzplay-mp3.c. All mailbox interaction is expressed as
 * codec/container/output descriptors so later backends do not require a
 * new player protocol.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "zz9k/sdk.h"
#include "zzplay-ahi.h"
#include "zzplay-audio.h"
#include "zzplay-ax.h"
#include "zzplay-controls.h"
#include "zzplay-core.h"
#include "zzplay-controller.h"
#include "zzplay-files.h"
#include "zzplay-formats.h"
#include "zzplay-frame-clock.h"
#include "zzplay-geometry.h"
#include "zzplay-gui.h"
#include "zzplay-launch.h"
#include "zzplay-media.h"
#include "zzplay-mp3.h"
#include "zzplay-options.h"
#include "zzplay-path.h"
#include "zzplay-playlist.h"
#include "zzplay-prefs.h"
#include "zzplay-probe.h"
#include "zzplay-stats.h"
#include "zzplay-stream.h"
#include "zzplay-sync.h"
#include "zzplay-tags.h"
#include "zzplay-video.h"
#include "zzplay-webm.h"


#include <devices/timer.h>
#include <exec/libraries.h>
#include <graphics/gfx.h>
#include <intuition/intuition.h>
#include <libraries/Picasso96.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/Picasso96.h>
#include <proto/timer.h>
#include <utility/tagitem.h>

#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ZZPLAY_INPUT_BYTES (64U * 1024U)
/* File reads land straight in the card-visible input buffer in sub-frame
 * chunks: the m68k is the present clock, and one 64 KB blocking read at
 * real-disk throughput stalled it for 1-2 frame periods, which showed as
 * a pause on every read. ~16 KB keeps a read under one frame period even
 * at ~2 MB/s and removes the staging copy entirely. */
#define ZZPLAY_READ_CHUNK_BYTES (16U * 1024U)
#define ZZPLAY_FEED_ROUNDS_PER_PASS 4U
#define ZZPLAY_PCM_BYTES (128U * 1024U)
#define ZZPLAY_Z2_INPUT_BYTES (24U * 1024U)
#define ZZPLAY_Z2_PCM_BYTES (32U * 1024U)
#define ZZPLAY_AHI_PERIODS_PER_SECOND 10U
#define ZZPLAY_MEDIA_MAX_SAMPLE_RATE 48000U
#define ZZPLAY_MEDIA_PCM_FRAME_BYTES 4U
#define ZZPLAY_PCM_LOW_WATER                                      \
  ((ZZPLAY_MEDIA_MAX_SAMPLE_RATE /                             \
    ZZPLAY_AHI_PERIODS_PER_SECOND) *                           \
   ZZPLAY_MEDIA_PCM_FRAME_BYTES * ZZPLAY_AHI_BUFFER_COUNT)
#define ZZPLAY_PCM_HIGH_WATER (96U * 1024U)
#define ZZPLAY_SYNC_POLL_US 2000U
#define ZZPLAY_FPS_REPORT_US 2000000U

struct Library *P96Base;
struct Device *TimerBase;

static volatile sig_atomic_t zzplay_ctrl_c_requested;

static const char zzplay_version[] = "$VER: ZZPlay 0.6 (07.10.2026)";

/* Minimum stack, enforced at startup by libnix's swapstack.o (linked by the
 * build scripts): a smaller launch stack is replaced before main() runs.
 * The player window's GadTools/ASL calls and the settings window's AHI
 * device and mode-database queries all run on this task's stack, nested
 * inside the playback loops; a Shell launch otherwise gets only its
 * default (often 4 KiB), which those calls overrun. */
unsigned long __stack = 65536UL;

struct ZZPlayTimer {
  struct MsgPort *port;
  struct timerequest *request;
};

struct ZZPlayStats {
  TimeVal_Type last_sample;
  TimeVal_Type report_started;
  TimeVal_Type profile_started;
  ZZPlayStatsCore core;
  uint32_t profile_wall_us;
  uint8_t started;
};

struct ZZPlayRuntime {
  ZZPlayCore core;
  ZZPlayOptions options;
  ZZPlayPrefs prefs;
  ZZPlayController *ctl;
  FILE *file;
  ZZ9KContext *ctx;
  ZZ9KSharedBuffer input;
  ZZ9KSharedBuffer pcm;
  ZZPlayPCMRing pcm_ring;
  ZZPlayAHISink ahi;
  ZZPlayAXSink ax;
  struct ZZPlayTimer timer;
  struct ZZPlayStats stats;
  struct Window *window;
  struct BitMap *bitmap;
   ZZPlayVideoInfo video_info;
   uint32_t begin_video_codec;
   uint32_t begin_container;
   uint32_t begin_audio_codec;
   uint32_t required_extra_flags;
   uint32_t required_audio_flags;
   uint32_t display_w;
   uint32_t display_h;
   uint8_t is_webm;
   uint8_t decode_skip;
   LONG pip_error;
  uint32_t session;
  uint32_t frames;
  /* `frames` when the current loop pass began: the displayed position. */
  uint32_t pass_frame_origin;
  uint32_t final_underruns;
  uint32_t completed_loops;
  uint64_t audio_origin_pts;
  uint64_t final_audio_frames;
  /* Per-item playback trace (diagnostics; --trace). dos.library
   * BPTR with one unbuffered Write() per line: diagnostics must not
   * take the player down with them, and partial data has to survive
   * a crash (the first stdio-file-write build gurud the machine with
   * the trace file created but empty). */
  BPTR trace;
  TimeVal_Type trace_started;
  TimeVal_Type trace_last_frame;
  uint64_t trace_video_pts;
  uint32_t trace_accepted;
  uint32_t trace_need_input;
  uint32_t trace_write_busy;
  uint32_t trace_decode_busy;
  uint32_t trace_reads;
  uint32_t trace_read_max_us;
  uint32_t trace_underruns;
  char trace_decision;
  uint8_t trace_anchored;
  ZZ9KMediaSessionAudioResult audio_result;
  ZZPlaySyncPolicy sync_policy;
  ZZPlayAudioBackend audio_backend;
  /* The requested backend was an explicit choice that must not silently
   * fall back (zzplay_prefs_requested_backend). */
  uint8_t audio_strict;
  /* Direct AX output is advertised and has not failed for this item: a
   * saved AHI output that cannot open may still fall back to it. */
  uint8_t ax_available;
  uint8_t audio_enabled;
  uint8_t audio_prepared;
  uint8_t audio_started;
  uint8_t audio_status_known;
  uint8_t audio_refresh_needed;
  uint8_t audio_totals_captured;
  uint8_t frame_held;
  uint8_t pip_open_failed;
  /* Runtime UX state (U6). */
  ZZPlayWindowGeometry saved_geometry;
  ZZPlayPresentInfo present;
  struct Screen *screen;
  uint16_t screen_w;
  uint16_t screen_h;
  uint8_t fullscreen;
  uint8_t present_known;
  uint8_t present_recheck;
  uint8_t title_dirty;
  char title[128];
};

/* Application state, which outlives every per-item engine run: options,
 * settings, the controller and the playlist the player window edits. */
typedef struct ZZPlayApp {
  ZZPlayOptions options;
  ZZPlayPrefs prefs;
  /* What ENV held before this launch's overrides, and the values the
   * session started with, so launch-only overrides are not saved. */
  ZZPlayPrefs stored_prefs;
  ZZPlayPrefs started_prefs;
  ZZPlayController ctl;
  ZZPlayPlaylist playlist;
  int player_mode;
  int had_failure;
  int32_t jump_index;      /* pending JUMP target */
  uint32_t start_seek_ms;  /* restart position for the next START */
  BPTR trace;
  /* Private copy of the playing entry's path: playlist edits must not
   * free the path an engine is reading from. */
  char item_path[ZZPLAY_PLAYLIST_PATH_MAX];
} ZZPlayApp;

static ZZPlayApp app;

static uint32_t zzplay_elapsed_us(const TimeVal_Type *start,
                                  const TimeVal_Type *end);

static void zzplay_sigint_handler(int signal_number)
{
  (void)signal_number;
  zzplay_ctrl_c_requested = 1;
}

static uint64_t zzplay_now_us(void)
{
  TimeVal_Type now;

  GetSysTime(&now);
  return (uint64_t)now.tv_secs * 1000000ULL + now.tv_micro;
}

static void zzplay_profile_begin(const struct ZZPlayRuntime *runtime,
                                 TimeVal_Type *started)
{
  if (runtime->stats.started) {
    GetSysTime(started);
  }
}

static void zzplay_profile_end(struct ZZPlayRuntime *runtime,
                               const TimeVal_Type *started,
                               ZZPlayProfileCategory category)
{
  TimeVal_Type ended;

  if (!runtime->stats.started) {
    return;
  }
  GetSysTime(&ended);
  zzplay_stats_record_profile(
      &runtime->stats.core, category,
      zzplay_elapsed_us(started, &ended));
}

/* Route a failure to whichever surface the user can see: the player
 * window's message line first (it is the desktop player's error surface),
 * then stderr for a CLI launch, or a Workbench requester. The redundant
 * "zzplay: " prefix and trailing newline are trimmed for the window and
 * the requester; stderr output stays byte-identical to the old one. */
static void zzplay_error(const struct ZZPlayRuntime *runtime,
                         const char *format, ...)
{
  va_list args;
  char message[320];
  char *text = message;
  size_t length;

  (void)runtime;
  va_start(args, format);
  /* Messages carry user paths of any length: truncate, never overrun. */
  (void)vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  if (strncmp(text, "zzplay: ", 8U) == 0) {
    text += 8;
  }
  length = strlen(text);
  while (length > 0U && text[length - 1U] == '\n') {
    length--;
    text[length] = '\0';
  }
  if (runtime && runtime->ctl) {
    zzplay_controller_set_message(runtime->ctl, text);
  }
  zzplay_launch_report_surface(&app.options, text);
}

static void zzplay_usage(FILE *stream)
{
  fprintf(stream,
          "%s\n"
          "Usage: zzplay [--fps|--benchmark] [--loop[=count]] "
          "[--fullscreen]\n"
          "          [--audio=auto|ahi|mhi|ax|none] [--player[=no]]\n"
          "          [--ahiunit=0..3] [--mhidriver=name] "
          "[--volume=0..100]\n"
          "          <file|drawer|pattern|playlist>...\n"
          "  --fps         rolling paced-playback and decode-call FPS\n"
          "  --benchmark   disable pacing and audio unless requested\n"
          "  --loop        repeat forever; --loop=N repeats N times\n"
          "  --fullscreen  start filling the screen, aspect preserved\n"
          "  --player      stay open as a desktop player (the default\n"
          "                without files, and from Workbench); "
          "--player=no\n"
          "                plays the named files once and exits\n"
          "  --ahiunit=N   ahi.device unit for AHI output (0..3)\n"
          "  --mhidriver=N MHI driver file in LIBS:MHI/ "
          "(default mhizz9000.library)\n"
          "  --volume=N    starting volume, 0..100\n"
          "  --quiet       no progress output (the default from Workbench)\n"
          "  --verbose     force progress output even from Workbench\n"
          "  --audio=...   audio output (MP3 AUTO: MHI then AHI;\n"
          "                FLAC/Ogg AUTO: card then AHI)\n"
          "  --trace[=p]   log per-frame timing diagnostics "
          "(default T:zzplay.trace)\n"
          "\n"
          "Several files, drawers, patterns or playlists (#?.m3u) may be\n"
          "named; each is expanded into the playlist.\n"
          "\n"
          "Keys (video and player window): space pause, N next, P\n"
          "previous, S shuffle, L repeat off/track/playlist, + and -\n"
          "(also cursor up/down) volume, cursor left/right seek (MP3),\n"
          "F fullscreen, Esc or Q stop.\n"
          "\n"
          "Workbench: drop files (or a drawer) on the zzplay icon; every\n"
          "dropped file is added to the playlist. ToolTypes FPS,\n"
          "BENCHMARK, LOOP[=N], FULLSCREEN, PLAYER[=NO], AHIUNIT=n,\n"
          "MHIDRIVER=name, VOLUME=n, QUIET, VERBOSE, TRACE[=PATH] and\n"
          "AUDIO=<backend> match the options above. A Workbench launch is\n"
          "quiet by default, because printing there makes AmigaDOS open an\n"
          "output window that never closes.\n",
          zzplay_version + 6);
}

static void zzplay_print_fps(const char *label, uint32_t playback_milli,
                             uint32_t decode_milli)
{
  zzplay_info("zzplay: %s %lu.%03lu fps playback, "
         "%lu.%03lu fps decode-call\n",
         label,
         (unsigned long)(playback_milli / 1000U),
         (unsigned long)(playback_milli % 1000U),
         (unsigned long)(decode_milli / 1000U),
         (unsigned long)(decode_milli % 1000U));
}

static void zzplay_stats_start(struct ZZPlayStats *stats)
{
  memset(stats, 0, sizeof(*stats));
  zzplay_stats_reset(&stats->core);
  GetSysTime(&stats->last_sample);
  stats->report_started = stats->last_sample;
  stats->profile_started = stats->last_sample;
  stats->started = 1U;
}

static void zzplay_stats_frame(struct ZZPlayStats *stats,
                               uint32_t decode_us)
{
  TimeVal_Type now;
  uint32_t sample_us;
  uint32_t report_us;

  GetSysTime(&now);
  sample_us = zzplay_elapsed_us(&stats->last_sample, &now);
  zzplay_stats_record_frame(&stats->core, sample_us, decode_us);
  stats->last_sample = now;

  report_us = zzplay_elapsed_us(&stats->report_started, &now);
  if (report_us >= ZZPLAY_FPS_REPORT_US) {
    zzplay_print_fps(
        "current",
        zzplay_fps_milli(stats->core.report_frames, report_us),
        zzplay_fps_milli(stats->core.report_frames,
                         stats->core.report_decode_us));
    stats->report_started = now;
    zzplay_stats_reset_report(&stats->core);
  }
}

static void zzplay_stats_stop(struct ZZPlayStats *stats)
{
  TimeVal_Type now;

  if (!stats->started || stats->profile_wall_us != 0U) {
    return;
  }
  GetSysTime(&now);
  stats->profile_wall_us =
      zzplay_elapsed_us(&stats->profile_started, &now);
}

static void zzplay_stats_finish(const struct ZZPlayStats *stats)
{
  static const char *profile_name[ZZPLAY_PROFILE_COUNT] = {
    "AHI poll",
    "AHI submit",
    "SDK audio-read",
    "PCM copy",
    "file read",
    "input copy",
    "SDK write",
    "SDK decode",
    "SDK retire"
  };
  uint64_t accounted_us;
  uint64_t other_us;
  int category;

  if (stats->core.total_frames == 0U || stats->core.wall_us == 0U) {
    zzplay_info("zzplay: average fps unavailable\n");
  } else {
    zzplay_print_fps(
        "average",
        zzplay_fps_milli(stats->core.total_frames, stats->core.wall_us),
        zzplay_fps_milli(stats->core.total_frames, stats->core.decode_us));
  }
  for (category = 0; category < ZZPLAY_PROFILE_COUNT; category++) {
    const ZZPlayProfileMetric *metric = &stats->core.profile[category];

    /* u64 -> u32 ms casts: libnix's printf predates the %ll length
     * modifier, so every 64-bit value is narrowed after the math
     * instead (totals fit 32 bits for any realistic session). */
    zzplay_info("zzplay: profile %-14s %lu calls, %lu ms total, "
           "%lu us average\n",
           profile_name[category],
           (unsigned long)metric->calls,
           (unsigned long)(metric->elapsed_us / 1000U),
           (unsigned long)zzplay_stats_profile_average_us(
               &stats->core, (ZZPlayProfileCategory)category));
  }
  accounted_us = zzplay_stats_profile_total_us(&stats->core);
  other_us = stats->profile_wall_us > accounted_us
                 ? stats->profile_wall_us - accounted_us
                 : 0U;
  zzplay_info("zzplay: profile wall %lu ms, accounted %lu ms, "
         "other %lu ms\n",
         (unsigned long)(stats->profile_wall_us / 1000U),
         (unsigned long)(accounted_us / 1000U),
         (unsigned long)(other_us / 1000U));
}

static int zzplay_timer_open(struct ZZPlayTimer *timer)
{
  memset(timer, 0, sizeof(*timer));
  timer->port = CreateMsgPort();
  if (!timer->port) {
    return 0;
  }
  timer->request = (struct timerequest *)CreateIORequest(
      timer->port, sizeof(*timer->request));
  if (!timer->request) {
    DeleteMsgPort(timer->port);
    timer->port = 0;
    return 0;
  }
  if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
                 (struct IORequest *)timer->request, 0) != 0) {
    DeleteIORequest((struct IORequest *)timer->request);
    DeleteMsgPort(timer->port);
    memset(timer, 0, sizeof(*timer));
    return 0;
  }
  TimerBase = (struct Device *)timer->request->tr_node.io_Device;
  return 1;
}

static void zzplay_timer_close(struct ZZPlayTimer *timer)
{
  if (!timer) {
    return;
  }
  if (timer->request) {
    CloseDevice((struct IORequest *)timer->request);
    DeleteIORequest((struct IORequest *)timer->request);
  }
  if (timer->port) {
    DeleteMsgPort(timer->port);
  }
  memset(timer, 0, sizeof(*timer));
  TimerBase = 0;
}

static void zzplay_wait_us(struct ZZPlayTimer *timer, uint32_t usec)
{
  if (!timer || !timer->request || usec == 0U) {
    return;
  }
  timer->request->tr_node.io_Command = TR_ADDREQUEST;
  timer->request->tr_time.tv_secs = usec / 1000000U;
  timer->request->tr_time.tv_micro = usec % 1000000U;
  DoIO((struct IORequest *)timer->request);
}

static uint32_t zzplay_elapsed_us(const TimeVal_Type *start,
                                  const TimeVal_Type *end)
{
  uint32_t seconds;
  int32_t micros;

  if (end->tv_secs < start->tv_secs) {
    return 0U;
  }
  seconds = end->tv_secs - start->tv_secs;
  micros = (int32_t)end->tv_micro - (int32_t)start->tv_micro;
  if (micros < 0) {
    if (seconds == 0U) {
      return 0U;
    }
    seconds--;
    micros += 1000000L;
  }
  if (seconds > 4294U) {
    return 0xffffffffU;
  }
  return seconds * 1000000U + (uint32_t)micros;
}

/* Defined further down; the window and drain helpers below need them. */
static int zzplay_release_resource(void *user, ZZPlayResource resource);
static const char *zzplay_audio_backend_name(ZZPlayAudioBackend backend);
static int zzplay_engine_service_input(struct ZZPlayRuntime *runtime,
                                       int *resized);
static void zzplay_engine_stop_from_request(
    struct ZZPlayRuntime *runtime);

static ZZPlayControlAction zzplay_poll_control(
    struct ZZPlayRuntime *runtime, int *resized)
{
  struct IntuiMessage *message;
  ZZPlayControlInput input;
  struct Window *window = runtime->window;

  memset(&input, 0, sizeof(input));
  if (resized) {
    *resized = 0;
  }
  /* libnix checks SIGBREAKF_CTRL_C from stdio read/write and raises SIGINT.
   * Keep the handler as the durable request bit, and consume a break seen
   * here so later cleanup printf/fclose calls cannot see it as an uncaught
   * abort and bypass the resource stack. */
  input.ctrl_c = zzplay_ctrl_c_requested != 0 ||
                 (SetSignal(0L, SIGBREAKF_CTRL_C) &
                  SIGBREAKF_CTRL_C) != 0U;
  while (window &&
         (message = (struct IntuiMessage *)GetMsg(window->UserPort))) {
    if (message->Class == IDCMP_CLOSEWINDOW) {
      input.window_close = 1;
    } else if (message->Class == IDCMP_VANILLAKEY) {
      ZZPlayControlAction action =
          zzplay_control_action_from_key((unsigned)message->Code);

      /* Keep the first meaningful key in this poll: a burst of autorepeat
       * must not let a later NONE overwrite a real request. */
      if (action != ZZPLAY_CONTROL_NONE && input.key == 0U) {
        input.key = (unsigned)message->Code;
      }
    } else if (message->Class == IDCMP_RAWKEY) {
      /* Cursor keys have no VANILLAKEY translation; rawkey is consulted
       * only when no vanilla key mapped to anything (see resolve). */
      ZZPlayControlAction action = zzplay_control_action_from_rawkey(
          (unsigned)message->Code);

      if (action != ZZPLAY_CONTROL_NONE && input.rawkey == 0U) {
        input.rawkey = (unsigned)message->Code;
      }
    } else if (message->Class == IDCMP_NEWSIZE) {
      if (resized) {
        *resized = 1;
      }
    }
    ReplyMsg((struct Message *)message);
  }
  return zzplay_control_resolve(&input);
}

/* Cache the screen dimensions. The fullscreen toggle has to close the
 * PIP before reopening it, and at that moment WScreen is gone. This
 * runs in the application, not inside a P96 driver callback, so the
 * LockPubScreen fallback is safe here (the deadlock noted in the P96
 * contract is a CreateFeature hazard). */
static void zzplay_cache_screen(struct ZZPlayRuntime *runtime)
{
  struct Screen *screen;

  /* The dedicated fullscreen screen is authoritative whenever it is
   * open: the Workbench window (or a public screen) reports the wrong
   * dimensions for placing the fullscreen PIP. */
  if (runtime->screen) {
    runtime->screen_w = (uint16_t)runtime->screen->Width;
    runtime->screen_h = (uint16_t)runtime->screen->Height;
    return;
  }
  if (runtime->window && runtime->window->WScreen) {
    runtime->screen_w = (uint16_t)runtime->window->WScreen->Width;
    runtime->screen_h = (uint16_t)runtime->window->WScreen->Height;
    return;
  }
  /* No window yet (first open, or the toggle's close-reopen window):
   * measure the same named screen the PIP opens on, not whatever the
   * system default public screen happens to be. */
  screen = LockPubScreen((CONST_STRPTR)"Workbench");
  if (!screen) {
    return;
  }
  runtime->screen_w = (uint16_t)screen->Width;
  runtime->screen_h = (uint16_t)screen->Height;
  UnlockPubScreen(0, screen);
}

/* `placement` is the window; the PIP always fills its inner area. Setting
 * the PIP rectangle explicitly with P96PIP_Left/Top/Width/Height was tried
 * and makes p96PIP_OpenTagList fail outright on this driver, so the window
 * is the only handle on the video geometry. */
static struct Window *zzplay_open_pip(const ZZPlayVideoInfo *info,
                                      const ZZPlayRect *placement,
                                      int fullscreen,
                                      struct Screen *screen,
                                      uint16_t limit_w, uint16_t limit_h,
                                      const char *title,
                                      struct BitMap **bitmap,
                                      LONG *pip_error)
{
  struct TagItem open_tags[28];
  struct TagItem get_tags[2];
  struct Window *window;
  ULONG bitmap_value = 0U;
  unsigned i = 0U;

  *bitmap = 0;
  *pip_error = 0;
  open_tags[i].ti_Tag = P96PIP_SourceFormat;
  open_tags[i++].ti_Data = RGBFB_YUV422CGX;
  open_tags[i].ti_Tag = P96PIP_SourceWidth;
  open_tags[i++].ti_Data = info->width;
  open_tags[i].ti_Tag = P96PIP_SourceHeight;
  open_tags[i++].ti_Data = info->height;
  open_tags[i].ti_Tag = P96PIP_Type;
  open_tags[i++].ti_Data = P96PIPT_MemoryWindow;
  open_tags[i].ti_Tag = P96PIP_ErrorCode;
  open_tags[i++].ti_Data = (ULONG)pip_error;
  /* Request an exact video-area size. WA_Width/Height include borders and
   * silently force scaling even when the user has not resized the window. */
  open_tags[i].ti_Tag = WA_InnerWidth;
  open_tags[i++].ti_Data = placement->width;
  open_tags[i].ti_Tag = WA_InnerHeight;
  open_tags[i++].ti_Data = placement->height;
  open_tags[i].ti_Tag = WA_Left;
  open_tags[i++].ti_Data = (ULONG)(LONG)placement->x;
  open_tags[i].ti_Tag = WA_Top;
  open_tags[i++].ti_Data = (ULONG)(LONG)placement->y;
  if (screen) {
    open_tags[i].ti_Tag = WA_CustomScreen;
    open_tags[i++].ti_Data = (ULONG)screen;
  } else {
    open_tags[i].ti_Tag = WA_PubScreenName;
    open_tags[i++].ti_Data = (ULONG)"Workbench";
  }
  open_tags[i].ti_Tag = WA_Activate;
  open_tags[i++].ti_Data = TRUE;
  /* Without these Intuition derives the size limits from the window's
   * opening dimensions, so it could never afterwards be enlarged to the
   * screen - which is what ChangeWindowBox() below has to be able to do. */
  open_tags[i].ti_Tag = WA_MinWidth;
  open_tags[i++].ti_Data = 32U;
  open_tags[i].ti_Tag = WA_MinHeight;
  open_tags[i++].ti_Data = 24U;
  open_tags[i].ti_Tag = WA_MaxWidth;
  open_tags[i++].ti_Data = limit_w != 0U ? limit_w : (ULONG)~0UL;
  open_tags[i].ti_Tag = WA_MaxHeight;
  open_tags[i++].ti_Data = limit_h != 0U ? limit_h : (ULONG)~0UL;
  if (fullscreen) {
    /* Borderless, deliberately NOT a backdrop window: Intuition manages
     * backdrop windows' position/size itself, and both fullscreen
     * rounds that used WA_Backdrop on the dedicated screen never got
     * their forced geometry (r5 "borderless but still 640x480", and
     * the colour-key window of issue #83 testing) while every
     * non-backdrop resize -- user drags, forced reopens -- scales
     * correctly. The letterbox margins show the dedicated screen's
     * black background. */
    open_tags[i].ti_Tag = WA_Borderless;
    open_tags[i++].ti_Data = TRUE;
  } else {
    open_tags[i].ti_Tag = WA_Title;
    open_tags[i++].ti_Data = (ULONG)title;
    open_tags[i].ti_Tag = WA_DragBar;
    open_tags[i++].ti_Data = TRUE;
    open_tags[i].ti_Tag = WA_CloseGadget;
    open_tags[i++].ti_Data = TRUE;
    open_tags[i].ti_Tag = WA_DepthGadget;
    open_tags[i++].ti_Data = TRUE;
    open_tags[i].ti_Tag = WA_SizeGadget;
    open_tags[i++].ti_Data = TRUE;
  }
  open_tags[i].ti_Tag = WA_IDCMP;
  open_tags[i++].ti_Data =
      IDCMP_CLOSEWINDOW | IDCMP_VANILLAKEY | IDCMP_RAWKEY |
      IDCMP_NEWSIZE;
  open_tags[i].ti_Tag = TAG_DONE;
  open_tags[i].ti_Data = 0U;

  window = p96PIP_OpenTagList(open_tags);
  if (!window) {
    return 0;
  }
  get_tags[0].ti_Tag = P96PIP_SourceBitMap;
  get_tags[0].ti_Data = (ULONG)&bitmap_value;
  get_tags[1].ti_Tag = TAG_DONE;
  get_tags[1].ti_Data = 0U;
  if (p96PIP_GetTagList(window, get_tags) == 0 || bitmap_value == 0U) {
    p96PIP_Close(window);
    return 0;
  }
  *bitmap = (struct BitMap *)bitmap_value;
  return window;
}

/* Make the window actually be the requested inner size and position, and
 * say what happened. ChangeWindowBox() is the same mechanism a user drag
 * uses, and takes outer dimensions, so the borders are added back here. */
static void zzplay_force_geometry(struct ZZPlayRuntime *runtime,
                                  const ZZPlayRect *want)
{
  struct Window *window = runtime->window;
  LONG border_w;
  LONG border_h;
  LONG outer_w;
  LONG outer_h;

  if (!window || want->width == 0U || want->height == 0U) {
    return;
  }
  border_w = window->BorderLeft + window->BorderRight;
  border_h = window->BorderTop + window->BorderBottom;
  outer_w = (LONG)want->width + border_w;
  outer_h = (LONG)want->height + border_h;
  if (window->Width != outer_w || window->Height != outer_h ||
      window->LeftEdge != (WORD)want->x ||
      window->TopEdge != (WORD)want->y) {
    ChangeWindowBox(window, (LONG)want->x, (LONG)want->y,
                    outer_w, outer_h);
  }
  /* Report what was asked for against what Intuition settled on: if these
   * ever disagree the geometry was refused, and that must be visible
   * rather than silently looking like the wrong mode. */
  zzplay_info("zzplay: window %ldx%ld at %ld,%ld (video area %ux%u "
              "requested, screen %ux%u)\n",
              (long)(window->Width - border_w),
              (long)(window->Height - border_h),
              (long)window->LeftEdge, (long)window->TopEdge,
              (unsigned)want->width, (unsigned)want->height,
              (unsigned)runtime->screen_w, (unsigned)runtime->screen_h);
}

/* Open a dedicated screen matching the video, so fullscreen needs no
 * scaling and no window resizing at all. The overlay rectangle is owned by
 * ZZ9000.card through P96's PIP API - there is no SDK op that positions it -
 * so the PIP is still how the video plane gets placed. Giving it a screen of
 * exactly the source size reduces that to a 1:1 fill, which is both the
 * fastest path and the one that avoids the sizing behaviour that repeatedly
 * failed on this driver. */
/* ~0 terminates the list and asks P96 for the default pen set, while
 * SharePens leaves them obtainable - between them the PIP can get its key. */
static UWORD zzplay_screen_pens[] = { (UWORD)~0 };

/* The margins around the fitted video are the screen's background, pen 0,
 * which a new screen otherwise takes from the Workbench palette; Intuition
 * also refills uncovered screen areas with it when the window is resized.
 * Make pen 0 black. Format: count << 16 | first pen, 32-bit R, G, B, 0. */
static ULONG zzplay_screen_colors[] = { 1UL << 16, 0UL, 0UL, 0UL, 0UL };

static const char *zzplay_pip_error_name(LONG error)
{
  switch (error) {
  case 1: return "out of memory";
  case 2: return "could not attach to the screen";
  case 3: return "PIP not available";
  case 4: return "no free pen for the colour key";
  case 5: return "bad dimensions or format";
  case 6: return "could not open the window";
  default: return "unknown";
  }
}

static int zzplay_open_video_screen(struct ZZPlayRuntime *runtime)
{
  uint16_t width = (uint16_t)runtime->video_info.width;
  uint16_t height = (uint16_t)runtime->video_info.height;
  ULONG depth = 16UL;
  ULONG mode;

  if (runtime->screen) {
    return 1;
  }
  /* Match the Workbench depth where we know it: both 16- and 32-bit are
   * hardware-qualified for the PIP, and the source stride P96 derives for
   * the packed-YUV bitmap depends on it. */
  if (runtime->window && runtime->window->WScreen) {
    ULONG current = (ULONG)p96GetBitMapAttr(
        runtime->window->WScreen->RastPort.BitMap, P96BMA_DEPTH);

    if (current == 15UL || current == 16UL || current == 32UL) {
      depth = current;
    }
  }
  /* ZZ9000.card pixel-doubles every mode smaller than 640x480, and the
   * card's overlay refuses doubled modes, so the PIP fails with "not
   * available" on such a screen. Ask for at least 640x480; the window fit
   * below scales a smaller video up to it. */
  if (width < 640U) {
    width = 640U;
  }
  if (height < 480U) {
    height = 480U;
  }
  mode = p96BestModeIDTags(
      P96BIDTAG_NominalWidth, (ULONG)width,
      P96BIDTAG_NominalHeight, (ULONG)height,
      P96BIDTAG_Depth, depth,
      TAG_DONE);
  if (mode == (ULONG)INVALID_ID) {
    zzplay_info("zzplay: no %ux%u screen mode available for fullscreen\n",
                (unsigned)width, (unsigned)height);
    return 0;
  }
  /* Open the mode at its NATIVE size, never at the video's size: a
   * custom-sized P96 screen is carved out of the bigger mode raster and
   * renders top-left with a background-pen border on this card, which
   * is exactly the "fullscreen but not centred" report of issue #83.
   * The video itself is then scaled by the window fit to fill the
   * screen. */
  runtime->screen = p96OpenScreenTags(
      P96SA_DisplayID, mode,
      P96SA_Depth, depth,
      P96SA_Title, (ULONG)"ZZPlay",
      P96SA_ShowTitle, FALSE,
      P96SA_Quiet, TRUE,
      P96SA_AutoScroll, FALSE,
      P96SA_SharePens, TRUE,
      P96SA_Pens, (ULONG)zzplay_screen_pens,
      P96SA_Colors32, (ULONG)zzplay_screen_colors,
      TAG_DONE);
  if (!runtime->screen) {
    zzplay_info("zzplay: could not open a fullscreen display\n");
    return 0;
  }
  (void)zzplay_resource_acquire(
      &runtime->core.resources, ZZPLAY_RESOURCE_VIDEO_SCREEN);
  return 1;
}

static void zzplay_close_video_screen(struct ZZPlayRuntime *runtime)
{
  (void)zzplay_resource_release(
      &runtime->core.resources, ZZPLAY_RESOURCE_VIDEO_SCREEN,
      zzplay_release_resource, runtime);
}

/* Display aspect when the file has one (WebM DisplayWidth/Height),
 * otherwise the coded frame. The PIP source stays the coded size. */
static void zzplay_aspect_pixels(const struct ZZPlayRuntime *runtime,
                                 uint16_t *width, uint16_t *height)
{
  uint32_t w = runtime->display_w != 0U ? runtime->display_w
                                        : runtime->video_info.width;
  uint32_t h = runtime->display_h != 0U ? runtime->display_h
                                        : runtime->video_info.height;

  if (w == 0U || w > 65535U) {
    w = runtime->video_info.width > 65535U ? 65535U
                                           : runtime->video_info.width;
  }
  if (h == 0U || h > 65535U) {
    h = runtime->video_info.height > 65535U ? 65535U
                                            : runtime->video_info.height;
  }
  *width = (uint16_t)w;
  *height = (uint16_t)h;
}

/* Where the windowed window should sit. A portrait frame taller than the
 * Workbench is fitted, aspect preserved, rather than opened off-screen. */
static ZZPlayRect zzplay_pip_placement(struct ZZPlayRuntime *runtime,
                                       int fullscreen)
{
  ZZPlayRect rect;
  uint16_t aspect_w;
  uint16_t aspect_h;
  uint16_t avail_w;
  uint16_t avail_h;

  (void)fullscreen;
  if (zzplay_geometry_restore(&runtime->saved_geometry, &rect)) {
    return rect;
  }
  zzplay_aspect_pixels(runtime, &aspect_w, &aspect_h);
  memset(&rect, 0, sizeof(rect));
  rect.width = aspect_w;
  rect.height = aspect_h;
  if (runtime->screen_w > 32U && runtime->screen_h > 48U) {
    avail_w = (uint16_t)(runtime->screen_w - 32U);
    avail_h = (uint16_t)(runtime->screen_h - 48U);
    if (aspect_w > avail_w || aspect_h > avail_h) {
      return zzplay_geometry_fit(aspect_w, aspect_h, avail_w, avail_h);
    }
  }
  return rect;
}

static void zzplay_close_pip(struct ZZPlayRuntime *runtime)
{
  if (!runtime->window) {
    return;
  }
  (void)zzplay_resource_release(
      &runtime->core.resources, ZZPLAY_RESOURCE_VIDEO_WINDOW,
      zzplay_release_resource, runtime);
}

static int zzplay_open_pip_mode(struct ZZPlayRuntime *runtime,
                                int fullscreen)
{
  ZZPlayRect placement;
  ZZPlayRect open_rect;
  uint16_t aspect_w;
  uint16_t aspect_h;

  zzplay_aspect_pixels(runtime, &aspect_w, &aspect_h);
  if (fullscreen && !zzplay_open_video_screen(runtime)) {
    /* Say so rather than silently presenting a windowed player as though
     * the request had been honoured. */
    zzplay_info("zzplay: staying windowed\n");
    fullscreen = 0;
  }
  if (fullscreen) {
    /* Scale to fill the dedicated screen, aspect preserved and centred
     * (zz9000-drivers#83). A 1:1 window on an exact-size dedicated screen
     * is not reliable either: small modes render top-left on the card's
     * minimum raster, which is what "fullscreen but not centred" was.
     * The window opens at the 1:1 source size (P96 does not reliably
     * adopt a larger opening size) and is then forced to the fitted
     * rectangle through the already-proven resize route below. */
    zzplay_cache_screen(runtime);
    placement = zzplay_geometry_fit(
        aspect_w, aspect_h,
        runtime->screen_w ? runtime->screen_w : aspect_w,
        runtime->screen_h ? runtime->screen_h : aspect_h);
    if (placement.width == 0U || placement.height == 0U) {
      /* Degenerate screen information: fall back to 1:1 at the origin
       * rather than refusing to present at all. */
      placement.x = 0;
      placement.y = 0;
      placement.width = aspect_w;
      placement.height = aspect_h;
    }
    open_rect = placement;
    open_rect.width = (uint16_t)runtime->video_info.width;
    open_rect.height = (uint16_t)runtime->video_info.height;
  } else {
    zzplay_close_video_screen(runtime);
    /* The dedicated screen just closed: re-read the public screen so
     * the windowed reopen's limits match the Workbench, not the
     * (smaller) fullscreen screen. A portrait frame is then fitted
     * into that screen instead of opening taller than it. */
    zzplay_cache_screen(runtime);
    placement = zzplay_pip_placement(runtime, 0);
    open_rect = placement;
  }

  runtime->pip_error = 0;
  runtime->window = zzplay_open_pip(
      &runtime->video_info, &open_rect, fullscreen, runtime->screen,
      runtime->screen_w, runtime->screen_h, runtime->title,
      &runtime->bitmap, &runtime->pip_error);
  if (!runtime->window) {
    runtime->pip_open_failed = 1U;
    zzplay_info("zzplay: PIP open failed for %s %ux%u at %d,%d "
                "(P96 error %ld: %s)\n",
                fullscreen ? "fullscreen" : "windowed",
                (unsigned)placement.width, (unsigned)placement.height,
                (int)placement.x, (int)placement.y,
                (long)runtime->pip_error,
                zzplay_pip_error_name(runtime->pip_error));
    /* Never leave a custom screen open with nothing driving it: that is a
     * displayed screen with no window on it, which is how the r5 round left
     * the machine wedged after this failure. */
    zzplay_close_video_screen(runtime);
    return 0;
  }
  /* p96PIP_OpenTagList() does not reliably adopt an opening size larger
   * than the PIP source, which left the first two bench rounds borderless
   * but still 640x480. Resizing afterwards is the same route a user drag
   * takes, and that path was already proven to scale correctly, so the
   * requested geometry is enforced here rather than trusted at open. */
  runtime->fullscreen = fullscreen ? 1U : 0U;
  /* Windowed reopens and scaled fullscreen both enforce the requested
   * geometry through the proven resize route; a 1:1 fullscreen (screen
   * exactly the video size) is already there. */
  zzplay_force_geometry(runtime, &placement);
  runtime->present_recheck = 1U;
  runtime->title_dirty = 1U;
  (void)zzplay_resource_acquire(
      &runtime->core.resources, ZZPLAY_RESOURCE_VIDEO_WINDOW);
  return 1;
}

static int zzplay_ensure_pip(struct ZZPlayRuntime *runtime)
{
  if (runtime->window) {
    return 1;
  }
  /* A visible memory-window PIP exposes its key color until the first
   * formatter-backed frame arrives, so create it only at retirement. */
  return zzplay_open_pip_mode(
      runtime, runtime->options.fullscreen ? 1 : 0);
}

/* Remember the current windowed placement so returning from fullscreen
 * restores what the user had. */
static void zzplay_remember_window(struct ZZPlayRuntime *runtime)
{
  ZZPlayRect rect;

  if (!runtime->window || runtime->fullscreen) {
    return;
  }
  rect.x = (int16_t)runtime->window->LeftEdge;
  rect.y = (int16_t)runtime->window->TopEdge;
  rect.width = (uint16_t)(runtime->window->Width -
                          runtime->window->BorderLeft -
                          runtime->window->BorderRight);
  rect.height = (uint16_t)(runtime->window->Height -
                           runtime->window->BorderTop -
                           runtime->window->BorderBottom);
  zzplay_geometry_remember(&runtime->saved_geometry, &rect);
}

/* The media session is bound to the overlay, not to this window, so the PIP
 * can be reopened mid-playback: P96 simply re-SETs the overlay geometry. */
static int zzplay_toggle_fullscreen(struct ZZPlayRuntime *runtime)
{
  int target;

  if (!runtime->window) {
    return 1;
  }
  target = runtime->fullscreen ? 0 : 1;
  /* Read the screen before the window that describes it is destroyed. */
  zzplay_cache_screen(runtime);
  zzplay_remember_window(runtime);
  zzplay_close_pip(runtime);
  if (zzplay_open_pip_mode(runtime, target)) {
    return 1;
  }
  /* Could not get the requested mode; fall back to the one that worked
   * before rather than continuing with no window at all. */
  runtime->pip_open_failed = 0U;
  if (zzplay_open_pip_mode(runtime, runtime->fullscreen ? 1 : 0)) {
    zzplay_info("zzplay: could not switch to %s\n",
           target ? "fullscreen" : "windowed");
    return 1;
  }
  return 0;
}

/* Snap a user resize back to the source aspect (R7). */
static void zzplay_apply_resize(struct ZZPlayRuntime *runtime)
{
  struct Window *window = runtime->window;
  uint16_t inner_w;
  uint16_t inner_h;
  uint16_t aspect_w;
  uint16_t aspect_h;
  ZZPlayRect fitted;
  LONG border_w;
  LONG border_h;

  if (!window || runtime->fullscreen) {
    return;
  }
  border_w = window->BorderLeft + window->BorderRight;
  border_h = window->BorderTop + window->BorderBottom;
  inner_w = (uint16_t)(window->Width - border_w);
  inner_h = (uint16_t)(window->Height - border_h);
  zzplay_aspect_pixels(runtime, &aspect_w, &aspect_h);
  fitted = zzplay_geometry_fit(
      aspect_w, aspect_h,
      inner_w, inner_h);
  if (fitted.width == 0U || fitted.height == 0U) {
    return;
  }
  /* Only an off-aspect window is resized. Refitting a fitted one rounds
   * down again, and the resize it causes comes straight back here. */
  if (!zzplay_geometry_is_fitted(inner_w, inner_h,
                                 runtime->video_info.width,
                                 runtime->video_info.height)) {
    ChangeWindowBox(window, window->LeftEdge, window->TopEdge,
                    (LONG)fitted.width + border_w,
                    (LONG)fitted.height + border_h);
  }
  zzplay_remember_window(runtime);
  runtime->present_recheck = 1U;
  runtime->title_dirty = 1U;
}

/* Ask firmware which path actually presented, and report transitions. */
static void zzplay_update_presentation(struct ZZPlayRuntime *runtime)
{
  ZZ9KMediaSessionStatusResult status;
  ZZPlayPresentInfo info;
  int result;

  if (runtime->session == 0U) {
    return;
  }
  memset(&status, 0, sizeof(status));
  result = zz9k_media_session_status(
      runtime->ctx, runtime->session, ZZ9K_MEDIA_STATUS_PRESENTATION,
      0U, &status);
  zzplay_present_from_status(result, status.flags, status.value, &info);
  if (runtime->present_known && !zzplay_present_changed(
                                     &runtime->present, &info)) {
    return;
  }
  /* Report the first observation and every later transition, but stay quiet
   * on firmware that cannot answer at all. */
  if (info.path != ZZPLAY_PATH_UNKNOWN &&
      (!runtime->present_known ||
       runtime->present.path != info.path)) {
    zzplay_info("zzplay: presentation path %s (%ux%u source, %ux%u shown)\n",
           zzplay_present_path_name(info.path),
           (unsigned)info.src_w, (unsigned)info.src_h,
           (unsigned)info.dst_w, (unsigned)info.dst_h);
  } else if (!runtime->present_known &&
             info.path == ZZPLAY_PATH_UNKNOWN) {
    zzplay_info("zzplay: presentation path unavailable "
           "(firmware predates path reporting)\n");
  }
  runtime->present = info;
  runtime->present_known = 1U;
  runtime->title_dirty = 1U;
}

/* U7: report where the card actually spent its time, so optimisation is
 * driven by measurement rather than assumption - in particular whether the
 * planar-to-YUY2 pack is material against decode, which is what gates the
 * planar FPGA subproject. */
static void zzplay_report_card_profile(struct ZZPlayRuntime *runtime)
{
  static const char *const stage_name[ZZ9K_MEDIA_PROFILE_STAGES] = {
    "video decode",
    "YUY2 pack",
    "present",
    "audio decode"
  };
  ZZ9KMediaSessionStatusResult status;
  int result;
  unsigned stage;

  if (runtime->session == 0U) {
    return;
  }
  memset(&status, 0, sizeof(status));
  result = zz9k_media_session_status(
      runtime->ctx, runtime->session, ZZ9K_MEDIA_STATUS_PROFILE, 0U,
      &status);
  if (result != ZZ9K_STATUS_OK) {
    zzplay_info("zzplay: card profiling unavailable "
                "(firmware predates it)\n");
    return;
  }
  zzplay_info("zzplay: card YUY2 pack kernel: %s\n",
              (status.flags & ZZ9K_MEDIA_PROFILE_FLAG_NEON_PACK) != 0U
                  ? "NEON"
                  : "scalar");
  for (stage = 0U; stage < ZZ9K_MEDIA_PROFILE_STAGES; stage++) {
    uint32_t microseconds =
        ZZ9K_MEDIA_PROFILE_US(status.value[stage]);
    uint32_t calls = ZZ9K_MEDIA_PROFILE_CALLS(status.value[stage]);
    uint32_t per_call_us = calls != 0U ? microseconds / calls : 0U;

    zzplay_info("zzplay: card %-13s %lu calls, %lu.%03lu ms total, "
                "%lu.%03lu ms each\n",
                stage_name[stage], (unsigned long)calls,
                (unsigned long)(microseconds / 1000U),
                (unsigned long)(microseconds % 1000U),
                (unsigned long)(per_call_us / 1000U),
                (unsigned long)(per_call_us % 1000U));
  }
}

static void zzplay_update_title(struct ZZPlayRuntime *runtime)
{
  const char *state;

  if (!runtime->window || !runtime->title_dirty ||
      runtime->fullscreen) {
    runtime->title_dirty = 0U;
    return;
  }
  state = runtime->core.state == ZZPLAY_STATE_PAUSED ? "paused"
                                                     : "playing";
  if (runtime->is_webm) {
    sprintf(runtime->title, "ZZPlay - WebM - %s - %s - %s%s",
            zzplay_audio_backend_name(runtime->audio_backend),
            runtime->present_known
                ? zzplay_present_path_name(runtime->present.path)
                : "starting",
            state,
            runtime->options.loop_mode != ZZPLAY_LOOP_NONE ? " - loop"
                                                           : "");
  } else if (runtime->video_info.is_program_stream) {
    sprintf(runtime->title, "ZZPlay - MPEG-1 - %s - %s - %s%s",
            zzplay_audio_backend_name(runtime->audio_backend),
            runtime->present_known
                ? zzplay_present_path_name(runtime->present.path)
                : "starting",
            state,
            runtime->options.loop_mode != ZZPLAY_LOOP_NONE ? " - loop"
                                                           : "");
  } else {
    sprintf(runtime->title, "ZZPlay - WebP - %s%s",
            state,
            (runtime->options.loop_mode != ZZPLAY_LOOP_NONE ||
             zzplay_controller_loop_item(runtime->ctl)) ? " - loop"
                                                        : "");
  }
  SetWindowTitles(runtime->window, (CONST_STRPTR)runtime->title,
                  (CONST_STRPTR)~0UL);
  runtime->title_dirty = 0U;
}

 static int zzplay_decode_once(struct ZZPlayRuntime *runtime,
                               ZZ9KMediaSessionMainResult *result)
 {
   uint32_t flags = 0U;
   int status;
 
   /* Held across NEED_INPUT: the card has not reached a keyframe yet. */
   if (runtime->is_webm && runtime->decode_skip) {
     flags = ZZ9K_MEDIA_DECODE_SKIP_TO_KEYFRAME;
   }
   status = zz9k_media_session_decode(
       runtime->ctx, runtime->session, flags, result);
   if (status == ZZ9K_STATUS_OK &&
       (result->flags & ZZ9K_MEDIA_SESSION_RESULT_FRAME_HELD) != 0U) {
     runtime->decode_skip = 0U;
   }
   return status;
 }

static int zzplay_ax_bind_call(
    void *user, uint32_t session, uint32_t flags,
    ZZ9KMediaSessionAudioResult *result)
{
  struct ZZPlayRuntime *runtime = (struct ZZPlayRuntime *)user;

  return zz9k_media_session_audio_bind(
      runtime->ctx, session, flags, result);
}

static int zzplay_ax_unbind_call(
    void *user, uint32_t session, uint32_t flags,
    ZZ9KMediaSessionAudioResult *result)
{
  struct ZZPlayRuntime *runtime = (struct ZZPlayRuntime *)user;

  return zz9k_media_session_audio_unbind(
      runtime->ctx, session, flags, result);
}

static int zzplay_ax_status_call(
    void *user, uint32_t session, uint32_t page, uint32_t flags,
    ZZ9KMediaSessionStatusResult *result)
{
  struct ZZPlayRuntime *runtime = (struct ZZPlayRuntime *)user;

  return zz9k_media_session_status(
      runtime->ctx, session, page, flags, result);
}

static const ZZPlayAXControlOps zzplay_ax_control_ops = {
  zzplay_ax_bind_call,
  zzplay_ax_unbind_call,
  zzplay_ax_status_call
};

static const char *zzplay_audio_backend_name(ZZPlayAudioBackend backend)
{
  switch (backend) {
    case ZZPLAY_AUDIO_AHI:
      return "AHI";
    case ZZPLAY_AUDIO_MHI:
      return "MHI";
    case ZZPLAY_AUDIO_AX:
      return "direct AX";
    case ZZPLAY_AUDIO_NONE:
      return "disabled";
    case ZZPLAY_AUDIO_AUTO:
    default:
      return "AUTO";
  }
}

static void zzplay_engine_refresh_output(struct ZZPlayRuntime *runtime)
{
  char output[ZZPLAY_NOW_OUTPUT_MAX];

  if (runtime->audio_backend == ZZPLAY_AUDIO_AX) {
    strcpy(output, "ZZ9000AX direct");
  } else if (runtime->audio_backend == ZZPLAY_AUDIO_AHI) {
    sprintf(output, "AHI unit %lu",
            (unsigned long)runtime->prefs.ahi_unit);
  } else {
    strcpy(output, "no audio");
  }
  if (runtime->ctl->volume == 0U &&
      runtime->ctl->now.volume_supported) {
    strcat(output, ", muted");
  }
  zzplay_controller_set_output(runtime->ctl, output);
}

/* Switch the item's audio backend, keeping what the player shows - the
 * output name and whether the volume slider works - in step with it. */
static void zzplay_engine_set_backend(struct ZZPlayRuntime *runtime,
                                      ZZPlayAudioBackend backend)
{
  runtime->audio_backend = backend;
  zzplay_controller_set_capabilities(
      runtime->ctl, 0, backend == ZZPLAY_AUDIO_AHI, 1);
  zzplay_engine_refresh_output(runtime);
}

static int zzplay_audio_prepare_from_result(
    struct ZZPlayRuntime *runtime,
    const ZZ9KMediaSessionAudioResult *audio)
{
  uint32_t period_frames;
  int status;

  if (runtime->audio_prepared) {
    if (runtime->audio_backend == ZZPLAY_AUDIO_AX) {
      return zzplay_ax_prepare(&runtime->ax, audio);
    }
    return audio->sample_rate == runtime->ahi.sample_rate &&
                   audio->channels == runtime->ahi.channels &&
                   audio->sample_format ==
                       ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE
               ? ZZ9K_STATUS_OK
               : ZZ9K_STATUS_UNSUPPORTED;
  }
  if (audio->sample_rate == 0U) {
    return ZZ9K_STATUS_OK;
  }
  if ((audio->channels != 1U && audio->channels != 2U) ||
      audio->sample_format != ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE) {
    return ZZ9K_STATUS_UNSUPPORTED;
  }
  if (runtime->audio_backend == ZZPLAY_AUDIO_AX) {
    status = zzplay_ax_prepare(&runtime->ax, audio);
    if (status != ZZ9K_STATUS_OK) {
      return status;
    }
    runtime->audio_prepared = 1U;
    (void)zzplay_resource_acquire(
        &runtime->core.resources, ZZPLAY_RESOURCE_AUDIO_SINK);
    zzplay_info("zzplay: audio path MP2 decode -> card-local AX DMA, "
           "%lu Hz, %lu channel%s\n",
           (unsigned long)audio->sample_rate,
           (unsigned long)audio->channels,
           audio->channels == 1U ? "" : "s");
    return ZZ9K_STATUS_OK;
  }
  if (runtime->audio_backend != ZZPLAY_AUDIO_AHI) {
    return ZZ9K_STATUS_UNSUPPORTED;
  }
  period_frames =
      audio->sample_rate / ZZPLAY_AHI_PERIODS_PER_SECOND;
  if (period_frames == 0U) {
    period_frames = 1U;
  }
  if (!zzplay_ahi_prepare(
          &runtime->ahi, runtime->prefs.ahi_unit, audio->sample_rate,
          audio->channels, period_frames)) {
    if (runtime->audio_strict || !runtime->ax_available) {
      return ZZ9K_STATUS_IO_ERROR;
    }
    /* A saved AHI output whose unit cannot be opened falls back like
     * AUTO to the card's direct AX output. */
    zzplay_info("zzplay: AHI unit %lu unavailable; falling back to "
           "direct AX\n", (unsigned long)runtime->prefs.ahi_unit);
    zzplay_engine_set_backend(runtime, ZZPLAY_AUDIO_AX);
    zzplay_ax_init(&runtime->ax, runtime->session,
                   &zzplay_ax_control_ops, runtime);
    return zzplay_audio_prepare_from_result(runtime, audio);
  }
  /* The controller's volume applies from the first queued request on. */
  zzplay_ahi_set_volume(&runtime->ahi, runtime->ctl->volume);
  runtime->audio_prepared = 1U;
  (void)zzplay_resource_acquire(
      &runtime->core.resources, ZZPLAY_RESOURCE_AUDIO_SINK);
  zzplay_info("zzplay: audio path MP2 decode -> AHI S16BE, "
         "%lu Hz, %lu channel%s\n",
         (unsigned long)audio->sample_rate,
         (unsigned long)audio->channels,
         audio->channels == 1U ? "" : "s");
  return ZZ9K_STATUS_OK;
}

static int zzplay_audio_result_valid(
    const struct ZZPlayRuntime *runtime,
    const ZZ9KMediaSessionAudioResult *audio)
{
  if (audio->session != runtime->session ||
      audio->pcm_produced < audio->pcm_acknowledged ||
      audio->pcm_produced - audio->pcm_acknowledged >
          runtime->pcm_ring.capacity) {
    return 0;
  }
  return runtime->audio_backend != ZZPLAY_AUDIO_AHI ||
         audio->pcm_acknowledged ==
             runtime->pcm_ring.acknowledged;
}

static int zzplay_audio_query_origin(struct ZZPlayRuntime *runtime)
{
  ZZ9KMediaSessionStatusResult status_result;
  int status;

  if (runtime->audio_origin_pts != ZZ9K_MEDIA_NO_PTS) {
    return ZZ9K_STATUS_OK;
  }
  status = zz9k_media_session_status(
      runtime->ctx, runtime->session, ZZ9K_MEDIA_STATUS_AUDIO,
      0U, &status_result);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  runtime->audio_origin_pts = status_result.value[3];
  return ZZ9K_STATUS_OK;
}

static int zzplay_audio_read(struct ZZPlayRuntime *runtime,
                             uint64_t acknowledged,
                             ZZ9KMediaSessionAudioResult *audio)
{
  TimeVal_Type started;
  int status;

  zzplay_profile_begin(runtime, &started);
  status = zz9k_media_session_audio_read(
      runtime->ctx, runtime->session, acknowledged, 0U, audio);
  zzplay_profile_end(
      runtime, &started, ZZPLAY_PROFILE_AUDIO_READ);
  return status;
}

static uint64_t zzplay_audio_prebuffered_frames(
    const struct ZZPlayRuntime *runtime)
{
  uint32_t frame_bytes;

  if (!runtime->audio_prepared ||
      runtime->audio_result.pcm_produced <
          runtime->audio_result.pcm_acknowledged) {
    return 0U;
  }
  frame_bytes = runtime->audio_result.channels * 2U;
  return frame_bytes == 0U
             ? 0U
             : (runtime->audio_result.pcm_produced -
                runtime->audio_result.pcm_acknowledged) /
                   frame_bytes;
}

static uint64_t zzplay_audio_queued_frames(
    const struct ZZPlayRuntime *runtime)
{
  if (runtime->audio_backend == ZZPLAY_AUDIO_AX) {
    return runtime->audio_started
               ? zzplay_ax_queued_frames(&runtime->ax)
               : zzplay_audio_prebuffered_frames(runtime);
  }
  return zzplay_ahi_queued_frames(&runtime->ahi);
}

static uint64_t zzplay_audio_low_water_frames(
    const struct ZZPlayRuntime *runtime)
{
  if (runtime->audio_backend != ZZPLAY_AUDIO_AX ||
      !runtime->audio_started || runtime->ax.sample_rate == 0U) {
    return 0U;
  }
  return runtime->ax.sample_rate / 50U;
}

static uint64_t zzplay_audio_master_pts(
    struct ZZPlayRuntime *runtime)
{
  if (runtime->audio_backend == ZZPLAY_AUDIO_AX) {
    return zzplay_ax_clock_pts(
        &runtime->ax, runtime->audio_origin_pts);
  }
  zzplay_audio_clock_update_presentation(
      &runtime->ahi.clock, zzplay_now_us());
  return zzplay_audio_clock_presentation_pts(
      &runtime->ahi.clock, runtime->audio_origin_pts);
}

static uint64_t zzplay_audio_played_frames(
    const struct ZZPlayRuntime *runtime)
{
  return runtime->audio_backend == ZZPLAY_AUDIO_AX
             ? zzplay_ax_played_frames(&runtime->ax)
             : zzplay_ahi_played_frames(&runtime->ahi);
}

static uint32_t zzplay_audio_underruns(
    const struct ZZPlayRuntime *runtime)
{
  return runtime->audio_backend == ZZPLAY_AUDIO_AX
             ? runtime->ax.underruns
             : runtime->ahi.clock.underruns;
}

/* Trace lines are hand-formatted: this path must depend on nothing
 * beyond dos.library. The --trace guru (80000004, reproducible with
 * RAM: targets and absent for --fps) survived switching off stdio
 * file writes, so snprintf/vsnprintf leave the path too. */
static char *zzplay_trace_put_str(char *at, char *end,
                                  const char *text)
{
  while (*text != '\0' && at < end)
    *at++ = *text++;
  return at;
}

static char *zzplay_trace_put_u32(char *at, char *end, uint32_t value)
{
  char digits[10];
  uint32_t n = value;
  uint32_t i = 0U;

  do {
    digits[i++] = (char)('0' + n % 10U);
    n /= 10U;
  } while (n != 0U && i < sizeof(digits));
  while (i != 0U && at < end)
    *at++ = digits[--i];
  return at;
}

static char *zzplay_trace_put_i32(char *at, char *end, int32_t value)
{
  uint32_t magnitude;

  if (value < 0 && at < end) {
    *at++ = '-';
    magnitude = (uint32_t)(-(value + 1)) + 1U;
  } else {
    magnitude = (uint32_t)value;
  }
  return zzplay_trace_put_u32(at, end, magnitude);
}

static void zzplay_trace_write(struct ZZPlayRuntime *runtime,
                               const char *line, uint32_t bytes)
{
  if (runtime->trace && bytes != 0U)
    (void)Write(runtime->trace, (APTR)line, (LONG)bytes);
}

/* Milliseconds since the trace clock anchored. The anchor is taken
 * lazily on first use: the trace file opens before zzplay_timer_open
 * has set TimerBase, and a GetSysTime through a NULL device base is
 * what gurud the machine instantly with --trace. */
static uint32_t zzplay_trace_ms(struct ZZPlayRuntime *runtime)
{
  TimeVal_Type now;

  GetSysTime(&now);
  if (!runtime->trace_anchored) {
    runtime->trace_started = now;
    runtime->trace_last_frame = now;
    runtime->trace_anchored = 1U;
  }
  return zzplay_elapsed_us(&runtime->trace_started, &now) / 1000U;
}

static void zzplay_trace_event(struct ZZPlayRuntime *runtime,
                               const char *label, uint32_t add,
                               uint32_t total, int have_counts)
{
  char line[128];
  char *at = line;
  char *end = line + sizeof(line) - 1U;

  if (!runtime->trace) {
    return;
  }
  at = zzplay_trace_put_str(at, end, "S ");
  at = zzplay_trace_put_u32(at, end, zzplay_trace_ms(runtime));
  at = zzplay_trace_put_str(at, end, " ");
  at = zzplay_trace_put_str(at, end, label);
  if (have_counts) {
    at = zzplay_trace_put_str(at, end, " +");
    at = zzplay_trace_put_u32(at, end, add);
    at = zzplay_trace_put_str(at, end, " (total ");
    at = zzplay_trace_put_u32(at, end, total);
    at = zzplay_trace_put_str(at, end, ")");
  }
  *at++ = '\n';
  zzplay_trace_write(runtime, line, (uint32_t)(at - line));
}

/* One line per presented or discarded frame. The counters (acc, ni, wb,
 * db, rd, rmax) cover everything since the previous F line, so a stall
 * shows up as a large gap with either a large dec (decode-bound), a large
 * ni/rd/rmax with small dec (input-bound), or growing und/q (audio-bound)
 * no matter which branch of the loop caused it. */
static void zzplay_trace_frame(struct ZZPlayRuntime *runtime,
                               uint32_t decode_us)
{
  TimeVal_Type now;
  uint64_t master_pts = ZZ9K_MEDIA_NO_PTS;
  uint64_t gap_us;

  char line[256];
  char *at = line;
  char *end = line + sizeof(line) - 1U;

  if (!runtime->trace) {
    return;
  }
  GetSysTime(&now);
  if (!runtime->trace_anchored) {
    runtime->trace_started = now;
    runtime->trace_last_frame = now;
    runtime->trace_anchored = 1U;
  }
  gap_us = zzplay_elapsed_us(&runtime->trace_last_frame, &now);
  runtime->trace_last_frame = now;
  if (runtime->audio_started) {
    master_pts = zzplay_audio_master_pts(runtime);
  }
  at = zzplay_trace_put_str(at, end, "F ");
  at = zzplay_trace_put_u32(at, end, runtime->frames);
  at = zzplay_trace_put_str(at, end, " t=");
  at = zzplay_trace_put_u32(at, end, zzplay_trace_ms(runtime));
  at = zzplay_trace_put_str(at, end, " v=");
  at = zzplay_trace_put_i32(at, end,
      runtime->trace_video_pts == ZZ9K_MEDIA_NO_PTS
          ? -1L
          : (int32_t)(runtime->trace_video_pts / 90U));
  at = zzplay_trace_put_str(at, end, " m=");
  at = zzplay_trace_put_i32(at, end,
      master_pts == ZZ9K_MEDIA_NO_PTS
          ? -1L
          : (int32_t)(master_pts / 90U));
  at = zzplay_trace_put_str(at, end, " dr=");
  at = zzplay_trace_put_i32(at, end,
      (int32_t)(runtime->stats.core.current_drift_pts / 90));
  at = zzplay_trace_put_str(at, end, " d=");
  if (at < end)
    *at++ = runtime->trace_decision;
  at = zzplay_trace_put_str(at, end, " dec=");
  at = zzplay_trace_put_u32(at, end, decode_us);
  at = zzplay_trace_put_str(at, end, " gap=");
  at = zzplay_trace_put_u32(at, end, (uint32_t)gap_us);
  at = zzplay_trace_put_str(at, end, " acc=");
  at = zzplay_trace_put_u32(at, end, runtime->trace_accepted);
  at = zzplay_trace_put_str(at, end, " ni=");
  at = zzplay_trace_put_u32(at, end, runtime->trace_need_input);
  at = zzplay_trace_put_str(at, end, " wb=");
  at = zzplay_trace_put_u32(at, end, runtime->trace_write_busy);
  at = zzplay_trace_put_str(at, end, " db=");
  at = zzplay_trace_put_u32(at, end, runtime->trace_decode_busy);
  at = zzplay_trace_put_str(at, end, " rd=");
  at = zzplay_trace_put_u32(at, end, runtime->trace_reads);
  at = zzplay_trace_put_str(at, end, " rmax=");
  at = zzplay_trace_put_u32(at, end, runtime->trace_read_max_us);
  at = zzplay_trace_put_str(at, end, " q=");
  at = zzplay_trace_put_u32(at, end,
      (uint32_t)zzplay_audio_queued_frames(runtime));
  at = zzplay_trace_put_str(at, end, " und=");
  at = zzplay_trace_put_u32(at, end, runtime->trace_underruns);
  *at++ = '\n';
  zzplay_trace_write(runtime, line, (uint32_t)(at - line));
  runtime->trace_accepted = 0U;
  runtime->trace_need_input = 0U;
  runtime->trace_write_busy = 0U;
  runtime->trace_decode_busy = 0U;
  runtime->trace_reads = 0U;
  runtime->trace_read_max_us = 0U;
}

static int zzplay_audio_fallback_to_ahi(
    struct ZZPlayRuntime *runtime, int ax_status)
{
  int status;

  if (runtime->audio_strict || runtime->ax.bound) {
    return ax_status;
  }
  status = zzplay_ax_close(&runtime->ax);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  /* AX failed for this item: an AHI failure must not bounce back to it. */
  runtime->ax_available = 0U;
  zzplay_engine_set_backend(runtime, ZZPLAY_AUDIO_AHI);
  runtime->audio_prepared = 0U;
  runtime->audio_started = 0U;
  runtime->pcm_ring.acknowledged =
      runtime->audio_result.pcm_acknowledged;
  memset(&runtime->ahi, 0, sizeof(runtime->ahi));
  zzplay_info("zzplay: direct AX unavailable (%s); "
         "AUTO falling back to AHI\n",
         zz9k_status_name(ax_status));
  return zzplay_audio_prepare_from_result(
      runtime, &runtime->audio_result);
}

static int zzplay_audio_start(struct ZZPlayRuntime *runtime)
{
  int status;

  if (runtime->audio_backend == ZZPLAY_AUDIO_AX) {
    status = zzplay_ax_play(&runtime->ax);
    if (status != ZZ9K_STATUS_OK) {
      return zzplay_audio_fallback_to_ahi(runtime, status);
    }
    runtime->audio_result = runtime->ax.audio;
    runtime->audio_started = 1U;
    return ZZ9K_STATUS_OK;
  }
  if (!zzplay_ahi_play(&runtime->ahi)) {
    return ZZ9K_STATUS_IO_ERROR;
  }
  zzplay_audio_clock_start_presentation(
      &runtime->ahi.clock, zzplay_now_us());
  runtime->audio_started = 1U;
  return ZZ9K_STATUS_OK;
}

static int zzplay_audio_pause(struct ZZPlayRuntime *runtime)
{
  if (!runtime->audio_started) {
    return ZZ9K_STATUS_OK;
  }
  if (runtime->audio_backend == ZZPLAY_AUDIO_AX) {
    return zzplay_ax_pause(&runtime->ax);
  }
  zzplay_audio_clock_update_presentation(
      &runtime->ahi.clock, zzplay_now_us());
  return zzplay_ahi_pause(&runtime->ahi)
             ? ZZ9K_STATUS_OK
             : ZZ9K_STATUS_IO_ERROR;
}

static int zzplay_audio_resume(struct ZZPlayRuntime *runtime)
{
  int status;

  if (!runtime->audio_started) {
    return ZZ9K_STATUS_OK;
  }
  if (runtime->audio_backend == ZZPLAY_AUDIO_AX) {
    return zzplay_ax_resume(&runtime->ax);
  }
  status = zzplay_ahi_resume(&runtime->ahi)
               ? ZZ9K_STATUS_OK
               : ZZ9K_STATUS_IO_ERROR;
  if (status == ZZ9K_STATUS_OK) {
    zzplay_audio_clock_start_presentation(
        &runtime->ahi.clock, zzplay_now_us());
  }
  return status;
}

static int zzplay_toggle_pause(struct ZZPlayRuntime *runtime)
{
  int status;

  if (runtime->core.state == ZZPLAY_STATE_PLAYING) {
    status = zzplay_audio_pause(runtime);
    if (status != ZZ9K_STATUS_OK) {
      return status;
    }
    if (!zzplay_core_pause(&runtime->core)) {
      return ZZ9K_STATUS_INTERNAL_ERROR;
    }
    zzplay_info("zzplay: paused\n");
    return ZZ9K_STATUS_OK;
  }
  if (runtime->core.state == ZZPLAY_STATE_PAUSED) {
    status = zzplay_audio_resume(runtime);
    if (status != ZZ9K_STATUS_OK) {
      return status;
    }
    if (!zzplay_core_resume(&runtime->core)) {
      return ZZ9K_STATUS_INTERNAL_ERROR;
    }
    zzplay_info("zzplay: resumed\n");
    return ZZ9K_STATUS_OK;
  }
  return ZZ9K_STATUS_BAD_REQUEST;
}

static int zzplay_audio_pump(struct ZZPlayRuntime *runtime,
                             int flush_tail,
                             int refresh_status)
{
  ZZ9KMediaSessionAudioResult audio;
  uint64_t available;
  size_t period_bytes;
  int status;

  if (!runtime->audio_enabled) {
    return ZZ9K_STATUS_OK;
  }
  if (runtime->audio_backend == ZZPLAY_AUDIO_AX &&
      runtime->audio_started) {
    status = zzplay_ax_poll(&runtime->ax);
    if (status == ZZ9K_STATUS_OK) {
      runtime->audio_result = runtime->ax.audio;
    }
    return status;
  }
  if (runtime->audio_backend == ZZPLAY_AUDIO_AHI &&
      runtime->audio_prepared) {
    TimeVal_Type started;
    int poll_ok;

    zzplay_audio_clock_update_presentation(
        &runtime->ahi.clock, zzplay_now_us());
    zzplay_profile_begin(runtime, &started);
    poll_ok = zzplay_ahi_poll(&runtime->ahi);
    zzplay_profile_end(
        runtime, &started, ZZPLAY_PROFILE_AHI_POLL);
    if (!poll_ok) {
      return ZZ9K_STATUS_IO_ERROR;
    }
  }
  if (refresh_status || !runtime->audio_status_known) {
    status = zzplay_audio_read(
        runtime, runtime->pcm_ring.acknowledged, &audio);
    if (status != ZZ9K_STATUS_OK) {
      return status;
    }
    if (!zzplay_audio_result_valid(runtime, &audio)) {
      return ZZ9K_STATUS_INTERNAL_ERROR;
    }
    runtime->audio_result = audio;
    runtime->audio_status_known = 1U;
  }
  status = zzplay_audio_prepare_from_result(
      runtime, &runtime->audio_result);
  if (status != ZZ9K_STATUS_OK || !runtime->audio_prepared) {
    return status;
  }
  status = zzplay_audio_query_origin(runtime);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  if (runtime->audio_backend == ZZPLAY_AUDIO_AX) {
    return ZZ9K_STATUS_OK;
  }

  period_bytes =
      (size_t)runtime->ahi.period_frames * runtime->ahi.frame_bytes;
  for (;;) {
    void *destination;
    size_t capacity;
    size_t copied;
    int submitted = 0;

    available = zzplay_pcm_ring_available(
        &runtime->pcm_ring, runtime->audio_result.pcm_produced);
    if (available == 0U ||
        (!flush_tail && available < period_bytes)) {
      break;
    }
    destination = zzplay_ahi_acquire_buffer(
        &runtime->ahi, &capacity);
    if (!destination) {
      break;
    }
    {
      TimeVal_Type started;

      zzplay_profile_begin(runtime, &started);
      copied = zzplay_pcm_ring_copy(
          &runtime->pcm_ring, runtime->audio_result.pcm_produced,
          destination, capacity, runtime->ahi.frame_bytes);
      zzplay_profile_end(
          runtime, &started, ZZPLAY_PROFILE_PCM_COPY);
    }
    if (copied != 0U) {
      TimeVal_Type started;

      zzplay_profile_begin(runtime, &started);
      submitted = zzplay_ahi_submit_buffer(&runtime->ahi, copied);
      zzplay_profile_end(
          runtime, &started, ZZPLAY_PROFILE_AHI_SUBMIT);
    }
    if (copied == 0U || !submitted ||
        !zzplay_pcm_ring_acknowledge(
            &runtime->pcm_ring,
            runtime->audio_result.pcm_produced, copied)) {
      return ZZ9K_STATUS_INTERNAL_ERROR;
    }
    status = zzplay_audio_read(
        runtime, runtime->pcm_ring.acknowledged, &audio);
    if (status != ZZ9K_STATUS_OK) {
      return status;
    }
    if (!zzplay_audio_result_valid(runtime, &audio)) {
      return ZZ9K_STATUS_INTERNAL_ERROR;
    }
    runtime->audio_result = audio;
    runtime->audio_status_known = 1U;
  }
  return ZZ9K_STATUS_OK;
}

static uint32_t zzplay_sync_wait_us(int64_t drift_pts,
                                    uint64_t hold_ahead_pts)
{
  uint64_t excess;
  uint64_t usec;

  if (drift_pts <= 0 ||
      (uint64_t)drift_pts <= hold_ahead_pts) {
    return 0U;
  }
  excess = (uint64_t)drift_pts - hold_ahead_pts;
  if (excess >=
      ((uint64_t)ZZPLAY_SYNC_POLL_US * 90000U) / 1000000U) {
    return ZZPLAY_SYNC_POLL_US;
  }
  usec = excess * 1000000ULL / 90000U;
  if (usec < ZZPLAY_SYNC_POLL_US) {
    return (uint32_t)usec;
  }
  return ZZPLAY_SYNC_POLL_US;
}

static int zzplay_retire_held_frame(
    struct ZZPlayRuntime *runtime,
    ZZ9KMediaSessionMainResult *result,
    uint32_t frame_period_us,
    uint32_t decode_us,
    int *retired)
{
  ZZPlaySyncDecision decision = ZZPLAY_SYNC_PRESENT;
  uint64_t queued_audio_frames =
      zzplay_audio_queued_frames(runtime);
  int64_t drift = 0;
  int status;

  *retired = 0;
  if (!runtime->audio_started && runtime->audio_prepared &&
      zzplay_audio_start_ready(
          runtime->audio_backend, queued_audio_frames,
          runtime->ahi.clock.queue_limit_frames)) {
    if (zzplay_sync_audio_may_start(
            result->video_pts, runtime->audio_origin_pts,
            runtime->sync_policy.drop_late_pts)) {
      if (!zzplay_ensure_pip(runtime)) {
        return ZZ9K_STATUS_UNSUPPORTED;
      }
      status = zzplay_audio_start(runtime);
      if (status != ZZ9K_STATUS_OK) {
        return status;
      }
      if (!runtime->audio_started) {
        /* AUTO selected AX but ownership was busy. The fallback AHI sink
         * is prepared now; keep this decoder-owned frame held while the
         * next loop copies its initial prebuffer. */
        return ZZ9K_STATUS_OK;
      }
    }
  }
  if (!runtime->options.uncapped && runtime->audio_enabled &&
      runtime->audio_started &&
      runtime->audio_origin_pts != ZZ9K_MEDIA_NO_PTS &&
      result->video_pts != ZZ9K_MEDIA_NO_PTS) {
    uint64_t master_pts = zzplay_audio_master_pts(runtime);

    decision = zzplay_sync_decide(
        &runtime->sync_policy, result->video_pts, master_pts,
        &drift);
    decision = zzplay_sync_resolve_audio_starvation(
        decision, zzplay_audio_queued_frames(runtime),
        zzplay_audio_low_water_frames(runtime));
    if (decision == ZZPLAY_SYNC_HOLD) {
      runtime->trace_decision = 'H';
      zzplay_stats_record_sync(
          &runtime->stats.core, decision, drift);
      zzplay_wait_us(
          &runtime->timer,
          zzplay_sync_wait_us(
              drift, runtime->sync_policy.hold_ahead_pts));
      return ZZ9K_STATUS_OK;
    }
  } else if (!runtime->options.uncapped) {
    runtime->trace_decision = 'N';
    zzplay_wait_us(
        &runtime->timer,
        zzplay_pacing_wait_us(
            frame_period_us, decode_us, 0));
  }

  if (decision != ZZPLAY_SYNC_DISCARD &&
      !zzplay_ensure_pip(runtime)) {
    return ZZ9K_STATUS_UNSUPPORTED;
  }
  {
    TimeVal_Type started;

    zzplay_profile_begin(runtime, &started);
    if (runtime->is_webm && decision == ZZPLAY_SYNC_DISCARD &&
        zzplay_sync_needs_keyframe_skip(drift)) {
      runtime->decode_skip = 1U;
    }
    if (decision == ZZPLAY_SYNC_DISCARD) {
      status = zz9k_media_session_discard(
          runtime->ctx, runtime->session, 0U, result);
    } else {
      status = zz9k_media_session_present(
          runtime->ctx, runtime->session, 0U, result);
    }
    zzplay_profile_end(
        runtime, &started, ZZPLAY_PROFILE_SDK_RETIRE);
  }
  if (status == ZZ9K_STATUS_OK) {
    runtime->trace_decision =
        decision == ZZPLAY_SYNC_DISCARD ? 'D' : 'P';
    zzplay_stats_record_sync(
        &runtime->stats.core, decision, drift);
  }
  if (status == ZZ9K_STATUS_BUSY) {
    zzplay_wait_us(&runtime->timer, ZZPLAY_SYNC_POLL_US);
    return ZZ9K_STATUS_OK;
  }
  if (status == ZZ9K_STATUS_OK) {
    *retired = 1;
  }
  return status;
}

/* Drain is bounded: end-of-stream must never require the user to
 * close a frozen window. The AX drained flag depends on the card
 * publishing AUDIO_DRAINED after a full TX ring of committed silence;
 * if that never arrives (or arrives late), waiting forever just
 * accumulates pump underruns -- one per 20 ms -- while the picture is
 * already over. Two seconds covers a full TX ring of silence several
 * times over; on expiry the drain is treated as complete. */
#define ZZPLAY_DRAIN_DEADLINE_US 2000000U

static int zzplay_drain_audio(struct ZZPlayRuntime *runtime)
{
  int draining = 0;
  int refresh_status = 1;
  TimeVal_Type drain_started;

  if (!runtime->audio_enabled) {
    return ZZ9K_STATUS_OK;
  }
  GetSysTime(&drain_started);
  if (runtime->audio_backend == ZZPLAY_AUDIO_AX) {
    for (;;) {
      TimeVal_Type now;
      int status = zzplay_audio_pump(
          runtime, 1, refresh_status);

      if (status != ZZ9K_STATUS_OK) {
        return status;
      }
      refresh_status = 0;
      if (!runtime->audio_prepared &&
          runtime->audio_result.pcm_produced ==
              runtime->audio_result.pcm_acknowledged) {
        return ZZ9K_STATUS_OK;
      }
      if (!runtime->audio_started &&
          zzplay_audio_prebuffered_frames(runtime) != 0U) {
        status = zzplay_audio_start(runtime);
        if (status != ZZ9K_STATUS_OK) {
          return status;
        }
        if (runtime->audio_backend != ZZPLAY_AUDIO_AX) {
          return zzplay_drain_audio(runtime);
        }
      }
      if (runtime->audio_started && !runtime->ax.draining) {
        status = zzplay_ax_begin_drain(&runtime->ax);
        if (status != ZZ9K_STATUS_OK) {
          return status;
        }
      }
      if (runtime->audio_started &&
          zzplay_ax_drained(&runtime->ax)) {
        return ZZ9K_STATUS_OK;
      }
      GetSysTime(&now);
      if (zzplay_elapsed_us(&drain_started, &now) >=
          ZZPLAY_DRAIN_DEADLINE_US) {
        return ZZ9K_STATUS_OK;
      }
      if (zzplay_engine_service_input(runtime, 0)) {
        zzplay_engine_stop_from_request(runtime);
        return ZZ9K_STATUS_CANCELLED;
      }
      zzplay_wait_us(&runtime->timer, ZZPLAY_SYNC_POLL_US);
    }
  }
  for (;;) {
    TimeVal_Type now;
    int status = zzplay_audio_pump(
        runtime, 1, refresh_status);

    if (status != ZZ9K_STATUS_OK) {
      return status;
    }
    refresh_status = 0;
    if (runtime->audio_prepared &&
        runtime->pcm_ring.acknowledged ==
            runtime->audio_result.pcm_produced) {
      zzplay_ahi_mark_end_of_stream(&runtime->ahi);
    }
    if (!runtime->audio_started && runtime->audio_prepared &&
        runtime->ahi.clock.queued_frames != 0U) {
      int start_status = zzplay_audio_start(runtime);

      if (start_status != ZZ9K_STATUS_OK) {
        return start_status;
      }
    }
    if (runtime->pcm_ring.acknowledged ==
            runtime->audio_result.pcm_produced &&
        !draining) {
      if (!runtime->audio_started) {
        if (runtime->ahi.clock.queued_frames == 0U) {
          return ZZ9K_STATUS_OK;
        }
        {
          int start_status = zzplay_audio_start(runtime);

          if (start_status != ZZ9K_STATUS_OK) {
            return start_status;
          }
        }
      }
      if (!zzplay_ahi_begin_drain(&runtime->ahi)) {
        return ZZ9K_STATUS_IO_ERROR;
      }
      draining = 1;
    }
    if (draining && zzplay_ahi_drained(&runtime->ahi)) {
      return ZZ9K_STATUS_OK;
    }
    GetSysTime(&now);
    if (zzplay_elapsed_us(&drain_started, &now) >=
        ZZPLAY_DRAIN_DEADLINE_US) {
      return ZZ9K_STATUS_OK;
    }
    if (zzplay_engine_service_input(runtime, 0)) {
      zzplay_engine_stop_from_request(runtime);
      return ZZ9K_STATUS_CANCELLED;
    }
    zzplay_wait_us(&runtime->timer, ZZPLAY_SYNC_POLL_US);
  }
}

static void zzplay_fail(struct ZZPlayRuntime *runtime,
                        ZZPlayFailure failure,
                        int status)
{
  zzplay_core_fail(&runtime->core, failure, status);
}

static int zzplay_release_resource(void *user,
                                   ZZPlayResource resource)
{
  struct ZZPlayRuntime *runtime = (struct ZZPlayRuntime *)user;

  switch (resource) {
    case ZZPLAY_RESOURCE_TIMER:
      zzplay_timer_close(&runtime->timer);
      break;
    case ZZPLAY_RESOURCE_AUDIO_SINK:
      if (runtime->audio_backend == ZZPLAY_AUDIO_AX) {
        int status = zzplay_ax_close(&runtime->ax);

        if (status != ZZ9K_STATUS_OK) {
          return status;
        }
      } else {
        zzplay_ahi_close(&runtime->ahi);
      }
      runtime->audio_prepared = 0U;
      runtime->audio_started = 0U;
      break;
    case ZZPLAY_RESOURCE_VIDEO_SESSION:
      if (runtime->session != 0U && runtime->ctx) {
        ZZ9KMediaSessionMainResult result;
        int status;
        unsigned retry;

        if (runtime->frame_held) {
          (void)zz9k_media_session_discard(
              runtime->ctx, runtime->session, 0U, &result);
          runtime->frame_held = 0U;
        }
        /* BUSY lasts until the card's main loop hands the last presented
         * frame to the compositor; give it a tick between attempts rather
         * than spending every retry inside one loop iteration. */
        status = ZZ9K_STATUS_BUSY;
        for (retry = 0U;
             retry < 16U && status == ZZ9K_STATUS_BUSY;
             retry++) {
          if (retry != 0U) {
            Delay(1);
          }
          status = zz9k_media_session_close(
              runtime->ctx, runtime->session, 0U, &result);
        }
        if (status == ZZ9K_STATUS_OK) {
          runtime->session = 0U;
        }
        return status;
      }
      break;
    case ZZPLAY_RESOURCE_PCM_BUFFER:
      if (runtime->pcm.handle != 0U && runtime->ctx) {
        (void)zz9k_free_shared(runtime->ctx, runtime->pcm.handle);
      }
      memset(&runtime->pcm, 0, sizeof(runtime->pcm));
      memset(&runtime->pcm_ring, 0, sizeof(runtime->pcm_ring));
      break;
    case ZZPLAY_RESOURCE_INPUT_BUFFER:
      if (runtime->input.handle != 0U && runtime->ctx) {
        (void)zz9k_free_shared(runtime->ctx, runtime->input.handle);
      }
      memset(&runtime->input, 0, sizeof(runtime->input));
      break;
    case ZZPLAY_RESOURCE_SDK_CONTEXT:
      if (runtime->ctx) {
        zz9k_close(runtime->ctx);
        runtime->ctx = 0;
      }
      break;
    case ZZPLAY_RESOURCE_VIDEO_WINDOW:
      if (runtime->window) {
        p96PIP_Close(runtime->window);
        runtime->window = 0;
        runtime->bitmap = 0;
      }
      break;
    case ZZPLAY_RESOURCE_VIDEO_SCREEN:
      /* Always after the window: the PIP lives on this screen. The resource
       * order guarantees it, since release runs from the highest index down
       * and the screen sits below the window. */
      if (runtime->screen) {
        p96CloseScreen(runtime->screen);
        runtime->screen = 0;
      }
      break;
    case ZZPLAY_RESOURCE_P96_LIBRARY:
      if (P96Base) {
        CloseLibrary(P96Base);
        P96Base = 0;
      }
      break;
    case ZZPLAY_RESOURCE_INPUT_FILE:
      if (runtime->file) {
        (void)fclose(runtime->file);
        runtime->file = 0;
      }
      break;
    default:
      return ZZ9K_STATUS_BAD_REQUEST;
  }
  return ZZ9K_STATUS_OK;
}

static void zzplay_capture_audio_totals(
    struct ZZPlayRuntime *runtime)
{
  uint32_t underruns;

  if (!runtime->audio_prepared ||
      runtime->audio_totals_captured) {
    return;
  }
  runtime->final_audio_frames +=
      zzplay_audio_played_frames(runtime);
  underruns = zzplay_audio_underruns(runtime);
  if (runtime->final_underruns > UINT32_MAX - underruns) {
    runtime->final_underruns = UINT32_MAX;
  } else {
    runtime->final_underruns += underruns;
  }
  runtime->audio_totals_captured = 1U;
}

static uint32_t zzplay_pcm_low_water(uint32_t capacity)
{
  uint32_t low = ZZPLAY_PCM_LOW_WATER;

  if (low >= capacity) {
    low = capacity / 2U;
  }
  return low;
}

static uint32_t zzplay_pcm_high_water(uint32_t capacity)
{
  uint32_t high = ZZPLAY_PCM_HIGH_WATER;
  uint32_t compact = capacity - capacity / 4U;

  if (high > compact) {
    high = compact;
  }
  return high;
}

static int zzplay_begin_session(struct ZZPlayRuntime *runtime)
{
  ZZ9KMediaSessionBeginDesc begin;
  ZZ9KMediaSessionMainResult result;
  int status;

  memset(&begin, 0, sizeof(begin));
  memset(&result, 0, sizeof(result));
  memset(&runtime->audio_result, 0, sizeof(runtime->audio_result));
  runtime->audio_origin_pts = ZZ9K_MEDIA_NO_PTS;
  runtime->audio_prepared = 0U;
  runtime->audio_started = 0U;
  runtime->audio_status_known = 0U;
  runtime->audio_refresh_needed = runtime->audio_enabled;
  runtime->audio_totals_captured = 0U;
  runtime->frame_held = 0U;
  if (runtime->audio_enabled) {
    zzplay_pcm_ring_init(&runtime->pcm_ring, &runtime->pcm);
  }

   begin.video_codec = runtime->begin_video_codec;
   begin.container = runtime->begin_container;
   begin.width = runtime->video_info.width;
   begin.height = runtime->video_info.height;
   begin.output_format = ZZ9K_VIDEO_OUTPUT_DIRECT_OVERLAY;
   begin.audio_codec = runtime->audio_enabled
                           ? runtime->begin_audio_codec
                           : ZZ9K_MEDIA_AUDIO_NONE;
  if (runtime->audio_enabled) {
    begin.pcm_ring_handle = runtime->pcm.handle;
    begin.pcm_ring_capacity = runtime->pcm.length;
    begin.pcm_low_water_bytes = zzplay_pcm_low_water(runtime->pcm.length);
    begin.pcm_high_water_bytes = zzplay_pcm_high_water(runtime->pcm.length);
  }
  status = zz9k_media_session_begin(runtime->ctx, &begin, &result);
  if (result.session != 0U) {
    runtime->session = result.session;
    (void)zzplay_resource_acquire(
        &runtime->core.resources, ZZPLAY_RESOURCE_VIDEO_SESSION);
  }
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  if (runtime->audio_backend == ZZPLAY_AUDIO_AX) {
    zzplay_ax_init(
        &runtime->ax, runtime->session,
        &zzplay_ax_control_ops, runtime);
  }
  return ZZ9K_STATUS_OK;
}

static int zzplay_restart_session(struct ZZPlayRuntime *runtime,
                                  ZZPlayTransport *transport)
{
  int status;

  zzplay_capture_audio_totals(runtime);
  status = zzplay_resource_release(
      &runtime->core.resources, ZZPLAY_RESOURCE_AUDIO_SINK,
      zzplay_release_resource, runtime);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = zzplay_resource_release(
      &runtime->core.resources, ZZPLAY_RESOURCE_VIDEO_SESSION,
      zzplay_release_resource, runtime);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  if (fseek(runtime->file, 0L, SEEK_SET) != 0) {
    return ZZ9K_STATUS_IO_ERROR;
  }
  zzplay_transport_init(transport);
  return zzplay_begin_session(runtime);
}

/* One feed round: a 16 KB file read into the card-visible input buffer
 * plus the matching WRITE, or the continuation/EOF bookkeeping when the
 * previous round left work. Split out of the main loop so a pass can top
 * the card's input ring up while the loop is between frames; one chunk
 * per frame pass capped the sustained feed at ~400 KB/s (16 KB x 25 fps),
 * which starved decode whenever the stream's local bitrate exceeded it.
 * The m68k is the present clock, so rounds stay bounded and reads stay
 * sub-frame (ZZPLAY_READ_CHUNK_BYTES). */
typedef enum ZZPlayFeedResult {
  ZZPLAY_FEED_MORE = 0,        /* chunk fully accepted; ring may take more */
  ZZPLAY_FEED_BACKPRESSURE,    /* ring full or almost full (partial accept) */
  ZZPLAY_FEED_DONE,            /* nothing left to feed (EOF sent) */
  ZZPLAY_FEED_ERROR            /* I/O or protocol failure; runtime failed */
} ZZPlayFeedResult;

static ZZPlayFeedResult zzplay_feed_round(
    struct ZZPlayRuntime *runtime, ZZPlayTransport *transport,
    ZZ9KMediaSessionMainResult *result)
{
  ZZ9KMediaSessionWriteDesc write;
  int status;

  if (transport->pending_length == 0U && !transport->eof) {
    TimeVal_Type started;
    TimeVal_Type read_started;
    TimeVal_Type read_ended;
    size_t read_capacity = runtime->input.length;
    size_t got;

    if (read_capacity > ZZPLAY_READ_CHUNK_BYTES) {
      read_capacity = ZZPLAY_READ_CHUNK_BYTES;
    }
    /* Read straight into the card-visible input buffer: the previous
     * chunk was fully accepted (pending is empty), so offset zero is
     * free. The write op below orders these stores to the card before
     * the firmware can read them, exactly as the old staging copy
     * did. */
    zzplay_profile_begin(runtime, &started);
    GetSysTime(&read_started);
    got = fread((void *)runtime->input.data, 1U,
                read_capacity, runtime->file);
    GetSysTime(&read_ended);
    zzplay_profile_end(
        runtime, &started, ZZPLAY_PROFILE_FILE_READ);
    if (runtime->trace) {
      uint32_t read_us =
          zzplay_elapsed_us(&read_started, &read_ended);

      runtime->trace_reads++;
      if (read_us > runtime->trace_read_max_us) {
        runtime->trace_read_max_us = read_us;
      }
    }
    if (ferror(runtime->file)) {
      zzplay_error(runtime, "zzplay: input read failed\n");
      zzplay_fail(runtime, ZZPLAY_FAILURE_IO, ZZ9K_STATUS_IO_ERROR);
      return ZZPLAY_FEED_ERROR;
    }
    zzplay_transport_set_chunk(
        transport, (uint32_t)got, got < read_capacity);
  }

  if (transport->pending_length == 0U &&
      !(transport->eof && !transport->eof_sent)) {
    return ZZPLAY_FEED_DONE;
  }
  memset(&write, 0, sizeof(write));
  write.session = runtime->session;
  write.src_handle = runtime->input.handle;
  write.src_offset = transport->pending_offset;
  write.src_length = transport->pending_length;
  write.flags = zzplay_transport_write_flags(transport);
  {
    TimeVal_Type started;

    zzplay_profile_begin(runtime, &started);
    status = zz9k_media_session_write(runtime->ctx, &write, result);
    zzplay_profile_end(
        runtime, &started, ZZPLAY_PROFILE_SDK_WRITE);
  }
  if (runtime->audio_enabled) {
    runtime->audio_refresh_needed = 1U;
  }
  if (runtime->trace) {
    if (status == ZZ9K_STATUS_BUSY) {
      runtime->trace_write_busy++;
    } else {
      runtime->trace_accepted += result->bytes_written;
    }
  }
  if (status != ZZ9K_STATUS_OK && status != ZZ9K_STATUS_BUSY) {
    if (runtime->is_webm && status == ZZ9K_STATUS_UNSUPPORTED) {
      zzplay_error(runtime, "zzplay: this WebM stream is not supported\n");
    } else {
      zzplay_error(runtime, "zzplay: stream write failed: %s\n",
                   zz9k_status_name(status));
    }
    zzplay_fail(runtime, ZZPLAY_FAILURE_IO, status);
    return ZZPLAY_FEED_ERROR;
  }
  if (status == ZZ9K_STATUS_BUSY) {
    return ZZPLAY_FEED_BACKPRESSURE;
  }
  if (write.src_length != 0U) {
    if (!zzplay_transport_advance(
            transport, result->bytes_accepted)) {
      zzplay_error(runtime,
                   "zzplay: firmware reported invalid input "
                   "progress\n");
      zzplay_fail(runtime, ZZPLAY_FAILURE_PROTOCOL,
                  ZZ9K_STATUS_INTERNAL_ERROR);
      return ZZPLAY_FEED_ERROR;
    }
    if (transport->pending_length != 0U) {
      return ZZPLAY_FEED_BACKPRESSURE;
    }
    if (transport->eof) {
      return ZZPLAY_FEED_DONE; /* short read fully staged; EOF next */
    }
    return ZZPLAY_FEED_MORE;
  }
  transport->eof_sent = 1;
  return ZZPLAY_FEED_DONE;
}

/* The large statics live in static storage, not on the shell's stack: a
 * caller-provided stack plus a DOS write path (the trace's first Write)
 * overflowed it on hardware and faulted inside the handler -- instant
 * guru before the window opened, with the trace file created but empty.
 * Plain and --fps runs never enter a file-write path, which is why only
 * --trace died. The runtime is reset per item; the app state is not. */
static struct ZZPlayRuntime runtime;
static ZZPlayLaunch launch;
static ZZPlayProbeInfo probe;
static ZZPlayVideoInfo info;
static ZZPlayTransport transport;

/* ------------------------------------------------------------------ */
/* Application: playlist, settings, request routing.                    */
/* ------------------------------------------------------------------ */

/* What the application loop should do next. */
typedef enum ZZPlayAppStep {
  ZZPLAY_APP_START,         /* play the current (or first) entry */
  ZZPLAY_APP_AUTO,          /* advance after EOF or failure */
  ZZPLAY_APP_USER_NEXT,     /* the user asked for next/previous */
  ZZPLAY_APP_USER_PREVIOUS,
  ZZPLAY_APP_JUMP,
  ZZPLAY_APP_IDLE,          /* player mode: wait for input */
  ZZPLAY_APP_EXIT,          /* leave the program normally */
  ZZPLAY_APP_QUIT           /* leave the program on a quit request */
} ZZPlayAppStep;

static void zzplay_app_after_input(ZZPlayApp *app)
{
  if (app->ctl.settings_changed) {
    app->ctl.settings_changed = 0;
    /* Device settings changed: entries that failed on the old output
     * deserve another chance on the new one. */
    zzplay_playlist_clear_failed(&app->playlist);
  }
  /* Repeat/shuffle may change from the video window's keys with no player
   * window open to sync them, and AUTO advance reads the playlist's copy. */
  if (app->playlist.repeat != app->ctl.repeat) {
    zzplay_playlist_set_repeat(&app->playlist, app->ctl.repeat);
  }
  if ((app->playlist.shuffle != 0) != (app->ctl.shuffle != 0)) {
    zzplay_playlist_set_shuffle(&app->playlist, app->ctl.shuffle, 0U);
  }
}

/* The generic pump samples Ctrl-C. MPEG has already consumed it with PIP
 * input, so its hot path calls zzplay_app_after_input() directly. */
static void zzplay_app_pump(void *user)
{
  ZZPlayApp *app = (ZZPlayApp *)user;

  if (zzplay_ctrl_c_requested != 0 ||
      (SetSignal(0L, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) != 0U) {
    zzplay_ctrl_c_requested = 1;
    (void)zzplay_controller_request(&app->ctl, ZZPLAY_REQUEST_QUIT);
  }
  zzplay_gui_poll();
  zzplay_app_after_input(app);
}

static void zzplay_app_open_gui(ZZPlayApp *app)
{
  ZZPlayGuiContext context;

  if (zzplay_gui_is_open()) {
    return;
  }
  context.controller = &app->ctl;
  context.playlist = &app->playlist;
  context.prefs = &app->prefs;
  context.version = zzplay_version + 6;
  if (!zzplay_gui_open(&context)) {
    /* Headless fallback: playback still works, and the message line
     * state is kept for the day the window does open. */
    zzplay_controller_set_message(&app->ctl,
                                  "cannot open the player window");
    zzplay_launch_report(&app->options,
                         "cannot open the player window; "
                         "continuing without it");
  }
}

/* ------------------------------------------------------------------ */
/* MPEG-1 Program Stream engine (the former main() playback body).     */
/* ------------------------------------------------------------------ */

/* PIP input plus the GUI pump, shared by the playback loop and the drain
 * wait. Returns 1 when the item must end (a request is pending). */
static int zzplay_engine_service_input(struct ZZPlayRuntime *runtime,
                                       int *resized)
{
  ZZPlayControlAction control = zzplay_poll_control(runtime, resized);

  if (control != ZZPLAY_CONTROL_NONE) {
    /* PIP-window actions become controller state; the stop keys post the
     * item-ending requests the application loop then routes. */
    (void)zzplay_controller_apply(runtime->ctl, control);
  }
  zzplay_gui_poll();
  zzplay_app_after_input(&app);
  return zzplay_controller_item_should_end(runtime->ctl);
}

static void zzplay_engine_stop_from_request(struct ZZPlayRuntime *runtime)
{
  zzplay_core_stop(&runtime->core,
                   runtime->ctl->request == ZZPLAY_REQUEST_QUIT
                       ? ZZPLAY_STOP_CTRL_C
                       : ZZPLAY_STOP_WINDOW_CLOSE);
}

static int zzplay_prepare_webm(struct ZZPlayRuntime *runtime,
                               const ZZPlayEngineRun *run,
                               ZZPlayBackendDecision *decision)
{
  const ZZPlayWebMInfo *webm;
  ZZPlayMediaAudio media = ZZPLAY_MEDIA_AUDIO_NONE;
  ZZPlayAudioBackend requested;
  int strict = 0;
  uint32_t rate;
  char format[ZZPLAY_NOW_TEXT_MAX];

  if (!run->probe) {
    return 0;
  }
  webm = &run->probe->webm;
  runtime->is_webm = 1U;
  switch (webm->refusal) {
  case ZZPLAY_WEBM_OK:
    break;
  case ZZPLAY_WEBM_MATROSKA:
    zzplay_error(runtime,
                 "zzplay: Matroska files that are not WebM are not supported\n");
    goto refused;
  case ZZPLAY_WEBM_TWO_VIDEO:
    zzplay_error(runtime,
                 "zzplay: WebM files with more than one video track "
                 "are not supported\n");
    goto refused;
  case ZZPLAY_WEBM_TWO_AUDIO:
    zzplay_error(runtime,
                 "zzplay: WebM files with more than one audio track "
                 "are not supported\n");
    goto refused;
  case ZZPLAY_WEBM_UNKNOWN_CODEC:
    zzplay_error(runtime, "zzplay: unsupported WebM codec\n");
    goto refused;
  case ZZPLAY_WEBM_OVERSIZE:
    zzplay_error(runtime,
                 "zzplay: WebM video %lux%lu is larger than the card can play\n",
                 (unsigned long)webm->width, (unsigned long)webm->height);
    goto refused;
  case ZZPLAY_WEBM_TRUNCATED:
    zzplay_error(runtime, "zzplay: truncated WebM header\n");
    goto refused;
  case ZZPLAY_WEBM_NO_VIDEO:
    zzplay_error(runtime, "zzplay: WebM file has no video track\n");
    goto refused;
  case ZZPLAY_WEBM_BAD_AUDIO:
    zzplay_error(runtime,
                 "zzplay: WebM audio must be mono or stereo, and Vorbis "
                 "must be 8 to 96 kHz\n");
    goto refused;
  default:
    zzplay_error(runtime, "zzplay: WebM file is not a supported stream\n");
    goto refused;
  }
  if (webm->width == 0U || webm->height == 0U ||
      !zzplay_webm_within_cap(webm->width, webm->height)) {
    zzplay_error(runtime,
                 "zzplay: WebM video %lux%lu is larger than the card can play\n",
                 (unsigned long)webm->width, (unsigned long)webm->height);
    goto refused;
  }
  memset(&info, 0, sizeof(info));
  info.width = webm->width;
  info.height = webm->height;
  rate = webm->frame_rate_milli != 0U ? webm->frame_rate_milli : 30000U;
  info.frame_rate_milli = rate;
  runtime->video_info = info;
  zzplay_webm_window_source(webm, &runtime->display_w, &runtime->display_h);
  runtime->begin_video_codec = zzplay_webm_video_codec_id(webm->video);
  runtime->begin_container = ZZ9K_VIDEO_CONTAINER_WEBM;
  runtime->begin_audio_codec = zzplay_webm_audio_codec_id(webm->audio);
  runtime->required_extra_flags =
      webm->video == ZZPLAY_WEBM_VIDEO_VP9
          ? ZZ9K_SERVICE_FLAG_VIDEO_WEBM_VP9
          : ZZ9K_SERVICE_FLAG_VIDEO_WEBM_VP8;
  runtime->required_audio_flags =
      webm->audio == ZZPLAY_WEBM_AUDIO_OPUS
          ? ZZ9K_SERVICE_FLAG_VIDEO_MEDIA_OPUS
          : webm->audio == ZZPLAY_WEBM_AUDIO_VORBIS
                ? ZZ9K_SERVICE_FLAG_VIDEO_MEDIA_VORBIS
                : 0U;
  if (webm->audio == ZZPLAY_WEBM_AUDIO_OPUS) {
    media = ZZPLAY_MEDIA_AUDIO_OPUS;
  } else if (webm->audio == ZZPLAY_WEBM_AUDIO_VORBIS) {
    media = ZZPLAY_MEDIA_AUDIO_VORBIS;
  }
  requested = zzplay_prefs_requested_backend(
      &runtime->prefs, &runtime->options, media, &strict);
  runtime->audio_strict = (uint8_t)(strict ? 1 : 0);
  if (webm->audio == ZZPLAY_WEBM_AUDIO_NONE &&
      requested != ZZPLAY_AUDIO_AUTO && requested != ZZPLAY_AUDIO_NONE &&
      strict) {
    zzplay_error(runtime, "zzplay: the WebM file has no audio track\n");
    goto refused;
  }
  memset(decision, 0, sizeof(*decision));
  if (webm->audio != ZZPLAY_WEBM_AUDIO_NONE) {
    ZZPlayAudioAvailability availability;

    memset(&availability, 0, sizeof(availability));
    availability.ahi = ZZPLAY_BACKEND_FREE;
    availability.mhi = ZZPLAY_BACKEND_MISSING;
    availability.ax = ZZPLAY_BACKEND_FREE;
    *decision = zzplay_audio_select(media, requested, &availability);
    if (decision->status != ZZPLAY_BACKEND_OK && !strict) {
      *decision = zzplay_audio_select(
          media, ZZPLAY_AUDIO_AUTO, &availability);
    }
    if (decision->status != ZZPLAY_BACKEND_OK) {
      zzplay_error(runtime,
                   "zzplay: audio backend %s cannot play WebM %s "
                   "(status %u)\n",
                   zzplay_audio_backend_name(requested),
                   zzplay_webm_audio_name(webm->audio),
                   (unsigned)decision->status);
      zzplay_fail(runtime, ZZPLAY_FAILURE_CAPABILITY,
                  ZZ9K_STATUS_UNSUPPORTED);
      return 0;
    }
    runtime->audio_backend = decision->selected;
    runtime->audio_enabled = decision->selected != ZZPLAY_AUDIO_NONE;
  } else {
    decision->status = ZZPLAY_BACKEND_OK;
    decision->selected = ZZPLAY_AUDIO_NONE;
    runtime->audio_backend = ZZPLAY_AUDIO_NONE;
    zzplay_info("zzplay: warning: video-only WebM\n");
  }
  zzplay_info("zzplay: WebM %s %lux%lu%s\n",
              zzplay_webm_video_name(webm->video),
              (unsigned long)webm->width, (unsigned long)webm->height,
              webm->audio == ZZPLAY_WEBM_AUDIO_NONE
                  ? ", no audio"
                  : "");
  if (webm->pose_roll_present) {
    int32_t roll = webm->pose_roll_milli;
    int32_t whole;
    int32_t frac;

    if (roll < 0) {
      frac = -roll;
    } else {
      frac = roll;
    }
    whole = frac / 1000;
    frac = frac % 1000;
    zzplay_info("zzplay: WebM projection pose roll %s%ld.%03ld degrees "
                "(not applied)\n",
                roll < 0 ? "-" : "", (long)whole, (long)frac);
  }
  zzplay_controller_set_capabilities(
      runtime->ctl, 0, runtime->audio_backend == ZZPLAY_AUDIO_AHI, 1);
  if (!zzplay_webm_format_line(webm, format, sizeof(format))) {
    format[0] = '\0';
  }
  zzplay_controller_set_format(runtime->ctl, format);
  zzplay_controller_set_duration(
      runtime->ctl, webm->has_duration ? webm->duration_ms : 0U);
  if (!zzplay_webm_realtime(webm->video, webm->width, webm->height)) {
    zzplay_controller_set_message(
        runtime->ctl, "large video: frames may be skipped");
    zzplay_info("zzplay: large video: frames may be skipped\n");
  }
  zzplay_engine_refresh_output(runtime);
  return 1;

refused:
  zzplay_fail(runtime, ZZPLAY_FAILURE_INVALID_INPUT, ZZ9K_STATUS_UNSUPPORTED);
  return 0;
}

static ZZPlayEngineResult zzplay_engine_mpeg(const ZZPlayEngineRun *run)
{
  ZZ9KBoard board;
  ZZ9KCaps caps;
  ZZ9KApertureLayout aperture;
  ZZ9KServiceInfo service;
  ZZ9KMediaSessionMainResult result;
  ZZPlayBackendDecision audio_decision;
  ZZPlayAudioBackend requested;
  uint32_t frame_period_us;
  uint32_t input_bytes;
  uint32_t pcm_bytes;
  uint32_t held_decode_us = 0U;
  int media_done = 0;
  int cleanup_status;
  int strict = 0;
  int gui_open;

  /* Fresh per-item state; options, prefs and the controller belong to
   * the application and are only read (the per-item options copy holds
   * the loop counters). */
  memset(&runtime, 0, sizeof(runtime));
  memset(&board, 0, sizeof(board));
  memset(&caps, 0, sizeof(caps));
  memset(&aperture, 0, sizeof(aperture));
  memset(&result, 0, sizeof(result));
  runtime.audio_origin_pts = ZZ9K_MEDIA_NO_PTS;
  runtime.ctl = run->ctl;
  runtime.options = *run->options;
  runtime.prefs = *run->prefs;
  runtime.trace = app.trace;
  runtime.begin_video_codec = ZZ9K_VIDEO_CODEC_MPEG1;
  runtime.begin_container = ZZ9K_VIDEO_CONTAINER_MPEG_PS;
  runtime.begin_audio_codec = ZZ9K_MEDIA_AUDIO_MP2;
  runtime.required_extra_flags =
      ZZ9K_SERVICE_FLAG_VIDEO_MPEG1 | ZZ9K_SERVICE_FLAG_VIDEO_MPEG_PS;
  runtime.required_audio_flags = ZZ9K_SERVICE_FLAG_VIDEO_MEDIA_MP2;
  gui_open = zzplay_gui_is_open();
  zzplay_core_init(&runtime.core);
  zzplay_transport_init(&transport);

  runtime.file = fopen(run->path, "rb");
  if (!runtime.file) {
    zzplay_error(&runtime, "zzplay: cannot open %s\n", run->path);
    return ZZPLAY_ENGINE_FAILED;
  }
  (void)zzplay_resource_acquire(
      &runtime.core.resources, ZZPLAY_RESOURCE_INPUT_FILE);

  if (run->probe->kind == ZZPLAY_MEDIA_KIND_WEBM) {
    if (!zzplay_prepare_webm(&runtime, run, &audio_decision)) {
      goto cleanup;
    }
    strict = runtime.audio_strict;
    info = runtime.video_info;
    goto session_ready;
  }
  info = run->probe->video;
  if (!zzplay_video_info_supported(&info)) {
    zzplay_error(&runtime, "zzplay: unsupported MPEG-1 video geometry\n");
    zzplay_fail(&runtime, ZZPLAY_FAILURE_INVALID_INPUT,
                ZZ9K_STATUS_UNSUPPORTED);
    goto cleanup;
  }
  if (!info.is_program_stream || !info.has_video_pes) {
    zzplay_error(&runtime,
            "zzplay: MPEG-1 elementary streams are not supported; "
            "a Program Stream is required\n");
    zzplay_fail(&runtime, ZZPLAY_FAILURE_INVALID_INPUT,
                ZZ9K_STATUS_UNSUPPORTED);
    goto cleanup;
  }
  requested = zzplay_prefs_requested_backend(
      &runtime.prefs, &runtime.options, ZZPLAY_MEDIA_AUDIO_MP2, &strict);
  runtime.audio_strict = (uint8_t)(strict ? 1 : 0);
  if (!info.has_audio_pes &&
      requested != ZZPLAY_AUDIO_AUTO && requested != ZZPLAY_AUDIO_NONE &&
      strict) {
    zzplay_error(&runtime,
            "zzplay: the Program Stream has no supported MP2 audio\n");
    zzplay_fail(&runtime, ZZPLAY_FAILURE_INVALID_INPUT,
                ZZ9K_STATUS_UNSUPPORTED);
    goto cleanup;
  }
  runtime.video_info = info;
  if (info.has_audio_pes) {
    ZZPlayAudioAvailability availability;

    memset(&availability, 0, sizeof(availability));
    availability.ahi = ZZPLAY_BACKEND_FREE;
    availability.mhi = ZZPLAY_BACKEND_MISSING;
    availability.ax = ZZPLAY_BACKEND_FREE;
    audio_decision = zzplay_audio_select(
        ZZPLAY_MEDIA_AUDIO_MP2, requested, &availability);
    if (audio_decision.status != ZZPLAY_BACKEND_OK && !strict) {
      /* A saved (non-strict) preference that cannot play falls back
       * exactly like AUTO instead of failing the item. */
      audio_decision = zzplay_audio_select(
          ZZPLAY_MEDIA_AUDIO_MP2, ZZPLAY_AUDIO_AUTO, &availability);
    }
    if (audio_decision.status != ZZPLAY_BACKEND_OK) {
      zzplay_error(&runtime,
              "zzplay: audio backend %s cannot play Program "
              "Stream MP2 (status %u)\n",
              zzplay_audio_backend_name(requested),
              (unsigned)audio_decision.status);
      zzplay_fail(&runtime, ZZPLAY_FAILURE_CAPABILITY,
                  ZZ9K_STATUS_UNSUPPORTED);
      goto cleanup;
    }
    runtime.audio_backend = audio_decision.selected;
    runtime.audio_enabled =
        audio_decision.selected != ZZPLAY_AUDIO_NONE;
  } else {
    memset(&audio_decision, 0, sizeof(audio_decision));
    audio_decision.status = ZZPLAY_BACKEND_OK;
    audio_decision.selected = ZZPLAY_AUDIO_NONE;
    runtime.audio_backend = ZZPLAY_AUDIO_NONE;
    zzplay_info("zzplay: warning: video-only Program Stream\n");
  }
  zzplay_info("zzplay: MPEG-1/PS %lux%lu, %lu.%03lu fps, "
         "program audio %s\n",
         (unsigned long)info.width, (unsigned long)info.height,
         (unsigned long)(info.frame_rate_milli / 1000U),
         (unsigned long)(info.frame_rate_milli % 1000U),
         info.has_audio_pes ? "MP2" : "none");
  zzplay_controller_set_capabilities(
      runtime.ctl, 0, runtime.audio_backend == ZZPLAY_AUDIO_AHI, 1);
  {
    char format[ZZPLAY_NOW_TEXT_MAX];

    sprintf(format, "MPEG-1 %lux%lu, %lu.%03lu fps, %s",
            (unsigned long)info.width, (unsigned long)info.height,
            (unsigned long)(info.frame_rate_milli / 1000U),
            (unsigned long)(info.frame_rate_milli % 1000U),
            info.has_audio_pes ? "MP2" : "no audio");
    zzplay_controller_set_format(runtime.ctl, format);
  }
  zzplay_controller_set_duration(runtime.ctl, 0U);
  zzplay_engine_refresh_output(&runtime);

session_ready:
  if (zz9k_find_board(&board) != ZZ9K_STATUS_OK ||
      (board.zorro_version != 2U && board.zorro_version != 3U)) {
    zzplay_error(&runtime,
            "zzplay: the P96 video window requires a supported ZZ9000 aperture\n");
    zzplay_fail(&runtime, ZZPLAY_FAILURE_UNSUPPORTED_BOARD,
                ZZ9K_STATUS_UNSUPPORTED);
    goto cleanup;
  }

  P96Base = OpenLibrary((CONST_STRPTR)"Picasso96API.library", 2U);
  if (!P96Base) {
    zzplay_error(&runtime, "zzplay: cannot open Picasso96API.library\n");
    zzplay_fail(&runtime, ZZPLAY_FAILURE_P96, ZZ9K_STATUS_UNSUPPORTED);
    goto cleanup;
  }
  (void)zzplay_resource_acquire(
      &runtime.core.resources, ZZPLAY_RESOURCE_P96_LIBRARY);

  cleanup_status = zz9k_open(&runtime.ctx);
  if (runtime.ctx) {
    (void)zzplay_resource_acquire(
        &runtime.core.resources, ZZPLAY_RESOURCE_SDK_CONTEXT);
  }
  if (cleanup_status != ZZ9K_STATUS_OK) {
    zzplay_error(&runtime, "zzplay: SDK open failed: %s\n",
            zz9k_status_name(cleanup_status));
    zzplay_fail(&runtime, ZZPLAY_FAILURE_SDK, cleanup_status);
    goto cleanup;
  }
  cleanup_status = zz9k_query_caps(runtime.ctx, &caps);
  if (cleanup_status != ZZ9K_STATUS_OK ||
      (caps.capability_bits &
       (ZZ9K_CAP_VIDEO_DECODE | ZZ9K_CAP_MEDIA_SESSION)) !=
          (ZZ9K_CAP_VIDEO_DECODE | ZZ9K_CAP_MEDIA_SESSION) ||
      (runtime.audio_enabled &&
       (caps.capability_bits & ZZ9K_CAP_AUDIO_DECODE) == 0U)) {
    zzplay_error(&runtime,
            "zzplay: firmware does not advertise the required "
            "media decode services\n");
    zzplay_fail(&runtime, ZZPLAY_FAILURE_CAPABILITY,
                ZZ9K_STATUS_UNSUPPORTED);
    goto cleanup;
  }
  if (board.zorro_version == 2U &&
      (((caps.capability_bits & ZZ9K_CAP_APERTURE_LAYOUT) == 0U) ||
       zz9k_query_aperture_layout(runtime.ctx, &aperture) !=
           ZZ9K_STATUS_OK ||
       !zzplay_video_z2_aperture_ready(
           &aperture, info.width, info.height))) {
    zzplay_error(&runtime,
            "zzplay: video does not fit an acknowledged Zorro 2 PIP pool\n");
    zzplay_fail(&runtime, ZZPLAY_FAILURE_UNSUPPORTED_BOARD,
                ZZ9K_STATUS_UNSUPPORTED);
    goto cleanup;
  }
  cleanup_status = zz9k_query_service(
      runtime.ctx, ZZ9K_SERVICE_VIDEO, &service);
  {
    uint32_t need = ZZ9K_SERVICE_FLAG_VIDEO_DIRECT_OVERLAY |
                    ZZ9K_SERVICE_FLAG_VIDEO_STREAMING_INPUT |
                    ZZ9K_SERVICE_FLAG_VIDEO_MEDIA_SESSION |
                    runtime.required_extra_flags;

    if (runtime.audio_enabled) {
      need |= runtime.required_audio_flags;
    }
    if (cleanup_status != ZZ9K_STATUS_OK ||
        (service.flags & need) != need) {
      if (runtime.is_webm && run->probe &&
          (service.flags & runtime.required_extra_flags) !=
              runtime.required_extra_flags) {
        zzplay_error(&runtime,
                     "zzplay: WebM %s is not supported by this firmware\n",
                     zzplay_webm_video_name(run->probe->webm.video));
      } else if (runtime.is_webm && runtime.audio_enabled && run->probe &&
                 (service.flags & runtime.required_audio_flags) !=
                     runtime.required_audio_flags) {
        zzplay_error(&runtime,
                     "zzplay: WebM %s is not supported by this firmware\n",
                     zzplay_webm_audio_name(run->probe->webm.audio));
      } else if (runtime.is_webm) {
        zzplay_error(&runtime,
                     "zzplay: WebM playback is not supported by this firmware\n");
      } else {
        zzplay_error(&runtime,
                "zzplay: required MPEG-1/PS direct-overlay backend "
                "is unavailable\n");
      }
      zzplay_fail(&runtime, ZZPLAY_FAILURE_CAPABILITY,
                  ZZ9K_STATUS_UNSUPPORTED);
      goto cleanup;
    }
  }
  runtime.ax_available =
      (uint8_t)((service.flags & ZZ9K_SERVICE_FLAG_VIDEO_AUDIO_BIND) != 0U);
  if (runtime.audio_backend == ZZPLAY_AUDIO_AX && !runtime.ax_available) {
    if (!strict) {
      zzplay_engine_set_backend(&runtime, ZZPLAY_AUDIO_AHI);
      audio_decision.selected = ZZPLAY_AUDIO_AHI;
      audio_decision.fell_back = 1;
      zzplay_info("zzplay: card-local AX media output unavailable; "
             "falling back to AHI\n");
    } else {
      zzplay_error(&runtime,
              "zzplay: firmware or hardware does not advertise "
              "card-local AX media output\n");
      zzplay_fail(&runtime, ZZPLAY_FAILURE_CAPABILITY,
                  ZZ9K_STATUS_UNSUPPORTED);
      goto cleanup;
    }
  }
  zzplay_info("zzplay: selected audio backend %s%s\n",
         zzplay_audio_backend_name(runtime.audio_backend),
         audio_decision.fell_back ? " (fallback)" : "");

  input_bytes = board.zorro_version == 2U ? ZZPLAY_Z2_INPUT_BYTES
                                         : ZZPLAY_INPUT_BYTES;
  pcm_bytes = board.zorro_version == 2U ? ZZPLAY_Z2_PCM_BYTES
                                       : ZZPLAY_PCM_BYTES;
  cleanup_status = zz9k_alloc_shared(
      runtime.ctx, input_bytes, 64U,
      ZZ9K_ALLOC_HOST_WINDOW, &runtime.input);
  if (runtime.input.handle != 0U) {
    (void)zzplay_resource_acquire(
        &runtime.core.resources, ZZPLAY_RESOURCE_INPUT_BUFFER);
  }
  if (cleanup_status != ZZ9K_STATUS_OK || !runtime.input.data) {
    zzplay_error(&runtime, "zzplay: input buffer allocation failed: %s\n",
            zz9k_status_name(cleanup_status));
    zzplay_fail(&runtime, ZZPLAY_FAILURE_ALLOCATION, cleanup_status);
    goto cleanup;
  }
  if (runtime.audio_enabled) {
    cleanup_status = zz9k_alloc_shared(
        runtime.ctx, pcm_bytes, 64U,
        ZZ9K_ALLOC_HOST_WINDOW, &runtime.pcm);
    if (runtime.pcm.handle != 0U) {
      (void)zzplay_resource_acquire(
          &runtime.core.resources, ZZPLAY_RESOURCE_PCM_BUFFER);
    }
    if (cleanup_status != ZZ9K_STATUS_OK || !runtime.pcm.data) {
      zzplay_error(&runtime,
              "zzplay: PCM ring allocation failed: %s\n",
              zz9k_status_name(cleanup_status));
      zzplay_fail(&runtime, ZZPLAY_FAILURE_ALLOCATION,
                  cleanup_status);
      goto cleanup;
    }
  }
  cleanup_status = zzplay_begin_session(&runtime);
  if (cleanup_status != ZZ9K_STATUS_OK) {
    if (runtime.is_webm && cleanup_status == ZZ9K_STATUS_UNSUPPORTED) {
      zzplay_error(&runtime,
                   "zzplay: WebM playback was refused by the firmware\n");
    } else {
      zzplay_error(&runtime, "zzplay: session begin failed: %s\n",
              zz9k_status_name(cleanup_status));
    }
    zzplay_fail(&runtime, ZZPLAY_FAILURE_SESSION, cleanup_status);
    goto cleanup;
  }
  if (!zzplay_timer_open(&runtime.timer)) {
    zzplay_error(&runtime, "zzplay: cannot open timer.device\n");
    zzplay_fail(&runtime, ZZPLAY_FAILURE_TIMER, ZZ9K_STATUS_IO_ERROR);
    goto cleanup;
  }
  (void)zzplay_resource_acquire(
      &runtime.core.resources, ZZPLAY_RESOURCE_TIMER);

  frame_period_us = zzplay_frame_period_us(info.frame_rate_milli);
  zzplay_sync_policy_init(
      &runtime.sync_policy, info.frame_rate_milli, 1000U);
  if (runtime.options.show_fps) {
    zzplay_info("zzplay: FPS reporting enabled%s\n",
           runtime.options.uncapped ? " (uncapped benchmark)" : "");
    zzplay_stats_start(&runtime.stats);
  }
  zzplay_info("zzplay: frame path direct planar overlay\n");
  (void)zzplay_core_begin_prebuffer(&runtime.core);
  (void)zzplay_core_start(&runtime.core);

playback_session:
  while (runtime.core.state == ZZPLAY_STATE_PLAYING ||
         runtime.core.state == ZZPLAY_STATE_PAUSED) {
    ZZPlayMediaAction action;
    int resized = 0;

    if (zzplay_engine_service_input(&runtime, &resized)) {
      zzplay_engine_stop_from_request(&runtime);
      break;
    }
    if (resized) {
      zzplay_apply_resize(&runtime);
    }
    /* Pause follows the controller and is confirmed back once the engine
     * is really holding, so modal operations may run while paused. */
    if ((runtime.core.state == ZZPLAY_STATE_PAUSED) !=
        (runtime.ctl->paused != 0)) {
      cleanup_status = zzplay_toggle_pause(&runtime);
      if (cleanup_status != ZZ9K_STATUS_OK) {
        zzplay_error(&runtime, "zzplay: pause/resume failed: %s\n",
                zz9k_status_name(cleanup_status));
        zzplay_fail(&runtime, ZZPLAY_FAILURE_IO, cleanup_status);
        break;
      }
      zzplay_controller_set_engine_paused(
          runtime.ctl,
          runtime.core.state == ZZPLAY_STATE_PAUSED);
      runtime.title_dirty = 1U;
    }
    if (zzplay_controller_take_fullscreen_toggle(runtime.ctl)) {
      if (!zzplay_toggle_fullscreen(&runtime)) {
        zzplay_error(&runtime,
                     "zzplay: cannot reopen the P96 PIP window "
                     "(error %ld)\n", (long)runtime.pip_error);
        zzplay_fail(&runtime, ZZPLAY_FAILURE_PIP,
                    ZZ9K_STATUS_UNSUPPORTED);
        break;
      }
    }
    {
      uint32_t volume;

      if (zzplay_controller_take_volume(runtime.ctl, &volume)) {
        if (runtime.audio_backend == ZZPLAY_AUDIO_AHI) {
          zzplay_ahi_set_volume(&runtime.ahi, volume);
        }
        zzplay_engine_refresh_output(&runtime);
      }
    }
    zzplay_update_title(&runtime);
    if (runtime.core.state == ZZPLAY_STATE_PAUSED) {
      zzplay_wait_us(&runtime.timer, ZZPLAY_SYNC_POLL_US);
      continue;
    }
    cleanup_status = zzplay_audio_pump(
        &runtime, 0, runtime.audio_refresh_needed);
    if (cleanup_status != ZZ9K_STATUS_OK) {
      zzplay_error(&runtime, "zzplay: audio output failed: %s\n",
              zz9k_status_name(cleanup_status));
      zzplay_fail(&runtime, ZZPLAY_FAILURE_IO, cleanup_status);
      break;
    }
    runtime.audio_refresh_needed = 0U;

    if (runtime.trace) {
      uint32_t underruns = zzplay_audio_underruns(&runtime);

      if (underruns != runtime.trace_underruns) {
        if (underruns > runtime.trace_underruns) {
          zzplay_trace_event(&runtime, "underrun",
                             underruns - runtime.trace_underruns,
                             underruns, 1);
        }
        /* A smaller value means the session restarted; adopt silently. */
        runtime.trace_underruns = underruns;
      }
    }

    if (runtime.frame_held) {
      int retired;

      cleanup_status = zzplay_retire_held_frame(
          &runtime, &result, frame_period_us, held_decode_us,
          &retired);
      if (cleanup_status != ZZ9K_STATUS_OK) {
        if (runtime.pip_open_failed) {
          zzplay_error(&runtime,
                  "zzplay: cannot open P96 PIP window (error %ld)\n",
                  (long)runtime.pip_error);
          zzplay_fail(&runtime, ZZPLAY_FAILURE_PIP,
                      ZZ9K_STATUS_UNSUPPORTED);
        } else {
          zzplay_error(&runtime,
                  "zzplay: frame presentation failed: %s\n",
                  zz9k_status_name(cleanup_status));
          zzplay_fail(&runtime, ZZPLAY_FAILURE_SESSION,
                      cleanup_status);
        }
        break;
      }
      if (retired) {
        runtime.frame_held = 0U;
        if (runtime.options.show_fps) {
          zzplay_stats_frame(&runtime.stats, held_decode_us);
        }
        zzplay_trace_frame(&runtime, held_decode_us);
        /* One mailbox round trip roughly twice a second, plus an immediate
         * recheck after any geometry change. */
        if (runtime.present_recheck || !runtime.present_known ||
            (runtime.frames % 15U) == 0U) {
          runtime.present_recheck = 0U;
          zzplay_update_presentation(&runtime);
          zzplay_update_title(&runtime);
        }
      }
      continue;
    }
    if (media_done) {
      break;
    }

    {
      uint32_t feed_rounds = 0U;

      for (;;) {
        const ZZPlayFeedResult feed = zzplay_feed_round(
            &runtime, &transport, &result);

        if (feed == ZZPLAY_FEED_ERROR) {
          goto playback_failed;
        }
        if (feed != ZZPLAY_FEED_MORE ||
            feed_rounds >= ZZPLAY_FEED_ROUNDS_PER_PASS) {
          break;
        }
        feed_rounds++;
      }
    }

    {
      TimeVal_Type started;
      TimeVal_Type ended;

      GetSysTime(&started);
      cleanup_status = zzplay_decode_once(&runtime, &result);
      GetSysTime(&ended);
      held_decode_us = zzplay_elapsed_us(&started, &ended);
      zzplay_stats_record_profile(
          &runtime.stats.core, ZZPLAY_PROFILE_SDK_DECODE,
          held_decode_us);
    }
    if (runtime.audio_enabled) {
      runtime.audio_refresh_needed = 1U;
    }
    if (cleanup_status == ZZ9K_STATUS_BUSY) {
      if (runtime.trace) {
        runtime.trace_decode_busy++;
      }
      zzplay_wait_us(&runtime.timer, ZZPLAY_SYNC_POLL_US);
      continue;
    }
    if (cleanup_status != ZZ9K_STATUS_OK) {
      if (runtime.is_webm && cleanup_status == ZZ9K_STATUS_UNSUPPORTED) {
        zzplay_error(&runtime, "zzplay: this WebM stream is not supported\n");
      } else {
        zzplay_error(&runtime, "zzplay: media decode failed: %s\n",
                zz9k_status_name(cleanup_status));
      }
      zzplay_fail(&runtime, ZZPLAY_FAILURE_SESSION,
              cleanup_status);
      break;
    }
    if (result.frame_rate_num != 0U &&
        result.frame_rate_den != 0U) {
      uint64_t usec =
          ((uint64_t)1000000U * result.frame_rate_den) /
          result.frame_rate_num;

      frame_period_us =
          usec > 0xffffffffULL ? 0xffffffffU : (uint32_t)usec;
      zzplay_sync_policy_init(
          &runtime.sync_policy, result.frame_rate_num,
          result.frame_rate_den);
    }
    action = zzplay_media_result_action(result.flags);
    if (action == ZZPLAY_MEDIA_FRAME_HELD) {
      runtime.frames++;
      runtime.frame_held = 1U;
      runtime.trace_video_pts = result.video_pts;
      if (gui_open) {
        /* Elapsed position from decoded frames is only visible in the
         * player window; one-shot MPEG has no seekable status to update. */
        if (runtime.is_webm &&
            result.video_pts != ZZ9K_MEDIA_NO_PTS) {
          uint64_t elapsed_ms = result.video_pts / 90U;

          zzplay_controller_set_position(
              runtime.ctl,
              elapsed_ms > 0xffffffffULL ? 0xffffffffU
                                         : (uint32_t)elapsed_ms,
              1);
        } else {
          zzplay_controller_set_position(
              runtime.ctl,
              (uint32_t)((uint64_t)(runtime.frames - runtime.pass_frame_origin) *
                         frame_period_us / 1000U),
              1);
        }
      }
      continue;
    }
    if (action == ZZPLAY_MEDIA_DONE) {
      media_done = 1;
      continue;
    }
    if (action == ZZPLAY_MEDIA_NEED_INPUT) {
      if (runtime.trace) {
        runtime.trace_need_input++;
      }
      if (transport.eof && transport.eof_sent &&
          transport.pending_length == 0U) {
        zzplay_error(&runtime,
                     "zzplay: truncated stream at end of input\n");
        zzplay_fail(&runtime, ZZPLAY_FAILURE_IO,
                    ZZ9K_STATUS_IO_ERROR);
        break;
      }
    }
  }

playback_failed:

  if (runtime.core.state == ZZPLAY_STATE_PLAYING && media_done) {
    (void)zzplay_core_begin_drain(&runtime.core);
    cleanup_status = zzplay_drain_audio(&runtime);
    if (cleanup_status == ZZ9K_STATUS_OK) {
      /* Repeat ONE is the seamless engine loop; a finite CLI LOOP=N
       * count is engine-internal and independent of it. */
      if (zzplay_controller_loop_item(runtime.ctl) ||
          (runtime.options.loop_mode == ZZPLAY_LOOP_FINITE &&
           runtime.options.loop_count != 0U)) {
        if (!zzplay_core_begin_loop(&runtime.core)) {
          zzplay_fail(
              &runtime, ZZPLAY_FAILURE_PROTOCOL,
              ZZ9K_STATUS_INTERNAL_ERROR);
          goto cleanup;
        }
        cleanup_status =
            zzplay_restart_session(&runtime, &transport);
        if (cleanup_status != ZZ9K_STATUS_OK) {
          zzplay_error(&runtime, "zzplay: loop restart failed: %s\n",
                  zz9k_status_name(cleanup_status));
          zzplay_fail(
              &runtime, ZZPLAY_FAILURE_SESSION, cleanup_status);
          goto cleanup;
        }
        if (runtime.options.loop_mode == ZZPLAY_LOOP_FINITE) {
          runtime.options.loop_count--;
        }
        runtime.completed_loops++;
        /* The displayed position restarts with each pass; `frames`
         * stays cumulative for the trace and the run summary. */
        runtime.pass_frame_origin = runtime.frames;
        media_done = 0;
        held_decode_us = 0U;
        memset(&result, 0, sizeof(result));
        runtime.trace_accepted = 0U;
        runtime.trace_need_input = 0U;
        runtime.trace_write_busy = 0U;
        runtime.trace_decode_busy = 0U;
        runtime.trace_reads = 0U;
        runtime.trace_read_max_us = 0U;
        runtime.trace_decision = 'N';
        zzplay_trace_event(&runtime, "loop restart", 0U, 0U, 0);
        frame_period_us =
            zzplay_frame_period_us(info.frame_rate_milli);
        zzplay_sync_policy_init(
            &runtime.sync_policy, info.frame_rate_milli, 1000U);
        if (!zzplay_core_restart_loop(&runtime.core) ||
            !zzplay_core_start(&runtime.core)) {
          zzplay_fail(
              &runtime, ZZPLAY_FAILURE_PROTOCOL,
              ZZ9K_STATUS_INTERNAL_ERROR);
          goto cleanup;
        }
        zzplay_info("zzplay: loop %lu\n",
               (unsigned long)runtime.completed_loops);
        goto playback_session;
      }
      zzplay_core_stop(&runtime.core, ZZPLAY_STOP_EOF);
    } else if (cleanup_status != ZZ9K_STATUS_CANCELLED) {
      zzplay_error(&runtime, "zzplay: audio drain failed: %s\n",
              zz9k_status_name(cleanup_status));
      zzplay_fail(&runtime, ZZPLAY_FAILURE_IO, cleanup_status);
    }
  }

cleanup:
  /* The trace file belongs to the application and serves every item. */
  runtime.trace = 0;
  if (runtime.options.show_fps) {
    zzplay_stats_stop(&runtime.stats);
    /* Must run before resource release closes the media session. */
    zzplay_report_card_profile(&runtime);
  }
  zzplay_capture_audio_totals(&runtime);
  cleanup_status = zzplay_resources_release_all(
      &runtime.core.resources, zzplay_release_resource, &runtime);
  if (runtime.core.state != ZZPLAY_STATE_ERROR &&
      cleanup_status != ZZ9K_STATUS_OK) {
    zzplay_fail(&runtime, ZZPLAY_FAILURE_SESSION, cleanup_status);
  }
  if (runtime.core.state != ZZPLAY_STATE_ERROR) {
    zzplay_info("zzplay: %lu decoded, %lu presented, %lu discarded "
           "frames",
           (unsigned long)runtime.frames,
           (unsigned long)runtime.stats.core.presented_frames,
           (unsigned long)runtime.stats.core.discarded_frames);
    if (runtime.audio_enabled) {
      zzplay_info(", %lu audio frames played, %lu underruns",
             /* saturating cast: 2^32-1 frames is ~27 h at 44.1 kHz */
             (unsigned long)(runtime.final_audio_frames > 0xffffffffULL
                                 ? 0xffffffffULL
                                 : runtime.final_audio_frames),
             (unsigned long)runtime.final_underruns);
    }
    if (runtime.completed_loops != 0U) {
      zzplay_info(", %lu loops",
             (unsigned long)runtime.completed_loops);
    }
    zzplay_info("\n");
    if (runtime.stats.core.max_abs_drift_pts != 0U) {
      zzplay_info("zzplay: A/V drift current %ld ms, max %lu ms, "
             "%lu hold polls, %lu late frames\n",
             (long)(runtime.stats.core.current_drift_pts / 90),
             (unsigned long)(
                 runtime.stats.core.max_abs_drift_pts / 90U),
             (unsigned long)runtime.stats.core.hold_events,
             (unsigned long)runtime.stats.core.late_frames);
    }
    if (runtime.options.show_fps) {
      zzplay_stats_finish(&runtime.stats);
    }
  }
  if (runtime.core.state == ZZPLAY_STATE_ERROR) {
    return ZZPLAY_ENGINE_FAILED;
  }
  if (runtime.ctl->request != ZZPLAY_REQUEST_NONE) {
    return ZZPLAY_ENGINE_STOPPED;
  }
  return ZZPLAY_ENGINE_EOF;
}

/* ------------------------------------------------------------------ */
/* WebP animation engine.                                             */
/* ------------------------------------------------------------------ */

static ZZPlayEngineResult zzplay_engine_webp(const ZZPlayEngineRun *run)
{
  struct ZZPlayRuntime runtime;
  ZZ9KBoard board;
  ZZ9KServiceInfo service;
  ZZ9KSharedBuffer staging;
  ZZ9KSurface surface;
  ZZ9KImageSessionBeginDesc begin;
  ZZ9KImageSessionResult session_result;
  ZZ9KImageAnimationFrameResult frame_result;
  ZZPlayFrameClock clock;
  ZZ9KRect output_rect;
  uint32_t active_token = 0U;
  uint32_t session = 0U;
  uint32_t width;
  uint32_t height;
  uint32_t pitch;
  uint64_t file_length = 0U;
  uint64_t file_offset = 0U;
  int cleanup_status;
  ZZPlayEngineResult outcome = ZZPLAY_ENGINE_FAILED;
  int session_open = 0;
  int staging_allocated = 0;
  int surface_allocated = 0;
  int timer_open = 0;
  int p96_open = 0;
  int sdk_open = 0;

  if (!run || !run->path || !run->ctl || !run->probe) {
    return ZZPLAY_ENGINE_FAILED;
  }
  if (run->probe->kind != ZZPLAY_MEDIA_KIND_WEBP ||
      !run->probe->webp.is_animated) {
    return ZZPLAY_ENGINE_FAILED;
  }
  if (!zzplay_webp_info_supported(&run->probe->webp)) {
    zzplay_info("zzplay: unsupported WebP animation geometry %lux%lu\n",
                (unsigned long)run->probe->webp.width,
                (unsigned long)run->probe->webp.height);
    return ZZPLAY_ENGINE_FAILED;
  }

  memset(&runtime, 0, sizeof(runtime));
  memset(&board, 0, sizeof(board));
  memset(&service, 0, sizeof(service));
  memset(&staging, 0, sizeof(staging));
  memset(&surface, 0, sizeof(surface));
  memset(&begin, 0, sizeof(begin));
  memset(&session_result, 0, sizeof(session_result));
  memset(&frame_result, 0, sizeof(frame_result));

  runtime.ctl = run->ctl;
  runtime.options = *run->options;
  runtime.prefs = *run->prefs;
  /* Packed YUV422 stores whole two-pixel macropixels: an odd-width canvas
   * ends in a duplicated pixel. The PIP source and the surface pitch both
   * cover that full final macropixel, which the firmware requires. */
  runtime.video_info.width = (run->probe->webp.width + 1U) & ~1U;
  runtime.video_info.height = run->probe->webp.height;
  runtime.video_info.frame_rate_milli = 25000U;
  zzplay_core_init(&runtime.core);

  width = run->probe->webp.width;
  height = run->probe->webp.height;
  pitch = ((width + 1U) / 2U) * 4U;

  runtime.file = fopen(run->path, "rb");
  if (!runtime.file) {
    zzplay_error(&runtime, "zzplay: cannot open %s\n", run->path);
    return ZZPLAY_ENGINE_FAILED;
  }
  if (fseek(runtime.file, 0L, SEEK_END) == 0) {
    long len = ftell(runtime.file);
    if (len > 0) {
      file_length = (uint64_t)len;
    }
    (void)fseek(runtime.file, 0L, SEEK_SET);
  }

  if (zz9k_find_board(&board) != ZZ9K_STATUS_OK ||
      (board.zorro_version != 2U && board.zorro_version != 3U)) {
    zzplay_error(&runtime,
                 "zzplay: the P96 video window requires a supported ZZ9000 aperture\n");
    zzplay_fail(&runtime, ZZPLAY_FAILURE_UNSUPPORTED_BOARD,
                ZZ9K_STATUS_UNSUPPORTED);
    goto cleanup;
  }

  P96Base = OpenLibrary((CONST_STRPTR)"Picasso96API.library", 2U);
  if (!P96Base) {
    zzplay_error(&runtime, "zzplay: cannot open Picasso96API.library\n");
    zzplay_fail(&runtime, ZZPLAY_FAILURE_P96, ZZ9K_STATUS_UNSUPPORTED);
    goto cleanup;
  }
  p96_open = 1;

  cleanup_status = zz9k_open(&runtime.ctx);
  if (cleanup_status != ZZ9K_STATUS_OK || !runtime.ctx) {
    zzplay_error(&runtime, "zzplay: SDK open failed (%d)\n", cleanup_status);
    zzplay_fail(&runtime, ZZPLAY_FAILURE_SDK, cleanup_status);
    goto cleanup;
  }
  sdk_open = 1;

  cleanup_status = zz9k_query_service(runtime.ctx, ZZ9K_SERVICE_IMAGE, &service);
  if (cleanup_status != ZZ9K_STATUS_OK) {
    zzplay_error(&runtime, "zzplay: image service query failed\n");
    zzplay_fail(&runtime, ZZPLAY_FAILURE_CAPABILITY, ZZ9K_STATUS_UNSUPPORTED);
    goto cleanup;
  }
  if ((service.flags & ZZ9K_SERVICE_FLAG_IMAGE_STREAMING_INPUT) == 0U ||
      (service.flags & ZZ9K_SERVICE_FLAG_IMAGE_WEBP) == 0U) {
    zzplay_error(&runtime, "zzplay: firmware does not support WebP streaming\n");
    zzplay_fail(&runtime, ZZPLAY_FAILURE_CAPABILITY, ZZ9K_STATUS_UNSUPPORTED);
    goto cleanup;
  }

  if (!zzplay_timer_open(&runtime.timer)) {
    zzplay_error(&runtime, "zzplay: timer device open failed\n");
    goto cleanup;
  }
  timer_open = 1;

  zzplay_controller_set_capabilities(run->ctl, 0, 0, 1);
  {
    char format_buf[ZZPLAY_NOW_TEXT_MAX];
    sprintf(format_buf, "WebP Animation %lux%lu",
            (unsigned long)width, (unsigned long)height);
    zzplay_controller_set_format(run->ctl, format_buf);
  }
  zzplay_controller_set_duration(run->ctl, 0U);

  cleanup_status = zz9k_alloc_shared(runtime.ctx, 32768U, 16U,
                                     ZZ9K_ALLOC_HOST_WINDOW, &staging);
  if (cleanup_status != ZZ9K_STATUS_OK) {
    zzplay_error(&runtime, "zzplay: WebP staging alloc failed (%d)\n", cleanup_status);
    goto cleanup;
  }
  staging_allocated = 1;

  cleanup_status = zz9k_alloc_surface_ex(
      runtime.ctx, width, height, ZZ9K_SURFACE_FORMAT_YUV422CGX,
      ZZ9K_SURFACE_FLAG_ARM_LOCAL, pitch, &surface);
  if (cleanup_status != ZZ9K_STATUS_OK) {
    zzplay_error(&runtime, "zzplay: WebP PIP surface alloc failed (%d)\n", cleanup_status);
    goto cleanup;
  }
  surface_allocated = 1;

  if (!zzplay_ensure_pip(&runtime)) {
    zzplay_error(&runtime, "zzplay: cannot open P96 PIP window\n");
    goto cleanup;
  }

  output_rect.x = 0U;
  output_rect.y = 0U;
  output_rect.w = width;
  output_rect.h = height;
  if (!zz9k_image_build_surface_session_begin_desc(
          &begin, ZZ9K_IMAGE_CODEC_WEBP, surface.handle,
          &output_rect, ZZ9K_SURFACE_FORMAT_YUV422CGX,
          ZZ9K_IMAGE_SESSION_BEGIN_ANIMATION)) {
    zzplay_error(&runtime, "zzplay: could not build WebP begin descriptor\n");
    goto cleanup;
  }

  cleanup_status = zz9k_image_session_begin(runtime.ctx, &begin, &session_result);
  if (cleanup_status != ZZ9K_STATUS_OK || session_result.session == 0U ||
      session_result.state != ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT) {
    zzplay_error(&runtime, "zzplay: WebP animation session begin failed (%d)\n",
                 cleanup_status);
    goto cleanup;
  }
  session = session_result.session;
  session_open = 1;

  while (file_offset < file_length) {
    size_t chunk = (size_t)(file_length - file_offset);
    size_t bytes_read;
    int is_eof;
    ZZ9KImageSessionFeedDesc feed;

    if (chunk > staging.length) {
      chunk = (size_t)staging.length;
    }
    bytes_read = fread((void *)staging.data, 1U, chunk, runtime.file);
    if (bytes_read == 0U && ferror(runtime.file)) {
      zzplay_error(&runtime, "zzplay: error reading WebP file\n");
      goto cleanup;
    }
    file_offset += (uint64_t)bytes_read;
    is_eof = (file_offset >= file_length);

    if (!zz9k_image_build_session_feed_desc(
            &feed, session, staging.handle, 0U, (uint32_t)bytes_read,
            is_eof ? ZZ9K_IMAGE_SESSION_FEED_EOF : 0U)) {
      zzplay_error(&runtime, "zzplay: could not build feed desc\n");
      goto cleanup;
    }

    cleanup_status = zz9k_image_session_feed(runtime.ctx, &feed, &session_result);
    if (cleanup_status != ZZ9K_STATUS_OK) {
      zzplay_error(&runtime, "zzplay: WebP feed failed (%d)\n", cleanup_status);
      goto cleanup;
    }
  }

  if (session_result.state != ZZ9K_IMAGE_SESSION_STATE_ANIMATION_READY) {
    zzplay_error(&runtime, "zzplay: WebP not animation ready after EOF (state %u)\n",
                 (unsigned)session_result.state);
    goto cleanup;
  }

  zzplay_frame_clock_init(&clock, zzplay_controller_loop_item(run->ctl));

  cleanup_status = zz9k_image_animation_frame_next(runtime.ctx, session, 0U, &frame_result);
  if (cleanup_status != ZZ9K_STATUS_OK || frame_result.frame_token == 0U) {
    zzplay_error(&runtime, "zzplay: failed to fetch first animation frame (%d)\n",
                 cleanup_status);
    goto cleanup;
  }
  active_token = frame_result.frame_token;

  {
    uint64_t now_us = zzplay_now_us();
    zzplay_frame_clock_start(&clock, now_us, frame_result.frame_duration_ms);
  }
  zzplay_controller_set_position(run->ctl, 0U, 1);

  (void)zz9k_image_animation_frame_present(runtime.ctx, session, active_token, 0U, &frame_result);
  (void)zzplay_core_start(&runtime.core);

  while (runtime.core.state == ZZPLAY_STATE_PLAYING ||
         runtime.core.state == ZZPLAY_STATE_PAUSED) {
    int resized = 0;
    uint64_t now_us;
    ZZPlayFrameAction action;

    if (zzplay_engine_service_input(&runtime, &resized)) {
      zzplay_engine_stop_from_request(&runtime);
      outcome = ZZPLAY_ENGINE_STOPPED;
      break;
    }
    if (resized) {
      zzplay_apply_resize(&runtime);
    }
    if (zzplay_controller_take_fullscreen_toggle(runtime.ctl)) {
      (void)zzplay_toggle_fullscreen(&runtime);
    }
    if ((runtime.core.state == ZZPLAY_STATE_PAUSED) != (runtime.ctl->paused != 0)) {
      now_us = zzplay_now_us();
      if (runtime.ctl->paused) {
        runtime.core.state = ZZPLAY_STATE_PAUSED;
        zzplay_frame_clock_pause(&clock, now_us);
      } else {
        runtime.core.state = ZZPLAY_STATE_PLAYING;
        zzplay_frame_clock_resume(&clock, now_us);
      }
      zzplay_controller_set_engine_paused(runtime.ctl, runtime.core.state == ZZPLAY_STATE_PAUSED);
      runtime.title_dirty = 1U;
    }
    zzplay_update_title(&runtime);

    if (run->pump) {
      run->pump(run->pump_user);
    }

    if (runtime.core.state == ZZPLAY_STATE_PAUSED) {
      zzplay_wait_us(&runtime.timer, ZZPLAY_SYNC_POLL_US);
      continue;
    }

    now_us = zzplay_now_us();
    action = zzplay_frame_clock_poll(&clock, now_us);

    if (action == ZZPLAY_FRAME_ACTION_WAIT) {
      uint32_t slice_us = zzplay_frame_clock_wait_slice_us(&clock, now_us, ZZPLAY_SYNC_POLL_US);
      if (slice_us > 0U) {
        zzplay_wait_us(&runtime.timer, slice_us);
      }
      continue;
    }

    if (action == ZZPLAY_FRAME_ACTION_ADVANCE) {
      if (active_token != 0U) {
        (void)zz9k_image_animation_frame_retire(runtime.ctx, session, active_token, 0U, &frame_result);
        active_token = 0U;
      }
      cleanup_status = zz9k_image_animation_frame_next(runtime.ctx, session, 0U, &frame_result);
      if (cleanup_status != ZZ9K_STATUS_OK) {
        zzplay_error(&runtime, "zzplay: next frame failed (%d)\n", cleanup_status);
        outcome = ZZPLAY_ENGINE_FAILED;
        break;
      }
      if (frame_result.state == ZZ9K_IMAGE_SESSION_STATE_ANIMATION_ENDED ||
          (frame_result.flags & ZZ9K_IMAGE_ANIMATION_FRAME_FLAG_ENDED)) {
        zzplay_frame_clock_on_end(&clock);
        continue;
      }
      active_token = frame_result.frame_token;
      now_us = zzplay_now_us();
      zzplay_frame_clock_advance(&clock, now_us, frame_result.frame_duration_ms);
      zzplay_controller_set_position(runtime.ctl, (uint32_t)clock.total_elapsed_ms, 1);
      (void)zz9k_image_animation_frame_present(runtime.ctx, session, active_token, 0U, &frame_result);
      continue;
    }

    if (action == ZZPLAY_FRAME_ACTION_RESTART) {
      if (active_token != 0U) {
        (void)zz9k_image_animation_frame_retire(runtime.ctx, session, active_token, 0U, &frame_result);
        active_token = 0U;
      }
      cleanup_status = zz9k_image_animation_restart(runtime.ctx, session, 0U, &frame_result);
      if (cleanup_status != ZZ9K_STATUS_OK) {
        zzplay_error(&runtime, "zzplay: restart failed (%d)\n", cleanup_status);
        outcome = ZZPLAY_ENGINE_FAILED;
        break;
      }
      cleanup_status = zz9k_image_animation_frame_next(runtime.ctx, session, 0U, &frame_result);
      if (cleanup_status != ZZ9K_STATUS_OK || frame_result.frame_token == 0U) {
        zzplay_error(&runtime, "zzplay: restart next frame failed (%d)\n", cleanup_status);
        outcome = ZZPLAY_ENGINE_FAILED;
        break;
      }
      active_token = frame_result.frame_token;
      now_us = zzplay_now_us();
      zzplay_frame_clock_on_restart(&clock, now_us, frame_result.frame_duration_ms);
      zzplay_controller_set_position(runtime.ctl, 0U, 1);
      (void)zz9k_image_animation_frame_present(runtime.ctx, session, active_token, 0U, &frame_result);
      continue;
    }

    if (action == ZZPLAY_FRAME_ACTION_EOF) {
      outcome = ZZPLAY_ENGINE_EOF;
      break;
    }
  }

cleanup:
  if (active_token != 0U) {
    (void)zz9k_image_animation_frame_retire(runtime.ctx, session, active_token, 0U, &frame_result);
    active_token = 0U;
  }
  if (session_open && session != 0U) {
    (void)zz9k_image_session_close(runtime.ctx, session, 0U);
    session = 0U;
  }
  if (surface_allocated && surface.handle != 0U) {
    (void)zz9k_free_surface(runtime.ctx, surface.handle);
    surface.handle = 0U;
  }
  if (staging_allocated && staging.handle != 0U) {
    (void)zz9k_free_shared(runtime.ctx, staging.handle);
    staging.handle = 0U;
  }
  zzplay_close_pip(&runtime);
  zzplay_close_video_screen(&runtime);
  if (timer_open) {
    zzplay_timer_close(&runtime.timer);
  }
  if (runtime.file) {
    fclose(runtime.file);
    runtime.file = NULL;
  }
  if (sdk_open && runtime.ctx) {
    (void)zz9k_close(runtime.ctx);
    runtime.ctx = NULL;
  }
  if (p96_open && P96Base) {
    CloseLibrary(P96Base);
    P96Base = NULL;
  }

  if (run->ctl && run->ctl->request != ZZPLAY_REQUEST_NONE) {
    return ZZPLAY_ENGINE_STOPPED;
  }
  return outcome;
}

/* ------------------------------------------------------------------ */
/* Engine table and the application loop.                             */
/* ------------------------------------------------------------------ */

static const struct ZZPlayEngineEntry {
  ZZPlayMediaKind kind;
  ZZPlayEngineFn run;
} zzplay_engines[] = {
  { ZZPLAY_MEDIA_KIND_MPEG_PS, zzplay_engine_mpeg },
  { ZZPLAY_MEDIA_KIND_MP3, zzplay_mp3_run },
  { ZZPLAY_MEDIA_KIND_WEBP, zzplay_engine_webp },
  { ZZPLAY_MEDIA_KIND_FLAC, zzplay_flac_run },
   { ZZPLAY_MEDIA_KIND_VORBIS, zzplay_vorbis_run },
   { ZZPLAY_MEDIA_KIND_WEBM, zzplay_engine_mpeg }
};
static ZZPlayEngineFn zzplay_engine_for_kind(ZZPlayMediaKind kind)
{
  unsigned i;

  for (i = 0U;
       i < (unsigned)(sizeof(zzplay_engines) / sizeof(zzplay_engines[0]));
       i++) {
    if (zzplay_engines[i].kind == kind) {
      return zzplay_engines[i].run;
    }
  }
  return 0;
}

/* Play one entry and say what happens next. Errors are reported to the
 * message line / stderr here or by the engine; the entry is marked failed
 * and playback advances, so one bad file never stops the list. */
static ZZPlayAppStep zzplay_app_play_index(ZZPlayApp *app, int32_t index)
{
  ZZPlayEngineRun run;
  ZZPlayEngineResult outcome = ZZPLAY_ENGINE_FAILED;
  ZZPlayEngineFn engine = 0;
  const ZZPlayFormat *format;
  ZZPlayTags tags;
  ZZPlayRequest request;
  FILE *file;
  uint64_t file_size = 0U;
  int32_t jump_index;
  uint32_t seek_ms;
  int probed = 0;

  if (index < 0 || (uint32_t)index >= app->playlist.count) {
    return app->player_mode ? ZZPLAY_APP_IDLE : ZZPLAY_APP_EXIT;
  }
  /* Private copy: the playlist may be edited while this item plays. */
  strncpy(app->item_path, app->playlist.entries[index].path,
          sizeof(app->item_path) - 1U);
  app->item_path[sizeof(app->item_path) - 1U] = '\0';
  (void)zzplay_playlist_set_current(&app->playlist, index);
  zzplay_controller_item_begin(&app->ctl);

  /* Title first, so the window shows the item even when it fails: an
   * ID3 "Artist - Title" for MP3, else the file name. Probing shares
   * the same open file. */
  memset(&tags, 0, sizeof(tags));
  memset(&probe, 0, sizeof(probe));
  file = fopen(app->item_path, "rb");
  if (file) {
    long length = 0L;

    (void)zzplay_tags_read(file, &tags);
    if (fseek(file, 0L, SEEK_END) == 0) {
      length = ftell(file);
      (void)fseek(file, 0L, SEEK_SET);
    }
    if (length > 0L) {
      file_size = (uint64_t)length;
    }
    probed = zzplay_probe_media_file(file, &probe);
    fclose(file);
  }
  {
    char display[2 * ZZPLAY_TAG_TEXT_MAX + 8];

    display[0] = '\0';
    if (probe.kind == ZZPLAY_MEDIA_KIND_MP3) {
      zzplay_tags_display(&tags, display, sizeof(display));
    }
    if (display[0] != '\0') {
      /* The row was drawn from the file name; redraw it only when the
       * tag actually changes what it shows. */
      if ((uint32_t)index < app->playlist.count &&
          strncmp(app->playlist.entries[index].title, display,
                  ZZPLAY_PLAYLIST_TITLE_MAX - 1U) != 0 &&
          zzplay_playlist_set_title(&app->playlist, (uint32_t)index,
                                    display)) {
        app->ctl.dirty |= ZZPLAY_DIRTY_PLAYLIST;
      }
      zzplay_controller_set_title(&app->ctl, display);
    } else {
      zzplay_controller_set_title(
          &app->ctl,
          zzplay_playlist_basename(app->item_path));
    }
  }

  if (probed) {
    engine = zzplay_engine_for_kind(probe.kind);
  }
  if (!file) {
    zzplay_launch_reportf(&app->ctl, &app->options, "cannot open %s",
                          app->item_path);
  } else if (!engine) {
    zzplay_launch_reportf(
        &app->ctl, &app->options,
         "cannot play %s: not a supported MPEG-1 Program Stream, WebM, MP3, FLAC, Ogg Vorbis or WebP animation file",
        app->item_path);
  } else {
    format = zzplay_format_for_kind(probe.kind);
    /* A one-shot run shows only the video window; an audio-only item
     * opens the player window lazily, as the status window's
     * replacement, and keeps it until exit. */
    if (!app->player_mode && format &&
        (format->flags & ZZPLAY_FORMAT_HAS_VIDEO) == 0U) {
      zzplay_app_open_gui(app);
    }
    memset(&run, 0, sizeof(run));
    run.ctl = &app->ctl;
    run.path = app->item_path;
    run.probe = &probe;
    run.options = &app->options;
    run.prefs = &app->prefs;
    run.audio_start = tags.audio_start;
    run.trailer_bytes = tags.trailer_bytes;
    run.file_size = file_size;
    run.seek_ms = app->start_seek_ms;
    run.pump = zzplay_app_pump;
    run.pump_user = app;
    app->start_seek_ms = 0U;
    outcome = engine(&run);
  }
  /* A pending request outranks whatever the engine reported; consume it
   * before item_end clears the per-item state. */
  request = zzplay_controller_take_request(&app->ctl, &jump_index,
                                           &seek_ms);
  zzplay_controller_item_end(&app->ctl);

  switch (request) {
    case ZZPLAY_REQUEST_QUIT:
      return ZZPLAY_APP_QUIT;
    case ZZPLAY_REQUEST_STOP:
      return app->player_mode ? ZZPLAY_APP_IDLE : ZZPLAY_APP_EXIT;
    case ZZPLAY_REQUEST_NEXT:
      return ZZPLAY_APP_USER_NEXT;
    case ZZPLAY_REQUEST_PREVIOUS:
      return ZZPLAY_APP_USER_PREVIOUS;
    case ZZPLAY_REQUEST_JUMP:
      app->jump_index = jump_index;
      return ZZPLAY_APP_JUMP;
    case ZZPLAY_REQUEST_PLAY:
    case ZZPLAY_REQUEST_SEEK:
      app->start_seek_ms = seek_ms;
      return ZZPLAY_APP_START;
    default:
      break;
  }
  switch (outcome) {
    case ZZPLAY_ENGINE_EOF:
      /* A retry that played through clears an earlier failure, so repeat
       * and later passes stop skipping the entry. */
      if (app->playlist.current >= 0) {
        zzplay_playlist_mark_failed(&app->playlist,
                                    (uint32_t)app->playlist.current, 0);
      }
      return ZZPLAY_APP_AUTO;
    case ZZPLAY_ENGINE_FAILED:
      /* Not `index`: the playlist may have been edited during playback.
       * `current` follows the entry through removals and moves, and is -1
       * when the entry itself was removed (nothing left to mark). */
      if (app->playlist.current >= 0) {
        zzplay_playlist_mark_failed(&app->playlist,
                                    (uint32_t)app->playlist.current, 1);
      }
      app->had_failure = 1;
      return ZZPLAY_APP_AUTO;
    case ZZPLAY_ENGINE_QUIT:
      return ZZPLAY_APP_QUIT;
    case ZZPLAY_ENGINE_STOPPED:
    default:
      return app->player_mode ? ZZPLAY_APP_IDLE : ZZPLAY_APP_EXIT;
  }
}

/* Player-mode idle wait: sleep until the GUI has input (or Ctrl-C), then
 * turn the resulting request into the next step. */
/* Player-mode idle wait services a ready modal before sleeping, so STOP cannot
 * strand an operation that became runnable during item teardown. */
static ZZPlayAppStep zzplay_app_idle(ZZPlayApp *app)
{
  for (;;) {
    ZZPlayRequest request;
    int32_t jump_index = 0;
    uint32_t seek_ms = 0U;

    zzplay_app_pump(app);
    if (zzplay_ctrl_c_requested != 0) {
      return ZZPLAY_APP_QUIT;
    }
    if (app->ctl.request == ZZPLAY_REQUEST_NONE) {
      uint32_t mask = zzplay_gui_signal_mask() | SIGBREAKF_CTRL_C;
      uint32_t signals = Wait(mask);

      if (zzplay_ctrl_c_requested != 0 ||
          (signals & SIGBREAKF_CTRL_C) != 0U) {
        zzplay_ctrl_c_requested = 1;
        return ZZPLAY_APP_QUIT;
      }
      zzplay_app_pump(app);
    }
    if (app->ctl.request == ZZPLAY_REQUEST_NONE) {
      continue;
    }
    request = zzplay_controller_take_request(&app->ctl, &jump_index,
                                             &seek_ms);
    switch (request) {
      case ZZPLAY_REQUEST_QUIT:
        return ZZPLAY_APP_QUIT;
      case ZZPLAY_REQUEST_STOP:
        return ZZPLAY_APP_IDLE;
      case ZZPLAY_REQUEST_NEXT:
        return ZZPLAY_APP_USER_NEXT;
      case ZZPLAY_REQUEST_PREVIOUS:
        return ZZPLAY_APP_USER_PREVIOUS;
      case ZZPLAY_REQUEST_JUMP:
        app->jump_index = jump_index;
        return ZZPLAY_APP_JUMP;
      case ZZPLAY_REQUEST_PLAY:
      case ZZPLAY_REQUEST_SEEK:
        if (app->playlist.count == 0U) {
          zzplay_launch_reportf(&app->ctl, &app->options,
                                "the playlist is empty");
          continue;
        }
        app->start_seek_ms = seek_ms;
        return ZZPLAY_APP_START;
      default:
        continue;
    }
  }
}

static int zzplay_app_run(ZZPlayApp *app)
{
  ZZPlayAppStep step;
  int32_t index;

  /* A one-shot run plays the list and exits. The player starts the files
   * it was launched with, and an empty player waits for input. */
  step = (app->player_mode && app->playlist.count == 0U)
             ? ZZPLAY_APP_IDLE
             : ZZPLAY_APP_START;
  if (!app->player_mode && app->playlist.count == 0U) {
    return app->had_failure ? 20 : 0;
  }
  while (step != ZZPLAY_APP_EXIT && step != ZZPLAY_APP_QUIT) {
    switch (step) {
      case ZZPLAY_APP_IDLE:
        step = zzplay_app_idle(app);
        break;
      case ZZPLAY_APP_START:
        /* PLAY restarts the current entry; with none current the list
         * starts from its first entry in play order. */
        index = app->playlist.current >= 0
                    ? app->playlist.current
                    : zzplay_playlist_next(&app->playlist,
                                           ZZPLAY_ADVANCE_AUTO);
        step = index >= 0 ? zzplay_app_play_index(app, index)
                          : (app->player_mode ? ZZPLAY_APP_IDLE
                                              : ZZPLAY_APP_EXIT);
        break;
      case ZZPLAY_APP_AUTO:
        /* AUTO honours repeat-track; failed entries are skipped, and an
         * exhausted (or all-failed) list ends the run instead of
         * spinning on it. */
        index = zzplay_playlist_next(&app->playlist,
                                     ZZPLAY_ADVANCE_AUTO);
        step = index >= 0 ? zzplay_app_play_index(app, index)
                          : (app->player_mode ? ZZPLAY_APP_IDLE
                                              : ZZPLAY_APP_EXIT);
        break;
      case ZZPLAY_APP_USER_NEXT:
        index = zzplay_playlist_next(&app->playlist,
                                     ZZPLAY_ADVANCE_USER);
        step = index >= 0 ? zzplay_app_play_index(app, index)
                          : (app->player_mode ? ZZPLAY_APP_IDLE
                                              : ZZPLAY_APP_EXIT);
        break;
      case ZZPLAY_APP_USER_PREVIOUS:
        index = zzplay_playlist_previous(&app->playlist);
        step = index >= 0 ? zzplay_app_play_index(app, index)
                          : (app->player_mode ? ZZPLAY_APP_IDLE
                                              : ZZPLAY_APP_EXIT);
        break;
      case ZZPLAY_APP_JUMP:
        if (app->jump_index >= 0 &&
            (uint32_t)app->jump_index < app->playlist.count) {
          step = zzplay_app_play_index(app, app->jump_index);
        } else {
          step = app->player_mode ? ZZPLAY_APP_IDLE : ZZPLAY_APP_EXIT;
        }
        break;
      default:
        step = ZZPLAY_APP_EXIT;
        break;
    }
  }
  if (step == ZZPLAY_APP_QUIT) {
    return 0;
  }
  if (!app->player_mode && app->had_failure) {
    return 20;
  }
  return 0;
}

int main(int argc, char **argv)
{
  ZZPlayOptionsResult options_result;
  uint32_t i;
  int exit_code;

  zzplay_ctrl_c_requested = 0;
  (void)signal(SIGINT, zzplay_sigint_handler);
  memset(&runtime, 0, sizeof(runtime));
  memset(&probe, 0, sizeof(probe));
  memset(&info, 0, sizeof(info));
  memset(&app, 0, sizeof(app));

  options_result = zzplay_launch_begin(argc, argv, &app.options,
                                       &launch);
  if (options_result == ZZPLAY_OPTIONS_HELP) {
    zzplay_usage(stdout);
    zzplay_launch_end(&launch);
    return 0;
  }
  if (options_result != ZZPLAY_OPTIONS_OK) {
    /* A Workbench launch has no console: the requester is the only place
     * the user will ever see this. */
    if (app.options.launch == ZZPLAY_LAUNCH_WORKBENCH) {
      zzplay_launch_report(&app.options,
                           "An icon ToolType is invalid.");
    } else {
      zzplay_usage(stderr);
    }
    zzplay_launch_end(&launch);
    return 20;
  }

  zzplay_set_quiet(app.options.quiet);
  app.player_mode = zzplay_options_wants_player(&app.options);

  /* Settings: defaults < ENV:ZZPlay.prefs < this launch's overrides. */
  zzplay_prefs_defaults(&app.prefs);
  (void)zzplay_prefs_load(&app.prefs, ZZPLAY_PREFS_ENV_PATH);
  app.stored_prefs = app.prefs;
  zzplay_prefs_apply_options(&app.prefs, &app.options);
  {
    ZZPlayRepeat repeat = app.prefs.repeat;

    /* CLI LOOP (forever) starts life as repeat ONE, which the engines
     * execute as their seamless loop; LOOP=N stays engine-internal and
     * starts with repeat off, or a saved repeat would outlast its count.
     * Like other launch options, neither is saved as the new default. */
    if (app.options.loop_mode == ZZPLAY_LOOP_FOREVER) {
      repeat = ZZPLAY_REPEAT_ONE;
    } else if (app.options.loop_mode == ZZPLAY_LOOP_FINITE) {
      repeat = ZZPLAY_REPEAT_OFF;
    }
    zzplay_controller_init(&app.ctl, app.prefs.volume, repeat,
                           app.prefs.shuffle);
  }
  app.started_prefs = app.prefs;
  app.started_prefs.volume = app.ctl.volume;
  app.started_prefs.repeat = app.ctl.repeat;
  app.started_prefs.shuffle = app.ctl.shuffle;

  zzplay_playlist_init(&app.playlist);
  for (i = 0U; i < app.options.path_count; i++) {
    uint32_t added =
        zzplay_files_add(&app.playlist, app.options.paths[i]);

    if (added == 0U) {
      zzplay_launch_reportf(&app.ctl, &app.options,
                            "%s added no playable files",
                            app.options.paths[i]);
      app.had_failure = 1;
    }
  }
  /* The playlist navigates by the same repeat/shuffle the user sees. A
   * remembered shuffle is seeded from the clock so each launch gets its
   * own order. */
  zzplay_playlist_set_repeat(&app.playlist, app.ctl.repeat);
  {
    struct DateStamp now;
    uint32_t seed;

    DateStamp(&now);
    seed = ((uint32_t)now.ds_Days << 20) ^ ((uint32_t)now.ds_Minute << 8) ^
           (uint32_t)now.ds_Tick;
    zzplay_playlist_set_shuffle(&app.playlist, app.ctl.shuffle,
                                seed != 0U ? seed : 1U);
  }

  if (app.options.trace_path) {
    app.trace = Open((CONST_STRPTR)app.options.trace_path,
                     MODE_NEWFILE);
    if (app.trace) {
      static const char header[] =
              "# zzplay trace v1: F frame t=ms v=videoMs m=masterMs "
              "dr=driftMs d=decision(P/H/D/N) dec=decodeUs gap=us "
              "acc=inputBytesSinceLastF ni=needInputPolls "
              "wb=writeBusy db=decodeBusy rd=fileReads "
              "rmax=maxReadUs q=audioQueuedFrames und=underruns\n";

      /* No GetSysTime here: TimerBase is not open yet (see
       * zzplay_trace_ms). The clock anchors on the first traced
       * frame, after the timer is open. */
      (void)Write(app.trace, (APTR)header,
                  (LONG)(sizeof(header) - 1U));
    } else {
      zzplay_info("zzplay: cannot open trace file %s\n",
                  app.options.trace_path);
    }
  }

  /* The desktop player opens its window up front; a one-shot run opens
   * it lazily before an audio-only item. */
  if (app.player_mode) {
    zzplay_app_open_gui(&app);
    if (!zzplay_gui_is_open()) {
      /* Without a window the idle wait could only ever be left with
       * Ctrl-C - invisible and unkillable from Workbench. Play what was
       * named and exit instead, reporting errors as a one-shot run does. */
      app.player_mode = 0;
      app.options.player = 0;
    }
  }
  if (!app.player_mode) {
    /* A one-shot run plays its list once, in order: remembered repeat
     * and shuffle belong to the desktop player, and only this launch's
     * LOOP may repeat a file. Also covers a player whose window failed. */
    zzplay_controller_set_repeat(
        &app.ctl, app.options.loop_mode == ZZPLAY_LOOP_FOREVER
                      ? ZZPLAY_REPEAT_ONE
                      : ZZPLAY_REPEAT_OFF);
    zzplay_controller_set_shuffle(&app.ctl, 0);
    zzplay_playlist_set_repeat(&app.playlist, app.ctl.repeat);
    zzplay_playlist_set_shuffle(&app.playlist, 0, 0U);
  }

  exit_code = zzplay_app_run(&app);

  /* Closing the window records its final position in prefs, so it must
   * happen before the session is saved. */
  zzplay_gui_close();
  if (app.player_mode) {
    app.prefs.volume = app.ctl.volume;
    app.prefs.repeat = app.ctl.repeat;
    app.prefs.shuffle = app.ctl.shuffle;
    zzplay_prefs_settle_session(&app.prefs, &app.stored_prefs,
                                &app.started_prefs);
    (void)zzplay_prefs_save_session(&app.prefs,
                                    ZZPLAY_PREFS_ENV_PATH);
    (void)zzplay_prefs_save_session(&app.prefs,
                                    ZZPLAY_PREFS_ENVARC_PATH);
  }
  if (app.trace) {
    Close(app.trace);
    app.trace = 0;
  }
  zzplay_playlist_free(&app.playlist);
  zzplay_launch_end(&launch);
  return exit_code;
}
