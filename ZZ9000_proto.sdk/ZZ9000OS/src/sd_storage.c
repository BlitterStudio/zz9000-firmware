/*
 * MNT ZZ9000 SD Card Storage
 * Copyright (C) 2026, MNT Research GmbH
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include "xsdps.h"
#include "xil_cache.h"
#include "diskio.h"
#include "sd_storage.h"
#include "memorymap.h"

#define SD_BLOCK_SIZE 512
#define SD_MAX_BLOCKS_AT_ONCE 8

static XSdPs SdInstance;
static int sd_card_available = 0;
static uint32_t sd_card_capacity = 0;

int sd_storage_init(void) {
    XSdPs_Config *SdConfig;
    int Status;

    SdConfig = XSdPs_LookupConfig(XPAR_XSDPS_0_DEVICE_ID);
    if (SdConfig == NULL) {
        printf("[SD] LookupConfig failed\n");
        return -1;
    }

    Status = XSdPs_CfgInitialize(&SdInstance, SdConfig, SdConfig->BaseAddress);
    if (Status != XST_SUCCESS) {
        printf("[SD] CfgInitialize failed: %d\n", Status);
        return -1;
    }

    Status = XSdPs_CardInitialize(&SdInstance);
    if (Status != XST_SUCCESS) {
        printf("[SD] CardInitialize failed: %d (no card?)\n", Status);
        return -1;
    }

    sd_card_capacity = SdInstance.SectorCount;
    sd_card_available = 1;

    printf("[SD] Card detected, capacity: %lu blocks (%lu MB)\n",
           (unsigned long)sd_card_capacity,
           (unsigned long)(sd_card_capacity / 2048));

    return 0;
}

uint32_t sd_storage_read_blocks(uint32_t block, uint32_t num_blocks, void *buffer) {
    if (!sd_card_available) return 0xFF;
    if (num_blocks == 0 || num_blocks > SD_MAX_BLOCKS_AT_ONCE) return 0xFE;

    int Status = XSdPs_ReadPolled(&SdInstance, block, num_blocks, buffer);
    if (Status != XST_SUCCESS) {
        printf("[SD] ReadPolled failed: block=%lu count=%lu status=%d\n",
               (unsigned long)block, (unsigned long)num_blocks, Status);
        return 0xFD;
    }

    Xil_DCacheInvalidateRange((UINTPTR)buffer, num_blocks * SD_BLOCK_SIZE);
    return 0;
}

uint32_t sd_storage_write_blocks(uint32_t block, uint32_t num_blocks, void *buffer) {
    if (!sd_card_available) return 0xFF;
    if (num_blocks == 0 || num_blocks > SD_MAX_BLOCKS_AT_ONCE) return 0xFE;

    Xil_DCacheFlushRange((UINTPTR)buffer, num_blocks * SD_BLOCK_SIZE);

    int Status = XSdPs_WritePolled(&SdInstance, block, num_blocks, buffer);
    if (Status != XST_SUCCESS) {
        printf("[SD] WritePolled failed: block=%lu count=%lu status=%d\n",
               (unsigned long)block, (unsigned long)num_blocks, Status);
        return 0xFD;
    }

    return 0;
}

uint32_t sd_storage_capacity(void) {
    return sd_card_capacity;
}

int sd_storage_available(void) {
    return sd_card_available;
}
