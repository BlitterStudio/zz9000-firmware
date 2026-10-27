/*
 * Host unit tests for SD boot metadata status handling.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sd_boot.h"
#include "sd_storage.h"

static int storage_available;
static uint32_t read_status;
static int read_count;
static int checks;
static int failures;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { \
        failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

int sd_storage_init(void)
{
    return 0;
}

uint32_t sd_storage_read_blocks(uint32_t block, uint32_t num_blocks, void *buffer)
{
    (void)block;
    (void)num_blocks;
    read_count++;
    memset(buffer, 0, 512);
    return read_status;
}

uint32_t sd_storage_write_blocks(uint32_t block, uint32_t num_blocks, void *buffer)
{
    (void)block;
    (void)num_blocks;
    (void)buffer;
    return 0;
}

uint32_t sd_storage_capacity(void)
{
    return 4096;
}

int sd_storage_available(void)
{
    return storage_available;
}

static void reset_storage(void)
{
    storage_available = 0;
    read_status = 0;
    read_count = 0;
}

static void test_no_hdf_returns_distinct_status(void)
{
    uint8_t info[sizeof(struct sd_boot_info)];

    reset_storage();
    CHECK(sd_boot_init() == SD_BOOT_STATUS_NO_HDF);
    CHECK(read_count == 0);
    CHECK(sd_boot_get_info(info) == SD_BOOT_STATUS_NO_HDF);
}

static void test_rdb_read_failure_remains_error(void)
{
    uint8_t info[sizeof(struct sd_boot_info)];

    reset_storage();
    storage_available = 1;
    read_status = 0xFD;
    CHECK(sd_boot_init() == -1);
    CHECK(read_count == 1);
    CHECK(sd_boot_get_info(info) == -1);
}

int main(void)
{
    test_no_hdf_returns_distinct_status();
    test_rdb_read_failure_remains_error();
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
