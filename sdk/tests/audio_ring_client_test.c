/*
 * Direct-ring client host tests (plan U4): the proof client's
 * lifecycle against a mock mailbox with real mapped ring memory --
 * acquire/map/write/publish/credit/release -- plus the session
 * helpers' own contracts: wrapped writes, cursor publication after
 * visibility, consumed-credit pacing with an unstable firmware
 * seqlock, independent contexts, Zorro II second-client refusal,
 * grant rejection, and Ctrl-C release.
 *
 * The tool is compiled in directly (single TU, the audio_meter_test
 * discipline) with its main() excluded, so every helper is reachable
 * without a board. The mock firmware consumes exactly one published
 * complete period per tick (the tick hook stands in for the passage
 * of time), so pacing that depended on STATE_GET or mailbox submits
 * would deadlock against the bounded waits and fail the test.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define ZZ9K_FABRICLEASE_NO_MAIN 1
#include "../tools/zz9k-fabriclease.c"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* Test seam exported (undeclared) by host/src/zz9k_host.c. */
void zz9k_set_idle_hook_for_test(void (*hook)(void));

static int g_failures;

#define CHECK(ok, name)                                                 \
  do {                                                                  \
    if (!(ok)) {                                                        \
      g_failures++;                                                     \
      printf("FAILED: %s (rc=%d, %s:%d)\n", (name), r, __FILE__,        \
             __LINE__);                                                 \
    }                                                                   \
  } while (0)


/* A board window at a fixed low address so the granted ring and
 * control mappings (board_addr + offset) are real, dereferenceable
 * memory: the sessions write their tone there and the mock firmware
 * reads it back through the same shared lines. */
#define MOCK_BOARD_ADDR 0x20000000UL
#define MOCK_RING_ENTRIES 8U
#define MOCK_GENERATION1 5U
#define MOCK_GENERATION2 6U

static void *mock_window_alloc(uint32_t size)
{
#if defined(_WIN32)
  return VirtualAlloc((LPVOID)MOCK_BOARD_ADDR, (SIZE_T)size,
                      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
  void *p = mmap((void *)MOCK_BOARD_ADDR, (size_t)size,
                 PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  return p == MAP_FAILED ? NULL : p;
#endif
}

static void mock_window_free(void *p, uint32_t size)
{
  if (!p) {
    return;
  }
#if defined(_WIN32)
  (void)size;
  VirtualFree(p, 0, MEM_RELEASE);
#else
  munmap(p, (size_t)size);
#endif
}

struct TestMailbox {
  ZZ9KMailboxDescriptor descriptor;
  ZZ9KMailboxWireEntry request_ring[MOCK_RING_ENTRIES];
  ZZ9KMailboxWireEntry completion_ring[MOCK_RING_ENTRIES];
};

/* One mock firmware slot: the grant the mock hands out plus the
 * firmware-side playback state, driven one period per tick. */
struct MockSlot {
  int granted;
  int released;
  uint32_t generation;
  uint32_t ring_offset;
  uint32_t ring_capacity;
  uint32_t control_offset;
  uint64_t published;      /* last stable write cursor observed */
  uint64_t consumed;       /* firmware-consumed bytes (credit) */
  uint32_t last_heartbeat;
  uint32_t ticks;           /* playback ticks since grant */
  uint32_t torn_ticks;      /* leave the firmware line odd (torn) */
  uint32_t revoke_at;      /* tick count that kills the generation */
  uint32_t period_bytes;   /* consumption quantum: 3840 bypass, or
                            * rate/50*4 for a source-rate lease */
};


/* Scripted slot-1 behavior applied by run_client_session after the
 * mock reset: torn firmware-line ticks and mid-run revocation. */
static uint32_t g_script_torn_ticks;
static uint32_t g_script_revoke_after;

static struct {
  struct TestMailbox mailbox;
  ZZ9KBoard board;
  uint32_t served_tail;
  struct MockSlot slots[ZZ9K_AUDIO_RING_SLOT_MAX + 1U];
  int served_requests;
  int acquire_calls, release_calls, state_calls, layout_calls;
  int refused_acquires;
  uint32_t acquire_slot[ZZ9K_AUDIO_RING_SLOT_MAX + 1U];
  uint32_t release_slot, release_generation;
  uint32_t slot_count;
  uint32_t z2_grant_ring_offset;  /* scriptable grant geometry */
  int override_grant;
  uint32_t override_contract;
  uint32_t override_rate;
} g_mock;

static struct MockSlot *mock_slot(uint32_t slot)
{
  return &g_mock.slots[slot % (ZZ9K_AUDIO_RING_SLOT_MAX + 1U)];
}

static volatile uint8_t *mock_board_ptr(uint32_t offset)
{
  return (volatile uint8_t *)(void *)((uintptr_t)g_mock.board.board_addr +
                                      (uintptr_t)offset);
}

static void mock_reset(uint32_t caps, uint16_t zorro_version,
                       uint32_t board_size)
{
  struct TestMailbox *m;

  memset(&g_mock, 0, sizeof(g_mock));
  m = &g_mock.mailbox;
  zz9k_put_be32(m->descriptor.magic, ZZ9K_ABI_MAGIC);
  zz9k_put_be16(m->descriptor.abi_major, ZZ9K_ABI_VERSION_MAJOR);
  zz9k_put_be16(m->descriptor.abi_minor, ZZ9K_ABI_VERSION_MINOR);
  zz9k_put_be32(m->descriptor.descriptor_size,
                (uint32_t)sizeof(m->descriptor));
  zz9k_put_be32(m->descriptor.request_ring_offset,
                (uint32_t)offsetof(struct TestMailbox, request_ring));
  zz9k_put_be32(m->descriptor.request_ring_entries, MOCK_RING_ENTRIES);
  zz9k_put_be32(m->descriptor.completion_ring_offset,
                (uint32_t)offsetof(struct TestMailbox, completion_ring));
  zz9k_put_be32(m->descriptor.completion_ring_entries, MOCK_RING_ENTRIES);
  zz9k_put_be32(m->descriptor.capability_bits, ZZ9K_CAP_MAILBOX | caps);
  g_mock.board.zorro_version = zorro_version;
  g_mock.board.board_addr = MOCK_BOARD_ADDR;
  g_mock.board.board_size = board_size;
  g_mock.board.product =
      zorro_version == 2U ? ZZ9K_PRODUCT_Z2 : ZZ9K_PRODUCT_Z3;
  g_mock.slot_count = zorro_version == 2U ? 1U : 2U;
}

static void mock_grant_slot(uint32_t slot, uint32_t generation,
                            uint32_t ring_offset, uint32_t ring_capacity,
                            uint32_t control_offset)
{
  struct MockSlot *s = mock_slot(slot);

  s->granted = 1;
  s->period_bytes = ZZ9K_AUDIO_RING_PERIOD_BYTES;
  s->generation = generation;
  s->ring_offset = ring_offset;
  s->ring_capacity = ring_capacity;
  s->control_offset = control_offset;
  /* Firmware initializes the control block before granting (AE6):
   * even sequence, lease generation, zero consumed credit. */
  memset((void *)mock_board_ptr(control_offset), 0,
         ZZ9K_AUDIO_RING_CONTROL_SIZE);
  zz9k_audio_ring_firmware_publish(
      (volatile ZZ9KAudioRingFirmwareLine *)(void *)mock_board_ptr(
          control_offset + ZZ9K_AUDIO_RING_CONTROL_LINE_SIZE),
      generation, 0U, ZZ9K_AUDIO_RING_STATUS_OK);
}

/* Stage the completion for the request at request_ring[head]; the
 * returned entry's payload is the caller's to fill. */
static ZZ9KMailboxWireEntry *mock_complete(const ZZ9KMailboxWireEntry *req,
                                           uint16_t status,
                                           uint16_t payload_len)
{
  struct TestMailbox *m = &g_mock.mailbox;
  ZZ9KMailboxWireEntry *reply;
  uint32_t tail = zz9k_get_be32(m->descriptor.completion_tail);

  reply = &m->completion_ring[tail % MOCK_RING_ENTRIES];
  memset((void *)reply->payload, 0, sizeof(reply->payload));
  zz9k_put_be32(reply->request_id, zz9k_get_be32(req->request_id));
  zz9k_put_be16(reply->opcode, zz9k_get_be16(req->opcode));
  zz9k_put_be16(reply->status, status);
  zz9k_put_be16(reply->payload_len, payload_len);
  zz9k_put_be32(reply->user_cookie, zz9k_get_be32(req->user_cookie));
  zz9k_put_be32(m->descriptor.completion_tail,
                (tail + 1U) % MOCK_RING_ENTRIES);
  zz9k_put_be32(m->descriptor.request_head,
                (zz9k_get_be32(m->descriptor.request_head) + 1U) %
                    MOCK_RING_ENTRIES);
  return reply;
}

static void mock_word(ZZ9KMailboxWireEntry *reply, uint32_t index,
                      uint32_t value)
{
  zz9k_put_be32(&reply->payload[index * 4U], value);
}

static uint32_t mock_req_word(const ZZ9KMailboxWireEntry *req,
                              uint32_t index)
{
  return zz9k_get_be32(&req->payload[index * 4U]);
}

/* The generation-2 Zorro II layout (the direct-ring carve): each
 * host-window heap is shrunk by the fixed 48-KiB direct region, so
 * the unreported gap between the heap's end and the audio region is
 * where the mock grants its ring (pinned: control 0x3E4000, ring
 * 0x3E4080 on the 4 MiB card). */
static void mock_put_z2_layout(ZZ9KMailboxWireEntry *reply,
                               uint32_t aperture_size)
{
  uint32_t flags = ZZ9K_APERTURE_FLAG_VALID | ZZ9K_APERTURE_FLAG_HOST_WINDOW |
                   ZZ9K_APERTURE_FLAG_PIP;
  uint32_t host_size = aperture_size == 0x00400000UL ? 0x00004000UL
                                                     : 0x00014000UL;

  mock_word(reply, 0, ZZ9K_APERTURE_PROFILE(
                          ZZ9K_APERTURE_LAYOUT_GENERATION_2, flags));
  mock_word(reply, 1, aperture_size);
  mock_word(reply, 2, 0x00010000UL); /* framebuffer base */
  mock_word(reply, 3, aperture_size == 0x00400000UL ? 0x00388000UL
                                                    : 0x00770000UL);
  mock_word(reply, 4, aperture_size == 0x00400000UL ? 0x00398000UL
                                                    : 0x00780000UL);
  mock_word(reply, 5, aperture_size == 0x00400000UL ? 0x00038000UL
                                                    : 0x00040000UL);
  mock_word(reply, 6, aperture_size == 0x00400000UL ? 0x003d0000UL
                                                    : 0x007c0000UL);
  mock_word(reply, 7, 0x00010000UL); /* template size */
  mock_word(reply, 8, aperture_size == 0x00400000UL ? 0x003e0000UL
                                                    : 0x007d0000UL);
  mock_word(reply, 9, host_size);    /* carved heap */
  mock_word(reply, 10, aperture_size == 0x00400000UL ? 0x003f0000UL
                                                     : 0x007f0000UL);
  mock_word(reply, 11, 0x00010000UL); /* audio size */
}

/* The reactive responder: installed as the test idle hook, it serves
 * the just-enqueued request during the first poll backoff of its
 * zz9k_call, exactly like the firmware dispatcher would. */
static void mock_respond(void)
{
  struct TestMailbox *m = &g_mock.mailbox;
  const ZZ9KMailboxWireEntry *req;
  uint32_t head = zz9k_get_be32(m->descriptor.request_head);
  uint32_t tail = zz9k_get_be32(m->descriptor.request_tail);
  uint16_t opcode;
  ZZ9KMailboxWireEntry *reply;

  if (head == tail || g_mock.served_tail == tail) {
    return;
  }
  g_mock.served_tail = tail;
  g_mock.served_requests++;
  req = &m->request_ring[head];
  opcode = zz9k_get_be16(req->opcode);

  switch (opcode) {
  case ZZ9K_OP_QUERY_CAPS:
    reply = mock_complete(req, ZZ9K_STATUS_OK, 40U);
    mock_word(reply, 0, ZZ9K_ABI_MAGIC);
    mock_word(reply, 1, ((uint32_t)ZZ9K_ABI_VERSION_MAJOR << 16) |
                            ZZ9K_ABI_VERSION_MINOR);
    mock_word(reply, 2,
              zz9k_get_be32(m->descriptor.capability_bits));
    mock_word(reply, 3, 48U);
    mock_word(reply, 4, 32U);
    mock_word(reply, 7, MOCK_RING_ENTRIES);
    mock_word(reply, 8, MOCK_RING_ENTRIES);
    break;
  case ZZ9K_OP_QUERY_APERTURE_LAYOUT:
    g_mock.layout_calls++;
    reply = mock_complete(req, ZZ9K_STATUS_OK,
                          sizeof(ZZ9KApertureLayoutPayload));
    mock_put_z2_layout(reply, g_mock.board.board_size);
    break;
  case ZZ9K_OP_AUDIO_RING_ACQUIRE: {
    uint32_t slot = mock_req_word(req, 0);
    uint32_t flags = mock_req_word(req, 3);
    uint32_t rate = mock_req_word(req, 4);
    uint32_t contract = ZZ9K_AUDIO_RING_CONTRACT_48K_STEREO_S16LE;
    uint32_t granted_rate = 48000U;
    struct MockSlot *s = mock_slot(slot);

    g_mock.acquire_calls++;
    g_mock.acquire_slot[slot] = slot;
    /* Bus admission (R9/R10): a slot beyond the advertised count is
     * refused deterministically, exactly like the Zorro II second
     * acquisition. A rate-bearing acquire mirrors firmware: the
     * contract flips to source-rate and the validated rate is
     * echoed; an off-vocabulary rate is refused. */
    if (slot > g_mock.slot_count ||
        ((flags & ZZ9K_AUDIO_RING_ACQUIRE_FLAG_SOURCE_RATE) != 0U &&
         !zz9k_audio_ring_rate_known(rate))) {
      g_mock.refused_acquires++;
      (void)mock_complete(req, ZZ9K_STATUS_BAD_REQUEST, 0U);
      break;
    }
    if ((flags & ZZ9K_AUDIO_RING_ACQUIRE_FLAG_SOURCE_RATE) != 0U) {
      contract = ZZ9K_AUDIO_RING_CONTRACT_SOURCE_RATE_STEREO_S16LE;
      granted_rate = rate;
      s->period_bytes = (rate / 50U) * 4U;
    }
    if (g_mock.override_grant) {
      contract = g_mock.override_contract;
      granted_rate = g_mock.override_rate;
    }
    reply = mock_complete(req, ZZ9K_STATUS_OK,
                          sizeof(ZZ9KAudioRingAcquireResultPayload));
    mock_word(reply, 0, slot);
    mock_word(reply, 1, s->generation);
    mock_word(reply, 2, s->ring_offset);
    mock_word(reply, 3, s->ring_capacity);
    mock_word(reply, 4, s->control_offset);
    mock_word(reply, 5, ZZ9K_AUDIO_RING_PERIOD_BYTES);
    mock_word(reply, 6, ZZ9K_AUDIO_RING_PERIOD_US);
    mock_word(reply, 7, contract);
    mock_word(reply, 8, 128U); /* gain applied */
    mock_word(reply, 9, g_mock.slot_count);
    mock_word(reply, 10, g_mock.board.zorro_version == 2U
                              ? ZZ9K_AUDIO_RING_RESULT_BUS_ZORRO2
                              : 0U);
    mock_word(reply, 11, granted_rate);
    break;
  }
  case ZZ9K_OP_AUDIO_RING_RELEASE:
    g_mock.release_calls++;
    g_mock.release_slot = mock_req_word(req, 0);
    g_mock.release_generation = mock_req_word(req, 1);
    mock_slot(mock_req_word(req, 0))->released = 1;
    (void)mock_complete(req, ZZ9K_STATUS_OK, 0U);
    break;
  case ZZ9K_OP_AUDIO_FABRIC_STATE_GET: {
    struct MockSlot *s = mock_slot(mock_req_word(req, 0));

    g_mock.state_calls++;
    reply = mock_complete(req, ZZ9K_STATUS_OK,
                          sizeof(ZZ9KAudioFabricStateResultPayload));
    mock_word(reply, 0, mock_req_word(req, 0));
    mock_word(reply, 1, s->granted ? s->generation : 0U);
    mock_word(reply, 2, s->released  || !s->granted
                            ? ZZ9K_AUDIO_FABRIC_SLOT_FREE
                            : (s->published != 0U
                                   ? ZZ9K_AUDIO_FABRIC_SLOT_ACTIVE
                                   : ZZ9K_AUDIO_FABRIC_SLOT_LEASED));
    mock_word(reply, 3, 20U); /* heartbeat_ms */
    mock_word(reply, 4, 0U);
    mock_word(reply, 5, (uint32_t)s->published);
    mock_word(reply, 6, 0U);
    mock_word(reply, 7, (uint32_t)s->consumed);
    mock_word(reply, 8, 0U);  /* starvation */
    mock_word(reply, 9, 0U);  /* flags */
    mock_word(reply, 10, 24000U); /* peak: 16.16 of amplitude 12000 */
    mock_word(reply, 11, 0U);     /* clips */
    break;
  }
  default:
    (void)mock_complete(req, ZZ9K_STATUS_UNSUPPORTED, 0U);
    break;
  }
}

/* The mock audio ISR: one tick = one 20-ms period of playback per
 * granted slot. Consumption reads only the published producer line
 * and consumes only complete periods (R6/R7), so a cursor can never
 * outrun visible PCM. */
static void mock_tick(void *user)
{
  uint32_t slot;

  (void)user;
  for (slot = 1U; slot <= ZZ9K_AUDIO_RING_SLOT_MAX; slot++) {
    struct MockSlot *s = mock_slot(slot);
    volatile ZZ9KAudioRingProducerLine *producer;
    volatile ZZ9KAudioRingFirmwareLine *firmware;
    ZZ9KAudioRingProducerSnapshot snap;

    if (!s->granted || s->released) {
      continue;
    }
    producer = (volatile ZZ9KAudioRingProducerLine *)(void *)mock_board_ptr(
        s->control_offset);
    firmware =
        (volatile ZZ9KAudioRingFirmwareLine *)(void *)mock_board_ptr(
            s->control_offset + ZZ9K_AUDIO_RING_CONTROL_LINE_SIZE);

    s->ticks++;
    if (s->revoke_at != 0U && s->ticks >= s->revoke_at) {
      /* Heartbeat expiry / cursor fault: the generation dies and
       * the credits are void (R13). */
      zz9k_audio_ring_firmware_publish(firmware, s->generation + 1U,
                                       s->consumed,
                                       ZZ9K_AUDIO_RING_STATUS_REVOKED_HEARTBEAT);
      continue;
    }
    if (!zz9k_audio_ring_producer_snapshot(producer, &snap)) {
      continue; /* torn producer line: nothing to consume this tick */
    }
    if (snap.generation != s->generation) {
      continue;
    }
    s->published = snap.write_cursor;
    s->last_heartbeat = snap.heartbeat;
    if (snap.write_cursor - s->consumed >= s->period_bytes) {
      s->consumed += s->period_bytes;
    }
    zz9k_audio_ring_firmware_publish(firmware, s->generation, s->consumed,
                                     ZZ9K_AUDIO_RING_STATUS_OK);
    if (s->torn_ticks != 0U) {
      /* Scripted in-flight writer: leave the sequence odd so the
       * client's next credit snapshot tears. */
      s->torn_ticks--;
      zz9k_put_be32(firmware->sequence,
                    zz9k_get_be32(firmware->sequence) + 1U);
    }
  }
}

static void mock_install_hooks(void)
{
  zz9k_set_idle_hook_for_test(mock_respond);
  zz9k_fabriclease_set_tick_hook_for_test(mock_tick, 0);
}

static void mock_remove_hooks(void)
{
  zz9k_set_idle_hook_for_test(0);
  zz9k_fabriclease_set_tick_hook_for_test(0, 0);
}

static int mock_attach(ZZ9KContext **ctx)
{
  int status = zz9k_attach_mailbox(ctx, &g_mock.board,
                                   &g_mock.mailbox.descriptor, 0, 0);
  if (status != ZZ9K_STATUS_OK) {
    mock_remove_hooks();
  }
  return status;
}

/* ---- pure gate ---- */

static int test_gate(void)
{
  /* Firmware without the capability declines cleanly (R15-era
   * mixed-version fallback). Zorro II is a supported single-slot
   * bus: admission is firmware's acquire-time decision, so no board
   * shape declines here. */
  if (zz9k_fabriclease_gate(0U) != ZZ9K_FABRICLEASE_DECLINE_NO_FABRIC) {
    return 1;
  }
  if (zz9k_fabriclease_gate(ZZ9K_CAP_AUDIO_FABRIC) != ZZ9K_FABRICLEASE_OK) {
    return 2;
  }
  if (zz9k_fabriclease_gate(ZZ9K_CAP_AUDIO_FABRIC |
                            ZZ9K_CAP_HOST_WINDOW_HEAP) !=
      ZZ9K_FABRICLEASE_OK) {
    return 3;
  }
  return 0;
}

/* ---- session helpers against plain ring memory ---- */

static void session_fill(ZZ9KAudioRingSession *session, uint32_t capacity,
                         volatile uint8_t *ring,
                         volatile ZZ9KAudioRingProducerLine *producer,
                         volatile ZZ9KAudioRingFirmwareLine *firmware)
{
  memset((void *)session, 0, sizeof(*session));
  session->grant.slot = 1U;
  session->grant.generation = 5U;
  session->grant.ring_capacity = capacity;
  session->grant.period_bytes = ZZ9K_AUDIO_RING_PERIOD_BYTES;
  session->heartbeat = 1U;
  session->ring = ring;
  session->producer_line = producer;
  session->firmware_line = firmware;
  session->mapped = 1U;
}

static int test_wrapped_write_and_credits(void)
{
  /* Two-period ring: every chunk after the first wraps, and credit
   * adoption bounds every write. */
  volatile uint8_t ring[2U * ZZ9K_AUDIO_RING_PERIOD_BYTES + 8U];
  ZZ9KAudioRingProducerLine producer;
  ZZ9KAudioRingFirmwareLine firmware;
  ZZ9KAudioRingSession session;
  uint8_t chunk[2U * ZZ9K_AUDIO_RING_PERIOD_BYTES];
  uint32_t i;

  memset((void *)ring, 0, sizeof(ring));
  memset(&producer, 0, sizeof(producer));
  memset(&firmware, 0, sizeof(firmware));
  for (i = 0; i < sizeof(chunk); i++) {
    chunk[i] = (uint8_t)(i + 1U);
  }
  session_fill(&session, 2U * ZZ9K_AUDIO_RING_PERIOD_BYTES, ring,
               &producer, &firmware);

  /* Free space bounds the write: a fresh two-period ring stages
   * exactly its capacity, never the three periods asked for. */
  if (zz9k_audio_ring_write(&session, chunk,
                            3U * ZZ9K_AUDIO_RING_PERIOD_BYTES) !=
          2U * ZZ9K_AUDIO_RING_PERIOD_BYTES ||
      session.write_cursor != 2U * ZZ9K_AUDIO_RING_PERIOD_BYTES) {
    return 1;
  }
  for (i = 0; i < 2U * ZZ9K_AUDIO_RING_PERIOD_BYTES; i++) {
    if (ring[i] != (uint8_t)(i + 1U)) {
      return 2; /* both periods landed at ring start, unwrapped */
    }
  }

  /* Ring full: zero credited space, zero staged. */
  if (zz9k_audio_ring_write(&session, chunk, 4U) != 0U) {
    return 3;
  }

  /* Credits: one consumed period frees exactly one period, and the
   * cursor wraps: the next period lands back at ring start while
   * the cursor stays monotonic (64-bit, never masked). */
  zz9k_audio_ring_firmware_publish(&firmware, 5U,
                                   ZZ9K_AUDIO_RING_PERIOD_BYTES,
                                   ZZ9K_AUDIO_RING_STATUS_OK);
  if (zz9k_audio_ring_take_credits(&session, 0U) !=
          ZZ9K_AUDIO_RING_CREDIT_OK ||
      session.consumed_cursor != ZZ9K_AUDIO_RING_PERIOD_BYTES ||
      zz9k_audio_ring_free_bytes(&session) != ZZ9K_AUDIO_RING_PERIOD_BYTES) {
    return 4;
  }
  if (zz9k_audio_ring_write(&session, chunk, ZZ9K_AUDIO_RING_PERIOD_BYTES) !=
          ZZ9K_AUDIO_RING_PERIOD_BYTES ||
      session.write_cursor != 3U * ZZ9K_AUDIO_RING_PERIOD_BYTES) {
    return 5;
  }
  if (ring[0] != 1U) {
    return 6; /* wrapped write overwrote from ring start */
  }
  /* One write crossing the physical end resumes at ring[0], not at
   * ring[first] and never beyond the grant. */
  memset((void *)ring, 0, sizeof(ring));
  session.write_cursor =
      2U * ZZ9K_AUDIO_RING_PERIOD_BYTES -
      ZZ9K_AUDIO_RING_PERIOD_BYTES / 2U;
  session.consumed_cursor = session.write_cursor;
  if (zz9k_audio_ring_write(&session, chunk,
                            ZZ9K_AUDIO_RING_PERIOD_BYTES) !=
          ZZ9K_AUDIO_RING_PERIOD_BYTES ||
      ring[2U * ZZ9K_AUDIO_RING_PERIOD_BYTES -
           ZZ9K_AUDIO_RING_PERIOD_BYTES / 2U] != 1U ||
      ring[0] != chunk[ZZ9K_AUDIO_RING_PERIOD_BYTES / 2U] ||
      ring[2U * ZZ9K_AUDIO_RING_PERIOD_BYTES] != 0U) {
    return 7;
  }


  /* 64-bit cursor arithmetic across the 32-bit boundary. */
  session.write_cursor = UINT64_C(0x100000000) + 3840U;
  session.consumed_cursor = UINT64_C(0xffffffff);
  if (zz9k_audio_ring_outstanding(&session) != 3841U ||
      !zz9k_audio_ring_distance_valid(session.write_cursor,
                                      session.consumed_cursor,
                                      session.grant.ring_capacity)) {
    return 7;
  }

  /* Torn (odd-sequence) firmware line: bounded attempt reports
   * RETRY and adopts nothing. */
  zz9k_put_be32(firmware.sequence,
                zz9k_get_be32(firmware.sequence) + 1U);
  if (zz9k_audio_ring_take_credits(&session, 4U) !=
          ZZ9K_AUDIO_RING_CREDIT_RETRY ||
      session.consumed_cursor != UINT64_C(0xffffffff)) {
    return 8;
  }

  /* Generation mismatch, faulted status, and a backward credit are
   * all REVOKED -- this generation is dead, the client stops. */
  zz9k_audio_ring_firmware_publish(&firmware, 9U, 0U,
                                   ZZ9K_AUDIO_RING_STATUS_OK);
  if (zz9k_audio_ring_take_credits(&session, 0U) !=
      ZZ9K_AUDIO_RING_CREDIT_REVOKED) {
    return 9;
  }
  zz9k_audio_ring_firmware_publish(&firmware, 5U, 0U,
                                   ZZ9K_AUDIO_RING_STATUS_FAULT_CURSOR);
  if (zz9k_audio_ring_take_credits(&session, 0U) !=
      ZZ9K_AUDIO_RING_CREDIT_REVOKED) {
    return 10;
  }
  zz9k_audio_ring_firmware_publish(&firmware, 5U, session.write_cursor + 1U,
                                   ZZ9K_AUDIO_RING_STATUS_OK);
  if (zz9k_audio_ring_take_credits(&session, 0U) !=
      ZZ9K_AUDIO_RING_CREDIT_REVOKED) {
    return 11;
  }
  return 0;
}

static int test_publish_ordering_and_heartbeat(void)
{
  volatile uint8_t ring[ZZ9K_AUDIO_RING_PERIOD_BYTES];
  ZZ9KAudioRingProducerLine producer;
  ZZ9KAudioRingFirmwareLine firmware;
  ZZ9KAudioRingSession session;
  ZZ9KAudioRingProducerSnapshot snap;
  uint32_t heartbeat_before;
  uint8_t chunk[ZZ9K_AUDIO_RING_PERIOD_BYTES];
  uint32_t i;

  memset((void *)ring, 0, sizeof(ring));
  memset(&producer, 0, sizeof(producer));
  memset(&firmware, 0, sizeof(firmware));
  memset(chunk, 0xa5, sizeof(chunk));
  session_fill(&session, ZZ9K_AUDIO_RING_PERIOD_BYTES, ring, &producer,
               &firmware);

  zz9k_audio_ring_publish(&session);
  if (!zz9k_audio_ring_producer_snapshot(&producer, &snap) ||
      snap.generation != 5U || snap.write_cursor != 0U ||
      snap.heartbeat != 2U) {
    return 1; /* initial publication: cursor 0, fresh heartbeat */
  }

  /* PCM lands but the cursor stays put until publish (R6): a
   * consumer polling the line cannot see the bytes. */
  heartbeat_before = snap.heartbeat;
  if (zz9k_audio_ring_write(&session, chunk, sizeof(chunk)) !=
      sizeof(chunk)) {
    return 2;
  }
  if (!zz9k_audio_ring_producer_snapshot(&producer, &snap) ||
      snap.write_cursor != 0U) {
    return 3; /* cursor moved before publication */
  }
  zz9k_audio_ring_publish(&session);
  if (!zz9k_audio_ring_producer_snapshot(&producer, &snap) ||
      snap.write_cursor != ZZ9K_AUDIO_RING_PERIOD_BYTES ||
      snap.heartbeat == heartbeat_before || snap.heartbeat == 0U) {
    return 4; /* cursor visible after PCM; heartbeat refreshed */
  }
  for (i = 0; i < ZZ9K_AUDIO_RING_PERIOD_BYTES; i++) {
    if (ring[i] != 0xa5U) {
      return 5;
    }
  }

  /* A publish with nothing new staged is a pure heartbeat refresh
   * (R12: paused producers stay live). */
  heartbeat_before = snap.heartbeat;
  zz9k_audio_ring_publish(&session);
  if (!zz9k_audio_ring_producer_snapshot(&producer, &snap) ||
      snap.write_cursor != ZZ9K_AUDIO_RING_PERIOD_BYTES ||
      snap.heartbeat == heartbeat_before) {
    return 6;
  }
  return 0;
}

/* ---- mock-mailbox sessions ---- */

static int run_client_session(uint16_t zorro, uint32_t board_size,
                              uint32_t seconds, uint32_t gain,
                              uint32_t slot, int *session_status)
{
  ZZ9KContext *ctx = 0;
  ZZ9KFabricLeaseOptions options;
  int status;

  mock_reset(ZZ9K_CAP_AUDIO_FABRIC, zorro, board_size);
  if (zorro == 3U) {
    mock_grant_slot(1U, MOCK_GENERATION1, 0x00100000UL,
                    2U * ZZ9K_AUDIO_RING_PERIOD_BYTES, 0x000ff000UL);
    mock_grant_slot(2U, MOCK_GENERATION2, 0x00200000UL,
                    2U * ZZ9K_AUDIO_RING_PERIOD_BYTES, 0x001ff000UL);
  } else {
    /* Zorro II compact single slot; the scripted grant geometry is
     * the caller's concern (see the Z2 tests). */
    mock_grant_slot(1U, MOCK_GENERATION1, g_mock.z2_grant_ring_offset,
                    12U * ZZ9K_AUDIO_RING_PERIOD_BYTES, 0x00001000UL);
  }
  mock_slot(1U)->torn_ticks = g_script_torn_ticks;
  mock_slot(1U)->revoke_at = g_script_revoke_after;
  g_script_torn_ticks = 0U;
  g_script_revoke_after = 0U;
  mock_install_hooks();
  status = mock_attach(&ctx);
  if (status != ZZ9K_STATUS_OK) {
    return -1;
  }
  memset(&options, 0, sizeof(options));
  options.seconds = seconds;
  options.gain = gain;
  options.slot = slot;
  *session_status = zz9k_fabriclease_session(ctx, &options);
  zz9k_close(ctx);
  mock_remove_hooks();
  return 0;
}

static int test_client_walkthrough(void)
{
  void *mapping = mock_window_alloc(0x01000000UL);
  struct MockSlot *s;
  int status = 0;
  int rc;

  if (!mapping) {
    return 1;
  }
  rc = run_client_session(3U, 0x01000000UL, 1U, 200U, 1U, &status);
  if (rc != 0) {
    mock_window_free(mapping, 0x01000000UL);
    return 2;
  }
  if (status != ZZ9K_STATUS_OK) {
    mock_window_free(mapping, 0x01000000UL);
    return 3;
  }
  s = mock_slot(1U);

  /* Exactly one lifecycle: one acquire, one release, telemetry only
   * at the low rate (never in the pacing path). */
  if (g_mock.acquire_calls != 1 || g_mock.release_calls != 1 ||
      g_mock.refused_acquires != 0) {
    mock_window_free(mapping, 0x01000000UL);
    return 4;
  }
  if (g_mock.state_calls > 8) {
    mock_window_free(mapping, 0x01000000UL);
    return 5; /* state polling crept into the data path */
  }
  /* The whole second of audio crossed the mailbox exactly at the
   * control plane: acquire + release + low-rate telemetry (R5). */
  if (g_mock.served_requests !=
      g_mock.acquire_calls + g_mock.release_calls + g_mock.state_calls) {
    mock_window_free(mapping, 0x01000000UL);
    return 9;
  }
  /* One second of audio fully staged and fully credited, over a
   * two-period ring: every write after the first wrapped. */
  if (s->published != ZZ9K_FABRICLEASE_BYTES_PER_SECOND ||
      s->consumed != ZZ9K_FABRICLEASE_BYTES_PER_SECOND) {
    mock_window_free(mapping, 0x01000000UL);
    return 6;
  }
  if (g_mock.release_slot != 1U ||
      g_mock.release_generation != MOCK_GENERATION1) {
    mock_window_free(mapping, 0x01000000UL);
    return 7; /* release presented the grant exactly as acquired */
  }
  /* The tone really landed in the mapped ring: the burst pattern's
   * block 0 (-12000 both channels, little-endian) at the ring start,
   * written through the wrapped producer path. */
  {
    volatile int16_t *window = (volatile int16_t *)(void *)mock_board_ptr(
        s->ring_offset);

    if (window[0] != -12000 || window[1] != -12000) {
      mock_window_free(mapping, 0x01000000UL);
      return 8;
    }
  }
  mock_window_free(mapping, 0x01000000UL);
  return 0;
}

static int test_publication_visibility(void)
{
  void *mapping = mock_window_alloc(0x01000000UL);
  ZZ9KContext *ctx = 0;
  ZZ9KAudioRingAcquireDesc desc;
  ZZ9KAudioRingSession session;
  struct MockSlot *s;
  uint8_t chunk[ZZ9K_AUDIO_RING_PERIOD_BYTES];

  if (!mapping) {
    return 1;
  }
  mock_reset(ZZ9K_CAP_AUDIO_FABRIC, 3U, 0x01000000UL);
  mock_grant_slot(1U, MOCK_GENERATION1, 0x00100000UL,
                  2U * ZZ9K_AUDIO_RING_PERIOD_BYTES, 0x000ff000UL);
  memset(chunk, 0x3c, sizeof(chunk));
  mock_install_hooks();
  if (mock_attach(&ctx) != 0) {
    mock_window_free(mapping, 0x01000000UL);
    return 2;
  }
  if (!zz9k_audio_build_ring_acquire_desc(&desc, 1U, ZZ9K_AUDIO_METER_IDENTITY_SDK_STREAM, 128U, 0U, 0U) ||
      zz9k_audio_ring_session_begin(ctx, &desc, &session) !=
          ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    mock_remove_hooks();
    mock_window_free(mapping, 0x01000000UL);
    return 3;
  }
  s = mock_slot(1U);

  /* AE3: PCM that is written but not yet published cannot be
   * consumed. */
  if (zz9k_audio_ring_write(&session, chunk, sizeof(chunk)) !=
      sizeof(chunk)) {
    goto fail;
  }
  mock_tick(0);
  if (s->published != 0U || s->consumed != 0U) {
    goto fail4;
  }
  /* After visibility and publication, exactly that complete period
   * becomes eligible. */
  zz9k_audio_ring_publish(&session);
  mock_tick(0);
  if (s->published != ZZ9K_AUDIO_RING_PERIOD_BYTES ||
      s->consumed != ZZ9K_AUDIO_RING_PERIOD_BYTES) {
    goto fail5;
  }
  /* The credit is real: the session adopts it and may overwrite the
   * consumed period (R7). */
  if (zz9k_audio_ring_take_credits(&session, 2U) !=
          ZZ9K_AUDIO_RING_CREDIT_OK ||
      session.consumed_cursor != ZZ9K_AUDIO_RING_PERIOD_BYTES ||
      zz9k_audio_ring_write(&session, chunk, sizeof(chunk)) !=
          sizeof(chunk)) {
    goto fail6;
  }
  if (zz9k_audio_ring_session_end(ctx, &session) != ZZ9K_STATUS_OK ||
      g_mock.release_calls != 1) {
    goto fail7;
  }
  zz9k_close(ctx);
  mock_remove_hooks();
  mock_window_free(mapping, 0x01000000UL);
  return 0;

fail:
  zz9k_audio_ring_session_end(ctx, &session);
  zz9k_close(ctx);
  mock_remove_hooks();
  mock_window_free(mapping, 0x01000000UL);
  return 8;
fail4:
  zz9k_audio_ring_session_end(ctx, &session);
  zz9k_close(ctx);
  mock_remove_hooks();
  mock_window_free(mapping, 0x01000000UL);
  return 9;
fail5:
  zz9k_audio_ring_session_end(ctx, &session);
  zz9k_close(ctx);
  mock_remove_hooks();
  mock_window_free(mapping, 0x01000000UL);
  return 10;
fail6:
  zz9k_audio_ring_session_end(ctx, &session);
  zz9k_close(ctx);
  mock_remove_hooks();
  mock_window_free(mapping, 0x01000000UL);
  return 11;
fail7:
  zz9k_close(ctx);
  mock_remove_hooks();
  mock_window_free(mapping, 0x01000000UL);
  return 12;
}

static int cancel_after_first_publish(void *user)
{
  (void)user;
  return g_mock.served_requests != 0 &&
         mock_slot(1U)->published >= ZZ9K_AUDIO_RING_PERIOD_BYTES;
}

static int test_ctrl_c_releases_slot(void)
{
  void *mapping = mock_window_alloc(0x01000000UL);
  int status = 0;
  int rc;

  if (!mapping) {
    return 1;
  }
  zz9k_fabriclease_set_cancel_hook_for_test(cancel_after_first_publish, 0);
  rc = run_client_session(3U, 0x01000000UL, 60U, 128U, 1U, &status);
  zz9k_fabriclease_set_cancel_hook_for_test(0, 0);
  if (rc != 0) {
    mock_window_free(mapping, 0x01000000UL);
    return 2;
  }
  if (status != ZZ9K_STATUS_CANCELLED) {
    mock_window_free(mapping, 0x01000000UL);
    return 3;
  }
  if (g_mock.release_calls != 1 || g_mock.release_slot != 1U ||
      g_mock.release_generation != MOCK_GENERATION1) {
    mock_window_free(mapping, 0x01000000UL);
    return 4; /* Ctrl-C released the grant under its generation */
  }
  mock_window_free(mapping, 0x01000000UL);
  return 0;
}

static int test_unstable_firmware_line(void)
{
  void *mapping = mock_window_alloc(0x01000000UL);
  int status = 0;
  int rc;

  if (!mapping) {
    return 1;
  }
  /* Three ticks of scripted in-flight firmware writes: every credit
   * snapshot in that window tears, and the client must retry its
   * way through without mailbox help. */
  g_script_torn_ticks = 3U;
  rc = run_client_session(3U, 0x01000000UL, 1U, 128U, 1U, &status);
  if (rc != 0) {
    mock_window_free(mapping, 0x01000000UL);
    return 2;
  }
  mock_window_free(mapping, 0x01000000UL);
  if (status != ZZ9K_STATUS_OK) {
    return 3;
  }
  return 0;
}

static int test_revoked_generation_stops_client(void)
{
  void *mapping = mock_window_alloc(0x01000000UL);
  int status = 0;
  int rc;

  if (!mapping) {
    return 1;
  }
  g_script_revoke_after = 8U;
  rc = run_client_session(3U, 0x01000000UL, 1U, 128U, 1U, &status);
  if (rc != 0) {
    mock_window_free(mapping, 0x01000000UL);
    return 2;
  }
  if (status != ZZ9K_STATUS_BAD_HANDLE) {
    mock_window_free(mapping, 0x01000000UL);
    return 3; /* revoked generation reported as a dead handle */
  }
  if (g_mock.release_calls != 1) {
    mock_window_free(mapping, 0x01000000UL);
    return 4; /* the dead generation was still released */
  }
  mock_window_free(mapping, 0x01000000UL);
  return 0;
}

/* Two contexts, two slots: the interleaved steady data path is pure
 * shared memory, so two producer tasks never serialize through the
 * mailbox (the failure mode that killed the copy-submit plane). */
static int test_independent_contexts(void)
{
  void *mapping = mock_window_alloc(0x01000000UL);
  ZZ9KContext *ctx_a = 0;
  ZZ9KContext *ctx_b = 0;
  ZZ9KAudioRingAcquireDesc desc;
  ZZ9KAudioRingSession session_a;
  ZZ9KAudioRingSession session_b;
  struct MockSlot *s1;
  struct MockSlot *s2;
  uint8_t chunk[ZZ9K_AUDIO_RING_PERIOD_BYTES];
  uint32_t pass;
  int requests_before;
  uint32_t heartbeat_a;
  int status;

  if (!mapping) {
    return 1;
  }
  mock_reset(ZZ9K_CAP_AUDIO_FABRIC, 3U, 0x01000000UL);
  mock_grant_slot(1U, MOCK_GENERATION1, 0x00100000UL,
                  4U * ZZ9K_AUDIO_RING_PERIOD_BYTES, 0x000ff000UL);
  mock_grant_slot(2U, MOCK_GENERATION2, 0x00200000UL,
                  4U * ZZ9K_AUDIO_RING_PERIOD_BYTES, 0x001ff000UL);
  memset(chunk, 0x5a, sizeof(chunk));
  mock_install_hooks();
  if (mock_attach(&ctx_a) != ZZ9K_STATUS_OK ||
      mock_attach(&ctx_b) != ZZ9K_STATUS_OK) {
    mock_remove_hooks();
    mock_window_free(mapping, 0x01000000UL);
    return 2;
  }
  if (!zz9k_audio_build_ring_acquire_desc(&desc, 1U, ZZ9K_AUDIO_METER_IDENTITY_SDK_STREAM, 128U, 0U, 0U) ||
      zz9k_audio_ring_session_begin(ctx_a, &desc, &session_a) !=
          ZZ9K_STATUS_OK) {
    status = 3;
    goto out;
  }
  if (!zz9k_audio_build_ring_acquire_desc(&desc, 2U, ZZ9K_AUDIO_METER_IDENTITY_MEDIA, 128U, 0U, 0U) ||
      zz9k_audio_ring_session_begin(ctx_b, &desc, &session_b) !=
          ZZ9K_STATUS_OK) {
    status = 4;
    goto out;
  }
  s1 = mock_slot(1U);
  s2 = mock_slot(2U);
  requests_before = g_mock.served_requests;
  heartbeat_a = s1->last_heartbeat;

  /* Interleaved feeding: A writes then B writes, both publish, one
   * tick consumes both -- no mailbox call anywhere in the loop. */
  for (pass = 0U; pass < 3U; pass++) {
    uint32_t staged;

    staged = zz9k_audio_ring_write(&session_a, chunk, sizeof(chunk));
    if (staged != sizeof(chunk)) {
      status = 5;
      goto out_rel;
    }
    zz9k_audio_ring_publish(&session_a);
    staged = zz9k_audio_ring_write(&session_b, chunk, sizeof(chunk));
    if (staged != sizeof(chunk)) {
      status = 6;
      goto out_rel;
    }
    zz9k_audio_ring_publish(&session_b);
    mock_tick(0);
    mock_tick(0);
  }
  if (session_a.grant.generation == session_b.grant.generation ||
      session_a.producer_line == session_b.producer_line ||
      session_a.ring == session_b.ring) {
    status = 9; /* sessions collapsed onto one lease */
    goto out_rel;
  }
  if (g_mock.served_requests != requests_before) {
    status = 7; /* the steady data path entered the mailbox */
    goto out_rel;
  }
  if (s1->consumed != s2->consumed ||
      s1->consumed != 3U * ZZ9K_AUDIO_RING_PERIOD_BYTES) {
    status = 8;
    goto out_rel;
  }
  if (s1->last_heartbeat == heartbeat_a) {
    status = 10; /* A's heartbeat never refreshed */
    goto out_rel;
  }
  if (mock_board_ptr(s2->ring_offset)[0] != 0x5aU ||
      mock_board_ptr(s1->ring_offset)[0] != 0x5aU) {
    status = 11; /* interleaved writes crossed rings */
    goto out_rel;
  }
  status = ZZ9K_STATUS_OK;


out_rel:
  if (zz9k_audio_ring_session_end(ctx_a, &session_a) != ZZ9K_STATUS_OK ||
      zz9k_audio_ring_session_end(ctx_b, &session_b) != ZZ9K_STATUS_OK) {
    status = status == ZZ9K_STATUS_OK ? 12 : status;
  }
  if (g_mock.release_calls != 2) {
    status = status == ZZ9K_STATUS_OK ? 13 : status;
  }
out:
  zz9k_close(ctx_a);
  zz9k_close(ctx_b);
  mock_remove_hooks();
  mock_window_free(mapping, 0x01000000UL);
  return status == ZZ9K_STATUS_OK ? 0 : status;
}

/* AE2: one active Zorro II producer; a second acquisition is refused
 * deterministically and the active producer is never disturbed. */
static int test_z2_second_client_refusal(void)
{
  void *mapping = mock_window_alloc(0x00400000UL);
  ZZ9KContext *ctx = 0;
  ZZ9KAudioRingSession active;
  ZZ9KAudioRingSession second;
  ZZ9KAudioRingAcquireDesc desc;
  struct MockSlot *s1;
  uint8_t chunk[ZZ9K_AUDIO_RING_PERIOD_BYTES];
  int status;

  if (!mapping) {
    return 1;
  }
  mock_reset(ZZ9K_CAP_AUDIO_FABRIC | ZZ9K_CAP_APERTURE_LAYOUT |
                 ZZ9K_CAP_HOST_WINDOW_HEAP,
             2U, 0x00400000UL);
  /* Grant geometry inside a (generation-2-shaped) direct-region gap
   * the mock reports: pinned firmware reservation. */
  mock_grant_slot(1U, MOCK_GENERATION1, 0x003e4080UL,
                  12U * ZZ9K_AUDIO_RING_PERIOD_BYTES, 0x003e4000UL);
  memset(chunk, 0x77, sizeof(chunk));
  mock_install_hooks();
  if (mock_attach(&ctx) != ZZ9K_STATUS_OK) {
    mock_remove_hooks();
    mock_window_free(mapping, 0x00400000UL);
    return 2;
  }

  /* The active producer acquires the single Zorro II slot through
   * the real session path: the generation-2 carve leaves its pinned
   * grant inside the direct-region gap, so mapping validates. */
  if (!zz9k_audio_build_ring_acquire_desc(&desc, 1U, ZZ9K_AUDIO_METER_IDENTITY_SDK_STREAM, 128U, 0U, 0U) ||
      zz9k_audio_ring_session_begin(ctx, &desc, &active) != ZZ9K_STATUS_OK ||
      (active.grant.flags & ZZ9K_AUDIO_RING_RESULT_BUS_ZORRO2) == 0U) {
    status = 3;
    goto out;
  }
  s1 = mock_slot(1U);
  if (zz9k_audio_ring_write(&active, chunk, sizeof(chunk)) !=
          sizeof(chunk) ||
      (zz9k_audio_ring_publish(&active), 0) != 0) {
    status = 4;
    goto out_rel;
  }
  mock_tick(0);
  if (s1->consumed != ZZ9K_AUDIO_RING_PERIOD_BYTES) {
    status = 5;
    goto out_rel;
  }

  /* The second client's acquisition: refused by bus admission, the
   * session fails closed, and nothing was released or disturbed. */
  if (!zz9k_audio_build_ring_acquire_desc(&desc, 2U, ZZ9K_AUDIO_METER_IDENTITY_SDK_STREAM, 128U, 0U, 0U)) {
    status = 6;
    goto out_rel;
  }
  memset(&second, 0xa5, sizeof(second));
  status = zz9k_audio_ring_session_begin(ctx, &desc, &second);
  if (status != ZZ9K_STATUS_BAD_REQUEST ||
      second.mapped != 0U || second.ring != 0) {
    status = 7;
    goto out_rel;
  }
  if (g_mock.refused_acquires != 1 || g_mock.release_calls != 0) {
    status = 8;
    goto out_rel;
  }
  /* The active producer keeps playing: the refusal left it alone,
   * and a freshly published period is consumed on the next tick. */
  if (zz9k_audio_ring_write(&active, chunk, sizeof(chunk)) !=
          sizeof(chunk) ||
      (zz9k_audio_ring_publish(&active), 0) != 0) {
    status = 11;
    goto out_rel;
  }
  mock_tick(0);
  if (s1->consumed != 2U * ZZ9K_AUDIO_RING_PERIOD_BYTES) {
    status = 9;
    goto out_rel;
  }

  status = ZZ9K_STATUS_OK;
out_rel:
  if (status != 3 && status != 4 &&
      zz9k_audio_ring_session_end(ctx, &active) != ZZ9K_STATUS_OK) {
    status = 10;
  }
out:
  zz9k_close(ctx);
  mock_remove_hooks();
  mock_window_free(mapping, 0x00400000UL);
  return status == ZZ9K_STATUS_OK ? 0 : status;
}

/* A Zorro II grant that lands inside a reported region (here: the
 * carved host-window heap) must never expose pointers (R2). */
static int test_z2_grant_inside_region_rejected(void)
{
  void *mapping = mock_window_alloc(0x00400000UL);
  ZZ9KContext *ctx = 0;
  ZZ9KAudioRingAcquireDesc desc;
  ZZ9KAudioRingSession session;
  int status;

  if (!mapping) {
    return 1;
  }
  mock_reset(ZZ9K_CAP_AUDIO_FABRIC | ZZ9K_CAP_APERTURE_LAYOUT |
                 ZZ9K_CAP_HOST_WINDOW_HEAP,
             2U, 0x00400000UL);
  /* Ring range intersects the carved host window
   * (0x003e0000..0x003e4000 under generation 2). */
  mock_grant_slot(1U, MOCK_GENERATION1, 0x003e0000UL,
                  4U * ZZ9K_AUDIO_RING_PERIOD_BYTES, 0x003e4000UL);
  mock_install_hooks();
  if (mock_attach(&ctx) != ZZ9K_STATUS_OK) {
    mock_remove_hooks();
    mock_window_free(mapping, 0x00400000UL);
    return 2;
  }
  if (!zz9k_audio_build_ring_acquire_desc(&desc, 1U, ZZ9K_AUDIO_METER_IDENTITY_SDK_STREAM, 128U, 0U, 0U)) {
    zz9k_close(ctx);
    mock_remove_hooks();
    mock_window_free(mapping, 0x00400000UL);
    return 3;
  }
  memset(&session, 0xa5, sizeof(session));
  status = zz9k_audio_ring_session_begin(ctx, &desc, &session);
  zz9k_close(ctx);
  mock_remove_hooks();
  mock_window_free(mapping, 0x00400000UL);
  if (status != ZZ9K_STATUS_INTERNAL_ERROR || session.mapped != 0U ||
      session.ring != 0 || g_mock.release_calls != 1) {
    /* PR #29 review: the slot is leased firmware-side from the successful
     * acquire, so a failed validation must release it or the next acquire
     * stays BUSY until heartbeat revocation. */
    return 4;
  }
  return 0;
}

/* The pinned firmware grant geometry (U2 contract) validates as free
 * aperture space under the generation-2 Zorro II layout: the direct
 * region is the gap between the (shrunk) host window and audio. */
static int test_z2_pinned_grant_geometry(void)
{
  ZZ9KApertureLayout gen2;

  memset(&gen2, 0, sizeof(gen2));
  gen2.aperture_size = 0x00400000UL;
  gen2.framebuffer_base = 0x00010000UL;
  gen2.framebuffer_size = 0x00388000UL;
  gen2.pip_base = 0x00398000UL;
  gen2.pip_size = 0x00038000UL;
  gen2.template_base = 0x003d0000UL;
  gen2.template_size = 0x00010000UL;
  gen2.host_base = 0x003e0000UL;
  gen2.host_size = 0x00004000UL; /* 16 KiB residual heap */
  gen2.audio_base = 0x003f0000UL;
  gen2.audio_size = 0x00010000UL;

  /* Pinned grant: control 0x3E4000, ring 0x3E4080, 12 periods. */
  if (!zz9k_aperture_range_free(&gen2, 0x003e4000UL,
                                ZZ9K_AUDIO_RING_CONTROL_SIZE) ||
      !zz9k_aperture_range_free(
          &gen2, 0x003e4080UL, 12U * ZZ9K_AUDIO_RING_PERIOD_BYTES)) {
    return 1;
  }
  /* Inside the host window, the audio region, or past the aperture:
   * rejected. */
  if (zz9k_aperture_range_free(&gen2, 0x003e0000UL,
                               ZZ9K_AUDIO_RING_CONTROL_SIZE)) {
    return 2;
  }
  if (zz9k_aperture_range_free(
          &gen2, 0x003f0000UL, ZZ9K_AUDIO_RING_PERIOD_BYTES)) {
    return 3;
  }
  if (zz9k_aperture_range_free(
          &gen2, 0x003fffffUL, ZZ9K_AUDIO_RING_PERIOD_BYTES)) {
    return 4;
  }
  return 0;
}

/* The pinned Zorro III grant geometry maps and runs end to end
 * (U2 contract): control 0x07FD0000, ring 0x07FD0080, 16 periods,
 * inside the 128 MiB window. */
static int test_z3_pinned_grant_geometry(void)
{
  void *mapping = mock_window_alloc(0x08000000UL);
  ZZ9KContext *ctx = 0;
  ZZ9KAudioRingAcquireDesc desc;
  ZZ9KAudioRingSession session;
  struct MockSlot *s;
  uint8_t chunk[ZZ9K_AUDIO_RING_PERIOD_BYTES];
  int status;

  if (!mapping) {
    return 1;
  }
  mock_reset(ZZ9K_CAP_AUDIO_FABRIC, 3U, 0x08000000UL);
  mock_grant_slot(1U, MOCK_GENERATION1, 0x07fd0080UL,
                  16U * ZZ9K_AUDIO_RING_PERIOD_BYTES, 0x07fd0000UL);
  mock_grant_slot(2U, MOCK_GENERATION2, 0x07fe0080UL,
                  16U * ZZ9K_AUDIO_RING_PERIOD_BYTES, 0x07fe0000UL);
  memset(chunk, 0x69, sizeof(chunk));
  mock_install_hooks();
  if (mock_attach(&ctx) != ZZ9K_STATUS_OK) {
    mock_remove_hooks();
    mock_window_free(mapping, 0x08000000UL);
    return 2;
  }
  if (!zz9k_audio_build_ring_acquire_desc(&desc, 1U, ZZ9K_AUDIO_METER_IDENTITY_SDK_STREAM, 128U, 0U, 0U) ||
      zz9k_audio_ring_session_begin(ctx, &desc, &session) !=
          ZZ9K_STATUS_OK ||
      session.grant.ring_capacity != 16U * ZZ9K_AUDIO_RING_PERIOD_BYTES ||
      session.grant.slot_count != 2U) {
    status = 3;
    goto out;
  }
  if (zz9k_audio_ring_write(&session, chunk, sizeof(chunk)) !=
          sizeof(chunk) ||
      (zz9k_audio_ring_publish(&session), 0) != 0) {
    status = 4;
    goto out_rel;
  }
  mock_tick(0);
  s = mock_slot(1U);
  if (s->consumed != ZZ9K_AUDIO_RING_PERIOD_BYTES ||
      s->published != ZZ9K_AUDIO_RING_PERIOD_BYTES) {
    status = 5;
    goto out_rel;
  }
  status = ZZ9K_STATUS_OK;
out_rel:
  if (status != ZZ9K_STATUS_OK || session.mapped) {
    if (session.mapped &&
        zz9k_audio_ring_session_end(ctx, &session) != ZZ9K_STATUS_OK) {
      status = 6;
    }
  }
out:
  zz9k_close(ctx);
  mock_remove_hooks();
  mock_window_free(mapping, 0x08000000UL);
  return status == ZZ9K_STATUS_OK ? 0 : status;
}

static int test_mismatched_rate_grants_release(void)
{
  void *mapping = mock_window_alloc(0x08000000UL);
  ZZ9KContext *ctx = 0;
  ZZ9KAudioRingAcquireDesc desc;
  ZZ9KAudioRingSession session;
  int status = 0;

  mock_reset(ZZ9K_CAP_AUDIO_FABRIC, 3U, 0x08000000UL);
  mock_grant_slot(1U, MOCK_GENERATION1, 0x07fd0080UL,
                  16U * ZZ9K_AUDIO_RING_PERIOD_BYTES, 0x07fd0000UL);
  mock_install_hooks();
  if (mock_attach(&ctx) != ZZ9K_STATUS_OK) {
    status = 1;
    goto out;
  }
  if (!zz9k_audio_build_ring_acquire_desc(
          &desc, 1U, ZZ9K_AUDIO_METER_IDENTITY_AHI, 128U,
          ZZ9K_AUDIO_RING_ACQUIRE_FLAG_SOURCE_RATE, 44100U)) {
    status = 2;
    goto out;
  }

  g_mock.override_grant = 1;
  g_mock.override_contract =
      ZZ9K_AUDIO_RING_CONTRACT_SOURCE_RATE_STEREO_S16LE;
  g_mock.override_rate = 32000U;
  memset(&session, 0xa5, sizeof(session));
  if (zz9k_audio_ring_session_begin(ctx, &desc, &session) !=
          ZZ9K_STATUS_INTERNAL_ERROR ||
      session.mapped != 0U || session.grant.generation != 0U ||
      g_mock.release_calls != 1 ||
      g_mock.release_slot != 1U ||
      g_mock.release_generation != MOCK_GENERATION1) {
    status = 3;
    goto out;
  }

  g_mock.override_contract = ZZ9K_AUDIO_RING_CONTRACT_48K_STEREO_S16LE;
  g_mock.override_rate = 48000U;
  memset(&session, 0xa5, sizeof(session));
  if (zz9k_audio_ring_session_begin(ctx, &desc, &session) !=
          ZZ9K_STATUS_INTERNAL_ERROR ||
      session.mapped != 0U || session.grant.generation != 0U ||
      g_mock.release_calls != 2 ||
      g_mock.release_slot != 1U ||
      g_mock.release_generation != MOCK_GENERATION1) {
    status = 4;
    goto out;
  }

  /* Decoder-level validation still owns a successfully granted slot.
   * The wrapper must preserve its generation long enough to release it
   * even though the public result/session is cleared on rejection. */
  g_mock.override_contract =
      ZZ9K_AUDIO_RING_CONTRACT_SOURCE_RATE_STEREO_S16LE;
  g_mock.override_rate = 44101U;
  memset(&session, 0xa5, sizeof(session));
  if (zz9k_audio_ring_session_begin(ctx, &desc, &session) !=
          ZZ9K_STATUS_INTERNAL_ERROR ||
      session.mapped != 0U || session.grant.generation != 0U ||
      g_mock.release_calls != 3 ||
      g_mock.release_slot != 1U ||
      g_mock.release_generation != MOCK_GENERATION1) {
    status = 5;
  }

out:
  if (ctx)
    zz9k_close(ctx);
  mock_remove_hooks();
  mock_window_free(mapping, 0x08000000UL);
  return status;
}

/* A rate-bearing acquire (AHI migration): the request carries the
 * SOURCE_RATE flag and the mix rate, the grant comes back under the
 * source-rate contract with the rate echoed, and a source-rate-sized
 * period (44.1 kHz -> 3528 bytes per 20 ms) stages and publishes
 * exactly like a bypass period. An off-vocabulary rate never reaches
 * the mailbox: the builder rejects it and session_begin fails closed
 * with a zeroed session. */
static int test_rate_lease_session(void)
{
  void *mapping = mock_window_alloc(0x08000000UL);
  ZZ9KContext *ctx = 0;
  ZZ9KAudioRingAcquireDesc desc;
  ZZ9KAudioRingSession session;
  struct MockSlot *s;
  uint8_t chunk[3528U];
  int status;

  mock_reset(ZZ9K_CAP_AUDIO_FABRIC, 3U, 0x08000000UL);
  mock_grant_slot(1U, MOCK_GENERATION1, 0x07fd0080UL,
                  16U * ZZ9K_AUDIO_RING_PERIOD_BYTES, 0x07fd0000UL);
  memset(chunk, 0x5a, sizeof(chunk));
  mock_install_hooks();
  if (mock_attach(&ctx) != ZZ9K_STATUS_OK) {
    mock_remove_hooks();
    mock_window_free(mapping, 0x08000000UL);
    return 1;
  }
  if (!zz9k_audio_build_ring_acquire_desc(
          &desc, 1U, ZZ9K_AUDIO_METER_IDENTITY_AHI, 128U,
          ZZ9K_AUDIO_RING_ACQUIRE_FLAG_SOURCE_RATE, 44100U)) {
    status = 2;
    goto out;
  }
  if (zz9k_audio_ring_session_begin(ctx, &desc, &session) !=
          ZZ9K_STATUS_OK ||
      session.grant.sample_contract !=
          ZZ9K_AUDIO_RING_CONTRACT_SOURCE_RATE_STEREO_S16LE ||
      session.grant.source_rate != 44100U) {
    status = 3;
    goto out;
  }
  if (zz9k_audio_ring_write(&session, chunk, sizeof(chunk)) !=
          sizeof(chunk) ||
      (zz9k_audio_ring_publish(&session), 0) != 0) {
    status = 4;
    goto out_rel;
  }
  mock_tick(0);
  s = mock_slot(1U);
  if (s->consumed != sizeof(chunk) || s->published != sizeof(chunk)) {
    status = 5;
    goto out_rel;
  }
  /* Off-vocabulary rate: the builder rejects it before any mailbox
   * traffic (the firmware-side ring tests cover the raw refusal). */
  if (zz9k_audio_build_ring_acquire_desc(
          &desc, 1U, ZZ9K_AUDIO_METER_IDENTITY_AHI, 128U,
          ZZ9K_AUDIO_RING_ACQUIRE_FLAG_SOURCE_RATE, 44101U)) {
    status = 6;
    goto out_rel;
  }
  status = ZZ9K_STATUS_OK;
out_rel:
  if (status != ZZ9K_STATUS_OK || session.mapped) {
    if (session.mapped &&
        zz9k_audio_ring_session_end(ctx, &session) != ZZ9K_STATUS_OK) {
      status = 7;
    }
  }
out:
  zz9k_close(ctx);
  mock_remove_hooks();
  mock_window_free(mapping, 0x08000000UL);
  return status == ZZ9K_STATUS_OK ? 0 : status;
}

int main(void)
{
  int r;

  printf("audio_ring_client_test: direct-ring client contract checks\n");
  r = test_gate();
  CHECK(r == 0, "capability gate declines old firmware only");
  r = test_wrapped_write_and_credits();
  CHECK(r == 0, "wrapped writes, credit adoption, revocation vocabulary");
  r = test_publish_ordering_and_heartbeat();
  CHECK(r == 0, "cursor publication after visibility; heartbeat refresh");
  r = test_client_walkthrough();
  CHECK(r == 0, "credit-paced client walkthrough over a wrapping ring");
  r = test_publication_visibility();
  CHECK(r == 0, "unpublished PCM is invisible; published periods consumed");
  r = test_ctrl_c_releases_slot();
  CHECK(r == 0, "Ctrl-C releases the grant under its generation");
  r = test_unstable_firmware_line();
  CHECK(r == 0, "torn firmware seqlock retries without mailbox help");
  r = test_revoked_generation_stops_client();
  CHECK(r == 0, "revoked generation stops the client and still releases");
  r = test_independent_contexts();
  CHECK(r == 0, "two contexts feed two slots without mailbox traffic");
  r = test_z2_second_client_refusal();
  CHECK(r == 0, "Zorro II second acquisition declines cleanly");
  r = test_z2_grant_inside_region_rejected();
  CHECK(r == 0, "grant inside a reported region fails closed");
  r = test_z2_pinned_grant_geometry();
  CHECK(r == 0, "pinned Z2 direct-region geometry validates");
  r = test_rate_lease_session();
  CHECK(r == 0, "rate lease grants contract 2 and paces source periods");
  r = test_mismatched_rate_grants_release();
  CHECK(r == 0, "mismatched rate grants fail closed and release");
  r = test_z3_pinned_grant_geometry();
  CHECK(r == 0, "pinned Z3 grant geometry maps and plays");

  if (g_failures != 0) {
    printf("audio_ring_client_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("audio_ring_client_test: all checks passed\n");
  return 0;
}
