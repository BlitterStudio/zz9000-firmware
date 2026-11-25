/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-reader.h"

#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <proto/dos.h>
#include <proto/exec.h>

/* 64 KiB reads run at the drive's speed (16 KiB stdio reads managed about
 * a tenth of it on an A4000 SCSI disk). */
#define ZZPLAY_READER_SLOT_MAX (64UL * 1024UL)
#define ZZPLAY_READER_MAX_SLOTS 4UL
/* The copy to the card runs at about 7.6 MB/s over Zorro III, so 8 KiB is
 * about 1.1 ms: the longest the player can be kept off the CPU by one
 * piece of the copy. */
#define ZZPLAY_READER_CHUNK 8192UL
/* How long the reader sleeps after a chunk when the player wants the CPU.
 * About one chunk's copy time, so the player gets at least half the CPU
 * and the reader still moves about 3.5 MB/s, well above any clip the card
 * decodes. */
#define ZZPLAY_READER_NAP_US 1000UL

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

/* The reader's own timer.device request: DoIO() waits on the signal of the
 * port's owning task, so the process creates and opens it itself. */
struct ZZPlayReaderNap {
  struct MsgPort *port;
  struct timerequest *request;
};

static int zzplay_reader_nap_open(struct ZZPlayReaderNap *nap)
{
  nap->port = CreateMsgPort();
  if (!nap->port) {
    return 0;
  }
  nap->request = (struct timerequest *)CreateIORequest(
      nap->port, sizeof(*nap->request));
  if (!nap->request) {
    DeleteMsgPort(nap->port);
    nap->port = 0;
    return 0;
  }
  if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
                 (struct IORequest *)nap->request, 0) != 0) {
    DeleteIORequest((struct IORequest *)nap->request);
    DeleteMsgPort(nap->port);
    nap->request = 0;
    nap->port = 0;
    return 0;
  }
  return 1;
}

static void zzplay_reader_nap_close(struct ZZPlayReaderNap *nap)
{
  if (nap->request) {
    CloseDevice((struct IORequest *)nap->request);
    DeleteIORequest((struct IORequest *)nap->request);
    nap->request = 0;
  }
  if (nap->port) {
    DeleteMsgPort(nap->port);
    nap->port = 0;
  }
}

static void zzplay_reader_nap(struct ZZPlayReaderNap *nap)
{
  nap->request->tr_node.io_Command = TR_ADDREQUEST;
  nap->request->tr_time.tv_secs = 0U;
  nap->request->tr_time.tv_micro = ZZPLAY_READER_NAP_US;
  (void)DoIO((struct IORequest *)nap->request);
}

/* Copies a staged slot to the card in chunks. The reader runs above the
 * player, so every chunk it copies is CPU the player does not get: a player
 * that is polling the card for a reply cannot see one that is already
 * published until the reader gives the CPU back. After each chunk the
 * reader looks at the player's task state. TS_READY means the player is
 * runnable and this process is what keeps it off the CPU, so the reader
 * sleeps on the timer and the player runs. The next chunk is copied after
 * the nap. A player that is asleep (TS_WAIT) costs nothing and the copy
 * goes on at full speed.
 *
 * The state is only a hint and is read without Forbid(): a stale value
 * costs one nap too many or one chunk too many, never correctness. The
 * player's task structure lives until zzplay_reader_close() has returned.
 * Nothing here waits for the player and the player never waits for this
 * process (apart from close, which signals first), so there is no signal
 * to lose and nothing to deadlock on. Returns 0 when the reader was told
 * to quit; the slot is then incomplete and must not be published. */
static int zzplay_reader_copy_slot(ZZPlayReader *reader,
                                   struct ZZPlayReaderNap *nap,
                                   uint8_t *destination, uint32_t length)
{
  uint32_t done = 0U;

  while (done < length) {
    uint32_t part = length - done;

    if (reader->quit) {
      return 0;
    }
    if (part > ZZPLAY_READER_CHUNK) {
      part = ZZPLAY_READER_CHUNK;
    }
    CopyMem(reader->stage + done, destination + done, (ULONG)part);
    done += part;
    if (done < length && nap->request &&
        reader->player->tc_State == TS_READY) {
      zzplay_reader_nap(nap);
    }
  }
  return 1;
}

static void zzplay_reader_entry(void)
{
  ZZPlayReader *reader = zzplay_reader_starting;
  struct ZZPlayReaderNap nap;

  /* Without the timer the copy runs in one piece, as it did before the
   * chunks. */
  nap.port = 0;
  nap.request = 0;
  (void)zzplay_reader_nap_open(&nap);
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
      } else if (zzplay_reader_copy_slot(
                     reader, &nap,
                     reader->card + index * reader->slot_bytes,
                     (uint32_t)got)) {
        /* Published only once the whole slot is on the card. */
        reader->length[index] = (uint32_t)got;
        reader->filled++;
      }
    } else {
      /* The player signals CTRL-F as it frees a slot, CTRL-C to stop. */
      (void)Wait(SIGBREAKF_CTRL_F | SIGBREAKF_CTRL_C);
    }
  }
  zzplay_reader_nap_close(&nap);
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
   * it, a finished read is copied to the card (about 8.6 ms per 64 KiB on
   * an A4000) inside that polling. The copy is cut into chunks and the
   * process naps between them while the player is waiting for the CPU
   * (see zzplay_reader_copy_slot), so the player loses at most one chunk
   * at a time instead of the whole slot. The rest of the time the
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
