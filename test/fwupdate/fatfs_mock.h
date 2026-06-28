/*
 * Test controls for the in-memory FatFs mock.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FATFS_MOCK_H
#define FATFS_MOCK_H

#include "ff.h"

/* Wipe the in-memory volume and clear any injected faults. */
void mock_reset(void);

/* Add a file at `path` carrying an opaque `tag` (used by tests to track
 * which firmware image ended up where). Returns 1 on success. */
int mock_add_file(const char *path, const char *tag);

/* 1 if `path` currently exists on the mock volume. */
int mock_exists(const char *path);

/* The tag of `path`, or "" if it does not exist. */
const char *mock_tag(const char *path);

/* Make the `nth` (1-based) subsequent f_rename call fail with FR_DISK_ERR.
 * 0 disables fault injection. Reset by mock_reset(). */
void mock_fail_rename_at(int nth);

#endif /* FATFS_MOCK_H */
