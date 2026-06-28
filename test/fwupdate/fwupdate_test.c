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

int main(void) {
    test_restore_promotes_backup();
    test_restore_without_backup_fails();
    test_restore_rollback_on_promote_failure();
    test_restore_rejected_while_transfer_open();
    test_restore_with_missing_active_promotes();
    test_restore_bak_target_uses_old();
    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
