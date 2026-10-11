/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-reader.h"

#include <dos/dos.h>
#include <dos/dostags.h>
#include <exec/memory.h>
#include <proto/dos.h>
#include <proto/exec.h>

/* 64 KiB reads run at the drive's speed (16 KiB stdio reads managed about
 * a tenth of it on an A4000 SCSI disk), and eight of them hold about half
 * a second of 9.8 Mbit/s DVD video. */
#define ZZPLAY_READER_BLOCK_BYTES (64UL * 1024UL)
#define ZZPLAY_READER_MAX_BLOCKS 8UL
#define ZZPLAY_READER_MIN_BLOCKS 2UL

struct ZZPlayReader {
  BPTR file;
  uint8_t *ring;
  uint32_t blocks;
  volatile uint32_t length[ZZPLAY_READER_MAX_BLOCKS];
  volatile uint32_t filled;    /* blocks the reader has filled */
  volatile uint32_t released;  /* blocks the player has taken */
  uint32_t offset;             /* bytes of the oldest block taken */
  volatile uint8_t eof;
  volatile uint8_t failed;
  volatile uint8_t quit;
  struct Task *player;
  struct Process *process;
  BYTE exit_signal;
};

/* The reader process finds its state here: one reader runs at a time, and
 * the pointer is set before the process exists. */
static ZZPlayReader *zzplay_reader_starting;

static void zzplay_reader_entry(void)
{
  ZZPlayReader *reader = zzplay_reader_starting;

  /* After EOF or a failed read the process waits for CTRL-C all the same:
   * the player signals it until it closes the reader. */
  while (!reader->quit) {
    if (!reader->eof && !reader->failed &&
        reader->filled - reader->released < reader->blocks) {
      uint32_t index = reader->filled % reader->blocks;
      LONG got = Read(reader->file,
                      reader->ring + index * ZZPLAY_READER_BLOCK_BYTES,
                      (LONG)ZZPLAY_READER_BLOCK_BYTES);

      if (got < 0) {
        reader->failed = 1U;
      } else if (got == 0) {
        reader->eof = 1U;
      } else {
        reader->length[index] = (uint32_t)got;
        reader->filled++;
      }
    } else {
      /* The player signals CTRL-F as it frees a block, CTRL-C to stop. */
      (void)Wait(SIGBREAKF_CTRL_F | SIGBREAKF_CTRL_C);
    }
  }
  /* The player frees this code's state once signalled: stay in Forbid()
   * until the process is gone. */
  Forbid();
  Signal(reader->player, 1UL << reader->exit_signal);
}

ZZPlayReader *zzplay_reader_open(const char *path)
{
  ZZPlayReader *reader;
  uint32_t blocks;

  if (!path || zzplay_reader_starting) {
    return 0;
  }
  reader = (ZZPlayReader *)AllocVec(sizeof(*reader), MEMF_ANY | MEMF_CLEAR);
  if (!reader) {
    return 0;
  }
  for (blocks = ZZPLAY_READER_MAX_BLOCKS;
       blocks >= ZZPLAY_READER_MIN_BLOCKS && !reader->ring; blocks /= 2U) {
    reader->ring = (uint8_t *)AllocVec(blocks * ZZPLAY_READER_BLOCK_BYTES,
                                       MEMF_ANY);
    reader->blocks = blocks;
  }
  reader->exit_signal = AllocSignal(-1);
  reader->file = Open((CONST_STRPTR)path, MODE_OLDFILE);
  if (!reader->ring || reader->exit_signal < 0 || !reader->file) {
    if (reader->file) {
      Close(reader->file);
    }
    if (reader->exit_signal >= 0) {
      FreeSignal(reader->exit_signal);
    }
    FreeVec(reader->ring);
    FreeVec(reader);
    return 0;
  }
  reader->player = FindTask(0);
  zzplay_reader_starting = reader;
  /* One above the player: a finished read is answered at once, and the
   * process sleeps in DOS or Wait() the rest of the time. */
  reader->process = CreateNewProcTags(
      NP_Entry, (ULONG)zzplay_reader_entry,
      NP_Name, (ULONG)"ZZPlay reader",
      NP_Priority, (ULONG)(reader->player->tc_Node.ln_Pri + 1),
      NP_StackSize, 4096UL,
      TAG_DONE);
  if (!reader->process) {
    zzplay_reader_starting = 0;
    Close(reader->file);
    FreeSignal(reader->exit_signal);
    FreeVec(reader->ring);
    FreeVec(reader);
    return 0;
  }
  return reader;
}

int zzplay_reader_peek(ZZPlayReader *reader, const uint8_t **data,
                       uint32_t *length, int *eof)
{
  uint32_t index;
  /* The end state before the block count: a reader that set eof (or
   * failed) had filled every block it ever will. */
  uint8_t ended = reader->eof;
  uint8_t failed = reader->failed;

  *data = 0;
  *length = 0U;
  *eof = 0;
  if (reader->filled == reader->released) {
    if (failed) {
      return 0;
    }
    *eof = ended ? 1 : 0;
    return 1;
  }
  index = reader->released % reader->blocks;
  *data = reader->ring + index * ZZPLAY_READER_BLOCK_BYTES + reader->offset;
  *length = reader->length[index] - reader->offset;
  /* Read before the block count: a reader that set eof has filled its
   * last block already. */
  *eof = reader->eof && reader->filled - reader->released == 1U ? 1 : 0;
  return 1;
}

void zzplay_reader_consume(ZZPlayReader *reader, uint32_t bytes)
{
  uint32_t index = reader->released % reader->blocks;

  reader->offset += bytes;
  if (reader->offset >= reader->length[index]) {
    reader->offset = 0U;
    reader->released++;
    Signal(&reader->process->pr_Task, SIGBREAKF_CTRL_F);
  }
}

void zzplay_reader_close(ZZPlayReader *reader)
{
  if (!reader) {
    return;
  }
  /* The child may exit as soon as it sees quit: no task switch until it
   * has been signalled. */
  Forbid();
  reader->quit = 1U;
  Signal(&reader->process->pr_Task, SIGBREAKF_CTRL_C);
  Permit();
  (void)Wait(1UL << reader->exit_signal);
  zzplay_reader_starting = 0;
  Close(reader->file);
  FreeSignal(reader->exit_signal);
  FreeVec(reader->ring);
  FreeVec(reader);
}
