/*
 * Plays a ProTracker module, or a once-per-second click, through the ZZ9000AX
 * voice engine with zz9k.library.
 *
 * MODULE mode runs libpt4 one replayer tick at a time (zz9k-voice-pt4) and
 * writes each tick's voice events into the engine's command ring as
 * timestamped records. The module's sample arena is uploaded as sample 0.
 * The first tick is stamped at the granted lead, so the engine frames before
 * it are silence; every later tick keeps its distance to the first. A tick is
 * run when its song frame is closer to the frame now leaving the DAC than the
 * granted lead, which keeps the ring a lead ahead of the player and never
 * further. Each pass reads the status block (which is also the ring
 * heartbeat), writes what is pending and sleeps about 5 ms.
 *
 * CLICK mode starts a short pulse once per second at lead + k * 48000, at
 * 48000 Hz so it plays 1:1. It is for listening, and for checking the
 * scheduling margin.
 *
 * The report counts what the firmware saw (late, rejected, underruns,
 * dropped), the smallest scheduling margin (record stamp minus the engine
 * horizon when the record was written, in frames), and the 68k time one tick
 * takes (replayer tick, record mapping and ring writes).
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

#define TEMPLATE "MODULE,LEAD/K/N,NTSC/S,LINEAR/S,SECONDS/K/N"
#define OPT_MODULE 0
#define OPT_LEAD 1
#define OPT_NTSC 2
#define OPT_LINEAR 3
#define OPT_SECONDS 4
#define OPT_COUNT 5

/* One tick of a 32 voice module is at most six events per voice. */
#define MAX_PENDING 256U
#define PASS_MICROS 5000U
#define STALL_MICROS 3000000U
#define CLICK_BYTES 48U
#define CLICK_BANK_BYTES 64U
#define CLICK_VOLUME 0x4000UL
#define CLICK_DEFAULT_SECONDS 10UL
#define BANK_ROUND 64U

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

static int is_click_name(const char *name)
{
  static const char word[] = "CLICK";
  uint32_t i;

  for (i = 0U; i < sizeof(word); i++) {
    char c = name[i];

    if (c >= 'a' && c <= 'z') {
      c = (char)(c - 'a' + 'A');
    }
    if (c != word[i]) {
      return 0;
    }
  }
  return 1;
}

static void usage(void)
{
  puts("zz9k-voiceplay: plays a module or a click through the ZZ9000AX voice engine");
  puts("usage: zz9k-voiceplay MODULE [LEAD=frames] [NTSC] [LINEAR] [SECONDS=n]");
  puts("       zz9k-voiceplay CLICK [LEAD=frames] [SECONDS=n]");
  puts("  MODULE   ProTracker module to play (DYN envelopes and EFx funk are refused)");
  puts("  CLICK    a short pulse once per second at 48000 Hz, for listening and");
  puts("           for checking the scheduling margin; 10 seconds unless SECONDS");
  puts("  LEAD     lead in 48 kHz frames; 0 or absent is the firmware floor");
  puts("  NTSC     Paula clock of the module is NTSC (else PAL)");
  puts("  LINEAR   linear interpolation (else nearest)");
  puts("  SECONDS  stop after this many seconds of engine time");
  puts("Ctrl-C stops, resets and closes. The report lists frames, ticks, records,");
  puts("ring-full waits, late, rejected, underruns, dropped, the smallest margin and");
  puts("the 68k time per tick. The engine need not be advertised as a service.");
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

    if (!stat_have_margin || margin < stat_min_margin) {
      stat_min_margin = margin;
      stat_have_margin = 1;
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
    played = status.playing_frame;
    if (limit_frames != 0U && played >= limit_frames) {
      return "time limit";
    }
    now = now_ticks();
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
  printf("zz9k-voiceplay: stopped: %s\n", reason);
  printf("  frames played %lu, ticks %lu, records %lu, ring-full waits %lu\n",
         (unsigned long)status.playing_frame, (unsigned long)stat_ticks,
         (unsigned long)stat_records, (unsigned long)stat_ring_waits);
  printf("  late %lu, rejected %lu, underruns %lu, dropped %lu\n",
         (unsigned long)status.late, (unsigned long)status.rejected,
         (unsigned long)status.underruns, (unsigned long)status.dropped);
  if (stat_have_margin) {
    printf("  smallest scheduling margin %ld frames (%ld us)\n",
           (long)stat_min_margin,
           (long)(((long long)stat_min_margin * 1000000LL) /
                  (long long)ZZ9K_VOICE_OUTPUT_RATE));
  } else {
    printf("  smallest scheduling margin: no record was written\n");
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
  click = is_click_name(name);
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
  }
  if (options[OPT_NTSC]) {
    flags |= ZZ9K_VOICE_OPEN_FLAG_NTSC;
  }
  if (options[OPT_LINEAR]) {
    flags |= ZZ9K_VOICE_OPEN_FLAG_LINEAR;
  }

  memset(&session, 0, sizeof(session));
  memset(&source, 0, sizeof(source));
  source.click = click;

  /* The module is read and checked before the card is touched. */
  if (click) {
    make_click();
    voice_count = 1U;
    bank_data = click_sample;
    bank_size = CLICK_BYTES;
    bank_bytes = CLICK_BANK_BYTES;
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
  result = ZZ9KVoiceOpen(&open_desc, &session);
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
  if (session.grant.voices < voice_count ||
      session.grant.bank_bytes < bank_size) {
    puts("zz9k-voiceplay: the engine granted less than the module needs");
    code = 20;
    goto close_session;
  }

  memset(&upload, 0, sizeof(upload));
  upload.sample_id = 0U;
  upload.bank_offset = 0U;
  upload.data = bank_data;
  upload.length = bank_size;
  upload.format = ZZ9K_VOICE_FORMAT_S8;
  result = ZZ9KVoiceUpload(&session, &upload);
  if (result != ZZ9K_STATUS_OK) {
    printf("zz9k-voiceplay: sample upload failed: %s (%d)\n",
           zz9k_status_text(result), result);
    code = 20;
    goto close_session;
  }

  reason = play(&source, seconds * ZZ9K_VOICE_OUTPUT_RATE);
  report(reason);
  if (strcmp(reason, "song end") != 0) {
    ZZ9KVoiceReset(&session);
  }
  if (pending_overflow || strcmp(reason, "status read failed") == 0 ||
      strcmp(reason, "engine frame counter stopped advancing") == 0) {
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
