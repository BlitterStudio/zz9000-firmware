/*
 * Host unit tests for the ZZ9000 firmware-file restore path.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <string.h>

#include "fatfs_mock.h"
#include "fw_update.h"

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(cond, msg)                                              \
    do {                                                             \
        g_checks++;                                                  \
        if (!(cond)) {                                               \
            g_fails++;                                               \
            printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
        }                                                           \
    } while (0)

/* One-way restore: BOOT.bak becomes the active BOOT.bin, and the
 * previously-active image is moved aside to a discard slot. */
static void test_restore_promotes_backup(void) {
    printf("test_restore_promotes_backup\n");
    mock_reset();
    mock_add_file("0:/BOOT.bin", "NEW");
    mock_add_file("0:/BOOT.bak", "OLD");

    uint16_t st = fw_update_restore("BOOT.bin");

    CHECK(st == FWUP_OK, "restore returns OK");
    CHECK(mock_exists("0:/BOOT.bin"), "BOOT.bin still present");
    CHECK(strcmp(mock_tag("0:/BOOT.bin"), "OLD") == 0,
          "BOOT.bin now holds the backup image (OLD)");
    CHECK(!mock_exists("0:/BOOT.bak"), "BOOT.bak consumed");
    CHECK(mock_exists("0:/ZZFWUP.DEL"),
          "replaced firmware moved to a discard slot");
    CHECK(strcmp(mock_tag("0:/ZZFWUP.DEL"), "NEW") == 0,
          "discard slot holds the replaced firmware (NEW)");
}

/* With no backup present, restore must refuse and leave the active
 * firmware completely untouched. */
static void test_restore_without_backup_fails(void) {
    printf("test_restore_without_backup_fails\n");
    mock_reset();
    mock_add_file("0:/BOOT.bin", "ACTIVE");

    uint16_t st = fw_update_restore("BOOT.bin");

    CHECK(st == FWUP_ERR_NO_BACKUP, "restore without backup returns NO_BACKUP");
    CHECK(mock_exists("0:/BOOT.bin"), "active firmware still present");
    CHECK(strcmp(mock_tag("0:/BOOT.bin"), "ACTIVE") == 0,
          "active firmware unchanged");
    CHECK(!mock_exists("0:/ZZFWUP.DEL"), "nothing was discarded");
}

/* If the backup->active promotion fails after the active file was moved
 * aside, restore must roll the active file back so the card keeps a
 * working firmware, and report the failure. */
static void test_restore_rollback_on_promote_failure(void) {
    printf("test_restore_rollback_on_promote_failure\n");
    mock_reset();
    mock_add_file("0:/BOOT.bin", "ACTIVE");
    mock_add_file("0:/BOOT.bak", "BACKUP");
    /* rename #1 = active -> discard (ok); rename #2 = backup -> active (fail). */
    mock_fail_rename_at(2);

    uint16_t st = fw_update_restore("BOOT.bin");

    CHECK(st == FWUP_ERR_RESTORE, "promote failure returns RESTORE error");
    CHECK(mock_exists("0:/BOOT.bin"), "active firmware preserved by rollback");
    CHECK(strcmp(mock_tag("0:/BOOT.bin"), "ACTIVE") == 0,
          "active firmware content intact after rollback");
    CHECK(!mock_exists("0:/ZZFWUP.DEL"), "discard slot rolled back");
    CHECK(mock_exists("0:/BOOT.bak"), "backup still present after failed restore");
}

/* Restore is a standalone operation; it must refuse to run while a
 * push transfer is open rather than corrupt that transfer's state. */
static void test_restore_rejected_while_transfer_open(void) {
    printf("test_restore_rejected_while_transfer_open\n");
    mock_reset();
    CHECK(fw_update_open("OTHER.bin") == FWUP_OK, "a push transfer is open");

    uint16_t st = fw_update_restore("BOOT.bin");

    CHECK(st == FWUP_ERR_STATE, "restore during open transfer returns STATE");

    fw_update_abort();
}

/* When the active file is absent but a backup exists, restore simply
 * promotes the backup into place with nothing to discard. */
static void test_restore_with_missing_active_promotes(void) {
    printf("test_restore_with_missing_active_promotes\n");
    mock_reset();
    mock_add_file("0:/BOOT.bak", "BACKUP");

    uint16_t st = fw_update_restore("BOOT.bin");

    CHECK(st == FWUP_OK, "restore with missing active returns OK");
    CHECK(strcmp(mock_tag("0:/BOOT.bin"), "BACKUP") == 0,
          "backup promoted to active");
    CHECK(!mock_exists("0:/BOOT.bak"), "backup consumed");
    CHECK(!mock_exists("0:/ZZFWUP.DEL"), "nothing discarded");
}

/* A target that already ends in .bak restores from its .old companion,
 * matching the backup-naming rule used when writing files. */
static void test_restore_bak_target_uses_old(void) {
    printf("test_restore_bak_target_uses_old\n");
    mock_reset();
    mock_add_file("0:/BOOT.bak", "ACTIVE");
    mock_add_file("0:/BOOT.old", "PREVIOUS");

    uint16_t st = fw_update_restore("BOOT.bak");

    CHECK(st == FWUP_OK, "restore of .bak target returns OK");
    CHECK(strcmp(mock_tag("0:/BOOT.bak"), "PREVIOUS") == 0,
          ".old companion promoted onto .bak target");
    CHECK(!mock_exists("0:/BOOT.old"), ".old companion consumed");
}

/* The Amiga reads the mirror as one big-endian longword: magic, a
 * sequence that changes on every completion, then the status. The
 * neighbouring ROM bytes stay untouched. */
static void test_status_mirror_layout_and_sequence(void) {
    uint32_t words[3];  /* the mirror is one aligned longword */
    uint8_t *rom = (uint8_t *)words;
    uint8_t *mirror = rom + 4;
    uint8_t first_seq;

    printf("test_status_mirror_layout_and_sequence\n");
    memset(rom, 0xAA, sizeof(words));
    fw_update_publish_status(mirror, FWUP_ERR_WRITE);
    CHECK(mirror[0] == FWUP_STATUS_MIRROR_MAGIC, "magic in byte 0");
    CHECK(mirror[2] == 0x00 && mirror[3] == FWUP_ERR_WRITE,
          "status big-endian in bytes 2..3");
    CHECK(rom[3] == 0xAA && rom[8] == 0xAA, "neighbouring bytes untouched");
    first_seq = mirror[1];

    fw_update_publish_status(mirror, FWUP_OK);
    CHECK(mirror[1] != first_seq, "sequence changes per completion");
    CHECK(mirror[2] == 0x00 && mirror[3] == FWUP_OK,
          "second status published");
}

/* A command runs on its staged copy: other drivers reuse the shared Zorro
 * buffer as soon as the command is accepted, before core 1 runs it. */
static void test_staged_job_runs_on_its_own_copy(void) {
    char shared[96];
    const unsigned char *data;
    unsigned len = 0;

    printf("test_staged_job_runs_on_its_own_copy\n");
    mock_reset();
    fw_update_abort();
    strcpy(shared, "NEW.bin");
    fw_update_stage(FWUP_CMD_OPEN, shared, 0);
    strcpy(shared, "EVIL.bin");
    CHECK(fw_update_run_staged() == FWUP_OK, "staged OPEN succeeds");

    memcpy(shared, "abc", 3);
    fw_update_stage(FWUP_CMD_WRITE, shared, 3);
    memcpy(shared, "xyz", 3);
    CHECK(fw_update_run_staged() == FWUP_OK, "staged WRITE succeeds");

    fw_update_stage(FWUP_CMD_CLOSE, shared, 0);
    CHECK(fw_update_run_staged() == FWUP_OK, "staged CLOSE succeeds");
    CHECK(mock_exists("0:/NEW.bin") && !mock_exists("0:/EVIL.bin"),
          "the staged name was opened, not the overwritten one");
    data = mock_data("0:/NEW.bin", &len);
    CHECK(data && len == 3 && memcmp(data, "abc", 3) == 0,
          "the staged chunk was written, not the overwritten one");
}

/* Staging keeps the command's limits: a name one character too long and
 * a chunk larger than the shared buffer are still refused. */
static void test_staged_job_keeps_limits(void) {
    static char big[24577];
    char name[80];

    printf("test_staged_job_keeps_limits\n");
    mock_reset();
    fw_update_abort();
    memset(name, 'A', 65);
    name[65] = '\0';
    fw_update_stage(FWUP_CMD_OPEN, name, 0);
    CHECK(fw_update_run_staged() == FWUP_ERR_BAD_NAME,
          "a 65-character name is rejected after staging");
    name[64] = '\0';
    fw_update_stage(FWUP_CMD_OPEN, name, 0);
    CHECK(fw_update_run_staged() == FWUP_OK,
          "a 64-character name is accepted after staging");
    fw_update_stage(FWUP_CMD_WRITE, big, sizeof(big));
    CHECK(fw_update_run_staged() == FWUP_ERR_LEN,
          "a chunk over 24 KiB is refused after staging");
    fw_update_abort();
}

/* Core 0 sees a queued job as busy until it retires; the result is taken
 * once. A job that faulted reports its command's error; a job dropped
 * with the task queue (Amiga reset) leaves no result. */
static void test_job_retirement(void) {
    uint16_t result = 0xFFFF;
    char shared[8] = "x";

    printf("test_job_retirement\n");
    fw_update_stage(FWUP_CMD_WRITE, shared, 1);
    fw_update_mark_queued();
    CHECK(fw_update_busy(), "a queued job is busy");
    CHECK(!fw_update_take_result(&result), "no result before it retires");
    fw_update_retire(FWUP_ERR_WRITE);
    CHECK(!fw_update_busy(), "a retired job is no longer busy");
    CHECK(fw_update_take_result(&result) && result == FWUP_ERR_WRITE,
          "the retired result is taken");
    CHECK(!fw_update_take_result(&result), "the result is taken only once");

    fw_update_mark_queued();
    fw_update_retire_failed();
    CHECK(fw_update_take_result(&result) && result == FWUP_ERR_WRITE,
          "a faulted WRITE reports WRITE's error");
    fw_update_stage(FWUP_CMD_OPEN, "BOOT.bin", 0);
    fw_update_mark_queued();
    fw_update_retire_failed();
    CHECK(fw_update_take_result(&result) && result == FWUP_ERR_OPEN,
          "a faulted OPEN reports OPEN's error");

    fw_update_mark_queued();
    fw_update_forget_queued();
    CHECK(!fw_update_busy() && !fw_update_take_result(&result),
          "a dropped job is neither busy nor has a result");
}

int main(void) {
    test_restore_promotes_backup();
    test_restore_without_backup_fails();
    test_restore_rollback_on_promote_failure();
    test_restore_rejected_while_transfer_open();
    test_restore_with_missing_active_promotes();
    test_restore_bak_target_uses_old();
    test_status_mirror_layout_and_sequence();
    test_staged_job_runs_on_its_own_copy();
    test_staged_job_keeps_limits();
    test_job_retirement();
    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
