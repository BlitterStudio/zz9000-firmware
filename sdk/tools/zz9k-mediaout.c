/*
 * Shared-buffer output of a media session, on hardware.
 *
 * Opens an MPEG-1 or MPEG-2 program stream, begins a media session whose
 * output is a shared buffer, configures the format, size and depth the
 * command line asks for, presents pictures and waits for each one. For every
 * picture it prints the slot the card used, the card's conversion time, the
 * time until the picture was complete and a checksum of the bytes in the
 * slot. With -w it also draws the picture in a window.
 *
 * Video only: it binds no audio. See docs/media-shared-output.md for the
 * contract and for the card test this tool is part of.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k-mediaout-logic.h"
#include "zz9k/caps.h"
#include "zz9k/host.h"
#include "zz9k/request.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__amigaos__) || defined(__amiga__) || defined(__AMIGA__) || \
    defined(__VBCC__)
#define ZZ9K_MEDIAOUT_AMIGA 1
#include <cybergraphx/cybergraphics.h>
#include <devices/timer.h>
#include <exec/types.h>
#include <intuition/intuition.h>
#include <proto/cybergraphics.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/timer.h>
#else
#include <time.h>
#endif

#define MEDIAOUT_CHUNK_BYTES 16384U
#define MEDIAOUT_WAIT_US 2000000UL
#define MEDIAOUT_RETRIES 4000U

/* Large buffers are static: a program started from a Shell gets a 4 KiB
 * stack unless the user raises it, and no function here may need more than
 * 1 KiB of it. */
static uint8_t header_buffer[MEDIAOUT_CHUNK_BYTES];
static uint8_t row_scratch[ZZ9K_MEDIAOUT_ROW_SCRATCH];
static uint8_t palette_bytes[ZZ9K_MEDIA_OUTPUT_PALETTE_BYTES];
static uint32_t palette_words[256];

#if ZZ9K_MEDIAOUT_AMIGA
struct Device *TimerBase;
struct Library *CyberGfxBase;
struct IntuitionBase *IntuitionBase;
#endif

typedef struct MediaOutTimer {
#if ZZ9K_MEDIAOUT_AMIGA
  struct MsgPort *port;
  struct timerequest *request;
  uint32_t ticks_per_second;
#else
  int unused;
#endif
} MediaOutTimer;

static void timer_open(MediaOutTimer *timer)
{
  memset(timer, 0, sizeof(*timer));
#if ZZ9K_MEDIAOUT_AMIGA
  timer->port = CreateMsgPort();
  if (timer->port) {
    timer->request = (struct timerequest *)CreateIORequest(
        timer->port, sizeof(*timer->request));
  }
  if (timer->request &&
      OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
                 (struct IORequest *)timer->request, 0) == 0) {
    struct EClockVal value;

    TimerBase = (struct Device *)timer->request->tr_node.io_Device;
    timer->ticks_per_second = ReadEClock(&value);
    if (timer->ticks_per_second != 0U) {
      return;
    }
    CloseDevice((struct IORequest *)timer->request);
    timer->request = 0;
  }
#endif
}

static void timer_close(MediaOutTimer *timer)
{
#if ZZ9K_MEDIAOUT_AMIGA
  if (timer->request) {
    if (timer->ticks_per_second != 0U) {
      CloseDevice((struct IORequest *)timer->request);
    }
    DeleteIORequest((struct IORequest *)timer->request);
  }
  if (timer->port) {
    DeleteMsgPort(timer->port);
  }
  TimerBase = 0;
#else
  (void)timer;
#endif
}

/* Microseconds from an arbitrary start; wraps after about 71 minutes, which
 * the callers' differences survive. */
static uint32_t timer_now_us(const MediaOutTimer *timer)
{
#if ZZ9K_MEDIAOUT_AMIGA
  struct EClockVal value;
  uint64_t ticks;

  if (timer->ticks_per_second == 0U) {
    return 0U;
  }
  ReadEClock(&value);
  ticks = ((uint64_t)value.ev_hi << 32) | value.ev_lo;
  return (uint32_t)((ticks * 1000000ULL) / timer->ticks_per_second);
#else
  (void)timer;
  return (uint32_t)(((uint64_t)clock() * 1000000ULL) / CLOCKS_PER_SEC);
#endif
}

static void pause_a_little(void)
{
#if ZZ9K_MEDIAOUT_AMIGA
  Delay(1);
#endif
}

/* ---- window ---------------------------------------------------------- */

#if ZZ9K_MEDIAOUT_AMIGA
struct MediaOutWindow {
  struct Window *window;
  uint32_t *argb;
};

static int window_open(struct MediaOutWindow *w, const ZZ9KMediaOutPlan *plan)
{
  memset(w, 0, sizeof(*w));
  IntuitionBase = (struct IntuitionBase *)OpenLibrary(
      (CONST_STRPTR)"intuition.library", 36UL);
  CyberGfxBase = OpenLibrary((CONST_STRPTR)"cybergraphics.library", 40UL);
  if (!IntuitionBase || !CyberGfxBase) {
    printf("zz9k-mediaout: -w needs intuition and cybergraphics.library\n");
    return 0;
  }
  w->argb = (uint32_t *)AllocVec(
      (ULONG)plan->width * 4UL, MEMF_ANY);
  w->window = OpenWindowTags(
      0, WA_Title, (ULONG)"zz9k-mediaout", WA_InnerWidth, plan->width,
      WA_InnerHeight, plan->height, WA_CloseGadget, TRUE, WA_DragBar, TRUE,
      WA_DepthGadget, TRUE, WA_Activate, TRUE, WA_IDCMP, IDCMP_CLOSEWINDOW,
      TAG_DONE);
  if (!w->window || !w->argb) {
    printf("zz9k-mediaout: cannot open the window\n");
    return 0;
  }
  return 1;
}

/* Draws one picture, row by row. Returns 0 when the window was closed. */
static int window_show(struct MediaOutWindow *w, const ZZ9KMediaOutPlan *plan,
                       const uint8_t *slot, const uint32_t *palette)
{
  struct IntuiMessage *message;
  int open = 1;
  uint32_t y;

  for (y = 0U; y < plan->height; y++) {
    zz9k_mediaout_row_to_rgb(plan, slot, y, palette, w->argb);
    WritePixelArray(w->argb, 0, 0, plan->width * 4, w->window->RPort,
                    w->window->BorderLeft, w->window->BorderTop + (LONG)y,
                    plan->width, 1, RECTFMT_ARGB);
  }
  while ((message = (struct IntuiMessage *)GetMsg(w->window->UserPort))) {
    if (message->Class == IDCMP_CLOSEWINDOW) {
      open = 0;
    }
    ReplyMsg((struct Message *)message);
  }
  return open;
}

static void window_close(struct MediaOutWindow *w)
{
  if (w->window) {
    CloseWindow(w->window);
  }
  if (w->argb) {
    FreeVec(w->argb);
  }
  if (CyberGfxBase) {
    CloseLibrary(CyberGfxBase);
  }
  if (IntuitionBase) {
    CloseLibrary((struct Library *)IntuitionBase);
  }
}
#endif

/* ---- helpers --------------------------------------------------------- */

static void print_error(const char *what, int status)
{
  printf("zz9k-mediaout: %s: %s (%d)\n", what, zz9k_status_name(status),
         status);
}

/* A call that answers BUSY while the card is occupied (a picture being
 * written, the core queue full) is repeated a bounded number of times. */
#define RETRY_BUSY(status_expr) \
  do { \
    uint32_t tries_ = 0U; \
    do { \
      status = (status_expr); \
      if (status != ZZ9K_STATUS_BUSY) { \
        break; \
      } \
      pause_a_little(); \
    } while (++tries_ < MEDIAOUT_RETRIES); \
  } while (0)

typedef struct MediaOutFeed {
  FILE *file;
  ZZ9KSharedBuffer buffer;
  uint32_t chunk;    /* size of the input buffer */
  uint32_t offset;   /* bytes of the chunk the card has taken */
  uint32_t length;   /* bytes in the chunk */
  int eof_sent;
  int at_eof;
} MediaOutFeed;

/* Sends the rest of the current chunk, reading the next one when it is
 * used up; at the end of the file sends EOF once. Returns a ZZ9K_STATUS_*. */
static int feed_once(ZZ9KContext *ctx, uint32_t session, MediaOutFeed *feed,
                     ZZ9KMediaSessionMainResult *result)
{
  ZZ9KMediaSessionWriteDesc desc;
  int status;

  if (feed->offset >= feed->length && !feed->at_eof) {
    size_t got = fread((void *)feed->buffer.data, 1U, feed->chunk,
                       feed->file);

    feed->length = (uint32_t)got;
    feed->offset = 0U;
    if (got == 0U) {
      feed->at_eof = 1;
    }
  }
  if (feed->offset >= feed->length && feed->eof_sent) {
    return ZZ9K_STATUS_OK;
  }
  memset(&desc, 0, sizeof(desc));
  desc.session = session;
  desc.src_handle = feed->buffer.handle;
  desc.src_offset = feed->offset;
  desc.src_length = feed->length - feed->offset;
  if (desc.src_length == 0U) {
    desc.flags = ZZ9K_MEDIA_SESSION_WRITE_EOF;
  }
  status = zz9k_media_session_write(ctx, &desc, result);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  feed->offset += result->bytes_written;
  if (desc.src_length == 0U) {
    feed->eof_sent = 1;
  }
  return ZZ9K_STATUS_OK;
}

/* Waits for the PRESENT just sent to finish: the output page stops saying
 * PENDING and counts the picture. Returns 0 on success. */
static int wait_for_picture(ZZ9KContext *ctx, uint32_t session,
                            const MediaOutTimer *timer,
                            ZZ9KMediaSessionStatusResult *page)
{
  uint32_t start = timer_now_us(timer);
  int status;

  for (;;) {
    status = zz9k_media_session_status(ctx, session, ZZ9K_MEDIA_STATUS_OUTPUT,
                                       0U, page);
    if (status != ZZ9K_STATUS_OK) {
      print_error("output status", status);
      return 1;
    }
    if ((page->flags & ZZ9K_MEDIA_OUTPUT_STATUS_PENDING) == 0U) {
      return 0;
    }
    if ((uint32_t)(timer_now_us(timer) - start) > MEDIAOUT_WAIT_US) {
      printf("zz9k-mediaout: a picture was still pending after 2 s\n");
      return 1;
    }
    pause_a_little();
  }
}

int main(int argc, char **argv)
{
  ZZ9KMediaOutOptions opts;
  ZZ9KMediaOutPlan plan;
  ZZ9KMediaOutMpegInfo info;
  ZZ9KContext *ctx = 0;
  ZZ9KCaps caps;
  ZZ9KServiceInfo video;
  ZZ9KSharedBuffer pictures;
  ZZ9KSharedBuffer palette_buffer;
  ZZ9KMediaSessionBeginDesc begin;
  ZZ9KMediaSessionOutputDesc output;
  ZZ9KMediaSessionMainResult result;
  ZZ9KMediaSessionStatusResult page;
  MediaOutFeed feed;
  MediaOutTimer timer;
  uint32_t session = 0U;
  uint32_t presented = 0U;
  uint32_t checksum_sum = 0U;
  uint32_t total_convert_us = 0U;
  uint32_t total_wait_us = 0U;
  uint32_t run_start;
  uint32_t i;
  size_t header_bytes;
  int status;
  int exit_code = 1;
  int stop = 0;
#if ZZ9K_MEDIAOUT_AMIGA
  struct MediaOutWindow window;

  memset(&window, 0, sizeof(window));
#endif

  memset(&pictures, 0, sizeof(pictures));
  memset(&palette_buffer, 0, sizeof(palette_buffer));
  memset(&feed, 0, sizeof(feed));
  memset(&timer, 0, sizeof(timer));
  if (!zz9k_mediaout_parse(argc, argv, &opts)) {
    zz9k_mediaout_usage();
    return 2;
  }

  feed.file = fopen(opts.path, "rb");
  if (!feed.file) {
    printf("zz9k-mediaout: cannot open %s\n", opts.path);
    return 1;
  }
  header_bytes = fread(header_buffer, 1U, sizeof(header_buffer), feed.file);
  if (!zz9k_mediaout_is_program_stream(header_buffer, (uint32_t)header_bytes) ||
      !zz9k_mediaout_mpeg_info(header_buffer, (uint32_t)header_bytes, &info)) {
    printf("zz9k-mediaout: %s is not an MPEG-1/2 program stream with a "
           "sequence header in its first %u bytes\n", opts.path,
           (unsigned)sizeof(header_buffer));
    goto done;
  }
  rewind(feed.file);
  status = zz9k_mediaout_plan(&opts, info.width, info.height, &plan);
  if (status != ZZ9K_STATUS_OK) {
    print_error("this output configuration", status);
    goto done;
  }

  status = zz9k_open(&ctx);
  if (status != ZZ9K_STATUS_OK) {
    print_error("open", status);
    goto done;
  }
  status = zz9k_query_caps(ctx, &caps);
  if (status != ZZ9K_STATUS_OK) {
    print_error("query caps", status);
    goto done;
  }
  status = zz9k_query_service(ctx, ZZ9K_SERVICE_VIDEO, &video);
  if (status != ZZ9K_STATUS_OK ||
      (video.flags & ZZ9K_SERVICE_FLAG_VIDEO_MEDIA_SESSION) == 0U) {
    printf("zz9k-mediaout: the card has no media sessions\n");
    goto done;
  }
  if (!zz9k_video_shared_output_available(&video)) {
    printf("zz9k-mediaout: the video service lists %lu opcodes, "
           "OUTPUT_CONFIG needs %lu: update the firmware\n",
           (unsigned long)video.opcode_count,
           (unsigned long)ZZ9K_VIDEO_OPCODE_COUNT_SHARED_OUTPUT);
    goto done;
  }

  /* The picture slots must be CPU-visible: a plain allocation on Zorro III,
   * a host-window one on Zorro II (the library drops the flag on Zorro III). */
  status = zz9k_alloc_shared(ctx, plan.buffer_bytes, 16U,
                             ZZ9K_ALLOC_HOST_WINDOW, &pictures);
  if (status != ZZ9K_STATUS_OK || !pictures.data) {
    printf("zz9k-mediaout: cannot allocate %lu bytes of picture buffer "
           "(%s)\n", (unsigned long)plan.buffer_bytes,
           status != ZZ9K_STATUS_OK ? zz9k_status_name(status)
                                    : "not host-visible");
    goto done;
  }
  memset(palette_words, 0, sizeof(palette_words));
  if ((plan.flags & ZZ9K_MEDIA_OUTPUT_PALETTE_HOST) != 0U) {
    status = zz9k_alloc_shared(ctx, ZZ9K_MEDIA_OUTPUT_PALETTE_BYTES, 16U,
                               ZZ9K_ALLOC_HOST_WINDOW, &palette_buffer);
    if (status != ZZ9K_STATUS_OK || !palette_buffer.data) {
      print_error("palette buffer", status);
      goto done;
    }
    zz9k_mediaout_gray_palette(plan.depth, palette_bytes);
    memcpy((void *)palette_buffer.data, palette_bytes, sizeof(palette_bytes));
    for (i = 0U; i < 256U; i++) {
      palette_words[i] = zz9k_mediaout_palette_word(palette_bytes, i);
    }
  } else if (plan.depth != 0U) {
    (void)zz9k_media_output_fixed_palette(plan.depth, palette_words);
  }

  /* The input buffer takes what the host window has left after the pictures
   * and the palette: try smaller chunks until one fits. */
  feed.chunk = MEDIAOUT_CHUNK_BYTES;
  for (;;) {
    status = zz9k_alloc_shared(ctx, feed.chunk, 16U, ZZ9K_ALLOC_HOST_WINDOW,
                               &feed.buffer);
    if (status == ZZ9K_STATUS_OK && feed.buffer.data) {
      break;
    }
    if (status == ZZ9K_STATUS_OK) {
      zz9k_free_shared(ctx, feed.buffer.handle);
      feed.buffer.handle = 0U;
    }
    feed.chunk = zz9k_mediaout_smaller_chunk(feed.chunk);
    if (feed.chunk == 0U) {
      print_error("input buffer", status);
      goto done;
    }
  }

  memset(&begin, 0, sizeof(begin));
  begin.video_codec = info.video_codec;
  begin.container = ZZ9K_VIDEO_CONTAINER_MPEG_PS;
  begin.width = info.width;
  begin.height = info.height;
  begin.output_format = ZZ9K_VIDEO_OUTPUT_SHARED_BUFFER;
  RETRY_BUSY(zz9k_media_session_begin(ctx, &begin, &result));
  if (result.session != 0U) {
    session = result.session;
  }
  if (status != ZZ9K_STATUS_OK) {
    print_error("begin", status);
    goto done;
  }

  memset(&output, 0, sizeof(output));
  output.session = session;
  output.buffer_handle = pictures.handle;
  output.slot_stride = plan.slot_stride;
  output.width = plan.width;
  output.height = plan.height;
  output.format = plan.format;
  output.pitch = plan.pitch;
  output.flags = plan.flags;
  output.palette_handle = palette_buffer.handle;
  status = zz9k_media_session_output(ctx, &output, &result);
  if (status != ZZ9K_STATUS_OK) {
    print_error("output config", status);
    goto done;
  }
  printf("stream %lux%lu %s -> %s %lux%lu pitch %lu depth %lu flags 0x%03lx\n",
         (unsigned long)info.width, (unsigned long)info.height,
         info.video_codec == ZZ9K_VIDEO_CODEC_MPEG2 ? "MPEG-2" : "MPEG-1",
         zz9k_mediaout_format_name(plan.format), (unsigned long)plan.width,
         (unsigned long)plan.height, (unsigned long)plan.pitch,
         (unsigned long)plan.depth, (unsigned long)plan.flags);
  printf("picture %lu bytes, %lu slot(s) of stride %lu in %lu bytes\n",
         (unsigned long)plan.picture_bytes, (unsigned long)plan.slots,
         (unsigned long)plan.slot_stride, (unsigned long)plan.buffer_bytes);
  status = zz9k_media_session_status(ctx, session, ZZ9K_MEDIA_STATUS_OUTPUT,
                                     0U, &page);
  if (status == ZZ9K_STATUS_OK) {
    printf("firmware accepts output flags 0x%03lx\n",
           (unsigned long)page.value[3]);
  }

  timer_open(&timer);
#if ZZ9K_MEDIAOUT_AMIGA
  if (opts.window && !window_open(&window, &plan)) {
    goto done;
  }
#else
  if (opts.window) {
    printf("zz9k-mediaout: -w is only available on AmigaOS\n");
    goto done;
  }
#endif

  run_start = timer_now_us(&timer);
  while (presented < opts.count && !stop) {
    status = ZZ9K_STATUS_BUSY;
    RETRY_BUSY(zz9k_media_session_decode(ctx, session, 0U, &result));
    if (status != ZZ9K_STATUS_OK) {
      print_error("decode", status);
      goto done;
    }
    if ((result.flags & ZZ9K_MEDIA_SESSION_RESULT_DONE) != 0U ||
        result.state == ZZ9K_MEDIA_SESSION_STATE_DONE) {
      printf("end of stream\n");
      break;
    }
    if ((result.flags & ZZ9K_MEDIA_SESSION_RESULT_NEED_INPUT) != 0U) {
      if (feed.eof_sent) {
        printf("end of stream\n");
        break;
      }
      RETRY_BUSY(feed_once(ctx, session, &feed, &result));
      if (status != ZZ9K_STATUS_OK) {
        print_error("write", status);
        goto done;
      }
      continue;
    }
    if ((result.flags & ZZ9K_MEDIA_SESSION_RESULT_FRAME_HELD) == 0U) {
      continue;
    }

    {
      uint32_t asked = timer_now_us(&timer);
      uint32_t waited;
      uint32_t slot;
      uint32_t hash = ZZ9K_MEDIAOUT_CHECKSUM_SEED;

      RETRY_BUSY(zz9k_media_session_present(ctx, session, 0U, &result));
      if (status != ZZ9K_STATUS_OK) {
        print_error("present", status);
        goto done;
      }
      slot = (result.flags & ZZ9K_MEDIA_SESSION_RESULT_OUTPUT_SLOT1) != 0U
                 ? 1U : 0U;
      if ((result.flags & ZZ9K_MEDIA_SESSION_RESULT_OUTPUT_PENDING) == 0U) {
        printf("note: PRESENT reported the picture already written\n");
      }
      if (wait_for_picture(ctx, session, &timer, &page)) {
        goto done;
      }
      waited = (uint32_t)(timer_now_us(&timer) - asked);
      if ((page.flags & ZZ9K_MEDIA_OUTPUT_STATUS_COMPLETE) == 0U ||
          ((page.flags & ZZ9K_MEDIA_OUTPUT_STATUS_SLOT1) != 0U ? 1U : 0U) !=
              slot) {
        printf("zz9k-mediaout: the output page names slot %lu, PRESENT "
               "named slot %lu\n",
               (unsigned long)((page.flags &
                                ZZ9K_MEDIA_OUTPUT_STATUS_SLOT1) != 0U),
               (unsigned long)slot);
        goto done;
      }
      presented++;
      total_convert_us += (uint32_t)page.value[2];
      total_wait_us += waited;
      if (opts.checksum) {
        hash = zz9k_mediaout_checksum_picture(
            &plan,
            (const volatile uint8_t *)pictures.data +
                (slot != 0U ? plan.slot_stride : 0U),
            hash, row_scratch);
        checksum_sum ^= hash;
      }
      printf("picture %4lu pts %9lu slot %lu convert %6lu us wait %6lu us "
             "sum %08lx\n",
             (unsigned long)presented, (unsigned long)result.video_pts,
             (unsigned long)slot, (unsigned long)page.value[2],
             (unsigned long)waited, (unsigned long)hash);
#if ZZ9K_MEDIAOUT_AMIGA
      if (window.window) {
        uint8_t *copy = (uint8_t *)AllocVec(plan.picture_bytes, MEMF_ANY);

        if (copy) {
          memcpy(copy, (const void *)((const volatile uint8_t *)
                     pictures.data + (slot != 0U ? plan.slot_stride : 0U)),
                 plan.picture_bytes);
          stop = !window_show(&window, &plan, copy, palette_words);
          FreeVec(copy);
        }
      }
#endif
    }
  }
  {
    uint32_t elapsed_us = (uint32_t)(timer_now_us(&timer) - run_start);

    printf("%lu pictures in %lu ms", (unsigned long)presented,
           (unsigned long)(elapsed_us / 1000UL));
    if (presented != 0U && elapsed_us != 0U) {
      uint64_t rate = zz9k_mediaout_rate_x100(presented, elapsed_us);

      printf(" (%lu.%02lu pictures/s)", (unsigned long)(rate / 100ULL),
             (unsigned long)(rate % 100ULL));
    }
    printf("\n");
  }
  if (presented != 0U) {
    printf("average card conversion %lu us, average wait for a picture "
           "%lu us\n", (unsigned long)(total_convert_us / presented),
           (unsigned long)(total_wait_us / presented));
    if (opts.checksum) {
      printf("run checksum %08lx\n", (unsigned long)checksum_sum);
    }
  }
  exit_code = presented != 0U ? 0 : 1;

done:
#if ZZ9K_MEDIAOUT_AMIGA
  window_close(&window);
#endif
  if (ctx && session != 0U) {
    int close_status;
    uint32_t tries = 0U;

    do {
      close_status = zz9k_media_session_close(ctx, session, 0U, &result);
      if (close_status != ZZ9K_STATUS_BUSY) {
        break;
      }
      pause_a_little();
    } while (++tries < MEDIAOUT_RETRIES);
    if (close_status != ZZ9K_STATUS_OK) {
      print_error("close", close_status);
    }
  }
  if (ctx) {
    if (pictures.handle != 0U) {
      zz9k_free_shared(ctx, pictures.handle);
    }
    if (feed.buffer.handle != 0U) {
      zz9k_free_shared(ctx, feed.buffer.handle);
    }
    if (palette_buffer.handle != 0U) {
      zz9k_free_shared(ctx, palette_buffer.handle);
    }
    zz9k_close(ctx);
  }
  timer_close(&timer);
  if (feed.file) {
    fclose(feed.file);
  }
  return exit_code;
}
