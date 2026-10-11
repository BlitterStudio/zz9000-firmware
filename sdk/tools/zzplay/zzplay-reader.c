/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-reader.h"

#include <dos/dos.h>
#include <dos/dostags.h>
#include <exec/memory.h>
#include <proto/dos.h>
#include <proto/exec.h>

/* 64 KiB reads run at the drive's speed (16 KiB stdio reads managed about
 * a tenth of it on an A4000 SCSI disk). */
#define ZZPLAY_READER_SLOT_MAX (64UL * 1024UL)
#define ZZPLAY_READER_MAX_SLOTS 4UL

struct ZZPlayReader {
  BPTR file;
  /* Read() lands in fast RAM and is copied to the card from there: a disk
   * driver that DMAs straight into board memory it does not know about is
   * not something to rely on. */
  uint8_t *stage;
  uint8_t *card;
  uint32_t slot_bytes;
  uint32_t slots;
  volatile uint32_t length[ZZPLAY_READER_MAX_SLOTS];
  volatile uint32_t filled;    /* slots the reader has filled */
  volatile uint32_t released;  /* slots the player has written out */
  uint32_t offset;             /* bytes of the oldest slot written out */
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
        reader->filled - reader->released < reader->slots) {
      uint32_t index = reader->filled % reader->slots;
      LONG got = Read(reader->file, reader->stage,
                      (LONG)reader->slot_bytes);

      if (got < 0) {
        reader->failed = 1U;
      } else if (got == 0) {
        reader->eof = 1U;
      } else {
        CopyMem(reader->stage,
                reader->card + index * reader->slot_bytes, (ULONG)got);
        reader->length[index] = (uint32_t)got;
        reader->filled++;
      }
    } else {
      /* The player signals CTRL-F as it frees a slot, CTRL-C to stop. */
      (void)Wait(SIGBREAKF_CTRL_F | SIGBREAKF_CTRL_C);
    }
  }
  /* The player frees this code's state once signalled: stay in Forbid()
   * until the process is gone. */
  Forbid();
  Signal(reader->player, 1UL << reader->exit_signal);
}

ZZPlayReader *zzplay_reader_open(const char *path, uint8_t *card,
                                 uint32_t card_bytes)
{
  ZZPlayReader *reader;
  uint32_t slot_bytes;
  BYTE priority;

  /* At least two slots, so one fills while the other is written out. */
  slot_bytes = card_bytes / 2U;
  if (slot_bytes > ZZPLAY_READER_SLOT_MAX) {
    slot_bytes = ZZPLAY_READER_SLOT_MAX;
  }
  if (!path || !card || slot_bytes == 0U || zzplay_reader_starting) {
    return 0;
  }
  reader = (ZZPlayReader *)AllocVec(sizeof(*reader), MEMF_ANY | MEMF_CLEAR);
  if (!reader) {
    return 0;
  }
  reader->card = card;
  reader->slot_bytes = slot_bytes;
  reader->slots = card_bytes / slot_bytes;
  if (reader->slots > ZZPLAY_READER_MAX_SLOTS) {
    reader->slots = ZZPLAY_READER_MAX_SLOTS;
  }
  reader->stage = (uint8_t *)AllocVec(slot_bytes, MEMF_ANY);
  reader->exit_signal = AllocSignal(-1);
  reader->file = Open((CONST_STRPTR)path, MODE_OLDFILE);
  if (!reader->stage || reader->exit_signal < 0 || !reader->file) {
    if (reader->file) {
      Close(reader->file);
    }
    if (reader->exit_signal >= 0) {
      FreeSignal(reader->exit_signal);
    }
    FreeVec(reader->stage);
    FreeVec(reader);
    return 0;
  }
  reader->player = FindTask(0);
  zzplay_reader_starting = reader;
  /* One above the player. The player does not sleep while it waits on
   * the card: ZZPlay's SDK context polls the completion ring, so a task
   * below it would get the CPU only in the player's timed sleeps. Above
   * it, a finished read is copied to the card (about 10 ms per 64 KiB on
   * an A4000) mostly inside that polling; the rest of the time the
   * process sleeps in DOS or Wait(). */
  priority = reader->player->tc_Node.ln_Pri;
  if (priority < 127) {
    priority++;
  }
  reader->process = CreateNewProcTags(
      NP_Entry, (ULONG)zzplay_reader_entry,
      NP_Name, (ULONG)"ZZPlay reader",
      NP_Priority, (ULONG)(LONG)priority,
      NP_StackSize, 4096UL,
      TAG_DONE);
  if (!reader->process) {
    zzplay_reader_starting = 0;
    Close(reader->file);
    FreeSignal(reader->exit_signal);
    FreeVec(reader->stage);
    FreeVec(reader);
    return 0;
  }
  return reader;
}

int zzplay_reader_peek(ZZPlayReader *reader, uint32_t *offset,
                       uint32_t *length, int *eof)
{
  uint32_t index;
  /* The end state before the slot count: a reader that set eof (or
   * failed) had filled every slot it ever will. */
  uint8_t ended = reader->eof;
  uint8_t failed = reader->failed;

  *offset = 0U;
  *length = 0U;
  *eof = 0;
  if (reader->filled == reader->released) {
    if (failed) {
      return 0;
    }
    *eof = ended ? 1 : 0;
    return 1;
  }
  index = reader->released % reader->slots;
  *offset = index * reader->slot_bytes + reader->offset;
  *length = reader->length[index] - reader->offset;
  *eof = ended && reader->filled - reader->released == 1U ? 1 : 0;
  return 1;
}

void zzplay_reader_consume(ZZPlayReader *reader, uint32_t bytes)
{
  uint32_t index = reader->released % reader->slots;

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
  FreeVec(reader->stage);
  FreeVec(reader);
}
