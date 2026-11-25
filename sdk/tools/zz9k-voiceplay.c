/*
 * Plays a ProTracker module, or a once-per-second click, through the ZZ9000AX
 * voice engine with zz9k.library.
 *
 * MODULE mode runs libpt4 one replayer tick at a time (zz9k-voice-pt4) and
 * writes each tick's voice events into the engine's command ring as
 * timestamped records. The module's sample arena is uploaded as sample 0.
 * The first tick is stamped at the granted lead, so the engine frames before
 * it are silence; every later tick keeps its distance to the first. A tick is
 * run when its song frame is closer to the playing frame than the
 * granted lead, which keeps the ring a lead ahead of the player and never
 * further. Each pass reads the status block (which is also the ring
 * heartbeat), writes what is pending and sleeps about 5 ms.
 *
 * CLICK mode starts a short pulse once per second at lead + k * 48000, at
 * 48000 Hz so it plays 1:1. It is for listening, and for checking the
 * scheduling margin.
 *
 * LIVE mode plays notes from the keyboard: each key writes a START at the
 * status horizon, the earliest frame the engine has not rendered, on the next
 * of eight voices, with the pitch set by the key. It is for the ear test of
 * the low-latency session, and counts the records the engine found late.
 *
 * LOAD mode is a steady synthetic load for the firmware's render bench: it
 * loops one 16-bit sample (a few sine cycles plus a little noise, mono or
 * interleaved stereo) on VOICES voices (1 to 32, default 32) at rates spread
 * from half to twice RATE (default 22050 Hz), volume 65535 divided by the
 * voice count per side (GAIN=n replaces that with an explicit volume per
 * side, 0 to 65535), for SECONDS (default 20). The AUDIO_VOICE_BENCH
 * firmware prints its per-period render times when the session closes.
 *
 * LOWLAT opens a low-latency session: the firmware shortens its audio
 * fill-ahead while it runs and the lead floor drops from 8 periods to 3. The
 * tool falls back to the standard floor, and says so, when zz9k.library is
 * older than revision 43 or the firmware refuses the flag.
 *
 * The report counts what the firmware saw (late, rejected, underruns,
 * dropped), the smallest scheduling margin (record stamp minus the engine
 * horizon when the record was written, in frames), and the 68k time one tick
 * takes (replayer tick, record mapping and ring writes). It also reports the
 * record latency (min, average and max of record stamp minus the playing
 * frame of the status read before the write, in frames and milliseconds: how
 * far ahead of the sound each record was queued, click records included) and
 * the engine's measured rate (playing frames advanced per 68k EClock time
 * between the first and the last live status read, in Hz, and its difference
 * from 48000 in ppm). A run ends with exit code 20 when the card closes the
 * session on its own, which happens when no status read reaches it for 2 s.
 *
 * usage: see the text printed without arguments.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "proto/zz9k.h"
#include "zz9k/sdk.h"
#include "zz9k/text.h"
#include "zz9k/voice.h"

#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <exec/ports.h>
#include <exec/types.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/timer.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zz9k-voice-pt4.h"

#define TEMPLATE "MODULE,LEAD/K/N,NTSC/S,LINEAR/S,SECONDS/K/N,LOWLAT/S," \
                 "VOICES/K/N,STEREO/S,RATE/K/N,RAMP/K/N,CHURN/S,GROUPS/K/N," \
                 "LIMITER/S,GAIN/K/N,INTERP/K,PERVOICE/S"
#define OPT_MODULE 0
#define OPT_LEAD 1
#define OPT_NTSC 2
#define OPT_LINEAR 3
#define OPT_SECONDS 4
#define OPT_LOWLAT 5
#define OPT_VOICES 6
#define OPT_STEREO 7
#define OPT_RATE 8
#define OPT_RAMP 9
#define OPT_CHURN 10
#define OPT_GROUPS 11
#define OPT_LIMITER 12
#define OPT_GAIN 13
#define OPT_INTERP 14
#define OPT_PERVOICE 15
#define OPT_COUNT 16

/* CHURN: a restart interval of the ramp length, but not below 32 frames (a
 * shorter one would write a record for every frame). */
#define CHURN_MIN_INTERVAL 32U
#define CHURN_DEFAULT_INTERVAL 256U
#define CHURN_RING_BYTES 8192U

/* The lead floor of a session without the low-latency flag. */
#define STANDARD_FLOOR_FRAMES 7680U
/* About 10.7 ms in the formatter FIFO, inferred from the RTL and not
 * measured; with the lead it bounds when a record sounds. */
#define FIFO_FRAMES 514U

/* One tick of a 32 voice module is at most six events per voice. */
#define MAX_PENDING 256U
#define PASS_MICROS 5000U
#define STALL_MICROS 3000000U
#define CLICK_BYTES 48U
#define CLICK_BANK_BYTES 64U
#define CLICK_VOLUME 0x4000UL
#define CLICK_DEFAULT_SECONDS 10UL
#define BANK_ROUND 64U

/* LOAD mode: one looping 16-bit sample of 4096 frames. The left side is 8
 * sine cycles and the right side of a stereo sample 11, so both wrap
 * seamlessly; the noise is +-256 on a peak of 20000. */
#define LOAD_FRAMES 4096U
#define LOAD_PEAK 20000L
#define LOAD_CYCLES_LEFT 8U
#define LOAD_CYCLES_RIGHT 11U
#define LOAD_DEFAULT_VOICES 32U
#define LOAD_DEFAULT_RATE 22050U
#define LOAD_DEFAULT_SECONDS 20UL
/* next_frame once the load records are written: never produce again. */
#define LOAD_DONE_FRAME 0xFFFFFFFFUL

/* libnix stack size request, honoured by the swapstack.o the build links:
 * libpt4's pt4_open keeps about 3 KiB of its own on the stack. */
unsigned long __stack = 32768UL;

struct Library *ZZ9KBase;
struct Device *TimerBase;

/* Libnix would otherwise end the program at the first library call after a
 * Ctrl-C; the player polls the signal itself so it can reset and close. */
void __chkabort(void)
{
}

static struct MsgPort *timer_port;
static struct timerequest *timer_request;
static uint32_t eclock_hz;

static ZZ9KVoiceSession session;
static ZZ9KVoiceStatus status;
static ZZ9KVoiceRecord pending[MAX_PENDING];
static uint32_t pending_count;
static uint32_t pending_done;
static int pending_overflow;

static uint32_t lead_frames;
static uint32_t voice_count;

static uint32_t stat_ticks;
static uint32_t stat_records;
static uint32_t stat_ring_waits;
static int32_t stat_min_margin;
static int stat_have_margin;
static uint64_t stat_work_sum;
static uint64_t stat_work_max;
static uint64_t tick_work;
static int64_t stat_latency_sum;
static int32_t stat_latency_min;
static int32_t stat_latency_max;
static int stat_have_drift;
static uint32_t drift_first_frame;
static uint32_t drift_last_frame;
static uint64_t drift_first_ticks;
static uint64_t drift_last_ticks;

static ZZ9KPt4Events events;

static uint64_t now_ticks(void)
{
  struct EClockVal value;

  eclock_hz = ReadEClock(&value);
  return ((uint64_t)value.ev_hi << 32) | value.ev_lo;
}

static unsigned long ticks_to_us(uint64_t ticks)
{
  if (eclock_hz == 0U) {
    return 0UL;
  }
  return (unsigned long)((ticks * 1000000ULL) / eclock_hz);
}

static void pause_pass(void)
{
  timer_request->tr_node.io_Command = TR_ADDREQUEST;
  timer_request->tr_time.tv_secs = 0;
  timer_request->tr_time.tv_micro = PASS_MICROS;
  DoIO((struct IORequest *)timer_request);
}

static int is_word(const char *name, const char *word)
{
  uint32_t i;

  for (i = 0U;; i++) {
    char c = name[i];

    if (c >= 'a' && c <= 'z') {
      c = (char)(c - 'a' + 'A');
    }
    if (c != word[i]) {
      return 0;
    }
    if (word[i] == '\0') {
      return 1;
    }
  }
}

static void usage(void)
{
  puts("zz9k-voiceplay: plays a module, a click or the keyboard through the ZZ9000AX voice engine");
  puts("usage: zz9k-voiceplay MODULE [LEAD=frames] [NTSC] [LINEAR] [SECONDS=n] [LOWLAT]");
  puts("       zz9k-voiceplay CLICK [LEAD=frames] [SECONDS=n] [LOWLAT]");
  puts("       zz9k-voiceplay LIVE [LEAD=frames] [SECONDS=n] [LOWLAT]");
  puts("       zz9k-voiceplay LOAD VOICES=n SECONDS=n [LINEAR] [STEREO] [RATE=hz] [GAIN=n] [CHURN] [GROUPS=n] [LOWLAT]");
  puts("       every mode also takes RAMP=frames, LIMITER, INTERP=nearest|linear|cubic and PERVOICE");
  puts("  MODULE   ProTracker module to play (DYN envelopes and EFx funk are refused)");
  puts("  CLICK    a short pulse once per second at 48000 Hz, for listening and");
  puts("           for checking the scheduling margin; 10 seconds unless SECONDS");
  puts("  LIVE     play notes from the keyboard: zsxdcvgbhnjm is one octave, then");
  puts("           q2w3er5t6y7u and i; ESC stops. Each key starts a note at the");
  puts("           earliest frame the engine has not rendered");
  puts("  LOAD     a steady load for the firmware render bench: VOICES voices (1 to");
  puts("           32, default 32) loop one 16-bit sine sample at rates spread from");
  puts("           half to twice RATE (default 22050 Hz, at most 192000), volume");
  puts("           65535 / VOICES per side; 20 seconds unless SECONDS. STEREO uses an");
  puts("           interleaved stereo sample (needs a firmware with the stereo");
  puts("           formats). Meant for a firmware built with AUDIO_VOICE_BENCH,");
  puts("           which prints the render times on its UART when the session closes");
  puts("           CHURN restarts the voices over and over, so that with RAMP the");
  puts("           fade pool stays full: every voice restarts every RAMP frames");
  puts("           (256 without RAMP), in GROUPS groups spread evenly over that");
  puts("           time (1 to 32, default 32; 1 restarts all voices together)");
  puts("           GAIN sets the volume of every voice, per side, 0 to 65535, in");
  puts("           place of 65535 / VOICES. GAIN=65535 on 32 voices is far past");
  puts("           full scale: it drives the master clip or the limiter knee and");
  puts("           the full-volume tails of a CHURN restart with RAMP. It is very");
  puts("           loud and can be harsh: turn the amplifier or speakers down");
  puts("           first, and use headphones only at a low level");
  puts("  RAMP     CONFIG ramp length in frames (0 to 512) sent before anything");
  puts("           else, for any mode: volume changes, starts, stops and restarts");
  puts("           fade over that many frames. Needs a firmware with CONFIG");
  puts("  LIMITER  CONFIG static soft knee, for any mode, in the same CONFIG");
  puts("           record as RAMP: magnitudes up to 24576 pass unchanged and");
  puts("           louder ones bend toward, never onto, the 16-bit rails. Needs a");
  puts("           firmware with CONFIG. A firmware that does not advertise");
  puts("           LIMITER runs it unqualified, for its qualification only");
  puts("  LEAD     lead in 48 kHz frames; 0 or absent is the firmware floor");
  puts("  NTSC     Paula clock of the module is NTSC (else PAL)");
  puts("  LINEAR   linear interpolation (else nearest)");
  puts("  INTERP   nearest, linear or cubic: the interpolation of the session,");
  puts("           sent before anything else as a CONFIG default (or with");
  puts("           PERVOICE as a MODE record for each voice), and in place of");
  puts("           LINEAR. Needs a firmware with the MODE and default records;");
  puts("           cubic and these records run unqualified, with a warning, on a");
  puts("           firmware that does not advertise CUBIC and VOICE_INTERP");
  puts("  PERVOICE with INTERP: one MODE record per voice instead of the CONFIG");
  puts("           default");
  puts("  SECONDS  stop after this many seconds of engine time");
  puts("  LOWLAT   low-latency session: shorter audio fill-ahead while it runs and");
  puts("           a lead floor of 3 periods (2880 frames) instead of 8. Without");
  puts("           firmware or library support the standard floor is used");
  puts("Ctrl-C stops, resets and closes. The report lists frames, ticks, records,");
  puts("ring-full waits, late, rejected, underruns, dropped, the jitter the");
  puts("firmware measured, the smallest margin, the record latency (min, average,");
  puts("max in frames and ms), the measured engine rate in Hz with its ppm");
  puts("difference from 48000, and the 68k time per tick. The engine need not be");
  puts("advertised as a service.");
}

/* Frames at 48 kHz as milliseconds with two decimals, truncated. */
static void format_ms(char *out, long long frames)
{
  long long ms100 = (frames * 25LL) / 12LL;
  unsigned long long magnitude = ms100 < 0 ? -ms100 : ms100;

  sprintf(out, "%s%lu.%02lu", ms100 < 0 ? "-" : "",
          (unsigned long)(magnitude / 100ULL),
          (unsigned long)(magnitude % 100ULL));
}

/* Remembers a status read of a live session for the rate measurement. */
static void note_live(uint32_t played, uint64_t ticks)
{
  if (!stat_have_drift) {
    drift_first_frame = played;
    drift_first_ticks = ticks;
    stat_have_drift = 1;
  }
  drift_last_frame = played;
  drift_last_ticks = ticks;
}

/* ---- Record building ---- */

static int on_left(uint32_t voice)
{
  return (voice & 3U) == 0U || (voice & 3U) == 3U;
}

static ZZ9KVoiceRecord *pending_next(uint32_t stamp, uint32_t voice,
                                     uint32_t op)
{
  ZZ9KVoiceRecord *record;

  if (pending_count >= MAX_PENDING) {
    pending_overflow = 1;
    return NULL;
  }
  record = &pending[pending_count++];
  memset(record, 0, sizeof(*record));
  record->stamp = stamp;
  record->voice = (uint8_t)voice;
  record->op = (uint8_t)op;
  return record;
}

/* The event to record mapping of the host oracle test: sample 0 is the whole
 * arena, volume is vol * 1024 / voices on the voice's side only, hard pan. */
static void map_event(void *user, const ZZ9KPt4Event *event)
{
  ZZ9KVoiceRecord *record;
  uint32_t stamp = lead_frames + event->frame;
  uint32_t volume;

  (void)user;
  switch (event->kind) {
  case ZZ9K_PT4_EV_START:
    record = pending_next(stamp, event->voice, ZZ9K_VOICE_OP_START);
    if (record) {
      record->flags = 0U;
      record->arg0 = event->a;
      record->arg1 = event->b;
    }
    break;
  case ZZ9K_PT4_EV_STOP:
    pending_next(stamp, event->voice, ZZ9K_VOICE_OP_STOP);
    break;
  case ZZ9K_PT4_EV_LOOP:
    record = pending_next(stamp, event->voice, ZZ9K_VOICE_OP_LOOP);
    if (record) {
      record->arg0 = event->a;
      record->arg1 = event->b;
    }
    break;
  case ZZ9K_PT4_EV_DIRECTION:
    record = pending_next(stamp, event->voice, ZZ9K_VOICE_OP_DIRECTION);
    if (record) {
      record->flags = event->a ? ZZ9K_VOICE_DIR_BACKWARD : 0U;
    }
    break;
  case ZZ9K_PT4_EV_PERIOD:
    record = pending_next(stamp, event->voice, ZZ9K_VOICE_OP_SET);
    if (record) {
      record->flags = ZZ9K_VOICE_SET_RATE;
      record->arg0 = event->a;
    }
    break;
  case ZZ9K_PT4_EV_VOLUME:
    record = pending_next(stamp, event->voice, ZZ9K_VOICE_OP_SET);
    if (record) {
      volume = event->a * 1024U / voice_count;
      record->flags = ZZ9K_VOICE_SET_VOLUME;
      record->arg1 = on_left(event->voice) ? (volume << 16) : volume;
    }
    break;
  default:
    break;
  }
}

/* ---- Ring writing ---- */

/* Writes what the ring takes of the pending records. Returns 1 when none is
 * left. The margin is taken against the horizon of the status read this
 * pass: the first record written is the one with the smallest stamp. */
static int flush_pending(void)
{
  uint64_t start;
  uint32_t left = pending_count - pending_done;
  uint32_t written;

  if (left == 0U) {
    return 1;
  }
  start = now_ticks();
  written = ZZ9KVoiceWrite(&session, pending + pending_done, left);
  tick_work += now_ticks() - start;
  if (written > 0U) {
    int32_t margin =
        (int32_t)(pending[pending_done].stamp - status.horizon);
    uint32_t i;

    if (!stat_have_margin || margin < stat_min_margin) {
      stat_min_margin = margin;
      stat_have_margin = 1;
    }
    for (i = 0U; i < written; i++) {
      int32_t latency = (int32_t)(pending[pending_done + i].stamp -
                                  status.playing_frame);

      if (stat_records + i == 0U || latency < stat_latency_min) {
        stat_latency_min = latency;
      }
      if (stat_records + i == 0U || latency > stat_latency_max) {
        stat_latency_max = latency;
      }
      stat_latency_sum += latency;
    }
    stat_records += written;
    pending_done += written;
  }
  if (pending_done < pending_count) {
    stat_ring_waits++;
    return 0;
  }
  pending_count = 0U;
  pending_done = 0U;
  stat_ticks++;
  stat_work_sum += tick_work;
  if (tick_work > stat_work_max) {
    stat_work_max = tick_work;
  }
  return 1;
}

/* ---- Module and click sources ---- */

static uint8_t *load_file(const char *path, uint32_t *length)
{
  FILE *file = fopen(path, "rb");
  uint8_t *data = NULL;
  long size;

  if (!file) {
    return NULL;
  }
  if (fseek(file, 0, SEEK_END) == 0 && (size = ftell(file)) > 0 &&
      size <= 0x7fffffffL && fseek(file, 0, SEEK_SET) == 0) {
    data = (uint8_t *)malloc((size_t)size);
    if (data && fread(data, 1, (size_t)size, file) != (size_t)size) {
      free(data);
      data = NULL;
    }
    if (data) {
      *length = (uint32_t)size;
    }
  }
  fclose(file);
  return data;
}

static uint8_t click_sample[CLICK_BYTES];

/* A 1 ms pulse: an 8 kHz tone with an eighth of its level lost every frame. */
static void make_click(void)
{
  uint32_t amplitude = 127U;
  uint32_t i;

  for (i = 0U; i < CLICK_BYTES; i++) {
    int value = (int)amplitude;

    click_sample[i] = (uint8_t)(int8_t)(((i / 3U) & 1U) ? -value : value);
    amplitude = amplitude * 7U / 8U;
  }
}

/* ---- Session ---- */

typedef struct Source {
  int click;
  int load;
  uint32_t next_frame;  /* song frame of the next tick, or click */
  uint32_t click_count;
  int ended;
  uint32_t end_stamp;
} Source;

static void click_produce(Source *source)
{
  uint32_t stamp = lead_frames + source->next_frame;
  ZZ9KVoiceRecord *record;

  if (source->click_count == 0U) {
    record = pending_next(stamp, 0U, ZZ9K_VOICE_OP_SET);
    if (record) {
      record->flags = ZZ9K_VOICE_SET_RATE | ZZ9K_VOICE_SET_RATE_HZ |
                      ZZ9K_VOICE_SET_VOLUME;
      record->arg0 = ZZ9K_VOICE_OUTPUT_RATE;
      record->arg1 = (CLICK_VOLUME << 16) | CLICK_VOLUME;
    }
  }
  record = pending_next(stamp, 0U, ZZ9K_VOICE_OP_START);
  if (record) {
    record->flags = 0U;
    record->arg0 = 0U;
    record->arg1 = 0U;
  }
  source->click_count++;
  source->next_frame += ZZ9K_VOICE_OUTPUT_RATE;
}

/* ---- Load source ---- */

static uint8_t load_sample[LOAD_FRAMES * 4U];
static uint32_t load_voices;
static uint32_t load_rate;
static uint32_t load_stereo;
static uint32_t load_bytes;
static int load_linear;
static int load_mode;

/* RAMP: the CONFIG ramp length sent first. LIMITER: the CONFIG soft knee
 * switch. CHURN: restart groups, 0 = none. load_volume: the LOAD volume per
 * side, 65535 / VOICES unless GAIN. */
static int ramp_set;
static uint32_t ramp_frames;
static int limiter_set;

/* INTERP: the interpolation asked for, sent as the CONFIG default or, with
 * PERVOICE, as a MODE record for each voice. It takes the place of LINEAR. */
static int interp_set;
static int interp_pervoice;
static uint32_t interp_value;

static int parse_interp(const char *text)
{
  if (is_word(text, "NEAREST")) {
    interp_value = ZZ9K_VOICE_INTERP_NEAREST;
  } else if (is_word(text, "LINEAR")) {
    interp_value = ZZ9K_VOICE_INTERP_LINEAR;
  } else if (is_word(text, "CUBIC")) {
    interp_value = ZZ9K_VOICE_INTERP_CUBIC;
  } else {
    return 0;
  }
  interp_set = 1;
  return 1;
}

/* What the session reads with: INTERP if given, else LINEAR or nearest. */
static const char *interp_name(void)
{
  if (interp_set) {
    return interp_value == ZZ9K_VOICE_INTERP_CUBIC ? "cubic"
           : interp_value == ZZ9K_VOICE_INTERP_LINEAR ? "linear" : "nearest";
  }
  return load_linear ? "linear" : "nearest";
}
static uint32_t load_volume;
static uint32_t churn_groups;
static uint32_t churn_interval;
static uint32_t churn_step;
static uint32_t churn_slot;
static uint32_t stat_restarts;
static int load_started;

/* A sine of LOAD_PEAK at phase (0 to LOAD_FRAMES - 1 is one cycle), by
 * Bhaskara's rational approximation on each half cycle. */
static long load_sine(uint32_t phase)
{
  const unsigned long long half = LOAD_FRAMES / 2U;
  unsigned long long p = phase % half;
  unsigned long long a = p * (half - p);
  long value = (long)(((unsigned long long)LOAD_PEAK * 16ULL * a) /
                      (5ULL * half * half - 4ULL * a));

  return phase >= half ? -value : value;
}

/* Big-endian 16-bit samples, interleaved when stereo, with a little noise
 * so the data is not a pure table. */
static void make_load_sample(void)
{
  uint32_t noise = 0x2545F491UL;
  uint32_t out = 0U;
  uint32_t i;

  for (i = 0U; i < LOAD_FRAMES; i++) {
    uint32_t side;

    for (side = 0U; side <= load_stereo; side++) {
      uint32_t cycles = side == 0U ? LOAD_CYCLES_LEFT : LOAD_CYCLES_RIGHT;
      long value;
      uint16_t raw;

      noise = noise * 1664525UL + 1013904223UL;
      value = load_sine((i * cycles) % LOAD_FRAMES) +
              (long)((noise >> 16) & 0x1FFU) - 256L;
      raw = (uint16_t)(int16_t)value;
      load_sample[out++] = (uint8_t)(raw >> 8);
      load_sample[out++] = (uint8_t)(raw & 0xFFU);
    }
  }
  load_bytes = out;
}

/* Half to twice RATE spread evenly over the voices; one voice plays RATE. */
static uint32_t load_voice_rate(uint32_t voice)
{
  uint32_t rate = load_rate;

  if (load_voices > 1U) {
    rate = load_rate / 2U +
           (uint32_t)(((unsigned long long)(load_rate + load_rate / 2U) *
                       voice) / (load_voices - 1U));
  }
  if (rate > ZZ9K_VOICE_RATE_MAX_HZ) {
    rate = ZZ9K_VOICE_RATE_MAX_HZ;
  }
  return rate == 0U ? 1U : rate;
}

static void load_start(Source *source)
{
  uint32_t stamp = lead_frames + source->next_frame;
  uint32_t volume = load_volume;
  uint32_t voice;

  for (voice = 0U; voice < load_voices; voice++) {
    ZZ9KVoiceRecord *record = pending_next(stamp, voice, ZZ9K_VOICE_OP_SET);

    if (record) {
      record->flags = ZZ9K_VOICE_SET_RATE | ZZ9K_VOICE_SET_RATE_HZ |
                      ZZ9K_VOICE_SET_VOLUME;
      record->arg0 = load_voice_rate(voice);
      record->arg1 = (volume << 16) | volume;
    }
    record = pending_next(stamp, voice, ZZ9K_VOICE_OP_START);
    if (record) {
      record->flags = 0U;
      record->arg0 = 0U;
      record->arg1 = 0U;
    }
    record = pending_next(stamp, voice, ZZ9K_VOICE_OP_LOOP);
    if (record) {
      record->arg0 = 0U;
      record->arg1 = load_bytes;
    }
  }
}

/* CHURN: every churn_step frames one group of voices (voice number modulo
 * churn_groups) restarts: a START and the LOOP over the whole sample, as a
 * player that retriggers a note. With a ramp every restart hands the old
 * sound to a fade slot, and since each voice restarts every churn_interval
 * frames (the ramp length) the slots stay in use all the time. Restarts are
 * written a batch at a time, as many as the pending array holds, up to a
 * lead ahead of the playing frame. */
static void load_churn(Source *source)
{
  uint32_t per_slot = 2U * ((load_voices + churn_groups - 1U) / churn_groups);

  while (source->next_frame < status.playing_frame + lead_frames &&
         pending_count + per_slot <= MAX_PENDING) {
    uint32_t stamp = lead_frames + source->next_frame;
    uint32_t voice;

    for (voice = churn_slot % churn_groups; voice < load_voices;
         voice += churn_groups) {
      ZZ9KVoiceRecord *record = pending_next(stamp, voice, ZZ9K_VOICE_OP_START);

      if (record) {
        record->flags = 0U;
        record->arg0 = 0U;
        record->arg1 = 0U;
      }
      record = pending_next(stamp, voice, ZZ9K_VOICE_OP_LOOP);
      if (record) {
        record->arg0 = 0U;
        record->arg1 = load_bytes;
      }
      stat_restarts++;
    }
    churn_slot++;
    source->next_frame += churn_step;
  }
}

/* The first call writes every voice's rate, volume, start and loop at the
 * granted lead. Without CHURN that is all; with it the restarts follow. */
static void load_produce(Source *source)
{
  if (!load_started) {
    load_started = 1;
    load_start(source);
    source->next_frame = churn_groups != 0U ? churn_step : LOAD_DONE_FRAME;
    churn_slot = 1U;
    return;
  }
  load_churn(source);
}

static void module_produce(Source *source)
{
  uint32_t frames = zz9k_pt4_events_tick(&events, map_event, NULL);

  if (frames == 0U) {
    source->ended = 1;
    source->end_stamp = lead_frames + events.frame;
  } else {
    source->next_frame = events.frame;
  }
}

/* ---- Live keyboard ---- */

#define LIVE_VOICES 8U
#define LIVE_BYTES 4096U
/* One 32-byte cycle played at this rate sounds middle C. */
#define LIVE_BASE_HZ 8372UL
#define LIVE_VOLUME 0x2800UL

static uint8_t live_sample[LIVE_BYTES];
static uint32_t live_next_voice;
static uint32_t stat_keys;
static int live_mode;

/* A decaying triangle wave, 32 samples to the cycle. */
static void make_live_sample(void)
{
  uint32_t i;

  for (i = 0U; i < LIVE_BYTES; i++) {
    long phase = (long)(i & 31U);
    long tri = phase < 16L ? phase : 32L - phase;
    long value = (tri - 8L) * 15L;

    value = value * (long)(LIVE_BYTES - i) / (long)LIVE_BYTES;
    live_sample[i] = (uint8_t)(int8_t)value;
  }
}

/* Semitones above middle C for a key, or -1. */
static int live_note(char key)
{
  static const char low[] = "zsxdcvgbhnjm";
  static const char high[] = "q2w3er5t6y7u";
  uint32_t i;

  if (key >= 'A' && key <= 'Z') {
    key = (char)(key - 'A' + 'a');
  }
  for (i = 0U; i < 12U; i++) {
    if (key == low[i]) {
      return (int)i;
    }
    if (key == high[i]) {
      return (int)(12U + i);
    }
  }
  return key == 'i' ? 24 : -1;
}

/* A note at stamp: the pitch, then the start, on the next voice. */
static void live_start(int note, uint32_t stamp)
{
  /* 2^(n/12) in Q16. */
  static const uint32_t ratio[12] = {
      65536UL, 69432UL, 73562UL, 77936UL, 82570UL, 87480UL,
      92682UL, 98193UL, 104032UL, 110218UL, 116772UL, 123715UL};
  uint32_t voice = live_next_voice++ % LIVE_VOICES;
  uint32_t rate = ((uint32_t)LIVE_BASE_HZ * ratio[note % 12]) >> 16;
  ZZ9KVoiceRecord *record;

  rate <<= note / 12;
  record = pending_next(stamp, voice, ZZ9K_VOICE_OP_SET);
  if (record) {
    record->flags = ZZ9K_VOICE_SET_RATE | ZZ9K_VOICE_SET_RATE_HZ |
                    ZZ9K_VOICE_SET_VOLUME;
    record->arg0 = rate;
    record->arg1 = (LIVE_VOLUME << 16) | LIVE_VOLUME;
  }
  record = pending_next(stamp, voice, ZZ9K_VOICE_OP_START);
  if (record) {
    record->flags = 0U;
    record->arg0 = 0U;
    record->arg1 = 0U;
  }
}

/* Runs until ESC, SECONDS, Ctrl-C, or the engine stops advancing. Every
 * pass reads the status (which is also the heartbeat), writes what is
 * pending and waits for a key for up to one pass. Keys are stamped at the
 * horizon of the status read of the same pass. */
static const char *live_play(uint32_t limit_frames)
{
  BPTR input = Input();
  uint32_t last_played = 0U;
  uint64_t last_change = now_ticks();

  for (;;) {
    uint32_t played;
    uint64_t now;

    if (SetSignal(0UL, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
      return "Ctrl-C";
    }
    if (ZZ9KVoiceGetStatus(&session, &status) != ZZ9K_STATUS_OK) {
      return "status read failed";
    }
    if (status.state == ZZ9K_VOICE_STATE_CLOSED) {
      return "session closed by the card (heartbeat lost or card reset)";
    }
    played = status.playing_frame;
    if (limit_frames != 0U && played >= limit_frames) {
      return "time limit";
    }
    now = now_ticks();
    if (status.state == ZZ9K_VOICE_STATE_LIVE) {
      note_live(played, now);
    }
    if (played != last_played) {
      last_played = played;
      last_change = now;
    } else if (status.state == ZZ9K_VOICE_STATE_LIVE &&
               ticks_to_us(now - last_change) > STALL_MICROS) {
      return "engine frame counter stopped advancing";
    }

    flush_pending();
    while (status.state == ZZ9K_VOICE_STATE_LIVE &&
           WaitForChar(input, 0L) != 0) {
      char key;
      int note;

      if (Read(input, &key, 1L) != 1L) {
        return "console closed";
      }
      if (key == 0x1b) {
        return "ESC";
      }
      note = live_note(key);
      if (note >= 0) {
        live_start(note, status.horizon);
        stat_keys++;
      }
    }
    if (pending_overflow) {
      return "a pass produced more records than expected";
    }
    flush_pending();
    WaitForChar(input, (LONG)PASS_MICROS);
  }
}

/* Runs until the song ends and drains, SECONDS passes, Ctrl-C, or the engine
 * stops advancing. Returns the reason. */
static const char *play(Source *source, uint32_t limit_frames)
{
  uint32_t last_played = 0U;
  uint64_t last_change = now_ticks();

  for (;;) {
    uint32_t played;
    uint64_t now;

    if (SetSignal(0UL, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
      return "Ctrl-C";
    }
    if (ZZ9KVoiceGetStatus(&session, &status) != ZZ9K_STATUS_OK) {
      return "status read failed";
    }
    if (status.state == ZZ9K_VOICE_STATE_CLOSED) {
      return "session closed by the card (heartbeat lost or card reset)";
    }
    played = status.playing_frame;
    if (limit_frames != 0U && played >= limit_frames) {
      return "time limit";
    }
    now = now_ticks();
    if (status.state == ZZ9K_VOICE_STATE_LIVE) {
      note_live(played, now);
    }
    if (played != last_played) {
      last_played = played;
      last_change = now;
    } else if (status.state == ZZ9K_VOICE_STATE_LIVE &&
               ticks_to_us(now - last_change) > STALL_MICROS) {
      return "engine frame counter stopped advancing";
    }

    flush_pending();
    while (pending_count == 0U && !source->ended &&
           source->next_frame < played + lead_frames) {
      uint64_t start = now_ticks();

      tick_work = 0U;
      if (source->click) {
        click_produce(source);
      } else if (source->load) {
        load_produce(source);
      } else {
        module_produce(source);
        if (source->ended) {
          break;
        }
      }
      tick_work += now_ticks() - start;
      if (pending_overflow) {
        return "a tick produced more records than expected";
      }
      if (!flush_pending()) {
        break;
      }
    }
    if (source->ended && pending_count == 0U &&
        played >= source->end_stamp) {
      return "song end";
    }
    pause_pass();
  }
}

static void report(const char *reason)
{
  unsigned long avg_us = 0UL;
  unsigned long max_us = ticks_to_us(stat_work_max);

  if (stat_ticks != 0U) {
    avg_us = ticks_to_us(stat_work_sum / stat_ticks);
  }
  ZZ9KVoiceGetStatus(&session, &status);
  if (status.state == ZZ9K_VOICE_STATE_LIVE) {
    note_live(status.playing_frame, now_ticks());
  }
  printf("zz9k-voiceplay: stopped: %s\n", reason);
  printf("  frames played %lu, ticks %lu, records %lu, ring-full waits %lu\n",
         (unsigned long)status.playing_frame, (unsigned long)stat_ticks,
         (unsigned long)stat_records, (unsigned long)stat_ring_waits);
  if (live_mode) {
    printf("  keys played %lu\n", (unsigned long)stat_keys);
  }
  if (load_mode) {
    printf("  load: %lu voices, %s samples, %s, rates %lu to %lu Hz, volume "
           "%lu per side\n", (unsigned long)load_voices,
           load_stereo ? "stereo" : "mono",
           interp_name(),
           (unsigned long)load_voice_rate(0U),
           (unsigned long)load_voice_rate(load_voices - 1U),
           (unsigned long)load_volume);
  }
  if (ramp_set) {
    printf("  ramp: %lu frames\n", (unsigned long)ramp_frames);
  }
  if (limiter_set) {
    puts("  limiter: on (static soft knee above 24576)");
  }
  if (interp_set) {
    printf("  interpolation: %s, set with %s\n", interp_name(),
           interp_pervoice ? "a MODE record for each voice"
                           : "the CONFIG default");
  }
  if (churn_groups != 0U) {
    printf("  churn: %lu restarts, %lu group%s, a restart every %lu frames "
           "(each voice every %lu)\n", (unsigned long)stat_restarts,
           (unsigned long)churn_groups, churn_groups == 1U ? "" : "s",
           (unsigned long)churn_step, (unsigned long)churn_interval);
  }
  printf("  session: %s, lead floor %lu frames\n",
         session.grant.lead_floor_frames < STANDARD_FLOOR_FRAMES
             ? "low latency" : "standard",
         (unsigned long)session.grant.lead_floor_frames);
  printf("  late %lu, rejected %lu, underruns %lu, dropped %lu\n",
         (unsigned long)status.late, (unsigned long)status.rejected,
         (unsigned long)status.underruns, (unsigned long)status.dropped);
  {
    ZZ9KVoiceJitter jitter;

    zz9k_voice_jitter_read(session.status, &jitter);
    if (jitter.poll_gap_max_us == 0U) {
      printf("  jitter: this firmware does not report it\n");
    } else if (jitter.margin_min_us == ZZ9K_VOICE_JITTER_NONE) {
      printf("  jitter: largest gap between firmware polls %lu us, no "
             "period taken yet\n", (unsigned long)jitter.poll_gap_max_us);
    } else {
      printf("  jitter: smallest wait of a rendered period for the audio "
             "interrupt %lu us, largest gap between firmware polls %lu us\n",
             (unsigned long)jitter.margin_min_us,
             (unsigned long)jitter.poll_gap_max_us);
    }
  }
  if (stat_have_margin) {
    printf("  smallest scheduling margin %ld frames (%ld us)\n",
           (long)stat_min_margin,
           (long)(((long long)stat_min_margin * 1000000LL) /
                  (long long)ZZ9K_VOICE_OUTPUT_RATE));
  } else {
    printf("  smallest scheduling margin: no record was written\n");
  }
  if (stat_records != 0U) {
    char min_ms[24];
    char avg_ms[24];
    char max_ms[24];
    long long average = stat_latency_sum / (long long)stat_records;

    format_ms(min_ms, stat_latency_min);
    format_ms(avg_ms, average);
    format_ms(max_ms, stat_latency_max);
    printf("  record latency min %ld, average %ld, max %ld frames "
           "(%s, %s, %s ms)\n",
           (long)stat_latency_min, (long)average, (long)stat_latency_max,
           min_ms, avg_ms, max_ms);
  } else {
    printf("  record latency: no record was written\n");
  }
  {
    uint32_t frames = drift_last_frame - drift_first_frame;
    uint64_t ticks = drift_last_ticks - drift_first_ticks;

    if (stat_have_drift && eclock_hz != 0U && ticks != 0U &&
        frames >= ZZ9K_VOICE_OUTPUT_RATE) {
      uint64_t hz100 = ((uint64_t)frames * eclock_hz * 100ULL + ticks / 2U) /
                       ticks;
      long long diff = (long long)hz100 -
                       (long long)ZZ9K_VOICE_OUTPUT_RATE * 100LL;

      printf("  engine rate %lu.%02lu Hz over %lu frames, %ld ppm from %lu\n",
             (unsigned long)(hz100 / 100ULL), (unsigned long)(hz100 % 100ULL),
             (unsigned long)frames, (long)(diff * 5LL / 24LL),
             (unsigned long)ZZ9K_VOICE_OUTPUT_RATE);
    } else {
      printf("  engine rate: less than a second of live playback\n");
    }
  }
  printf("  68k time per tick: average %lu us, max %lu us (EClock %lu Hz)\n",
         avg_us, max_us, (unsigned long)eclock_hz);
}

static void open_failed(int result)
{
  printf("zz9k-voiceplay: voice open refused: %s (%d)\n",
         zz9k_status_text(result), result);
  if (result == ZZ9K_STATUS_UNSUPPORTED) {
    puts("  the firmware has no voice engine");
  } else if (result == ZZ9K_STATUS_BUSY) {
    puts("  another voice session is open");
  } else if (result == ZZ9K_STATUS_NO_MEMORY) {
    puts("  not enough shared memory for the ring, status, staging or bank");
  }
}

int main(void)
{
  LONG options[OPT_COUNT];
  struct RDArgs *rdargs;
  const char *name;
  int click;
  int live;
  int load;
  int want_low_latency;
  uint32_t seconds = 0U;
  uint32_t lead_request = 0U;
  uint32_t flags = 0U;
  uint32_t bank_bytes;
  const uint8_t *bank_data;
  uint32_t bank_size;
  uint8_t *file_data = NULL;
  pt4_song *song = NULL;
  ZZ9KVoiceOpenDesc open_desc;
  ZZ9KVoiceUploadDesc upload;
  Source source;
  const char *reason;
  int result;
  int code = 0;

  memset(options, 0, sizeof(options));
  rdargs = ReadArgs((CONST_STRPTR)TEMPLATE, options, NULL);
  if (!rdargs) {
    PrintFault(IoErr(), (CONST_STRPTR)"zz9k-voiceplay");
    usage();
    return 10;
  }
  name = (const char *)options[OPT_MODULE];
  if (!name) {
    FreeArgs(rdargs);
    usage();
    return 5;
  }
  click = is_word(name, "CLICK");
  live = is_word(name, "LIVE");
  load = is_word(name, "LOAD");
  if (!load && (options[OPT_VOICES] || options[OPT_STEREO] ||
                options[OPT_RATE] || options[OPT_CHURN] ||
                options[OPT_GAIN])) {
    puts("zz9k-voiceplay: VOICES, STEREO, RATE, GAIN and CHURN belong to LOAD "
         "mode");
    FreeArgs(rdargs);
    usage();
    return 10;
  }
  load_voices = LOAD_DEFAULT_VOICES;
  load_rate = LOAD_DEFAULT_RATE;
  if (options[OPT_VOICES]) {
    LONG value = *(LONG *)options[OPT_VOICES];

    if (value < 1 || value > (LONG)ZZ9K_VOICE_MAX_VOICES) {
      FreeArgs(rdargs);
      usage();
      return 10;
    }
    load_voices = (uint32_t)value;
  }
  if (options[OPT_RATE]) {
    LONG value = *(LONG *)options[OPT_RATE];

    if (value < 1 || value > (LONG)ZZ9K_VOICE_RATE_MAX_HZ) {
      FreeArgs(rdargs);
      usage();
      return 10;
    }
    load_rate = (uint32_t)value;
  }
  load_volume = 0xFFFFU / load_voices;
  if (options[OPT_GAIN]) {
    LONG value = *(LONG *)options[OPT_GAIN];

    if (value < 0 || value > 65535L) {
      puts("zz9k-voiceplay: GAIN is 0 to 65535");
      FreeArgs(rdargs);
      return 10;
    }
    load_volume = (uint32_t)value;
  }
  limiter_set = options[OPT_LIMITER] != 0;
  load_stereo = options[OPT_STEREO] != 0;
  load_linear = options[OPT_LINEAR] != 0;
  if (options[OPT_INTERP]) {
    if (!parse_interp((const char *)options[OPT_INTERP])) {
      puts("zz9k-voiceplay: INTERP is nearest, linear or cubic");
      FreeArgs(rdargs);
      return 10;
    }
  }
  if (options[OPT_PERVOICE] && !options[OPT_INTERP]) {
    puts("zz9k-voiceplay: PERVOICE belongs to INTERP");
    FreeArgs(rdargs);
    return 10;
  }
  interp_pervoice = options[OPT_PERVOICE] != 0;
  if (options[OPT_LEAD]) {
    LONG value = *(LONG *)options[OPT_LEAD];

    if (value < 0) {
      FreeArgs(rdargs);
      usage();
      return 10;
    }
    lead_request = (uint32_t)value;
  }
  if (options[OPT_SECONDS]) {
    LONG value = *(LONG *)options[OPT_SECONDS];

    if (value <= 0 || value > 86400) {
      FreeArgs(rdargs);
      usage();
      return 10;
    }
    seconds = (uint32_t)value;
  } else if (click) {
    seconds = (uint32_t)CLICK_DEFAULT_SECONDS;
  } else if (load) {
    seconds = (uint32_t)LOAD_DEFAULT_SECONDS;
  }
  if (options[OPT_NTSC]) {
    flags |= ZZ9K_VOICE_OPEN_FLAG_NTSC;
  }
  if (options[OPT_LINEAR]) {
    flags |= ZZ9K_VOICE_OPEN_FLAG_LINEAR;
  }
  want_low_latency = options[OPT_LOWLAT] != 0;
  if (options[OPT_RAMP]) {
    LONG value = *(LONG *)options[OPT_RAMP];

    if (value < 0 || value > (LONG)ZZ9K_VOICE_RAMP_MAX_FRAMES) {
      puts("zz9k-voiceplay: RAMP is 0 to 512 frames");
      FreeArgs(rdargs);
      return 10;
    }
    ramp_frames = (uint32_t)value;
    ramp_set = 1;
  }
  if (options[OPT_GROUPS] && !options[OPT_CHURN]) {
    puts("zz9k-voiceplay: GROUPS belongs to CHURN");
    FreeArgs(rdargs);
    return 10;
  }
  if (options[OPT_CHURN]) {
    uint32_t groups = 32U;

    if (options[OPT_GROUPS]) {
      LONG value = *(LONG *)options[OPT_GROUPS];

      if (value < 1 || value > (LONG)ZZ9K_VOICE_MAX_VOICES) {
        puts("zz9k-voiceplay: GROUPS is 1 to 32");
        FreeArgs(rdargs);
        return 10;
      }
      groups = (uint32_t)value;
    }
    churn_groups = groups < load_voices ? groups : load_voices;
    churn_interval = ramp_frames >= CHURN_MIN_INTERVAL ? ramp_frames
                                                       : CHURN_DEFAULT_INTERVAL;
    churn_step = churn_interval / churn_groups;
    if (churn_step == 0U) {
      churn_step = 1U;
    }
  }

  memset(&session, 0, sizeof(session));
  memset(&source, 0, sizeof(source));
  source.click = click;
  source.load = load;

  /* The module is read and checked before the card is touched. */
  if (live && !IsInteractive(Input())) {
    puts("zz9k-voiceplay: LIVE needs an interactive console for its keys");
    code = 10;
    goto done;
  }
  if (click) {
    make_click();
    voice_count = 1U;
    bank_data = click_sample;
    bank_size = CLICK_BYTES;
    bank_bytes = CLICK_BANK_BYTES;
  } else if (load) {
    make_load_sample();
    load_mode = 1;
    voice_count = load_voices;
    bank_data = load_sample;
    bank_size = load_bytes;
    bank_bytes = load_bytes;
  } else if (live) {
    make_live_sample();
    live_mode = 1;
    voice_count = LIVE_VOICES;
    bank_data = live_sample;
    bank_size = LIVE_BYTES;
    bank_bytes = LIVE_BYTES;
  } else {
    uint32_t file_length = 0U;
    int err = 0;

    file_data = load_file(name, &file_length);
    if (!file_data) {
      printf("zz9k-voiceplay: cannot read %s\n", name);
      code = 10;
      goto done;
    }
    song = pt4_open(file_data, file_length, NULL, &err);
    if (!song) {
      printf("zz9k-voiceplay: %s is not a module libpt4 loads (error %d)\n",
             name, err);
      code = 10;
      goto done;
    }
    if ((pt4_extensions(song) & PT4_X_DYN) != 0) {
      printf("zz9k-voiceplay: %s uses DYN volume envelopes, which cannot be "
             "played as voice records\n", name);
      code = 10;
      goto done;
    }
    if (zz9k_pt4_events_init(&events, song, 0) != PT4_OK) {
      printf("zz9k-voiceplay: %s uses the EFx funk effect, which cannot be "
             "played as voice records\n", name);
      code = 10;
      goto done;
    }
    voice_count = (uint32_t)zz9k_pt4_events_voices(&events);
    bank_data = zz9k_pt4_events_bank(&events, &bank_size);
    if (bank_size == 0U || bank_size > ZZ9K_VOICE_BANK_MAX_BYTES - BANK_ROUND) {
      printf("zz9k-voiceplay: sample arena of %lu bytes does not fit the "
             "bank\n", (unsigned long)bank_size);
      code = 10;
      goto done;
    }
    bank_bytes = (bank_size + BANK_ROUND - 1U) / BANK_ROUND * BANK_ROUND;
  }

  ZZ9KBase = OpenLibrary((CONST_STRPTR)ZZ9K_LIBRARY_NAME,
                         ZZ9K_LIBRARY_VERSION);
  if (!ZZ9KBase) {
    puts("zz9k-voiceplay: cannot open zz9k.library");
    code = 20;
    goto done;
  }
  if (ZZ9KBase->lib_Revision < ZZ9K_LIBRARY_MIN_REVISION_AUDIO_VOICE) {
    printf("zz9k-voiceplay: zz9k.library %u.%u is too old, the voice engine "
           "calls need revision %u\n", ZZ9KBase->lib_Version,
           ZZ9KBase->lib_Revision, ZZ9K_LIBRARY_MIN_REVISION_AUDIO_VOICE);
    code = 20;
    goto close_library;
  }
  if (want_low_latency) {
    if (ZZ9KBase->lib_Revision <
        ZZ9K_LIBRARY_MIN_REVISION_VOICE_LOW_LATENCY) {
      printf("zz9k-voiceplay: zz9k.library %u.%u has no low-latency flag "
             "(needs revision %u): using the standard floor\n",
             ZZ9KBase->lib_Version, ZZ9KBase->lib_Revision,
             ZZ9K_LIBRARY_MIN_REVISION_VOICE_LOW_LATENCY);
    } else {
      flags |= ZZ9K_VOICE_OPEN_FLAG_LOW_LATENCY;
    }
  }

  timer_port = CreateMsgPort();
  if (timer_port) {
    timer_request = (struct timerequest *)CreateIORequest(
        timer_port, sizeof(struct timerequest));
  }
  if (!timer_request ||
      OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
                 (struct IORequest *)timer_request, 0) != 0) {
    puts("zz9k-voiceplay: timer.device is not available");
    code = 20;
    goto close_timer;
  }
  TimerBase = timer_request->tr_node.io_Device;
  now_ticks();

  memset(&open_desc, 0, sizeof(open_desc));
  open_desc.voices = voice_count;
  open_desc.bank_bytes = bank_bytes;
  open_desc.lead_frames = lead_request;
  open_desc.flags = flags;
  /* CHURN writes a restart every few frames: a deeper ring. */
  open_desc.ring_bytes = churn_groups != 0U ? CHURN_RING_BYTES : 0U;
  result = ZZ9KVoiceOpen(&open_desc, &session);
  if (result == ZZ9K_STATUS_BAD_REQUEST &&
      (flags & ZZ9K_VOICE_OPEN_FLAG_LOW_LATENCY) != 0U) {
    puts("zz9k-voiceplay: the firmware refused the low-latency flag, so it has "
         "no low-latency mode: retrying with the standard floor");
    flags &= ~ZZ9K_VOICE_OPEN_FLAG_LOW_LATENCY;
    open_desc.flags = flags;
    result = ZZ9KVoiceOpen(&open_desc, &session);
  }
  if (result == ZZ9K_STATUS_NO_MEMORY && open_desc.ring_bytes != 0U) {
    puts("zz9k-voiceplay: no room for the deeper CHURN ring: using the "
         "default ring");
    open_desc.ring_bytes = 0U;
    result = ZZ9KVoiceOpen(&open_desc, &session);
  }
  if (result != ZZ9K_STATUS_OK) {
    open_failed(result);
    code = 20;
    goto close_device;
  }
  lead_frames = session.grant.lead_frames;
  printf("zz9k-voiceplay: %s, %lu voices, bank %lu bytes, lead %lu frames "
         "(floor %lu, max %lu), ring %lu records\n",
         click ? "click" : name, (unsigned long)session.grant.voices,
         (unsigned long)session.grant.bank_bytes,
         (unsigned long)lead_frames,
         (unsigned long)session.grant.lead_floor_frames,
         (unsigned long)session.grant.lead_max_frames,
         (unsigned long)session.grant.ring_records);
  {
    char early[24];
    char late[24];

    format_ms(early, (long long)lead_frames - 960LL + (long long)FIFO_FRAMES);
    format_ms(late, (long long)lead_frames + (long long)FIFO_FRAMES);
    printf("  %s session; a record written at the earliest frame should sound "
           "%s to %s ms later (model: lead plus about 10.7 ms in the "
           "formatter FIFO, which is inferred and not measured)\n",
           session.grant.lead_floor_frames < STANDARD_FLOOR_FRAMES
               ? "low-latency" : "standard", early, late);
  }
  if (session.grant.voices < voice_count ||
      session.grant.bank_bytes < bank_size) {
    puts("zz9k-voiceplay: the engine granted less than the module needs");
    code = 20;
    goto close_session;
  }
  {
    ZZ9KVoiceExtStatus ext;

    zz9k_voice_ext_status_read(session.status, &ext);
    printf("  engine features 0x%lx%s%s%s%s%s%s\n", (unsigned long)ext.features,
           (ext.features & ZZ9K_VOICE_FEATURE_CONFIG) ? " CONFIG" : "",
           (ext.features & ZZ9K_VOICE_FEATURE_RAMP) ? " RAMP" : "",
           (ext.features & ZZ9K_VOICE_FEATURE_LIMITER) ? " LIMITER" : "",
           (ext.features & ZZ9K_VOICE_FEATURE_CUBIC) ? " CUBIC" : "",
           (ext.features & ZZ9K_VOICE_FEATURE_VOICE_INTERP) ? " VOICE_INTERP"
                                                            : "",
           ext.features == 0U ? " (none: an older firmware)" : "");
    if ((ramp_set || limiter_set || (interp_set && !interp_pervoice)) &&
        (ext.features & ZZ9K_VOICE_FEATURE_CONFIG) == 0U) {
      puts("zz9k-voiceplay: RAMP, LIMITER and INTERP need a firmware with the "
           "CONFIG record");
      code = 20;
      goto close_session;
    }
    if (ramp_set && ramp_frames != 0U &&
        (ext.features & ZZ9K_VOICE_FEATURE_RAMP) == 0U) {
      puts("  the firmware does not advertise RAMP yet: running the ramp "
           "for its qualification");
    }
    if (limiter_set && (ext.features & ZZ9K_VOICE_FEATURE_LIMITER) == 0U) {
      puts("zz9k-voiceplay: WARNING: the firmware does not advertise "
           "LIMITER: the soft knee is unqualified, running it for its "
           "qualification only. Keep the volume low and the speakers "
           "protected");
    }
    if (interp_set && (ext.features & ZZ9K_VOICE_FEATURE_VOICE_INTERP) == 0U) {
      puts("zz9k-voiceplay: WARNING: the firmware does not advertise "
           "VOICE_INTERP: the INTERP records are unqualified, running them "
           "for their qualification only. A firmware older than the MODE "
           "record counts them as rejected and plays as before");
    }
    if (interp_set && interp_value == ZZ9K_VOICE_INTERP_CUBIC &&
        (ext.features & ZZ9K_VOICE_FEATURE_CUBIC) == 0U) {
      puts("zz9k-voiceplay: WARNING: the firmware does not advertise CUBIC: "
           "the cubic interpolation is unqualified, running it for its "
           "qualification only. Keep the volume low and the speakers "
           "protected");
    }
  }

  memset(&upload, 0, sizeof(upload));
  upload.sample_id = 0U;
  upload.bank_offset = 0U;
  upload.data = bank_data;
  upload.length = bank_size;
  upload.format = load ? (load_stereo ? ZZ9K_VOICE_FORMAT_S16S
                                      : ZZ9K_VOICE_FORMAT_S16)
                       : ZZ9K_VOICE_FORMAT_S8;
  result = ZZ9KVoiceUpload(&session, &upload);
  if (result != ZZ9K_STATUS_OK) {
    printf("zz9k-voiceplay: sample upload failed: %s (%d)\n",
           zz9k_status_text(result), result);
    if (load && load_stereo) {
      puts("  the firmware may lack the stereo sample formats "
           "(audio-voice-stereo)");
    }
    code = 20;
    goto close_session;
  }

  /* The engine timeline starts with the first record. A module whose first
   * rows are silent produces no event until its first note, and the pacing
   * loop would wait for a playing frame that never advances, so a unity
   * master gain goes out first, stamped at the start of the song. */
  {
    ZZ9KVoiceRecord *record;

    if (ramp_set || limiter_set) {
      record = pending_next(lead_frames, 0U, ZZ9K_VOICE_OP_CONFIG);
      if (record) {
        /* One record at the lead stamp, ahead of the master gain and every
         * note, so the ring order puts the settings first. Both settings in
         * the record are validated together by the firmware. */
        if (ramp_set && limiter_set) {
          zz9k_voice_build_config_ramp_limiter(record, lead_frames,
                                               ramp_frames, 1);
        } else if (limiter_set) {
          zz9k_voice_build_config_limiter(record, lead_frames, 1);
        } else {
          zz9k_voice_build_config_ramp(record, lead_frames, ramp_frames);
        }
      }
    }
    if (interp_set) {
      /* The interpolation goes out at the lead stamp too, ahead of every
       * note: the session default in one CONFIG record, or a MODE record
       * for each voice. */
      if (interp_pervoice) {
        uint32_t v;

        for (v = 0U; v < voice_count; v++) {
          record = pending_next(lead_frames, v, ZZ9K_VOICE_OP_MODE);
          if (record) {
            zz9k_voice_build_mode_interp(record, lead_frames, (uint8_t)v,
                                         interp_value);
          }
        }
      } else {
        record = pending_next(lead_frames, 0U, ZZ9K_VOICE_OP_CONFIG);
        if (record) {
          zz9k_voice_build_config_interp(record, lead_frames, interp_value);
        }
      }
    }
    record = pending_next(lead_frames, 0U, ZZ9K_VOICE_OP_MASTER);
    if (record) {
      record->arg0 = 256U;
    }
  }

  if (live) {
    puts("zz9k-voiceplay: live: zsxdcvgbhnjm, q2w3er5t6y7u and i play notes, "
         "ESC stops");
    SetMode(Input(), 1L);
    reason = live_play(seconds * ZZ9K_VOICE_OUTPUT_RATE);
    SetMode(Input(), 0L);
  } else {
    reason = play(&source, seconds * ZZ9K_VOICE_OUTPUT_RATE);
  }
  report(reason);
  if (strcmp(reason, "song end") != 0) {
    ZZ9KVoiceReset(&session);
  }
  if (pending_overflow || strcmp(reason, "status read failed") == 0 ||
      strcmp(reason, "engine frame counter stopped advancing") == 0 ||
      strncmp(reason, "session closed by the card", 26) == 0) {
    code = 20;
  }

close_session:
  result = ZZ9KVoiceClose(&session);
  if (result != ZZ9K_STATUS_OK) {
    printf("zz9k-voiceplay: close: %s (%d)\n", zz9k_status_text(result),
           result);
  }
close_device:
  CloseDevice((struct IORequest *)timer_request);
close_timer:
  if (timer_request) {
    DeleteIORequest((struct IORequest *)timer_request);
  }
  if (timer_port) {
    DeleteMsgPort(timer_port);
  }
close_library:
  CloseLibrary(ZZ9KBase);
done:
  if (song) {
    pt4_close(song);
  }
  free(file_data);
  FreeArgs(rdargs);
  return code;
}
