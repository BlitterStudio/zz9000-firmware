/*
 * The software mpega.library fallback, see mpega_fallback.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mpega_fallback.h"

#include <dos/dos.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/nodes.h>
#include <exec/resident.h>
#include <exec/tasks.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <stdint.h>

#include "mpega_romtag.h"

/*
 * Register calls into the software library: a6 is its base, the arguments sit
 * in the registers its .fd names, and a library may change d0, d1, a0 and a1.
 * a6 itself is kept by the callee, as for every library vector. The offsets
 * are the library's own: Open -6, Close -12, Expunge -18, then the MPEGA
 * functions from -30 in the order of mpega.fd.
 */
#define MPEGA_FB_CLOBBERS_D0 "cc", "memory", "d1", "a0", "a1"
#define MPEGA_FB_CLOBBERS_D0_A0 "cc", "memory", "d1", "a1"
#define MPEGA_FB_CLOBBERS_D0_A0_A1 "cc", "memory", "d1"
#define MPEGA_FB_CLOBBERS_VOID_A0 "cc", "memory", "d0", "d1", "a1"

static struct Library *mpega_fb_lib_open(struct Library *lib)
{
  register struct Library *fb_d0 __asm("d0") = 0;
  register struct Library *fb_a6 __asm("a6") = lib;

  __asm volatile("jsr -6(a6)"
                 : "+r"(fb_d0)
                 : "r"(fb_a6)
                 : MPEGA_FB_CLOBBERS_D0);
  return fb_d0;
}

static BPTR mpega_fb_lib_close(struct Library *lib)
{
  register BPTR fb_d0 __asm("d0");
  register struct Library *fb_a6 __asm("a6") = lib;

  __asm volatile("jsr -12(a6)"
                 : "=r"(fb_d0)
                 : "r"(fb_a6)
                 : MPEGA_FB_CLOBBERS_D0);
  return fb_d0;
}

static BPTR mpega_fb_lib_expunge(struct Library *lib)
{
  register BPTR fb_d0 __asm("d0");
  register struct Library *fb_a6 __asm("a6") = lib;

  __asm volatile("jsr -18(a6)"
                 : "=r"(fb_d0)
                 : "r"(fb_a6)
                 : MPEGA_FB_CLOBBERS_D0);
  return fb_d0;
}

MPEGA_STREAM *mpega_fb_open(struct Library *lib, char *filename,
                            MPEGA_CTRL *ctrl)
{
  register MPEGA_STREAM *fb_d0 __asm("d0");
  register struct Library *fb_a6 __asm("a6") = lib;
  register char *fb_a0 __asm("a0") = filename;
  register MPEGA_CTRL *fb_a1 __asm("a1") = ctrl;

  __asm volatile("jsr -30(a6)"
                 : "=r"(fb_d0), "+r"(fb_a0), "+r"(fb_a1)
                 : "r"(fb_a6)
                 : MPEGA_FB_CLOBBERS_D0_A0_A1);
  return fb_d0;
}

void mpega_fb_close(struct Library *lib, MPEGA_STREAM *stream)
{
  register struct Library *fb_a6 __asm("a6") = lib;
  register MPEGA_STREAM *fb_a0 __asm("a0") = stream;

  __asm volatile("jsr -36(a6)"
                 : "+r"(fb_a0)
                 : "r"(fb_a6)
                 : MPEGA_FB_CLOBBERS_VOID_A0);
}

LONG mpega_fb_decode_frame(struct Library *lib, MPEGA_STREAM *stream,
                           WORD **pcm)
{
  register LONG fb_d0 __asm("d0");
  register struct Library *fb_a6 __asm("a6") = lib;
  register MPEGA_STREAM *fb_a0 __asm("a0") = stream;
  register WORD **fb_a1 __asm("a1") = pcm;

  __asm volatile("jsr -42(a6)"
                 : "=r"(fb_d0), "+r"(fb_a0), "+r"(fb_a1)
                 : "r"(fb_a6)
                 : MPEGA_FB_CLOBBERS_D0_A0_A1);
  return fb_d0;
}

LONG mpega_fb_seek(struct Library *lib, MPEGA_STREAM *stream,
                   ULONG ms_time_position)
{
  register ULONG fb_d0 __asm("d0") = ms_time_position;
  register struct Library *fb_a6 __asm("a6") = lib;
  register MPEGA_STREAM *fb_a0 __asm("a0") = stream;

  __asm volatile("jsr -48(a6)"
                 : "+r"(fb_d0), "+r"(fb_a0)
                 : "r"(fb_a6)
                 : MPEGA_FB_CLOBBERS_D0_A0);
  return (LONG)fb_d0;
}

LONG mpega_fb_time(struct Library *lib, MPEGA_STREAM *stream,
                   ULONG *ms_time_position)
{
  register LONG fb_d0 __asm("d0");
  register struct Library *fb_a6 __asm("a6") = lib;
  register MPEGA_STREAM *fb_a0 __asm("a0") = stream;
  register ULONG *fb_a1 __asm("a1") = ms_time_position;

  __asm volatile("jsr -54(a6)"
                 : "=r"(fb_d0), "+r"(fb_a0), "+r"(fb_a1)
                 : "r"(fb_a6)
                 : MPEGA_FB_CLOBBERS_D0_A0_A1);
  return fb_d0;
}

LONG mpega_fb_scale(struct Library *lib, MPEGA_STREAM *stream,
                    LONG scale_percent)
{
  register LONG fb_d0 __asm("d0") = scale_percent;
  register struct Library *fb_a6 __asm("a6") = lib;
  register MPEGA_STREAM *fb_a0 __asm("a0") = stream;

  __asm volatile("jsr -66(a6)"
                 : "+r"(fb_d0), "+r"(fb_a0)
                 : "r"(fb_a6)
                 : MPEGA_FB_CLOBBERS_D0_A0);
  return fb_d0;
}

static int mpega_fallback_in_process(void)
{
  return FindTask(0)->tc_Node.ln_Type == NT_PROCESS;
}

void mpega_fallback_init(MPEGAFallback *fb)
{
  InitSemaphore(&fb->lock);
  fb->libs.lh_Head = (struct Node *)&fb->libs.lh_Tail;
  fb->libs.lh_Tail = 0;
  fb->libs.lh_TailPred = (struct Node *)&fb->libs.lh_Head;
  fb->libs.lh_Type = NT_LIBRARY;
  fb->lib = 0;
  fb->open_streams = 0;
}

/* The Resident of the first segment that has one. A segment's size sits in
 * the longword before its next-segment pointer and counts itself. */
static const struct Resident *mpega_fallback_find_romtag(BPTR seglist)
{
  BPTR segment;

  for (segment = seglist; segment; segment = *(BPTR *)BADDR(segment)) {
    ULONG *block = (ULONG *)BADDR(segment);
    const struct Resident *resident;

    if (block[-1] < 8UL) {
      continue;
    }
    resident = mpega_romtag_scan((const uint8_t *)(block + 1),
                                 (uint32_t)(block[-1] - 8UL));
    if (resident) {
      return resident;
    }
  }

  return 0;
}

/* Moves the library's node from exec's list to ours. InitResident() has put
 * it on exec's list (the init code or exec did that), and its own Expunge
 * will Remove() it from whatever list it is on. A node that exec's list does
 * not hold is not a library InitResident() started, so it is left alone. */
static int mpega_fallback_detach(MPEGAFallback *fb, struct Library *lib)
{
  struct Node *node;
  int found = 0;

  Forbid();
  for (node = SysBase->LibList.lh_Head; node->ln_Succ; node = node->ln_Succ) {
    if (node == (struct Node *)lib) {
      Remove(node);
      AddTail(&fb->libs, node);
      found = 1;
      break;
    }
  }
  Permit();

  return found;
}

static void mpega_fallback_expunge(struct Library *lib)
{
  BPTR segment;

  Forbid();
  segment = mpega_fb_lib_expunge(lib);
  Permit();
  if (segment) {
    UnLoadSeg(segment);
  }
}

static void mpega_fallback_load(MPEGAFallback *fb)
{
  BPTR seglist;
  const struct Resident *resident;
  struct Library *lib;

  seglist = LoadSeg((CONST_STRPTR)MPEGA_FALLBACK_PATH);
  if (!seglist) {
    return;
  }

  resident = mpega_fallback_find_romtag(seglist);
  if (!resident || resident->rt_Type != NT_LIBRARY ||
      mpega_romtag_is_zz9000(resident)) {
    UnLoadSeg(seglist);
    return;
  }

  lib = (struct Library *)InitResident(resident, seglist);
  if (!lib) {
    UnLoadSeg(seglist);
    return;
  }
  if (!mpega_fallback_detach(fb, lib)) {
    /* Not a library on exec's list: nothing here can tell what it is, so
     * its segment stays loaded. */
    return;
  }

  if (!mpega_fb_lib_open(lib)) {
    mpega_fallback_expunge(lib);
    return;
  }
  fb->lib = lib;
}

struct Library *mpega_fallback_acquire(MPEGAFallback *fb)
{
  struct Library *lib;

  if (!fb || !mpega_fallback_in_process()) {
    return 0;
  }

  ObtainSemaphore(&fb->lock);
  if (!fb->lib) {
    mpega_fallback_load(fb);
  }
  lib = fb->lib;
  ReleaseSemaphore(&fb->lock);

  return lib;
}

void mpega_fallback_stream_opened(MPEGAFallback *fb)
{
  Forbid();
  fb->open_streams++;
  Permit();
}

void mpega_fallback_stream_closed(MPEGAFallback *fb)
{
  Forbid();
  if (fb->open_streams != 0) {
    fb->open_streams--;
  }
  Permit();
}

int mpega_fallback_busy(const MPEGAFallback *fb)
{
  if (fb->open_streams != 0) {
    return 1;
  }
  return fb->lib && !mpega_fallback_in_process();
}

void mpega_fallback_unload(MPEGAFallback *fb)
{
  struct Library *lib = fb->lib;
  BPTR segment;

  if (!lib) {
    return;
  }
  fb->lib = 0;

  /* Close hands the segment back if the library expunged itself on its last
   * close, and then the library is gone: expunging it again would use freed
   * memory. */
  segment = mpega_fb_lib_close(lib);
  if (segment) {
    UnLoadSeg(segment);
    return;
  }
  mpega_fallback_expunge(lib);
}
