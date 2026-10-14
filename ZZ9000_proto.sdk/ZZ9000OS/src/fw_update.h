/*
 * ZZ9000 Firmware-File Push Interface
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Lets an AmigaOS-side CLI tool write files to the FAT32 SD card via
 * the Zorro register window, so users can drop a new BOOT.bin (or any
 * other firmware-companion file) onto the card without removing it
 * from the slot. Operates through the same FatFs instance already
 * mounted for SD HDF boot, so there is no raw-block / FAT-handler
 * concurrency hazard with `zz9000.hdf`.
 *
 * Protocol (see main.c register handlers, REG_ZZ_FWUP_*):
 *   1. Amiga stages NUL-terminated filename in the 24 KB shared
 *      buffer at USB_BLOCK_STORAGE_ADDRESS, writes REG_ZZ_FWUP_CMD = OPEN.
 *      The firmware opens a temporary root file; the target is not
 *      replaced at this point.
 *   2. For each chunk: Amiga stages up to 24 KB in the shared buffer,
 *      writes REG_ZZ_FWUP_LEN = chunk_bytes, REG_ZZ_FWUP_CMD = WRITE.
 *   3. Amiga writes REG_ZZ_FWUP_CMD = CLOSE to finalize and commit the
 *      temporary file over the requested target.
 *   4. ABORT (cmd 4) at any time discards the temporary file.
 *   5. RESTORE (cmd 5): with a target name staged like OPEN, promote
 *      that target's saved backup (e.g. BOOT.bak) back over it; the
 *      replaced file is moved to a discard slot reclaimed at next boot.
 *
 * REG_ZZ_FWUP_STATUS reads 0xFFFF while a command is in flight, 0 on
 * success, or one of FWUP_ERR_* on failure.
 *
 * Status mirror (ZZ_FW_CAP_FWUP_STATUS_MIRROR): every completed command
 * is also published as one longword in the last longword of the boot
 * ROM window (BOOT_ROM_FWUP_MIRROR_OFFSET, board offset 0x7FFC), read
 * big-endian by the Amiga:
 *   byte 0  FWUP_STATUS_MIRROR_MAGIC
 *   byte 1  sequence, incremented on every completion
 *   2..3    the command's status
 * The register is answered by the ARM main loop, so a read issued while
 * a FatFs call runs holds the Amiga's bus cycle until it returns. The
 * FPGA serves the boot ROM window from DDR directly, so a client that
 * notes the sequence, issues the command and polls for a new sequence
 * never waits on the ARM. The mirror stays out of the shared I/O buffer
 * because other clients (zzsd, USB proxy) stage their own payloads there
 * while a FWUP command may be completing.
 */

#ifndef FW_UPDATE_H
#define FW_UPDATE_H

#include <stdint.h>

enum fw_update_cmd {
    FWUP_CMD_OPEN    = 1,
    FWUP_CMD_WRITE   = 2,
    FWUP_CMD_CLOSE   = 3,
    FWUP_CMD_ABORT   = 4,
    FWUP_CMD_RESTORE = 5,
};

enum fw_update_status {
    FWUP_OK             = 0x00,
    FWUP_ERR_NO_SD      = 0x02,
    FWUP_ERR_BAD_NAME   = 0x03,
    FWUP_ERR_OPEN       = 0x04,
    FWUP_ERR_WRITE      = 0x05,
    FWUP_ERR_CLOSE      = 0x06,
    FWUP_ERR_STATE      = 0x07,
    FWUP_ERR_LEN        = 0x08,
    FWUP_ERR_UNKNOWN    = 0x09,
    FWUP_ERR_NO_BACKUP  = 0x0A,
    FWUP_ERR_RESTORE    = 0x0B,
};

#define FWUP_STATUS_MIRROR_MAGIC  0x46u

/* Filename is read from `name_buf` (NUL-terminated, max 64 chars after
 * the optional leading '/'). Opens a temporary file; the requested
 * target is replaced only by fw_update_close(). */
uint16_t fw_update_open(const char *name_buf);

/* Appends `len` bytes from `buf` to the open file. `len` must be > 0
 * and <= the shared-buffer size. */
uint16_t fw_update_write(const void *buf, uint32_t len);

/* f_sync + f_close. After CLOSE the state machine returns to IDLE. */
uint16_t fw_update_close(void);

/* Restore a previously-saved backup as the active firmware file.
 * `name_buf` is the active target name (e.g. "BOOT.bin", NUL-terminated,
 * same rules as fw_update_open). The matching backup (e.g. "BOOT.bak")
 * is renamed onto the target; the replaced file is moved to a discard
 * slot reclaimed at next boot. Must be called while idle. */
uint16_t fw_update_restore(const char *name_buf);

/* Deletes stale internal discard files left when replacing an existing
 * visible .bak backup. Intended for boot/reset-time cleanup, not during
 * the live CLOSE path. */
void fw_update_cleanup_backups(void);

/* Closes + f_unlink the temporary file. Safe to call from any state. */
uint16_t fw_update_abort(void);

/* Reset transient state (called on Amiga reset). Same effect as ABORT
 * but does not return a status. */
void fw_update_reset(void);

/* Publish a completed command's status with the next sequence to the
 * mirror longword at `mirror`, and push it to DDR for the FPGA's direct
 * read. */
void fw_update_publish_status(volatile uint8_t *mirror, uint16_t status);

/*
 * A command as one job, so it can run on core 1. An SD write can take most
 * of a second, and on the core-0 main loop that stalls the Zorro register
 * service the Amiga's other drivers need (the Amiga then crashes). Core 0
 * stages the command, queues it, and keeps serving the Amiga; whoever runs
 * the job (the core-1 worker, or core 0 inline when core 1 is unavailable)
 * calls fw_update_run_staged(). While a job is queued, nothing on core 0
 * may touch FatFs or the SD card (fw_update_busy()).
 */

/* Runs one command with its staged bytes in `buf`. */
uint16_t fw_update_run(uint16_t cmd, const void *buf, uint32_t len);

/* Copies the command's bytes out of the shared Zorro buffer (OPEN and
 * RESTORE: the name; WRITE: the chunk), which other drivers reuse once the
 * command is accepted, and cleans the copy to memory. */
void fw_update_stage(uint16_t cmd, const void *shared, uint32_t len);

/* Runs the staged command. */
uint16_t fw_update_run_staged(void);

/* Core 0 bookkeeping: the staged job was queued on core 1; it retired with
 * `result` (the job ran) or failed before producing one; or it was dropped
 * with the task queue (Amiga reset). */
void fw_update_mark_queued(void);
void fw_update_retire(uint16_t result);
void fw_update_retire_failed(void);
void fw_update_forget_queued(void);

/* 1 while a queued job has not retired. */
int fw_update_busy(void);

/* Takes a retired job's result once: 1 and *result set, else 0. */
int fw_update_take_result(uint16_t *result);

#endif
