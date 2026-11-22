/*
 * Host tests for the voice session (audio_voice_session.c): the ring, the
 * event queue, rendering into the fabric's voice slot, the status block,
 * reset/close/heartbeat, and uploads, driven through the module's public
 * entry points against the real compositor (audio_fabric.c,
 * audio_fabric_lease.c) with the shared fabric host seam
 * (fabric_test_common.c). xil_cache.h compiles in recording mode so the
 * cache-fidelity scenario can assert the exact ranges.
 *
 * Main loop model: one "period" is 20 ms. Each period the poll runs, then
 * the DMA advances one period and the compositor ISR runs. The XTime shim
 * (host_stubs/xtime_l.h) counts milliseconds in bench_clock.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_voice.h"
#include "audio_voice_session.h"
#include "fabric_test_common.h"
#include "sdk_mailbox.h"
#include "xil_cache.h"
#include "xtime_l.h"

uint64_t bench_clock;

uint32_t smp_local_irq_save(void);
void smp_local_irq_restore(uint32_t saved_i_bit);
uint32_t smp_local_irq_save(void) { return 0U; }
void smp_local_irq_restore(uint32_t saved_i_bit) { (void)saved_i_bit; }

#define RING_RECORDS 256U
#define RING_LEN (AUDIO_VOICE_RING_HEADER_BYTES + \
	RING_RECORDS * AUDIO_VOICE_RING_RECORD_BYTES)
#define STAGING_LEN 4096U
#define BANK_LEN 16384U

#define PEER_PCM 7777
#define LOOP_SAMPLES 50U   /* 960 mod 50 != 0: periods differ */
#define SAMPLE_ID 0U

static uint8_t g_ring[RING_LEN];
/* The firmware writes and flushes the status block: it sits on its own
 * cache lines. */
static uint8_t g_status[AUDIO_VOICE_STATUS_BYTES] __attribute__((aligned(32)));
static uint8_t g_status_spare[AUDIO_VOICE_STATUS_BYTES + 32U]
	__attribute__((aligned(32)));
static uint8_t g_staging[STAGING_LEN];
static uint8_t g_bank[BANK_LEN];
static int g_release_calls;
static uint16_t g_wc;       /* records written so far */
static uint32_t g_hb;       /* client heartbeat */
static struct audio_voice_session_grant g_grant;

static void release_hook(void *ctx)
{
	(void)ctx;
	g_release_calls++;
}

/* ---- client side: ring and status, big endian ---- */

static void put16(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t get16(const uint8_t *p)
{
	return ((uint32_t)p[0] << 8) | p[1];
}

static void rec(uint32_t stamp, uint8_t voice, uint8_t op, uint16_t flags,
	uint32_t arg0, uint32_t arg1)
{
	uint8_t *p = g_ring + AUDIO_VOICE_RING_HEADER_BYTES +
		(g_wc & (RING_RECORDS - 1U)) * AUDIO_VOICE_RING_RECORD_BYTES;

	put32(p, stamp);
	p[4] = voice;
	p[5] = op;
	put16(p + 6, flags);
	put32(p + 8, arg0);
	put32(p + 12, arg1);
	g_wc++;
}

static void publish(void)
{
	put16(g_ring + AUDIO_VOICE_RING_OFF_WRITE, g_wc);
}

static void beat(void)
{
	g_hb++;
	put32(g_ring + AUDIO_VOICE_RING_OFF_HEARTBEAT, g_hb);
}

static uint32_t st32(uint32_t off) { return get32(g_status + off); }
static uint32_t st16(uint32_t off) { return get16(g_status + off); }
static uint32_t st_playing(void) { return st32(AUDIO_VOICE_STATUS_OFF_PLAYING); }
static uint32_t st_horizon(void) { return st32(AUDIO_VOICE_STATUS_OFF_HORIZON); }
static uint32_t st_state(void) { return st16(AUDIO_VOICE_STATUS_OFF_STATE); }
static uint32_t st_underruns(void) { return st32(AUDIO_VOICE_STATUS_OFF_UNDERRUNS); }

/* ---- sample data: a 50-sample ramp, S16 big endian ---- */

static int16_t ramp(uint32_t i)
{
	return (int16_t)(1000 + 100 * (int)i);
}

/* What the engine outputs for ramp sample i at full volume. */
static int16_t ramp_out(uint32_t i)
{
	return (int16_t)(((int32_t)ramp(i % LOOP_SAMPLES) * 65535) >> 16);
}

static void staging_put_ramp(uint32_t offset)
{
	uint32_t i;

	for (i = 0U; i < LOOP_SAMPLES; i++) {
		uint16_t s = (uint16_t)ramp(i);

		g_staging[offset + 2U * i] = (uint8_t)(s >> 8);
		g_staging[offset + 2U * i + 1U] = (uint8_t)s;
	}
}

/* Upload the ramp (COPY + DEFINE) to bank offset 0 as sample 0. */
static uint16_t upload_ramp(uint32_t session)
{
	struct audio_voice_session_upload up;

	staging_put_ramp(0U);
	memset(&up, 0, sizeof(up));
	up.flags = AUDIO_VOICE_UPLOAD_COPY | AUDIO_VOICE_UPLOAD_DEFINE;
	up.staging_offset = 0U;
	up.bank_offset = 0U;
	up.length = 2U * LOOP_SAMPLES;
	up.sample_id = SAMPLE_ID;
	up.sample_offset = 0U;
	up.sample_length = 2U * LOOP_SAMPLES;
	up.format = AUDIO_VOICE_FMT_S16;
	return audio_voice_session_upload(session, &up);
}

/* The three records that start the looping ramp on a voice at stamp. */
static void rec_start_ramp(uint32_t stamp, uint8_t voice)
{
	rec(stamp, voice, AUDIO_VOICE_OP_SET,
	    AUDIO_VOICE_SET_RATE | AUDIO_VOICE_SET_VOLUME | AUDIO_VOICE_RATE_HZ,
	    48000U, 0xFFFFFFFFU);
	rec(stamp, voice, AUDIO_VOICE_OP_START, SAMPLE_ID, 0U, 0U);
	rec(stamp, voice, AUDIO_VOICE_OP_LOOP, 0U, 0U, 2U * LOOP_SAMPLES);
}

/* ---- session and fabric plumbing ---- */

static struct audio_voice_session_open open_params(uint32_t lead,
	uint32_t voices, uint32_t flags)
{
	struct audio_voice_session_open o;

	memset(&o, 0, sizeof(o));
	o.ring = g_ring;
	o.ring_len = RING_LEN;
	o.status = g_status;
	o.status_len = sizeof(g_status);
	o.staging = g_staging;
	o.staging_len = STAGING_LEN;
	o.bank = g_bank;
	o.bank_len = BANK_LEN;
	o.lead_frames = lead;
	o.flags = flags;
	o.voices = voices;
	o.release = release_hook;
	return o;
}

/* Fresh world: no session, empty fabric and buffers, clock at 1 s. */
static void fresh(void)
{
	audio_voice_session_shutdown();
	fabric_reset_state();
	g_dma_count = 0U;
	memset(g_ring, 0, sizeof(g_ring));
	memset(g_status, 0, sizeof(g_status));
	memset(g_staging, 0, sizeof(g_staging));
	memset(g_bank, 0, sizeof(g_bank));
	g_release_calls = 0;
	g_wc = 0U;
	g_hb = 0U;
	bench_clock = 1000U;
}

static uint16_t open_session(uint32_t lead, uint32_t voices)
{
	struct audio_voice_session_open o = open_params(lead, voices, 0U);

	return audio_voice_session_open(&o, &g_grant);
}

/* One 20 ms main-loop period: heartbeat (optional), poll, DMA, ISR. */
static void period(int with_beat)
{
	bench_clock += 20U;
	if (with_beat)
		beat();
	audio_voice_session_poll();
	dma_tick(1);
	audio_fabric_isr();
}

static void run(uint32_t n)
{
	while (n-- != 0U)
		period(1);
}

static const int16_t *tx_period(uint32_t index)
{
	return (const int16_t *)(g_fabric_tx + (index % AUDIO_NUM_PERIODS) *
		TICK_BYTES);
}

static uint32_t dma_period(void)
{
	return (g_dma_count / TICK_BYTES) % AUDIO_NUM_PERIODS;
}

static int period_all(const int16_t *p, int16_t value)
{
	uint32_t i;

	for (i = 0U; i < TICK_BYTES / 2U; i++) {
		if (p[i] != value)
			return 0;
	}
	return 1;
}

/* ---- peer: a constant lease on the mailbox slot ---- */

static struct {
	uint32_t sequence;
	uint32_t token;
	uint32_t generation;
} g_lease;

static void lease_publish(void)
{
	volatile struct SDKAudioRingProducerLine *p =
		(volatile struct SDKAudioRingProducerLine *)g_ring_control_a;
	struct audio_fabric_slot_state st;
	uint64_t write;
	uint32_t next = (g_lease.sequence + 2U) & ~1U;

	(void)audio_fabric_slot_state(AUDIO_FABRIC_SLOT_MAILBOX,
		SDK_AUDIO_METER_IDENTITY_MEDIA, 0, &st);
	write = st.consumed_bytes + RING_TEST_CAPACITY;
	g_lease.token++;
	put32((uint8_t *)p->sequence, next | 1U);
	put32((uint8_t *)p->generation, g_lease.generation);
	put32((uint8_t *)p->write_cursor_hi, (uint32_t)(write >> 32));
	put32((uint8_t *)p->write_cursor_lo, (uint32_t)write);
	put32((uint8_t *)p->heartbeat, g_lease.token);
	put32((uint8_t *)p->flags, 0U);
	put32((uint8_t *)p->sequence, next);
	g_lease.sequence = next;
}

/* Acquire the peer lease and run it until it is ACTIVE and mixing. */
static void lease_start(void)
{
	struct audio_fabric_ring_grant grant;
	uint32_t i;

	memset(&g_lease, 0, sizeof(g_lease));
	check(audio_fabric_ring_acquire(AUDIO_FABRIC_SLOT_MAILBOX,
		SDK_AUDIO_METER_IDENTITY_MEDIA, 128U, 0U, &grant) ==
		AUDIO_FABRIC_LEASE_OK, "peer: acquire", "");
	g_lease.generation = grant.generation;
	for (i = 0U; i + 1U < RING_TEST_CAPACITY; i += 2U) {
		g_ring_pcm_a[i] = (uint8_t)(uint16_t)PEER_PCM;
		g_ring_pcm_a[i + 1U] = (uint8_t)((uint16_t)PEER_PCM >> 8);
	}
	lease_publish();
	for (i = 0U; i < 4U; i++) {
		lease_publish();
		dma_tick(1);
		audio_fabric_isr();
	}
}

/* Period with the peer lease kept alive. */
static void peer_period(int with_beat)
{
	lease_publish();
	period(with_beat);
}

static void peer_run(uint32_t n)
{
	while (n-- != 0U)
		peer_period(1);
}

/* ---- A. open ---- */

static void scenario_open(void)
{
	struct audio_voice_session_open o;
	struct audio_voice_session_grant second;

	fresh();
	check(open_session(100U, 8U) == SDK_STATUS_OK, "open: valid", "");
	check(g_grant.session != 0U && g_grant.voices == 8U &&
	      g_grant.bank_bytes == BANK_LEN &&
	      g_grant.lead_floor_frames == 7680U &&
	      g_grant.lead_max_frames == 24000U &&
	      g_grant.ring_records == RING_RECORDS &&
	      g_grant.output_rate == 48000U && g_grant.max_samples == 256U,
	      "open: grant fields", fmt("session=%u voices=%u records=%u",
	      g_grant.session, g_grant.voices, g_grant.ring_records));
	check(g_grant.lead_frames == 7680U,
	      "open: lead below the floor is raised to the floor",
	      fmt("lead=%u", g_grant.lead_frames));
	check(audio_voice_session_active() &&
	      audio_fabric_output_busy() != 0,
	      "open: session active and the voice slot attached", "");
	check(st_state() == AUDIO_VOICE_STATE_OPEN &&
	      st32(AUDIO_VOICE_STATUS_OFF_SESSION) == g_grant.session,
	      "open: status published as open with the session id",
	      fmt("state=%u", st_state()));
	o = open_params(7680U, 4U, 0U);
	check(audio_voice_session_open(&o, &second) == SDK_STATUS_BUSY,
	      "open: second open is busy", "");
	check(audio_voice_session_close(g_grant.session + 1U) ==
	      SDK_STATUS_BAD_HANDLE && audio_voice_session_active(),
	      "open: close with a wrong session id changes nothing", "");
	check(audio_voice_session_close(g_grant.session) == SDK_STATUS_OK &&
	      !audio_voice_session_active() && g_release_calls == 1 &&
	      st_state() == AUDIO_VOICE_STATE_CLOSED,
	      "open: close releases once and publishes closed", "");
	check(open_session(30000U, 32U) == SDK_STATUS_OK &&
	      g_grant.lead_frames == 24000U && g_grant.voices == 32U,
	      "open: lead is capped at the maximum",
	      fmt("lead=%u", g_grant.lead_frames));
	check(audio_voice_session_close(g_grant.session) == SDK_STATUS_OK,
	      "open: close after cap", "");
	check(open_session(10000U, 1U) == SDK_STATUS_OK &&
	      g_grant.lead_frames == 10560U,
	      "open: lead is a whole number of periods",
	      fmt("lead=%u", g_grant.lead_frames));
	{
		uint32_t previous = g_grant.session;

		(void)audio_voice_session_close(g_grant.session);
		check(open_session(7680U, 1U) == SDK_STATUS_OK &&
		      g_grant.session != previous && g_grant.session != 0U,
		      "open: the session id changes on every open", "");
	}
}

static void refuse(const char *name, struct audio_voice_session_open o)
{
	struct audio_voice_session_grant grant;

	fresh();
	check(audio_voice_session_open(&o, &grant) == SDK_STATUS_BAD_REQUEST,
	      name, "");
	check(!audio_voice_session_active() && g_release_calls == 0 &&
	      audio_fabric_output_busy() == 0,
	      fmt("%s: nothing attached", name), "");
}

static void scenario_open_refused(void)
{
	struct audio_voice_session_open o;

	o = open_params(7680U, 4U, 0U);
	o.status_len = 63U;
	refuse("open refused: status block under 64 bytes", o);
	o = open_params(7680U, 4U, 0U);
	o.flags = 4U;
	refuse("open refused: unknown flag", o);
	o = open_params(7680U, 0U, 0U);
	refuse("open refused: zero voices", o);
	o = open_params(7680U, 33U, 0U);
	refuse("open refused: 33 voices", o);
	o = open_params(7680U, 4U, 0U);
	o.ring_len = AUDIO_VOICE_RING_HEADER_BYTES +
		15U * AUDIO_VOICE_RING_RECORD_BYTES;
	refuse("open refused: ring under 16 records", o);
	o = open_params(7680U, 4U, 0U);
	o.bank_len = 0U;
	refuse("open refused: empty bank", o);
	o = open_params(7680U, 4U, 0U);
	o.bank_len = AUDIO_VOICE_BANK_MAX_BYTES + 1U;
	refuse("open refused: bank over 32 MiB", o);
	o = open_params(7680U, 4U, 0U);
	o.status = g_status_spare + 16U;
	refuse("open refused: status block not 32-byte aligned", o);
	o = open_params(7680U, 4U, 0U);
	o.status = g_status_spare + 4U;
	refuse("open refused: status block at a word boundary", o);
	check(audio_voice_session_check(RING_LEN, (uintptr_t)g_status, 64U, 1U,
	      1U, 3U) == SDK_STATUS_OK &&
	      audio_voice_session_check(RING_LEN, (uintptr_t)g_status, 64U, 1U,
	      1U, 8U) == SDK_STATUS_BAD_REQUEST &&
	      audio_voice_session_check(RING_LEN, (uintptr_t)g_status + 16U,
	      64U, 1U, 1U, 3U) == SDK_STATUS_BAD_REQUEST,
	      "open refused: the side-effect-free check agrees", "");
}

static void scenario_ring_geometry(void)
{
	struct audio_voice_session_open o;
	struct audio_voice_session_grant grant;

	fresh();
	o = open_params(7680U, 4U, 0U);
	o.ring_len = AUDIO_VOICE_RING_HEADER_BYTES +
		16U * AUDIO_VOICE_RING_RECORD_BYTES;
	check(audio_voice_session_open(&o, &grant) == SDK_STATUS_OK &&
	      grant.ring_records == 16U, "ring: minimum ring is 16 records",
	      fmt("records=%u", grant.ring_records));
	(void)audio_voice_session_close(grant.session);
	o.ring_len = AUDIO_VOICE_RING_HEADER_BYTES +
		31U * AUDIO_VOICE_RING_RECORD_BYTES;
	check(audio_voice_session_open(&o, &grant) == SDK_STATUS_OK &&
	      grant.ring_records == 16U,
	      "ring: capacity is the largest power of two that fits",
	      fmt("records=%u", grant.ring_records));
	(void)audio_voice_session_close(grant.session);
	o.ring_len = 16U + 100000U * 16U;
	check(audio_voice_session_open(&o, &grant) == SDK_STATUS_OK &&
	      grant.ring_records == 4096U,
	      "ring: capacity is capped at 4096 records",
	      fmt("records=%u", grant.ring_records));
}

/* ---- B. records at their stamps, cursor discipline ---- */

/* Look for the TX period holding the stamp frame and verify the exact
 * sample where the voice begins. */
static int find_exact_start(uint32_t stamp)
{
	uint32_t index = stamp % AUDIO_VOICE_PERIOD_FRAMES;
	uint32_t i;

	for (i = 0U; i < AUDIO_NUM_PERIODS; i++) {
		const int16_t *p = tx_period(i);
		uint32_t f;

		if (p[0] != 0 || p[2U * (AUDIO_VOICE_PERIOD_FRAMES - 1U)] == 0)
			continue;
		for (f = 0U; f < AUDIO_VOICE_PERIOD_FRAMES; f++) {
			int16_t want = f < index ? 0 : ramp_out(f - index);

			if (p[2U * f] != want || p[2U * f + 1U] != want)
				return 0;
		}
		return 1;
	}
	return 0;
}

/* ---- B2. ring wrap, cursor wrap, overrun ---- */

/* Records whose stamp is behind every frame: applied on the next render
 * and counted late. */
#define STALE_STAMP 0xFFFFFFFFU

static void rec_reject(uint8_t voice)
{
	rec(STALE_STAMP, voice, AUDIO_VOICE_OP_START, 77U, 0U, 0U);
}

static void rec_stop(uint8_t voice)
{
	rec(STALE_STAMP, voice, AUDIO_VOICE_OP_STOP, 0U, 0U, 0U);
}

static uint32_t st_late(void)
{
	return st32(AUDIO_VOICE_STATUS_OFF_LATE);
}

static uint32_t st_rejected(void)
{
	return st32(AUDIO_VOICE_STATUS_OFF_REJECTED);
}

static uint32_t st_dropped(void)
{
	return st32(AUDIO_VOICE_STATUS_OFF_DROPPED);
}

/* A published batch that straddles the last ring slot is read in two
 * pieces, tail then head, each invalidated, and every record is applied
 * exactly once. Rejected counts the first 56 records of the second batch
 * (the tail) and all of the first; the head slots, which held rejected
 * records of the first batch, now hold STOPs that must not be replayed as
 * their old contents. */
static void scenario_ring_wrap(void)
{
	uint32_t i;
	unsigned ring_ops = 0U;
	uintptr_t first_adr = 0U;
	unsigned long first_len = 0UL;
	uintptr_t second_adr = 0U;
	unsigned long second_len = 0UL;
	int kind;
	uintptr_t adr;
	unsigned long len;

	fresh();
	check(open_session(7680U, 4U) == SDK_STATUS_OK, "wrap: open", "");
	for (i = 0U; i < 200U; i++)
		rec_reject(0U);
	publish();
	run(4);
	check(st_rejected() == 200U && st_late() == 200U &&
	      st_dropped() == 0U && st16(AUDIO_VOICE_STATUS_OFF_READ) == 200U,
	      "wrap: the first batch is applied once",
	      fmt("rejected=%u late=%u read=%u", st_rejected(), st_late(),
	      st16(AUDIO_VOICE_STATUS_OFF_READ)));
	for (i = 0U; i < 56U; i++)
		rec_reject(1U);
	for (i = 0U; i < 44U; i++)
		rec_stop(2U);
	publish();
	xil_cache_mock_reset();
	xil_cache_mock_set_enabled(1);
	period(1);
	xil_cache_mock_set_enabled(0);
	for (i = 0U; xil_cache_mock_op(i, &kind, &adr, &len); i++) {
		if (kind != XIL_CACHE_MOP_INVALIDATE ||
		    adr < (uintptr_t)g_ring + AUDIO_VOICE_RING_HEADER_BYTES ||
		    adr >= (uintptr_t)g_ring + RING_LEN)
			continue;
		if (ring_ops == 0U) {
			first_adr = adr;
			first_len = len;
		} else if (ring_ops == 1U) {
			second_adr = adr;
			second_len = len;
		}
		ring_ops++;
	}
	check(ring_ops == 2U &&
	      first_adr == (uintptr_t)g_ring + AUDIO_VOICE_RING_HEADER_BYTES +
		200U * AUDIO_VOICE_RING_RECORD_BYTES &&
	      first_len == 56U * AUDIO_VOICE_RING_RECORD_BYTES &&
	      second_adr == (uintptr_t)g_ring + AUDIO_VOICE_RING_HEADER_BYTES &&
	      second_len == 44U * AUDIO_VOICE_RING_RECORD_BYTES,
	      "wrap: a straddling batch invalidates the tail, then the head",
	      fmt("ops=%u first=%lu+%lu second=%lu+%lu", ring_ops,
	      (unsigned long)(first_adr - (uintptr_t)g_ring), first_len,
	      (unsigned long)(second_adr - (uintptr_t)g_ring), second_len));
	run(4);
	check(st_rejected() == 256U && st_late() == 300U &&
	      st_dropped() == 0U && st16(AUDIO_VOICE_STATUS_OFF_READ) == 300U,
	      "wrap: every record of the straddling batch is applied once",
	      fmt("rejected=%u late=%u read=%u", st_rejected(), st_late(),
	      st16(AUDIO_VOICE_STATUS_OFF_READ)));
}

/* The 16-bit write cursor wraps from 65535 to 0. A session opened on a ring
 * whose cursor already stands near the top starts from it: the records
 * written before the open are not replayed. */
static void scenario_cursor_wrap(void)
{
	uint32_t i;

	fresh();
	g_wc = 65520U;
	for (i = 0U; i < 10U; i++)
		rec_reject(0U);
	publish();
	check(g_wc == 65530U, "cursor wrap: ring parked near the top", "");
	check(open_session(7680U, 4U) == SDK_STATUS_OK, "cursor wrap: open", "");
	check(st16(AUDIO_VOICE_STATUS_OFF_READ) == 65530U,
	      "cursor wrap: open takes the write cursor as the read baseline",
	      fmt("read=%u", st16(AUDIO_VOICE_STATUS_OFF_READ)));
	run(3);
	check(st_state() == AUDIO_VOICE_STATE_OPEN && st_rejected() == 0U &&
	      st_late() == 0U && st_dropped() == 0U &&
	      st16(AUDIO_VOICE_STATUS_OFF_READ) == 65530U,
	      "cursor wrap: records written before the open are not replayed",
	      fmt("state=%u rejected=%u read=%u", st_state(), st_rejected(),
	      st16(AUDIO_VOICE_STATUS_OFF_READ)));
	for (i = 0U; i < 12U; i++)
		rec_reject(0U);
	publish();
	check(g_wc == 6U, "cursor wrap: the writer cursor wrapped", "");
	run(4);
	check(st_state() == AUDIO_VOICE_STATE_LIVE && st_rejected() == 12U &&
	      st_late() == 12U && st_dropped() == 0U &&
	      st16(AUDIO_VOICE_STATUS_OFF_READ) == 6U,
	      "cursor wrap: records across 65535 to 0 are applied once",
	      fmt("state=%u rejected=%u late=%u dropped=%u read=%u", st_state(),
	      st_rejected(), st_late(), st_dropped(),
	      st16(AUDIO_VOICE_STATUS_OFF_READ)));
}

/* A client that moves its write cursor by more than the ring holds has
 * wrapped over records the firmware never took: none of that span can be
 * trusted, so none fires; all of it is counted and the cursor catches up. */
static void scenario_ring_overrun(void)
{
	uint32_t i;

	fresh();
	check(open_session(7680U, 4U) == SDK_STATUS_OK, "overrun: open", "");
	for (i = 0U; i < 10U; i++)
		rec_reject(0U);
	publish();
	run(3);
	check(st_rejected() == 10U && st_late() == 10U,
	      "overrun: records before the jump are applied", "");
	for (i = 0U; i < RING_RECORDS + 44U; i++)
		rec_reject(1U);
	publish();
	run(4);
	check(st_dropped() == RING_RECORDS + 44U &&
	      st16(AUDIO_VOICE_STATUS_OFF_READ) == g_wc,
	      "overrun: a jump past the ring is dropped and the cursor catches up",
	      fmt("dropped=%u read=%u wc=%u", st_dropped(),
	      st16(AUDIO_VOICE_STATUS_OFF_READ), g_wc));
	check(st_rejected() == 10U && st_late() == 10U,
	      "overrun: no record of the lapped span fires",
	      fmt("rejected=%u late=%u", st_rejected(), st_late()));
	/* The ring works again from there. */
	rec_reject(2U);
	publish();
	run(3);
	check(st_rejected() == 11U && st_dropped() == RING_RECORDS + 44U,
	      "overrun: records after the jump are applied",
	      fmt("rejected=%u", st_rejected()));
}

/* ---- B3. open flags reach the engine ---- */

#define FLAG_PERIOD 428U
#define FLAG_FRAMES 200U

/* The left channel the engine produces for the ramp at period 428 for
 * `frames` frames from the start of a period. */
static int16_t flag_expected(uint32_t clock, int linear, uint32_t frame)
{
	uint32_t hz = clock / FLAG_PERIOD;
	uint64_t step = ((uint64_t)hz << 16) / 48000U;
	uint64_t total = step * frame;
	uint32_t pos = (uint32_t)(total >> 16);
	int32_t frac = (int32_t)(total & 0xFFFFU);
	int32_t a = ramp(pos);
	int32_t v = a;

	if (linear)
		v = a + (int32_t)(((int64_t)(ramp(pos + 1U) - a) * frac) >> 16);
	return (int16_t)((v * 65535) >> 16);
}

static int flag_period_matches(const int16_t *p, uint32_t clock, int linear)
{
	uint32_t f;

	for (f = 0U; f < FLAG_FRAMES; f++) {
		int16_t want = flag_expected(clock, linear, f);

		if (p[2U * f] != want || p[2U * f + 1U] != want)
			return 0;
	}
	return 1;
}

/* NTSC and LINEAR in the open flags select the engine's clock (period to
 * rate) and interpolation: each of the four combinations produces its own
 * samples and none of the other three. */
static void scenario_open_flags(void)
{
	static const uint32_t clocks[2] = {
		AUDIO_VOICE_CLOCK_PAL, AUDIO_VOICE_CLOCK_NTSC
	};
	uint32_t combo;

	for (combo = 0U; combo < 4U; combo++) {
		struct audio_voice_session_open o;
		uint32_t flags = 0U;
		int seen[4] = { 0, 0, 0, 0 };
		uint32_t i;
		uint32_t k;

		if ((combo & 1U) != 0U)
			flags |= AUDIO_VOICE_OPEN_NTSC;
		if ((combo & 2U) != 0U)
			flags |= AUDIO_VOICE_OPEN_LINEAR;
		fresh();
		o = open_params(7680U, 4U, flags);
		check(audio_voice_session_open(&o, &g_grant) == SDK_STATUS_OK,
		      "flags: open", fmt("flags=%u", flags));
		check(upload_ramp(g_grant.session) == SDK_STATUS_OK,
		      "flags: upload", "");
		rec(960U * 2U, 0U, AUDIO_VOICE_OP_SET,
		    AUDIO_VOICE_SET_RATE | AUDIO_VOICE_SET_VOLUME, FLAG_PERIOD,
		    0xFFFFFFFFU);
		rec(960U * 2U, 0U, AUDIO_VOICE_OP_START, SAMPLE_ID, 0U, 0U);
		rec(960U * 2U, 0U, AUDIO_VOICE_OP_LOOP, 0U, 0U,
		    2U * LOOP_SAMPLES);
		publish();
		for (i = 0U; i < 12U; i++) {
			period(1);
			for (k = 0U; k < AUDIO_NUM_PERIODS; k++) {
				uint32_t c;

				for (c = 0U; c < 4U; c++) {
					if (flag_period_matches(tx_period(k),
					    clocks[c & 1U], (c & 2U) != 0U))
						seen[c] = 1;
				}
			}
		}
		for (k = 0U; k < 4U; k++)
			check(seen[k] == (k == combo),
			      "flags: the open flags choose clock and "
			      "interpolation",
			      fmt("opened %u, output matches %u: %d", combo, k,
			      seen[k]));
	}
}

static void scenario_stamps(void)
{
	uint32_t i;
	int found = 0;

	fresh();
	check(open_session(7680U, 4U) == SDK_STATUS_OK, "stamps: open", "");
	check(upload_ramp(g_grant.session) == SDK_STATUS_OK,
	      "stamps: upload", "");
	/* Records are in the ring but the cursor has not moved. */
	rec_start_ramp(4805U, 0U);
	run(4);
	check(st_state() == AUDIO_VOICE_STATE_OPEN &&
	      st16(AUDIO_VOICE_STATUS_OFF_READ) == 0U &&
	      st_horizon() == 0U,
	      "stamps: records beyond the cursor are ignored",
	      fmt("state=%u read=%u horizon=%u", st_state(),
	      st16(AUDIO_VOICE_STATUS_OFF_READ), st_horizon()));
	check(period_all(tx_period(0), (int16_t)0xAAAA) &&
	      period_all(tx_period(3), (int16_t)0xAAAA),
	      "stamps: an open session leaves the TX ring alone", "");
	publish();
	period(1);
	check(st_state() == AUDIO_VOICE_STATE_LIVE &&
	      st16(AUDIO_VOICE_STATUS_OFF_READ) == g_wc,
	      "stamps: the first record moves open to live and the cursor",
	      fmt("state=%u read=%u", st_state(),
	      st16(AUDIO_VOICE_STATUS_OFF_READ)));
	for (i = 0U; i < 12U; i++) {
		if (find_exact_start(4805U))
			found = 1;
		period(1);
	}
	check(found, "stamps: the voice starts on exactly its stamp frame",
	      "");
}

static void scenario_lead(uint32_t lead)
{
	uint32_t i;
	int bounded = 1;
	int moved = 0;
	uint32_t last = 0U;

	fresh();
	check(open_session(lead, 4U) == SDK_STATUS_OK, "lead: open", "");
	(void)upload_ramp(g_grant.session);
	rec_start_ramp(960U * 100U, 0U);
	publish();
	for (i = 0U; i < 90U; i++) {
		period(1);
		if (st_horizon() - st_playing() > g_grant.lead_frames)
			bounded = 0;
		if (st_horizon() != last)
			moved = 1;
		last = st_horizon();
	}
	check(bounded, "lead: the engine never renders past playing + lead",
	      fmt("horizon=%u playing=%u", st_horizon(), st_playing()));
	check(moved && st_playing() > 0U &&
	      st_horizon() - st_playing() == g_grant.lead_frames,
	      "lead: the engine keeps the full lead and the playing frame moves",
	      fmt("horizon=%u playing=%u", st_horizon(), st_playing()));
	check(st_underruns() == 0U, "lead: steady state counts no underrun",
	      fmt("underruns=%u", st_underruns()));
}

static void scenario_stall(void)
{
	uint32_t i;
	uint32_t before;
	uint32_t after;

	fresh();
	check(open_session(7680U, 4U) == SDK_STATUS_OK, "stall: open", "");
	(void)upload_ramp(g_grant.session);
	rec_start_ramp(960U * 400U, 0U);
	publish();
	run(20);
	check(st_underruns() == 0U, "stall: clean before the stall",
	      fmt("underruns=%u", st_underruns()));
	/* One period late is inside the margin (lead 8 - fill-ahead 6). */
	dma_tick(1);
	audio_fabric_isr();
	run(4);
	check(st_underruns() == 0U,
	      "stall: a stall inside lead minus fill-ahead costs nothing",
	      fmt("underruns=%u", st_underruns()));
	/* The poll stops for 6 periods (the clock stays under 2 s). */
	for (i = 0U; i < 6U; i++) {
		bench_clock += 20U;
		dma_tick(1);
		audio_fabric_isr();
	}
	period(1);
	check(st_underruns() > 0U, "stall: a longer stall counts underruns",
	      fmt("underruns=%u", st_underruns()));
	/* The first ISR after the poll finds the frontier caught up and bumps
	 * once more (the compositor's rebase); then it is steady. */
	run(3);
	before = st_underruns();
	run(12);
	after = st_underruns();
	check(after == before, "stall: the session recovers and stops counting",
	      fmt("underruns %u -> %u", before, after));
	check(st_horizon() - st_playing() == g_grant.lead_frames,
	      "stall: the lead is rebuilt after the stall",
	      fmt("horizon=%u playing=%u", st_horizon(), st_playing()));
}

/* ---- C. status counters ---- */

static void scenario_status(void)
{
	uint32_t i;

	fresh();
	check(open_session(7680U, 4U) == SDK_STATUS_OK, "status: open", "");
	(void)upload_ramp(g_grant.session);
	rec_start_ramp(960U * 2U, 0U);
	publish();
	run(10);
	check(st16(AUDIO_VOICE_STATUS_OFF_READ) == 3U &&
	      st32(AUDIO_VOICE_STATUS_OFF_LATE) == 0U &&
	      st32(AUDIO_VOICE_STATUS_OFF_REJECTED) == 0U &&
	      st32(AUDIO_VOICE_STATUS_OFF_DROPPED) == 0U,
	      "status: counters start at zero, read cursor follows",
	      fmt("read=%u", st16(AUDIO_VOICE_STATUS_OFF_READ)));
	/* A record whose frame is already rendered is late. */
	rec(0U, 0U, AUDIO_VOICE_OP_STOP, 0U, 0U, 0U);
	publish();
	run(2);
	check(st32(AUDIO_VOICE_STATUS_OFF_LATE) == 1U &&
	      st16(AUDIO_VOICE_STATUS_OFF_READ) == 4U,
	      "status: a late record is counted",
	      fmt("late=%u", st32(AUDIO_VOICE_STATUS_OFF_LATE)));
	/* Undefined sample, and a voice above the grant, are rejected. */
	rec(st_horizon() + 960U, 1U, AUDIO_VOICE_OP_START, 77U, 0U, 0U);
	rec(st_horizon() + 960U, 9U, AUDIO_VOICE_OP_STOP, 0U, 0U, 0U);
	publish();
	run(6);
	check(st32(AUDIO_VOICE_STATUS_OFF_REJECTED) == 2U,
	      "status: rejected records are counted",
	      fmt("rejected=%u", st32(AUDIO_VOICE_STATUS_OFF_REJECTED)));
	check(st32(AUDIO_VOICE_STATUS_OFF_SESSION) == g_grant.session &&
	      st_horizon() > st_playing(),
	      "status: session id and horizon", "");

	/* Overflow of the card-side queue: future records that are never
	 * consumed fill it, then are dropped. */
	fresh();
	check(open_session(7680U, 4U) == SDK_STATUS_OK, "dropped: open", "");
	for (i = 0U; i < 17U; i++) {
		uint32_t n;

		for (n = 0U; n < RING_RECORDS; n++)
			rec(0x40000000U, 0U, AUDIO_VOICE_OP_MASTER, 0U, 256U,
			    0U);
		publish();
		period(1);
	}
	check(st32(AUDIO_VOICE_STATUS_OFF_DROPPED) ==
	      17U * RING_RECORDS - AUDIO_VOICE_QUEUE_RECORDS &&
	      st16(AUDIO_VOICE_STATUS_OFF_READ) == (uint16_t)g_wc,
	      "dropped: records over the queue are dropped and counted",
	      fmt("dropped=%u read=%u", st32(AUDIO_VOICE_STATUS_OFF_DROPPED),
	      st16(AUDIO_VOICE_STATUS_OFF_READ)));
}

/* ---- D. reset ---- */

/* Periods strictly ahead of the DMA position that differ from peer-only. */
static uint32_t voice_periods_ahead(void)
{
	uint32_t pos = dma_period();
	uint32_t count = 0U;
	uint32_t k;

	for (k = 1U; k <= 5U; k++) {
		if (!period_all(tx_period(pos + k), PEER_PCM))
			count++;
	}
	return count;
}

static void scenario_reset(void)
{
	uint32_t i;
	uint32_t horizon_before;
	uint32_t playing_before;

	fresh();
	g_dma_count = 0U;
	lease_start();
	check(open_session(7680U, 4U) == SDK_STATUS_OK, "reset: open", "");
	(void)upload_ramp(g_grant.session);
	rec_start_ramp(960U * 2U, 0U);
	publish();
	peer_run(20);
	check(voice_periods_ahead() >= 4U,
	      "reset: the voice is mixed into the queued periods",
	      fmt("voice periods=%u", voice_periods_ahead()));
	/* Future events: one batch already taken by the poll, one still in
	 * the ring. */
	rec_start_ramp(st_playing() + 960U * 20U, 1U);
	publish();
	peer_period(1);
	rec_start_ramp(st_playing() + 960U * 20U, 2U);
	publish();
	horizon_before = st_horizon();
	playing_before = st_playing();
	check(audio_voice_session_reset(g_grant.session + 1U) ==
	      SDK_STATUS_BAD_HANDLE, "reset: wrong session id", "");
	check(audio_voice_session_reset(g_grant.session) == SDK_STATUS_OK,
	      "reset: ok", "");
	check(st16(AUDIO_VOICE_STATUS_OFF_READ) == g_wc,
	      "reset: records up to the write cursor are dropped",
	      fmt("read=%u wc=%u", st16(AUDIO_VOICE_STATUS_OFF_READ), g_wc));
	check(st_playing() >= playing_before && st_horizon() > 0U,
	      "reset: the timeline keeps counting",
	      fmt("playing %u -> %u horizon %u -> %u", playing_before,
	      st_playing(), horizon_before, st_horizon()));
	/* One compositor pass rebuilds the queue: only the current period
	 * may still carry the voice. */
	peer_period(1);
	check(voice_periods_ahead() == 0U,
	      "reset: queued periods hold only peer audio",
	      fmt("voice periods ahead=%u", voice_periods_ahead()));
	/* The future records never fire, and the voice stays silent. */
	for (i = 0U; i < 40U; i++) {
		peer_period(1);
		if (voice_periods_ahead() != 0U)
			break;
	}
	check(i == 40U, "reset: queued future records are gone",
	      fmt("voice appeared after %u periods", i));
	check(st_underruns() == 0U && st_state() == AUDIO_VOICE_STATE_LIVE,
	      "reset: no underrun, session stays live",
	      fmt("underruns=%u", st_underruns()));
	check(st_horizon() - st_playing() == g_grant.lead_frames,
	      "reset: the lead is restored",
	      fmt("horizon=%u playing=%u", st_horizon(), st_playing()));
	/* Samples survive a reset: a new start plays. */
	rec_start_ramp(st_playing() + g_grant.lead_frames, 0U);
	publish();
	peer_run(14);
	check(voice_periods_ahead() != 0U,
	      "reset: samples survive, a new start plays", "");
	/* Reset before the session is live drops the pending records. */
	fresh();
	check(open_session(7680U, 4U) == SDK_STATUS_OK, "reset open: open", "");
	rec_start_ramp(0U, 0U);
	publish();
	check(audio_voice_session_reset(g_grant.session) == SDK_STATUS_OK,
	      "reset open: ok", "");
	run(3);
	check(st_state() == AUDIO_VOICE_STATE_OPEN &&
	      st16(AUDIO_VOICE_STATUS_OFF_READ) == g_wc,
	      "reset open: the dropped records never open the session",
	      fmt("state=%u", st_state()));
}

/* ---- E. staged periods survive a peer detach ---- */

static void scenario_peer_detach(uint32_t lead)
{
	static int16_t before[AUDIO_NUM_PERIODS][TICK_BYTES / 2U];
	uint32_t pos;
	uint32_t k;
	uint32_t i;
	int exact = 1;
	int voice_present = 0;

	fresh();
	lease_start();
	check(open_session(lead, 4U) == SDK_STATUS_OK, "detach: open", "");
	(void)upload_ramp(g_grant.session);
	rec_start_ramp(960U * 2U, 0U);
	publish();
	peer_run(60);
	for (i = 0U; i < AUDIO_NUM_PERIODS; i++)
		memcpy(before[i], tx_period(i), TICK_BYTES);
	/* Four queued periods will be rebuilt; the fifth is filled fresh. */
	check(voice_periods_ahead() >= 4U,
	      "detach: voice and peer mixed before the release", "");
	check(audio_fabric_ring_release(AUDIO_FABRIC_SLOT_MAILBOX,
		g_lease.generation) == AUDIO_FABRIC_LEASE_OK,
	      "detach: release the peer lease", "");
	period(1);
	/* The DMA moved one period: the queue is rebuilt from the voice
	 * slot's staged periods alone, byte for byte what it mixed before
	 * minus the peer. */
	pos = dma_period();
	for (k = 1U; k <= 4U; k++) {
		const int16_t *now = tx_period(pos + k);
		uint32_t s;

		/* The same period before the release held voice + peer. */
		for (s = 0U; s < TICK_BYTES / 2U; s++) {
			int32_t want = (int32_t)before[(pos + k) %
				AUDIO_NUM_PERIODS][s] - PEER_PCM;

			if (now[s] != want)
				exact = 0;
			if (now[s] != 0)
				voice_present = 1;
		}
	}
	check(voice_present && exact,
	      "detach: rebuilt periods still carry the staged voice exactly",
	      fmt("present=%d exact=%d", voice_present, exact));
	run(6);
	check(st_underruns() == 0U,
	      "detach: the voice slot keeps playing without underruns",
	      fmt("underruns=%u", st_underruns()));
}

/* ---- F. heartbeat, shutdown ---- */

static void scenario_heartbeat(void)
{
	uint32_t i;

	fresh();
	lease_start();
	check(open_session(7680U, 4U) == SDK_STATUS_OK, "heartbeat: open", "");
	(void)upload_ramp(g_grant.session);
	rec_start_ramp(960U * 2U, 0U);
	publish();
	/* A client that beats every 500 ms stays open for 6 s. */
	for (i = 0U; i < 300U; i++) {
		bench_clock += 20U;
		if (i % 25U == 0U || i == 299U)
			beat();
		lease_publish();
		audio_voice_session_poll();
		dma_tick(1);
		audio_fabric_isr();
	}
	check(audio_voice_session_active(),
	      "heartbeat: a client beating every 500 ms stays open", "");
	/* Then silence: open at 1.9 s, closed at 2 s. */
	for (i = 0U; i < 95U; i++) {
		bench_clock += 20U;
		lease_publish();
		audio_voice_session_poll();
		dma_tick(1);
		audio_fabric_isr();
	}
	check(audio_voice_session_active() && g_release_calls == 0,
	      "heartbeat: still open just under 2 s", "");
	for (i = 0U; i < 6U; i++) {
		bench_clock += 20U;
		lease_publish();
		audio_voice_session_poll();
		dma_tick(1);
		audio_fabric_isr();
	}
	check(!audio_voice_session_active() && g_release_calls == 1 &&
	      st_state() == AUDIO_VOICE_STATE_CLOSED,
	      "heartbeat: 2 s without a change closes the session",
	      fmt("active=%d releases=%d", audio_voice_session_active(),
	      g_release_calls));
	/* The slot is detached: a peer keeps playing, no voice audio. */
	lease_publish();
	period(0);
	peer_run(2);
	check(voice_periods_ahead() == 0U,
	      "heartbeat: the voice slot is detached, the peer plays on", "");
	/* Closed means the poll is inert. */
	period(1);
	check(g_release_calls == 1, "heartbeat: release is called once", "");

	/* Alone on the fabric, closing hands the output back. */
	fresh();
	check(open_session(7680U, 4U) == SDK_STATUS_OK, "heartbeat 2: open", "");
	for (i = 0U; i < 101U; i++) {
		bench_clock += 20U;
		audio_voice_session_poll();
	}
	check(!audio_voice_session_active() &&
	      audio_fabric_output_busy() == 0,
	      "heartbeat: closing the last producer idles the fabric", "");
}

static void scenario_warm_reset(void)
{
	fresh();
	lease_start();
	check(open_session(7680U, 4U) == SDK_STATUS_OK, "warm: open", "");
	(void)upload_ramp(g_grant.session);
	rec_start_ramp(960U * 2U, 0U);
	publish();
	peer_run(12);
	check(voice_periods_ahead() != 0U, "warm: playing", "");
	/* The path sdk_mailbox_init takes before the tables are wiped. */
	audio_voice_session_shutdown();
	check(!audio_voice_session_active() && g_release_calls == 1 &&
	      st_state() == AUDIO_VOICE_STATE_CLOSED,
	      "warm: the session is closed and released", "");
	peer_period(1);
	check(voice_periods_ahead() == 0U,
	      "warm: the slot is detached and its queue rebuilt", "");
	audio_voice_session_shutdown();
	check(g_release_calls == 1, "warm: shutdown twice releases once", "");
	check(audio_voice_session_close(1U) == SDK_STATUS_BAD_HANDLE &&
	      audio_voice_session_reset(1U) == SDK_STATUS_BAD_HANDLE,
	      "warm: no session, no handle", "");
}

/* ---- G. cache fidelity ---- */

static void scenario_cache(void)
{
	struct audio_voice_session_upload up;
	uint32_t i;
	uintptr_t last_period = 0U;
	int kind;
	uintptr_t adr;
	unsigned long len;
	int saw_header = 0;
	int saw_records = 0;
	int saw_status = 0;
	unsigned staged_ops = 0U;
	int contiguous = 1;

	fresh();
	check(open_session(7680U, 4U) == SDK_STATUS_OK, "cache: open", "");
	(void)upload_ramp(g_grant.session);
	rec_start_ramp(960U * 3U, 0U);
	publish();

	xil_cache_mock_reset();
	xil_cache_mock_set_enabled(1);
	bench_clock += 20U;
	beat();
	audio_voice_session_poll();
	xil_cache_mock_set_enabled(0);
	for (i = 0U; xil_cache_mock_op(i, &kind, &adr, &len); i++) {
		if (kind == XIL_CACHE_MOP_INVALIDATE && adr == (uintptr_t)g_ring &&
		    len == AUDIO_VOICE_RING_HEADER_BYTES)
			saw_header = 1;
		if (kind == XIL_CACHE_MOP_INVALIDATE &&
		    adr == (uintptr_t)g_ring + AUDIO_VOICE_RING_HEADER_BYTES &&
		    len == 3U * AUDIO_VOICE_RING_RECORD_BYTES)
			saw_records = saw_header;   /* after the header */
		if (kind == XIL_CACHE_MOP_FLUSH && adr == (uintptr_t)g_status &&
		    len == AUDIO_VOICE_STATUS_BYTES)
			saw_status = saw_records;   /* after the records */
		/* A staged period: a flush outside the TX ring. */
		if (kind == XIL_CACHE_MOP_FLUSH && len == TICK_BYTES &&
		    (adr < (uintptr_t)g_fabric_tx ||
		     adr >= (uintptr_t)g_fabric_tx + AUDIO_TX_BUFFER_SIZE)) {
			if (staged_ops != 0U && adr != last_period + TICK_BYTES)
				contiguous = 0;
			last_period = adr;
			staged_ops++;
		}
	}
	check(saw_header, "cache: the ring header is invalidated before the "
	      "cursor is read", "");
	check(saw_records, "cache: the new record range is invalidated "
	      "before the records are read", "");
	check(saw_status, "cache: the status block is flushed after it is "
	      "written", "");
	check(staged_ops == st_horizon() / AUDIO_VOICE_PERIOD_FRAMES &&
	      contiguous && staged_ops != 0U,
	      "cache: every staged period is flushed",
	      fmt("flushes=%u horizon=%u", staged_ops, st_horizon()));

	/* Upload: the staging source is invalidated before the copy. */
	staging_put_ramp(100U);
	memset(&up, 0, sizeof(up));
	up.flags = AUDIO_VOICE_UPLOAD_COPY;
	up.staging_offset = 100U;
	up.bank_offset = 400U;
	up.length = 2U * LOOP_SAMPLES;
	xil_cache_mock_reset();
	xil_cache_mock_set_enabled(1);
	check(audio_voice_session_upload(g_grant.session, &up) ==
	      SDK_STATUS_OK, "cache: upload ok", "");
	xil_cache_mock_set_enabled(0);
	check(g_xil_cache_mock.count >= 1U, "cache: upload logged", "");
	check(xil_cache_mock_op(0U, &kind, &adr, &len) &&
	      kind == XIL_CACHE_MOP_INVALIDATE &&
	      adr == (uintptr_t)g_staging + 100U &&
	      len == 2U * LOOP_SAMPLES,
	      "cache: the upload source is invalidated before the copy",
	      fmt("ops=%u", g_xil_cache_mock.count));
	check(memcmp(g_bank + 400U, g_staging + 100U, 2U * LOOP_SAMPLES) == 0,
	      "cache: the bytes arrived", "");
}

/* ---- H. uploads ---- */

static void scenario_upload(void)
{
	struct audio_voice_session_upload up;
	static uint8_t bank_before[BANK_LEN];
	uint32_t i;
	int16_t seen = 0;

	fresh();
	check(open_session(7680U, 4U) == SDK_STATUS_OK, "upload: open", "");
	memset(&up, 0, sizeof(up));
	check(audio_voice_session_upload(g_grant.session + 1U, &up) ==
	      SDK_STATUS_BAD_HANDLE, "upload: wrong session", "");
	check(audio_voice_session_upload(g_grant.session, &up) ==
	      SDK_STATUS_BAD_REQUEST, "upload: no flags is refused", "");
	up.flags = 4U;
	check(audio_voice_session_upload(g_grant.session, &up) ==
	      SDK_STATUS_BAD_REQUEST, "upload: unknown flag is refused", "");

	/* Out of range: nothing changes. */
	for (i = 0U; i < STAGING_LEN; i++)
		g_staging[i] = (uint8_t)(i * 7U + 1U);
	memcpy(bank_before, g_bank, sizeof(bank_before));
	memset(&up, 0, sizeof(up));
	up.flags = AUDIO_VOICE_UPLOAD_COPY;
	up.staging_offset = STAGING_LEN - 8U;
	up.length = 16U;
	check(audio_voice_session_upload(g_grant.session, &up) ==
	      SDK_STATUS_BAD_REQUEST, "upload: staging range overrun", "");
	up.staging_offset = 0U;
	up.bank_offset = BANK_LEN - 8U;
	check(audio_voice_session_upload(g_grant.session, &up) ==
	      SDK_STATUS_BAD_REQUEST, "upload: bank range overrun", "");
	up.staging_offset = 0xFFFFFFF0U;
	up.bank_offset = 0U;
	check(audio_voice_session_upload(g_grant.session, &up) ==
	      SDK_STATUS_BAD_REQUEST, "upload: offset overflow", "");
	/* A good COPY with a bad DEFINE changes nothing, not even the copy. */
	memset(&up, 0, sizeof(up));
	up.flags = AUDIO_VOICE_UPLOAD_COPY | AUDIO_VOICE_UPLOAD_DEFINE;
	up.length = 64U;
	up.sample_id = SAMPLE_ID;
	up.sample_offset = BANK_LEN - 8U;
	up.sample_length = 16U;
	up.format = AUDIO_VOICE_FMT_S8;
	check(audio_voice_session_upload(g_grant.session, &up) ==
	      SDK_STATUS_BAD_REQUEST &&
	      memcmp(g_bank, bank_before, sizeof(bank_before)) == 0,
	      "upload: a bad DEFINE refuses the whole request", "");
	up.sample_offset = 0U;
	up.sample_length = 16U;
	up.sample_id = 256U;
	check(audio_voice_session_upload(g_grant.session, &up) ==
	      SDK_STATUS_BAD_REQUEST &&
	      memcmp(g_bank, bank_before, sizeof(bank_before)) == 0,
	      "upload: sample id 256 is refused", "");
	up.sample_id = 3U;
	up.format = 2U;
	check(audio_voice_session_upload(g_grant.session, &up) ==
	      SDK_STATUS_BAD_REQUEST &&
	      memcmp(g_bank, bank_before, sizeof(bank_before)) == 0,
	      "upload: an unknown format is refused", "");
	check(memcmp(g_bank, bank_before, sizeof(bank_before)) == 0,
	      "upload: refused requests changed nothing", "");

	/* COPY then DEFINE: playable, and the refused one stayed undefined. */
	check(upload_ramp(g_grant.session) == SDK_STATUS_OK,
	      "upload: copy + define", "");
	rec_start_ramp(960U * 2U, 0U);
	rec(960U * 2U, 1U, AUDIO_VOICE_OP_START, 3U, 0U, 0U);
	publish();
	run(12);
	for (i = 0U; i < AUDIO_NUM_PERIODS; i++) {
		if (tx_period(i)[2] != 0)
			seen = tx_period(i)[2];
	}
	check(seen != 0, "upload: a defined sample plays", "");
	check(st32(AUDIO_VOICE_STATUS_OFF_REJECTED) == 1U,
	      "upload: the refused DEFINE left sample 3 undefined",
	      fmt("rejected=%u", st32(AUDIO_VOICE_STATUS_OFF_REJECTED)));
}

/* ---- H2. a client in the middle of a long upload is alive ---- */

static void scenario_upload_liveness(void)
{
	struct audio_voice_session_upload up;
	uint32_t i;

	fresh();
	check(open_session(7680U, 4U) == SDK_STATUS_OK, "liveness: open", "");
	memset(&up, 0, sizeof(up));
	up.flags = AUDIO_VOICE_UPLOAD_COPY;
	up.length = 64U;
	/* A chunk every 200 ms for 6 s, resets in between, the ring
	 * heartbeat never moves: the open session counts as alive. */
	for (i = 0U; i < 300U; i++) {
		bench_clock += 20U;
		if (i % 10U == 0U)
			check(audio_voice_session_upload(g_grant.session,
			      &up) == SDK_STATUS_OK, "liveness: chunk", "");
		if (i % 10U == 5U)
			check(audio_voice_session_reset(g_grant.session) ==
			      SDK_STATUS_OK, "liveness: reset", "");
		audio_voice_session_poll();
	}
	check(audio_voice_session_active() && g_release_calls == 0,
	      "liveness: uploads and resets keep the session open past the "
	      "heartbeat timeout", "");
	/* A request that names no session of the client's is no sign of
	 * life. */
	for (i = 0U; i < 110U; i++) {
		bench_clock += 20U;
		if (i % 10U == 0U)
			check(audio_voice_session_upload(g_grant.session + 1U,
			      &up) == SDK_STATUS_BAD_HANDLE,
			      "liveness: wrong session", "");
		audio_voice_session_poll();
	}
	check(!audio_voice_session_active() && g_release_calls == 1,
	      "liveness: without a valid request the session times out",
	      fmt("active=%d", audio_voice_session_active()));
}

/* Redefining a playing sample takes effect only at the next START. */
static void scenario_redefine(void)
{
	struct audio_voice_session_upload up;
	uint32_t i;
	uint32_t start_b;
	int old_pattern = 1;
	int new_pattern = 0;

	fresh();
	check(open_session(7680U, 4U) == SDK_STATUS_OK, "redefine: open", "");
	(void)upload_ramp(g_grant.session);
	rec_start_ramp(960U * 2U, 0U);
	publish();
	run(14);
	/* Sample 0 now points at a flat second sample; voice 0 keeps the
	 * ramp it started with. */
	memset(&up, 0, sizeof(up));
	up.flags = AUDIO_VOICE_UPLOAD_COPY | AUDIO_VOICE_UPLOAD_DEFINE;
	for (i = 0U; i < 2U * LOOP_SAMPLES; i += 2U) {
		g_staging[200U + i] = 0x20U;
		g_staging[200U + i + 1U] = 0x00U;
	}
	up.staging_offset = 200U;
	up.bank_offset = 1000U;
	up.length = 2U * LOOP_SAMPLES;
	up.sample_id = SAMPLE_ID;
	up.sample_offset = 1000U;
	up.sample_length = 2U * LOOP_SAMPLES;
	up.format = AUDIO_VOICE_FMT_S16;
	check(audio_voice_session_upload(g_grant.session, &up) ==
	      SDK_STATUS_OK, "redefine: upload over a playing id", "");
	start_b = st_playing() + g_grant.lead_frames + 960U * 4U;
	run(8);
	/* Voice 0 is still on the ramp (at most 5899 out); the flat sample
	 * would read 8191. */
	for (i = 0U; i < AUDIO_NUM_PERIODS; i++) {
		const int16_t *p = tx_period(i);
		uint32_t f;

		for (f = 0U; f < TICK_BYTES / 2U; f++) {
			if (p[f] > ramp_out(LOOP_SAMPLES - 1U))
				old_pattern = 0;
		}
	}
	check(old_pattern, "redefine: the playing voice keeps its geometry",
	      "");
	/* A new START on voice 1 uses the new bytes: flat 8191 on top of
	 * the ramp. */
	rec_start_ramp(start_b, 1U);
	publish();
	run(14);
	for (i = 0U; i < AUDIO_NUM_PERIODS; i++) {
		const int16_t *p = tx_period(i);
		uint32_t f;

		for (f = 0U; f < TICK_BYTES / 2U; f++) {
			if (p[f] >= 8191 + ramp_out(0U))
				new_pattern = 1;
		}
	}
	check(new_pattern, "redefine: the next START plays the new bytes", "");
}

int main(void)
{
	scenario_open();
	scenario_open_refused();
	scenario_ring_geometry();
	scenario_stamps();
	scenario_ring_wrap();
	scenario_cursor_wrap();
	scenario_ring_overrun();
	scenario_open_flags();
	scenario_lead(7680U);
	scenario_lead(24000U);
	scenario_stall();
	scenario_status();
	scenario_reset();
	scenario_peer_detach(7680U);
	scenario_peer_detach(24000U);
	scenario_heartbeat();
	scenario_warm_reset();
	scenario_cache();
	scenario_upload();
	scenario_upload_liveness();
	scenario_redefine();
	if (failures != 0)
		printf("audio_voice_session_test: %d FAILED\n", failures);
	else
		printf("audio_voice_session_test: all scenarios passed\n");
	return failures != 0;
}
