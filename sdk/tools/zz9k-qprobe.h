/*
 * Wire contract of the queue coherence probe (AAR cutover, slice 2).
 *
 * A throwaway diagnostic. It checks that a control block and a queue pair in
 * the host-visible heap, on the normal write-back shareable mapping, stay
 * coherent between the Amiga (Zorro III through the ACP) and firmware code on
 * either ARM core: the core-0 mailbox loop and the core-1 system thread. It is
 * removed when the AAR transport (slice 6) replaces it.
 *
 * Shared by the firmware (sdk_qprobe.c), the Amiga tool (zz9k-qprobe.c) and
 * the host tests. No AmigaOS or firmware calls.
 *
 * Memory: one queue pair is ZZ9K_QP_PAIR_BYTES of an SDK shared buffer
 * (zz9k_alloc_shared, host-visible heap), 4 KiB aligned:
 *
 *   0x0000  control block, 4 KiB; every word that changes hands sits alone
 *           at the start of its own 64-byte line
 *     0x000   header line: magic, version, queue, owner (firmware writes at
 *             ATTACH), mode (Amiga writes)
 *     0x040   down_head  firmware writes: down entries produced
 *     0x080   down_tail  Amiga writes: down entries consumed
 *     0x0c0   up_head    Amiga writes: up entries produced
 *     0x100   up_tail    firmware writes: up entries consumed
 *   0x1000  down ring: 64 entries of 64 bytes, firmware to Amiga
 *   0x2000  up ring: 64 entries of 64 bytes, Amiga to firmware
 *
 * Indices are free-running 32-bit counts. The entry produced at count n sits
 * in slot n % 64 and carries seq n + 1. A ring is full when head - tail == 64.
 * A producer writes the entry, then a barrier, then the head; a consumer reads
 * the head, then a barrier, then the entries, then writes the tail.
 *
 * Every word in shared memory is big-endian, so the 68k reads native longs.
 *
 * Entry words (16):
 *   w0      seq
 *   w1      kind
 *   w2      queue << 16 | origin (ZZ9K_QP_ORIGIN_*)
 *   w3      stamp: firmware global-timer low word (down), Amiga value (up)
 *   w4      argument: PONG echoes the PING id; COMPLETE names the highest
 *           down seq the Amiga has consumed; otherwise 0
 *   w5-w14  zz9k_qp_pattern(seq, queue, word)
 *   w15     ~seq, written last so a torn read shows
 *
 * Down kinds: STREAM (produced while mode is STREAM), PONG (answers a PING).
 * Up kinds: COMPLETE (one per consumed down entry), PING (id in w3).
 *
 * Mailbox: ZZ9K_OP_DIAG_QPROBE (0x0904). Request and reply payloads are
 * big-endian words.
 *
 *   ATTACH  [cmd, shared handle, queue, owner]
 *           -> [arm address, pair bytes, version]
 *           The buffer must belong to the calling client and hold
 *           ZZ9K_QP_PAIR_BYTES; the firmware clears the pair and writes the
 *           header. Freeing the buffer or releasing the client detaches.
 *   DETACH  [cmd, queue] -> the STATUS reply, taken after the owner has
 *           stopped touching the pair
 *   KICK    [cmd] -> [queues scanned, up entries consumed, handler ticks,
 *           timer hz]
 *           The doorbell: core 0 services every DOORBELL-owned queue once
 *           (consume up entries, answer PINGs) and then replies.
 *   STATUS  [cmd, queue] -> [owner, down produced, up checked, up errors,
 *           first error code, first error seq, ring full count, passes,
 *           timer hz, highest completed down seq, pongs sent]
 *
 * Owners: CORE0 is serviced by the core-0 main loop, CORE1 by the core-1
 * system thread, DOORBELL only by KICK. Only the owner writes down entries,
 * down_head and up_tail.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZ9K_QPROBE_H
#define ZZ9K_QPROBE_H

#include <stdint.h>

#define ZZ9K_QP_VERSION 1U
#define ZZ9K_QP_MAGIC 0x51505231U /* 'QPR1' */

#define ZZ9K_QP_MAX_QUEUES 32U
#define ZZ9K_QP_ENTRIES 64U
#define ZZ9K_QP_ENTRY_BYTES 64U
#define ZZ9K_QP_ENTRY_WORDS 16U
#define ZZ9K_QP_LINE_BYTES 64U

#define ZZ9K_QP_CONTROL_OFFSET 0x0000U
#define ZZ9K_QP_DOWN_RING_OFFSET 0x1000U
#define ZZ9K_QP_UP_RING_OFFSET 0x2000U
#define ZZ9K_QP_PAIR_BYTES 0x3000U
#define ZZ9K_QP_PAIR_ALIGN 0x1000U

/* Byte offsets inside the control block. */
#define ZZ9K_QP_HDR_MAGIC 0x000U
#define ZZ9K_QP_HDR_VERSION 0x004U
#define ZZ9K_QP_HDR_QUEUE 0x008U
#define ZZ9K_QP_HDR_OWNER 0x00cU
#define ZZ9K_QP_HDR_MODE 0x010U
#define ZZ9K_QP_DOWN_HEAD 0x040U
#define ZZ9K_QP_DOWN_TAIL 0x080U
#define ZZ9K_QP_UP_HEAD 0x0c0U
#define ZZ9K_QP_UP_TAIL 0x100U

#define ZZ9K_QP_MODE_IDLE 0U
#define ZZ9K_QP_MODE_STREAM 1U

#define ZZ9K_QP_OWNER_CORE0 0U
#define ZZ9K_QP_OWNER_CORE1 1U
#define ZZ9K_QP_OWNER_DOORBELL 2U
#define ZZ9K_QP_OWNER_COUNT 3U

/* w2 low half: who wrote the entry. */
#define ZZ9K_QP_ORIGIN_CORE0 0U
#define ZZ9K_QP_ORIGIN_CORE1 1U
#define ZZ9K_QP_ORIGIN_AMIGA 2U

#define ZZ9K_QP_KIND_STREAM 1U
#define ZZ9K_QP_KIND_PONG 2U
#define ZZ9K_QP_KIND_COMPLETE 3U
#define ZZ9K_QP_KIND_PING 4U

#define ZZ9K_QP_CMD_ATTACH 1U
#define ZZ9K_QP_CMD_DETACH 2U
#define ZZ9K_QP_CMD_KICK 3U
#define ZZ9K_QP_CMD_STATUS 4U

/* Reply word indices. */
#define ZZ9K_QP_ATTACH_ARM_ADDR 0U
#define ZZ9K_QP_ATTACH_PAIR_BYTES 1U
#define ZZ9K_QP_ATTACH_VERSION 2U
#define ZZ9K_QP_ATTACH_WORDS 3U

#define ZZ9K_QP_KICK_QUEUES 0U
#define ZZ9K_QP_KICK_CONSUMED 1U
#define ZZ9K_QP_KICK_TICKS 2U
#define ZZ9K_QP_KICK_TIMER_HZ 3U
#define ZZ9K_QP_KICK_WORDS 4U

#define ZZ9K_QP_STATUS_OWNER 0U
#define ZZ9K_QP_STATUS_DOWN_PRODUCED 1U
#define ZZ9K_QP_STATUS_UP_CHECKED 2U
#define ZZ9K_QP_STATUS_UP_ERRORS 3U
#define ZZ9K_QP_STATUS_FIRST_ERROR 4U
#define ZZ9K_QP_STATUS_FIRST_ERROR_SEQ 5U
#define ZZ9K_QP_STATUS_RING_FULL 6U
#define ZZ9K_QP_STATUS_PASSES 7U
#define ZZ9K_QP_STATUS_TIMER_HZ 8U
#define ZZ9K_QP_STATUS_COMPLETED 9U
#define ZZ9K_QP_STATUS_PONGS 10U
#define ZZ9K_QP_STATUS_WORDS 11U

/* Entry check results, also the first-error code in STATUS. */
#define ZZ9K_QP_OK 0U
#define ZZ9K_QP_ERR_STALE 1U   /* w0 is not the expected seq: the head was
                                  seen before the entry, or an entry was
                                  skipped */
#define ZZ9K_QP_ERR_TORN 2U    /* w15 is not ~w0 */
#define ZZ9K_QP_ERR_PATTERN 3U /* a pattern word is wrong */
#define ZZ9K_QP_ERR_QUEUE 4U   /* w2 names another queue or origin */
#define ZZ9K_QP_ERR_KIND 5U    /* a kind this direction never carries */
#define ZZ9K_QP_ERR_ORDER 6U   /* COMPLETE out of order or ahead of what
                                  was produced; PONG for no PING */
#define ZZ9K_QP_ERR_INDEX 7U   /* head - tail beyond the ring size */
#define ZZ9K_QP_ERR_COUNT 8U

static inline const char *zz9k_qp_error_name(uint32_t code)
{
  switch (code) {
  case ZZ9K_QP_OK: return "ok";
  case ZZ9K_QP_ERR_STALE: return "stale";
  case ZZ9K_QP_ERR_TORN: return "torn";
  case ZZ9K_QP_ERR_PATTERN: return "pattern";
  case ZZ9K_QP_ERR_QUEUE: return "queue";
  case ZZ9K_QP_ERR_KIND: return "kind";
  case ZZ9K_QP_ERR_ORDER: return "order";
  case ZZ9K_QP_ERR_INDEX: return "index";
  default: return "?";
  }
}

/* Native value <-> big-endian word in shared memory. */
static inline uint32_t zz9k_qp_swap(uint32_t value)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  return value;
#else
  return (value >> 24) | ((value >> 8) & 0x0000ff00U) |
         ((value << 8) & 0x00ff0000U) | (value << 24);
#endif
}

static inline uint32_t zz9k_qp_get(const volatile void *base, uint32_t offset)
{
  return zz9k_qp_swap(*(const volatile uint32_t *)
                      ((const volatile uint8_t *)base + offset));
}

static inline void zz9k_qp_set(volatile void *base, uint32_t offset,
                               uint32_t value)
{
  *(volatile uint32_t *)((volatile uint8_t *)base + offset) =
      zz9k_qp_swap(value);
}

static inline volatile uint32_t *zz9k_qp_slot(volatile void *pair,
                                              uint32_t ring_offset,
                                              uint32_t count)
{
  return (volatile uint32_t *)((volatile uint8_t *)pair + ring_offset +
                               (count % ZZ9K_QP_ENTRIES) *
                                   ZZ9K_QP_ENTRY_BYTES);
}

/* Pattern word: the murmur3 finalizer of seq, queue and word index. */
static inline uint32_t zz9k_qp_pattern(uint32_t seq, uint32_t queue,
                                       uint32_t word)
{
  uint32_t h = seq * 16U + word + (queue << 26);

  h ^= h >> 16;
  h *= 0x85ebca6bU;
  h ^= h >> 13;
  h *= 0xc2b2ae35U;
  h ^= h >> 16;
  return h;
}

/* Writes an entry: w0 first, w15 last. The caller publishes the head after a
 * barrier. */
static inline void zz9k_qp_entry_write(volatile uint32_t *entry, uint32_t seq,
                                       uint32_t kind, uint32_t queue,
                                       uint32_t origin, uint32_t stamp,
                                       uint32_t arg)
{
  uint32_t word;

  entry[0] = zz9k_qp_swap(seq);
  entry[1] = zz9k_qp_swap(kind);
  entry[2] = zz9k_qp_swap((queue << 16) | origin);
  entry[3] = zz9k_qp_swap(stamp);
  entry[4] = zz9k_qp_swap(arg);
  for (word = 5U; word < 15U; word++)
    entry[word] = zz9k_qp_swap(zz9k_qp_pattern(seq, queue, word));
  entry[15] = zz9k_qp_swap(~seq);
}

typedef struct ZZ9KQpEntry {
  uint32_t seq;
  uint32_t kind;
  uint32_t origin;
  uint32_t stamp;
  uint32_t arg;
} ZZ9KQpEntry;

/* Checks the entry the consumer expects to carry `expected_seq` and decodes
 * it into `out`. `origin_mask` has bit (1 << origin) set for each origin
 * allowed in this direction. Kind checks are left to the caller, who knows
 * the direction. */
static inline uint32_t zz9k_qp_entry_check(const volatile uint32_t *entry,
                                           uint32_t expected_seq,
                                           uint32_t queue,
                                           uint32_t origin_mask,
                                           ZZ9KQpEntry *out)
{
  uint32_t words[ZZ9K_QP_ENTRY_WORDS];
  uint32_t word;

  for (word = 0U; word < ZZ9K_QP_ENTRY_WORDS; word++)
    words[word] = zz9k_qp_swap(entry[word]);
  if (out) {
    out->seq = words[0];
    out->kind = words[1];
    out->origin = words[2] & 0xffffU;
    out->stamp = words[3];
    out->arg = words[4];
  }
  if (words[0] != expected_seq)
    return ZZ9K_QP_ERR_STALE;
  if (words[15] != ~words[0])
    return ZZ9K_QP_ERR_TORN;
  for (word = 5U; word < 15U; word++) {
    if (words[word] != zz9k_qp_pattern(words[0], queue, word))
      return ZZ9K_QP_ERR_PATTERN;
  }
  if ((words[2] >> 16) != queue || (words[2] & 0xffffU) > 31U ||
      ((1U << (words[2] & 0xffffU)) & origin_mask) == 0U)
    return ZZ9K_QP_ERR_QUEUE;
  return ZZ9K_QP_OK;
}

/* Entries a consumer may take, or -1 if head - tail exceeds the ring. */
static inline int32_t zz9k_qp_available(uint32_t head, uint32_t tail)
{
  uint32_t used = head - tail;

  return used > ZZ9K_QP_ENTRIES ? -1 : (int32_t)used;
}

#endif
