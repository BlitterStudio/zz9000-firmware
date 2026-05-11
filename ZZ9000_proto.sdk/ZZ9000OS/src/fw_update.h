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
 *
 * REG_ZZ_FWUP_STATUS reads 0xFFFF while a command is in flight, 0 on
 * success, or one of FWUP_ERR_* on failure.
 */

#ifndef FW_UPDATE_H
#define FW_UPDATE_H

#include <stdint.h>

enum fw_update_cmd {
    FWUP_CMD_OPEN  = 1,
    FWUP_CMD_WRITE = 2,
    FWUP_CMD_CLOSE = 3,
    FWUP_CMD_ABORT = 4,
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
};

/* Filename is read from `name_buf` (NUL-terminated, max 64 chars after
 * the optional leading '/'). Opens a temporary file; the requested
 * target is replaced only by fw_update_close(). */
uint16_t fw_update_open(const char *name_buf);

/* Appends `len` bytes from `buf` to the open file. `len` must be > 0
 * and <= the shared-buffer size. */
uint16_t fw_update_write(const void *buf, uint32_t len);

/* f_sync + f_close. After CLOSE the state machine returns to IDLE. */
uint16_t fw_update_close(void);

/* Deletes stale internal discard files left when replacing an existing
 * visible .bak backup. Intended for boot/reset-time cleanup, not during
 * the live CLOSE path. */
void fw_update_cleanup_backups(void);

/* Closes + f_unlink the temporary file. Safe to call from any state. */
uint16_t fw_update_abort(void);

/* Reset transient state (called on Amiga reset). Same effect as ABORT
 * but does not return a status. */
void fw_update_reset(void);

#endif
