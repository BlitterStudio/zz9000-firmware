/*
 * Single-step SDK v2 mailbox diagnostic.
 *
 * This tool deliberately avoids zz9k_host.c so it can print before and after
 * each bus operation in the first QUERY_CAPS request path.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/abi.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__amigaos__) || defined(__amiga__) || defined(__AMIGA__) || \
    defined(__VBCC__)
#define ZZ9K_STEP_AMIGA 1
#include <exec/types.h>
#include <libraries/configvars.h>
#include <libraries/expansion.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#endif

#if ZZ9K_STEP_AMIGA
static void say(const char *message)
{
  printf("%s\n", message);
  fflush(stdout);
}

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

static void write_reg16(uint32_t board_addr, uint32_t offset, uint16_t value)
{
  *reg16(board_addr, offset) = value;
}

static int board_contains_offset(uint32_t board_size, uint32_t offset,
                                 uint32_t size)
{
  if (size == 0 || offset > (UINT32_MAX - (size - 1U))) {
    return 0;
  }
  if (board_size == 0) {
    return 1;
  }

  return offset < board_size && size <= board_size - offset;
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
  if (!board_contains_offset(board_size, offset, sizeof(uint16_t))) {
    return 0;
  }

  *board_offset = offset;
  return 1;
}

static void spin_wait(void)
{
  volatile uint32_t i;

  for (i = 0; i < 500000UL; i++) {
  }
}

static int board_range_fits(uint32_t board_size, uint32_t board_offset,
                            uint32_t length)
{
  if (length == 0 || board_offset > (UINT32_MAX - (length - 1U))) {
    return 0;
  }
  if (board_size == 0) {
    return 1;
  }

  return board_offset < board_size && length <= (board_size - board_offset);
}

static int map_arm_to_board_offset(uint32_t board_size, uint32_t arm_addr,
                                   uint32_t length,
                                   uint32_t *board_offset)
{
  uint32_t last_addr;
  uint32_t offset;

  if (!board_offset || length == 0 ||
      arm_addr > (UINT32_MAX - (length - 1U))) {
    return 0;
  }

  last_addr = arm_addr + length - 1U;
  if (arm_addr >= ZZ9K_MAPPED_IO_ARM_START &&
      last_addr < (ZZ9K_MAPPED_IO_ARM_START + ZZ9K_MAPPED_IO_WINDOW_SIZE)) {
    offset =
        ZZ9K_MAPPED_IO_BOARD_OFFSET + (arm_addr - ZZ9K_MAPPED_IO_ARM_START);
    if (!board_range_fits(board_size, offset, length)) {
      return 0;
    }
    *board_offset = offset;
    return 1;
  }

  if (arm_addr < ZZ9K_ARM_MEMORY_START ||
      last_addr > ZZ9K_ARM_MEMORY_VISIBLE_END) {
    return 0;
  }

  offset = ZZ9K_AMIGA_MEMORY_OFFSET + (arm_addr - ZZ9K_ARM_MEMORY_START);
  if (!board_range_fits(board_size, offset, length)) {
    return 0;
  }

  *board_offset = offset;
  return 1;
}

static void put16(volatile uint8_t *p, uint16_t value)
{
  *(volatile uint16_t *)p = value;
}

static void put32(volatile uint8_t *p, uint32_t value)
{
  *(volatile uint16_t *)p = (uint16_t)((value >> 16) & 0xffffU);
  *(volatile uint16_t *)(p + 2) = (uint16_t)(value & 0xffffU);
}

static uint32_t next_index(uint32_t index, uint32_t entries)
{
  index++;
  if (index >= entries) {
    index = 0;
  }
  return index;
}

static int stop_after(int step, int requested_stop)
{
  if (requested_stop == step) {
    printf("stopping after step %d\n", step);
    fflush(stdout);
    return 1;
  }
  return 0;
}
#endif

int main(int argc, char **argv)
{
#if ZZ9K_STEP_AMIGA
  struct Library *expansion_base;
  struct ConfigDev *config_dev;
  uint32_t board_addr;
  uint32_t board_size;
  uint32_t zorro_version;
  uint32_t mailbox_addr;
  uint32_t mailbox_offset;
  uint32_t doorbell_offset;
  uint32_t hdl_status_offset;
  volatile ZZ9KMailboxDescriptor *mailbox;
  volatile ZZ9KMailboxWireEntry *request_ring;
  volatile ZZ9KMailboxWireEntry *completion_ring;
  uint32_t request_offset;
  uint32_t completion_offset;
  uint32_t capability_bits;
  uint32_t request_entries;
  uint32_t completion_entries;
  uint32_t request_head;
  uint32_t request_tail;
  uint32_t completion_head;
  uint32_t completion_tail;
  uint32_t next_tail;
  int requested_stop = 0;
  int read_hdl_diag = 0;
  int arg_index;

  for (arg_index = 1; arg_index < argc; arg_index++) {
    if (strcmp(argv[arg_index], "--hdl") == 0) {
      read_hdl_diag = 1;
    } else {
      requested_stop = (int)strtoul(argv[arg_index], 0, 0);
    }
  }

  say("step 1: opening expansion.library");
  expansion_base = OpenLibrary((CONST_STRPTR)"expansion.library", 0);
  if (!expansion_base) {
    printf("open expansion.library failed\n");
    return 1;
  }
  if (stop_after(1, requested_stop)) return 0;

  say("step 2: finding ZZ9000");
  config_dev = FindConfigDev(0, ZZ9K_MNT_MANUFACTURER, ZZ9K_PRODUCT_Z3);
  zorro_version = 3;
  if (!config_dev) {
    config_dev = FindConfigDev(0, ZZ9K_MNT_MANUFACTURER, ZZ9K_PRODUCT_Z2);
    zorro_version = 2;
  }
  if (!config_dev) {
    CloseLibrary(expansion_base);
    printf("ZZ9000 not found\n");
    return 1;
  }
  board_addr = (uint32_t)config_dev->cd_BoardAddr;
  board_size = config_dev->cd_BoardSize;
  printf("board=0x%08lx size=0x%08lx zorro=%lu\n",
         (unsigned long)board_addr, (unsigned long)board_size,
         (unsigned long)zorro_version);
  fflush(stdout);
  CloseLibrary(expansion_base);
  if (stop_after(2, requested_stop)) return 0;

  say("step 3: reading SDK registers");
  printf("magic=0x%04x version=0x%04x status=0x%04x\n",
         (unsigned)read_reg16(board_addr, ZZ9K_REG_SDK_MAGIC),
         (unsigned)read_reg16(board_addr, ZZ9K_REG_SDK_VERSION),
         (unsigned)read_reg16(board_addr, ZZ9K_REG_SDK_STATUS));
  mailbox_addr =
      ((uint32_t)read_reg16(board_addr, ZZ9K_REG_SDK_MAILBOX_HI) << 16) |
      read_reg16(board_addr, ZZ9K_REG_SDK_MAILBOX_LO);
  printf("mailbox arm=0x%08lx\n", (unsigned long)mailbox_addr);
  fflush(stdout);
  if (stop_after(3, requested_stop)) return 0;

  say("step 4: mapping mailbox");
  if (!map_arm_to_board_offset(board_size, mailbox_addr,
                               ZZ9K_MAILBOX_DESCRIPTOR_SIZE,
                               &mailbox_offset)) {
    printf("mailbox not mappable\n");
    return 1;
  }
  printf("mailbox board offset=0x%08lx\n", (unsigned long)mailbox_offset);
  fflush(stdout);
  mailbox = (volatile ZZ9KMailboxDescriptor *)((uintptr_t)board_addr +
                                               mailbox_offset);
  if (stop_after(4, requested_stop)) return 0;

  say("step 5: reading descriptor");
  printf("desc magic=0x%08lx abi=%u.%u\n",
         (unsigned long)zz9k_get_be32(mailbox->magic),
         (unsigned)zz9k_get_be16(mailbox->abi_major),
         (unsigned)zz9k_get_be16(mailbox->abi_minor));
  request_offset = zz9k_get_be32(mailbox->request_ring_offset);
  completion_offset = zz9k_get_be32(mailbox->completion_ring_offset);
  capability_bits = zz9k_get_be32(mailbox->capability_bits);
  request_entries = zz9k_get_be32(mailbox->request_ring_entries);
  completion_entries = zz9k_get_be32(mailbox->completion_ring_entries);
  printf("caps=0x%08lx req off=0x%08lx entries=%lu comp off=0x%08lx entries=%lu\n",
         (unsigned long)capability_bits,
         (unsigned long)request_offset, (unsigned long)request_entries,
         (unsigned long)completion_offset, (unsigned long)completion_entries);
  fflush(stdout);
  request_ring = (volatile ZZ9KMailboxWireEntry *)((volatile uint8_t *)mailbox +
                                                   request_offset);
  completion_ring =
      (volatile ZZ9KMailboxWireEntry *)((volatile uint8_t *)mailbox +
                                        completion_offset);
  if (stop_after(5, requested_stop)) return 0;

  say("step 6: reading ring indexes");
  request_head = zz9k_get_be32(mailbox->request_head);
  request_tail = zz9k_get_be32(mailbox->request_tail);
  completion_head = zz9k_get_be32(mailbox->completion_head);
  completion_tail = zz9k_get_be32(mailbox->completion_tail);
  printf("req h=%lu t=%lu comp h=%lu t=%lu\n",
         (unsigned long)request_head, (unsigned long)request_tail,
         (unsigned long)completion_head, (unsigned long)completion_tail);
  fflush(stdout);
  if (request_head >= request_entries || request_tail >= request_entries ||
      completion_head >= completion_entries ||
      completion_tail >= completion_entries) {
    printf("bad ring index\n");
    return 1;
  }
  next_tail = next_index(request_tail, request_entries);
  if (next_tail == request_head) {
    printf("request ring full\n");
    return 1;
  }
  if (stop_after(6, requested_stop)) return 0;

  say("step 7: writing request_id");
  put32(request_ring[request_tail].request_id, 1);
  say("step 7 ok");
  if (stop_after(7, requested_stop)) return 0;

  say("step 8: writing opcode/status/flags/payload_len");
  put16(request_ring[request_tail].opcode, ZZ9K_OP_QUERY_CAPS);
  put16(request_ring[request_tail].status, ZZ9K_STATUS_QUEUED);
  put16(request_ring[request_tail].flags, ZZ9K_ENTRY_INLINE_PAYLOAD);
  put16(request_ring[request_tail].payload_len, 0);
  say("step 8 ok");
  if (stop_after(8, requested_stop)) return 0;

  say("step 9: writing user_cookie");
  put32(request_ring[request_tail].user_cookie, 0);
  say("step 9 ok");
  if (stop_after(9, requested_stop)) return 0;

  say("step 10: publishing request_tail");
  put32(mailbox->request_tail, next_tail);
  say("step 10 ok");
  if (stop_after(10, requested_stop)) return 0;

  if (capability_bits & ZZ9K_CAP_DOORBELL) {
    say("step 11: writing advertised HDL doorbell");
    if (!z3_register_window_offset(zorro_version, board_size,
                                   ZZ9K_REG_SDK_DOORBELL,
                                   &doorbell_offset)) {
      printf("no safe Z3 register aperture for advertised doorbell\n");
      return 3;
    }
    printf("doorbell board offset=0x%08lx\n",
           (unsigned long)doorbell_offset);
    fflush(stdout);
    write_reg16(board_addr, doorbell_offset, 1);
  } else {
    say("step 11: skipping doorbell write");
  }
  say("step 11 ok");
  if (stop_after(11, requested_stop)) return 0;

  say("step 12: idle spin");
  spin_wait();
  say("step 12 ok");
  if (stop_after(12, requested_stop)) return 0;

  say("step 13: reading SDK status register");
  printf("status=0x%04x\n",
         (unsigned)read_reg16(board_addr, ZZ9K_REG_SDK_STATUS));
  printf("diag write=0x%08lx data=0x%08lx z3addr=0x%08lx\n",
         (unsigned long)read_reg32_pair(board_addr, ZZ9K_REG_SDK_DIAG_WRITE),
         (unsigned long)read_reg32_pair(board_addr, ZZ9K_REG_SDK_DIAG_DATA),
         (unsigned long)read_reg32_pair(board_addr, ZZ9K_REG_SDK_DIAG_Z3ADDR));
  if (read_hdl_diag &&
      z3_register_window_offset(zorro_version, board_size,
                                ZZ9K_REG_SDK_STATUS, &hdl_status_offset)) {
    printf("hdl status=0x%04x\n",
           (unsigned)read_reg16(board_addr, hdl_status_offset));
    printf("hdl diag write=0x%08lx data=0x%08lx z3addr=0x%08lx\n",
           (unsigned long)read_reg32_pair(
               board_addr, ZZ9K_Z3_REGISTER_WINDOW_OFFSET +
                           ZZ9K_REG_SDK_DIAG_WRITE),
           (unsigned long)read_reg32_pair(
               board_addr, ZZ9K_Z3_REGISTER_WINDOW_OFFSET +
                           ZZ9K_REG_SDK_DIAG_DATA),
           (unsigned long)read_reg32_pair(
               board_addr, ZZ9K_Z3_REGISTER_WINDOW_OFFSET +
                           ZZ9K_REG_SDK_DIAG_Z3ADDR));
  }
  fflush(stdout);
  if (stop_after(13, requested_stop)) return 0;

  say("step 14: reading completion indexes");
  completion_head = zz9k_get_be32(mailbox->completion_head);
  completion_tail = zz9k_get_be32(mailbox->completion_tail);
  printf("comp h=%lu t=%lu\n",
         (unsigned long)completion_head, (unsigned long)completion_tail);
  fflush(stdout);
  if (stop_after(14, requested_stop)) return 0;

  if (completion_head == completion_tail) {
    printf("no completion yet\n");
    return 2;
  }

  say("step 15: reading completion entry");
  printf("reply id=%lu opcode=0x%04x status=%u payload=%u\n",
         (unsigned long)zz9k_get_be32(completion_ring[completion_head].request_id),
         (unsigned)zz9k_get_be16(completion_ring[completion_head].opcode),
         (unsigned)zz9k_get_be16(completion_ring[completion_head].status),
         (unsigned)zz9k_get_be16(completion_ring[completion_head].payload_len));
  fflush(stdout);
  if (stop_after(15, requested_stop)) return 0;

  say("step 16: consuming completion");
  put32(mailbox->completion_head,
        next_index(completion_head, completion_entries));
  say("step 16 ok");
  if (stop_after(16, requested_stop)) return 0;

  return 0;
#else
  (void)argc;
  (void)argv;
  printf("zz9k-step: AmigaOS build required\n");
  return 1;
#endif
}
