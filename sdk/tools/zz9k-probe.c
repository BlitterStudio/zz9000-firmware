/*
 * Safe SDK v2 discovery probe.
 *
 * This tool intentionally does not dereference the mailbox pointer. It only
 * reads 16-bit SDK registers and prints the board window reported by
 * expansion.library, so it is the first real-hardware diagnostic to run after
 * a firmware update.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/abi.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__amigaos__) || defined(__amiga__) || defined(__AMIGA__) || \
    defined(__VBCC__)
#define ZZ9K_PROBE_AMIGA 1
#include <exec/types.h>
#include <libraries/configvars.h>
#include <libraries/expansion.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#endif

#if ZZ9K_PROBE_AMIGA
static volatile uint16_t *reg16(uint32_t board_addr, uint32_t offset)
{
  return (volatile uint16_t *)((uint8_t *)board_addr + offset);
}

static uint16_t read_reg16(uint32_t board_addr, uint32_t offset)
{
  return *reg16(board_addr, offset);
}

static uint32_t read_reg32_pair(uint32_t board_addr, uint32_t offset)
{
  return ((uint32_t)read_reg16(board_addr, offset) << 16) |
         read_reg16(board_addr, offset + 2U);
}

static int board_range_fits(uint32_t board_size, uint32_t offset,
                            uint32_t length)
{
  if (length == 0 || offset > (UINT32_MAX - (length - 1U))) {
    return 0;
  }
  if (board_size == 0) {
    return 1;
  }

  return offset < board_size && length <= (board_size - offset);
}

static int z3_register_window_offset(uint32_t zorro_version,
                                     uint32_t board_size,
                                     uint32_t register_offset,
                                     uint32_t *board_offset)
{
  uint32_t offset;

  if (!board_offset || zorro_version != 3) {
    return 0;
  }

  offset = ZZ9K_Z3_REGISTER_WINDOW_OFFSET + register_offset;
  if (!board_range_fits(board_size, offset, sizeof(uint16_t))) {
    return 0;
  }

  *board_offset = offset;
  return 1;
}

static int mapped_io_mailbox_fits(uint32_t board_size, uint32_t mailbox_addr)
{
  uint32_t last_addr;
  uint32_t offset;

  if (mailbox_addr < ZZ9K_MAPPED_IO_ARM_START ||
      mailbox_addr > (UINT32_MAX - (ZZ9K_MAILBOX_DESCRIPTOR_SIZE - 1U))) {
    return 0;
  }

  last_addr = mailbox_addr + ZZ9K_MAILBOX_DESCRIPTOR_SIZE - 1U;
  if (last_addr >=
      (ZZ9K_MAPPED_IO_ARM_START + ZZ9K_MAPPED_IO_WINDOW_SIZE)) {
    return 0;
  }

  offset = ZZ9K_MAPPED_IO_BOARD_OFFSET +
           (mailbox_addr - ZZ9K_MAPPED_IO_ARM_START);
  return board_range_fits(board_size, offset, ZZ9K_MAILBOX_DESCRIPTOR_SIZE);
}

static int direct_mailbox_fits(uint32_t board_size, uint32_t mailbox_addr)
{
  uint32_t last_addr;
  uint32_t offset;

  if (mailbox_addr < ZZ9K_ARM_MEMORY_START ||
      mailbox_addr > (UINT32_MAX - (ZZ9K_MAILBOX_DESCRIPTOR_SIZE - 1U))) {
    return 0;
  }

  last_addr = mailbox_addr + ZZ9K_MAILBOX_DESCRIPTOR_SIZE - 1U;
  if (last_addr > ZZ9K_ARM_MEMORY_VISIBLE_END) {
    return 0;
  }

  offset = ZZ9K_AMIGA_MEMORY_OFFSET + (mailbox_addr - ZZ9K_ARM_MEMORY_START);
  return board_range_fits(board_size, offset, ZZ9K_MAILBOX_DESCRIPTOR_SIZE);
}

static int mailbox_fits(uint32_t board_size, uint32_t mailbox_addr)
{
  return mapped_io_mailbox_fits(board_size, mailbox_addr) ||
         direct_mailbox_fits(board_size, mailbox_addr);
}
#endif

int main(int argc, char **argv)
{
#if ZZ9K_PROBE_AMIGA
  struct Library *expansion_base;
  struct ConfigDev *config_dev;
  uint32_t board_addr;
  uint32_t board_size;
  uint32_t zorro_version;
  uint32_t hdl_status_offset;
  uint16_t sdk_magic;
  uint16_t sdk_version;
  uint32_t mailbox_addr;
  uint16_t sdk_status;
  uint32_t sdk_diag_write;
  uint32_t sdk_diag_data;
  uint32_t sdk_diag_z3addr;
  int read_hdl_diag = 0;

  if (argc > 1 && strcmp(argv[1], "--hdl") == 0) {
    read_hdl_diag = 1;
  }

  expansion_base = OpenLibrary((CONST_STRPTR)"expansion.library", 0);
  if (!expansion_base) {
    printf("zz9k-probe: failed to open expansion.library\n");
    return 1;
  }

  config_dev = FindConfigDev(0, ZZ9K_MNT_MANUFACTURER, ZZ9K_PRODUCT_Z3);
  zorro_version = 3;
  if (!config_dev) {
    config_dev = FindConfigDev(0, ZZ9K_MNT_MANUFACTURER, ZZ9K_PRODUCT_Z2);
    zorro_version = 2;
  }
  if (!config_dev) {
    CloseLibrary(expansion_base);
    printf("zz9k-probe: ZZ9000 not found\n");
    return 1;
  }

  board_addr = (uint32_t)config_dev->cd_BoardAddr;
  board_size = config_dev->cd_BoardSize;
  printf("board addr:      0x%08lx\n", (unsigned long)board_addr);
  printf("board size:      0x%08lx\n", (unsigned long)board_size);
  printf("zorro:           %lu\n", (unsigned long)zorro_version);
  printf("product:         %u\n", (unsigned)config_dev->cd_Rom.er_Product);

  sdk_magic = read_reg16(board_addr, ZZ9K_REG_SDK_MAGIC);
  sdk_version = read_reg16(board_addr, ZZ9K_REG_SDK_VERSION);
  mailbox_addr =
      ((uint32_t)read_reg16(board_addr, ZZ9K_REG_SDK_MAILBOX_HI) << 16) |
      read_reg16(board_addr, ZZ9K_REG_SDK_MAILBOX_LO);
  sdk_status = read_reg16(board_addr, ZZ9K_REG_SDK_STATUS);
  sdk_diag_write = read_reg32_pair(board_addr, ZZ9K_REG_SDK_DIAG_WRITE);
  sdk_diag_data = read_reg32_pair(board_addr, ZZ9K_REG_SDK_DIAG_DATA);
  sdk_diag_z3addr = read_reg32_pair(board_addr, ZZ9K_REG_SDK_DIAG_Z3ADDR);

  printf("sdk magic:       0x%04x\n", (unsigned)sdk_magic);
  printf("sdk version:     0x%04x\n", (unsigned)sdk_version);
  printf("mailbox arm:     0x%08lx\n", (unsigned long)mailbox_addr);
  printf("sdk status:      0x%04x\n", (unsigned)sdk_status);
  printf("sdk diag write:  0x%08lx\n", (unsigned long)sdk_diag_write);
  printf("sdk diag data:   0x%08lx\n", (unsigned long)sdk_diag_data);
  printf("sdk diag z3addr: 0x%08lx\n", (unsigned long)sdk_diag_z3addr);
  if (read_hdl_diag &&
      z3_register_window_offset(zorro_version, board_size,
                                ZZ9K_REG_SDK_STATUS, &hdl_status_offset)) {
    printf("hdl status:      0x%04x\n",
           (unsigned)read_reg16(board_addr, hdl_status_offset));
    printf("hdl diag write:  0x%08lx\n",
           (unsigned long)read_reg32_pair(
               board_addr, ZZ9K_Z3_REGISTER_WINDOW_OFFSET +
                           ZZ9K_REG_SDK_DIAG_WRITE));
    printf("hdl diag data:   0x%08lx\n",
           (unsigned long)read_reg32_pair(
               board_addr, ZZ9K_Z3_REGISTER_WINDOW_OFFSET +
                           ZZ9K_REG_SDK_DIAG_DATA));
    printf("hdl diag z3addr: 0x%08lx\n",
           (unsigned long)read_reg32_pair(
               board_addr, ZZ9K_Z3_REGISTER_WINDOW_OFFSET +
                           ZZ9K_REG_SDK_DIAG_Z3ADDR));
  }
  printf("mailbox in win:  %s\n",
         mailbox_fits(board_size, mailbox_addr) ? "yes" : "no");

  CloseLibrary(expansion_base);
  return 0;
#else
  (void)argc;
  (void)argv;
  printf("zz9k-probe: AmigaOS build required\n");
  return 1;
#endif
}
