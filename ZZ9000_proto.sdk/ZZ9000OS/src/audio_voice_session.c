/*
 * ZZ9000AX voice session: see audio_voice_session.h.
 *
 * Data flow, all on the core-0 main loop except the fabric callbacks:
 *
 *   client ring --drain--> card event queue --render--> staging ring
 *   staging ring --(fabric ISR: copy)--> TX ring
 *
 * The staging ring is the voice slot's source ring as the fabric sees it
 * (a byte ring of whole periods). The poll is the single writer of
 * `produced`; the fabric ISR is the single writer of `consumed` (stage op)
 * and `retired` (retire op). The queued-period rebuild replays periods
 * behind `consumed` when a peer lease detaches, so the ring keeps a TX
 * window of periods behind it intact, as the lease preconvert ring does
 * (AUDIO_FABRIC_LEASE_REPLAY_PERIODS).
 *
 * The engine timeline is its own frame counter: frame 0 is the first frame
 * handed to the fabric, and the engine frame always equals produced / 4.
 * playing_frame is retired / 4.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "audio_fabric.h"
#include "audio_voice.h"
#include "audio_voice_session.h"
#include "memorymap.h"
#include "sdk_mailbox.h"
#include "sdk_smp_lock.h"
#include "xil_cache.h"
#include "xtime_l.h"

#define VOICE_SLOT AUDIO_FABRIC_SLOT_VOICE
#define PERIOD_BYTES AUDIO_BYTES_PER_PERIOD

/*
 * Staging ring: the largest lead (25 periods) plus up to one period of
 * whole-period overshoot, plus the 8-period TX window the rebuild may
 * replay, plus margin.
 */
#define STAGING_PERIODS 40U
#define STAGING_BYTES (STAGING_PERIODS * PERIOD_BYTES)

typedef char voice_staging_holds_lead_and_replay[
	(STAGING_PERIODS >= (AUDIO_VOICE_LEAD_MAX_FRAMES /
	                     AUDIO_VOICE_PERIOD_FRAMES + 2U +
	                     AUDIO_NUM_PERIODS)) ? 1 : -1];
typedef char voice_period_is_960_frames[
	(PERIOD_BYTES == AUDIO_VOICE_PERIOD_FRAMES * 4U) ? 1 : -1];

struct voice_session {
	uint8_t state;           /* AUDIO_VOICE_STATE_* */
	uint8_t slot_live;       /* the fabric slot was published */
	uint8_t low_latency;     /* the session lowered the fabric fill-ahead */
	uint32_t id;
	struct audio_voice_session_open res; /* resolved buffers and hook */
	uint32_t voices;
	uint32_t lead;           /* granted, a whole number of periods */
	uint32_t ring_records;   /* ring capacity N */
	uint16_t read_cursor;
	uint32_t queue_head;
	uint32_t queue_count;
	uint32_t dropped;
	uint32_t extra_rejected; /* records for voices above the grant */
	uint32_t heartbeat;
	uint64_t heartbeat_ms;   /* when the heartbeat last changed */
	uint32_t underrun_base;
	uint32_t underruns;
	/*
	 * Jitter telemetry. The poll stamps each period it renders with a
	 * timer tick (0 = not stamped) and keeps the largest gap between
	 * polls; the fabric ISR keeps the smallest age of a period when it
	 * takes it, in raw ticks so it does no division.
	 */
	uint64_t period_stamp[STAGING_PERIODS];
	volatile uint32_t margin_min_ticks; /* ISR writes, 0xFFFFFFFF = none */
	uint64_t last_poll_ticks;
	uint64_t poll_gap_max_ticks;
	uint8_t poll_seen;
	/* Byte cursors of the staging ring. */
	volatile uint64_t produced;
	volatile uint64_t consumed;
	volatile uint64_t retired;
	uint8_t shadow[AUDIO_VOICE_STATUS_BYTES];
	uint8_t shadow_valid;
};

static struct voice_session g_s;
static uint32_t g_next_id;
static struct audio_voice_engine g_engine;
static struct audio_voice_cmd g_queue[AUDIO_VOICE_QUEUE_RECORDS];
static uint8_t g_staging[STAGING_BYTES] __attribute__((aligned(32)));

/* ---- big-endian access to the client's shared buffers ---- */

static uint16_t rd16(const volatile uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t rd32(const volatile uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void wr16(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/*
 * The fabric ISR is the single writer of consumed and retired; a main-loop
 * read can tear a 64-bit load across its two words if the ISR lands
 * between them, so read high-low-high and retry while the high word moves
 * (fabric_lease_read_cursor in audio_fabric_internal.h).
 */
static uint64_t read_cursor64(const volatile uint64_t *cursor)
{
	const volatile uint32_t *words =
		(const volatile uint32_t *)(const volatile void *)cursor;
	uint32_t high_before;
	uint32_t low;
	uint32_t high_after;

	do {
		high_before = words[1];
		low = words[0];
		high_after = words[1];
	} while (high_before != high_after);
	return ((uint64_t)high_before << 32) | (uint64_t)low;
}

static uint64_t now_ms(void)
{
	XTime t;

	XTime_GetTime(&t);
	return (uint64_t)t * 1000U / COUNTS_PER_SECOND;
}

static uint64_t now_ticks(void)
{
	XTime t;

	XTime_GetTime(&t);
	return (uint64_t)t;
}

/* Timer ticks to microseconds, kept below the "none" marker. */
static uint32_t ticks_to_us(uint64_t ticks)
{
	uint64_t us = ticks * 1000000U / COUNTS_PER_SECOND;

	return us >= AUDIO_VOICE_STATUS_MARGIN_NONE ?
		AUDIO_VOICE_STATUS_MARGIN_NONE - 1U : (uint32_t)us;
}

/*
 * Every request the client makes of its open session proves it is there, so
 * the heartbeat timeout restarts: a long multi-chunk upload keeps a session
 * open that the ring heartbeat alone would let lapse.
 */
static void client_alive(void)
{
	g_s.heartbeat_ms = now_ms();
}

/* ---- producer ops: the voice slot as the fabric sees it ---- */

static int voice_snapshot(struct audio_fabric_source *source)
{
	memset(source, 0, sizeof(*source));
	source->ring = g_staging;
	source->capacity = STAGING_BYTES;
	source->produced_bytes = read_cursor64(&g_s.produced);
	source->staged_bytes = read_cursor64(&g_s.consumed);
	source->sample_rate = AUDIO_VOICE_OUT_HZ;
	source->channels = 2U;
	source->sample_format = SDK_AUDIO_SAMPLE_FORMAT_S16LE;
	return 1;
}

static int voice_stage(uint32_t bytes)
{
	if (bytes > g_s.produced - g_s.consumed)
		return 0;
	/* Age of the period the ISR takes, for the margin telemetry: one
	 * array load, one timer read and a compare, no division. */
	if (g_s.slot_live) {
		uint64_t stamp = g_s.period_stamp[
			(g_s.consumed / PERIOD_BYTES) % STAGING_PERIODS];

		if (stamp != 0U) {
			uint64_t age = now_ticks() - stamp;
			uint32_t ticks = age >= AUDIO_VOICE_STATUS_MARGIN_NONE ?
				AUDIO_VOICE_STATUS_MARGIN_NONE - 1U :
				(uint32_t)age;

			if (ticks < g_s.margin_min_ticks)
				g_s.margin_min_ticks = ticks;
		}
	}
	g_s.consumed += bytes;
	return 1;
}

static void voice_retire(uint32_t bytes)
{
	g_s.retired += bytes;
}

static const struct audio_fabric_producer_ops g_voice_ops = {
	.snapshot = voice_snapshot,
	.stage = voice_stage,
	.retire = voice_retire,
};

/* ---- status block ---- */

static void publish_status(void)
{
	uint8_t block[AUDIO_VOICE_STATUS_BYTES];
	uint32_t underruns = g_s.underruns;

	if (g_s.state != AUDIO_VOICE_STATE_CLOSED && g_s.slot_live) {
		underruns = audio_fabric_slot_underruns(VOICE_SLOT) -
			g_s.underrun_base;
		g_s.underruns = underruns;
	}
	memset(block, 0, sizeof(block));
	wr32(block + AUDIO_VOICE_STATUS_OFF_PLAYING,
	     (uint32_t)(read_cursor64(&g_s.retired) / 4U));
	wr32(block + AUDIO_VOICE_STATUS_OFF_HORIZON, g_engine.frame);
	wr16(block + AUDIO_VOICE_STATUS_OFF_READ, g_s.read_cursor);
	wr16(block + AUDIO_VOICE_STATUS_OFF_STATE, g_s.state);
	wr32(block + AUDIO_VOICE_STATUS_OFF_LATE, g_engine.late);
	wr32(block + AUDIO_VOICE_STATUS_OFF_REJECTED,
	     g_engine.rejected + g_s.extra_rejected);
	wr32(block + AUDIO_VOICE_STATUS_OFF_UNDERRUNS, underruns);
	wr32(block + AUDIO_VOICE_STATUS_OFF_DROPPED, g_s.dropped);
	wr32(block + AUDIO_VOICE_STATUS_OFF_SESSION, g_s.id);
	wr32(block + AUDIO_VOICE_STATUS_OFF_MARGIN,
	     g_s.margin_min_ticks == AUDIO_VOICE_STATUS_MARGIN_NONE ?
	     AUDIO_VOICE_STATUS_MARGIN_NONE :
	     ticks_to_us(g_s.margin_min_ticks));
	wr32(block + AUDIO_VOICE_STATUS_OFF_POLL_GAP,
	     ticks_to_us(g_s.poll_gap_max_ticks));
	if (g_s.shadow_valid && memcmp(g_s.shadow, block, sizeof(block)) == 0)
		return;
	memcpy(g_s.res.status, block, sizeof(block));
	Xil_DCacheFlushRange((INTPTR)g_s.res.status, AUDIO_VOICE_STATUS_BYTES);
	memcpy(g_s.shadow, block, sizeof(block));
	g_s.shadow_valid = 1U;
}

/* ---- render ---- */

/* Render one period at `produced` and publish it to the ISR. */
static void render_period(void)
{
	uint64_t produced = g_s.produced;
	uint8_t *out = g_staging + (uint32_t)(produced % STAGING_BYTES);
	size_t used;
	uint32_t irq_state;

	used = audio_voice_render(&g_engine, (int16_t *)(void *)out,
		AUDIO_VOICE_PERIOD_FRAMES, g_queue + g_s.queue_head,
		g_s.queue_count);
	g_s.queue_head += (uint32_t)used;
	g_s.queue_count -= (uint32_t)used;
	if (g_s.queue_count == 0U)
		g_s.queue_head = 0U;
	Xil_DCacheFlushRange((INTPTR)out, PERIOD_BYTES);
	if (g_s.slot_live)
		g_s.period_stamp[(produced / PERIOD_BYTES) %
			STAGING_PERIODS] = now_ticks();
	irq_state = smp_local_irq_save();
	g_s.produced = produced + PERIOD_BYTES;
	smp_local_irq_restore(irq_state);
}

/*
 * Render whole periods into the staging ring while the engine is less than
 * the granted lead past the playing frame. The rendered-but-unstaged
 * periods never exceed the lead (the ISR consumes before it retires), so
 * the ring keeps the TX replay window behind `consumed` intact; the size
 * assert at the top of the file holds that arithmetic.
 */
static void render_to_lead(void)
{
	for (;;) {
		uint32_t playing =
			(uint32_t)(read_cursor64(&g_s.retired) / 4U);

		if (g_engine.frame - playing >= g_s.lead)
			break;
		render_period();
	}
}

/* ---- ring ---- */

static void queue_push(const struct audio_voice_cmd *cmd)
{
	if (g_s.queue_count >= AUDIO_VOICE_QUEUE_RECORDS) {
		g_s.dropped++;
		return;
	}
	if (g_s.queue_head + g_s.queue_count >= AUDIO_VOICE_QUEUE_RECORDS) {
		memmove(g_queue, g_queue + g_s.queue_head,
			g_s.queue_count * sizeof(g_queue[0]));
		g_s.queue_head = 0U;
	}
	g_queue[g_s.queue_head + g_s.queue_count] = *cmd;
	g_s.queue_count++;
}

static void decode_record(const volatile uint8_t *p)
{
	struct audio_voice_cmd cmd;

	cmd.stamp = rd32(p);
	cmd.voice = p[4];
	cmd.op = p[5];
	cmd.flags = rd16(p + 6);
	cmd.arg0 = rd32(p + 8);
	cmd.arg1 = rd32(p + 12);
	/* The grant limits the voices; master gain names no voice. */
	if (cmd.op != AUDIO_VOICE_OP_MASTER && cmd.voice >= g_s.voices) {
		g_s.extra_rejected++;
		return;
	}
	queue_push(&cmd);
}

/* Take every record the client has published; returns how many. */
static uint32_t drain_ring(uint16_t write_cursor)
{
	uint32_t n = (uint16_t)(write_cursor - g_s.read_cursor);
	uint32_t index;
	uint32_t first;
	uint32_t i;

	if (n == 0U)
		return 0U;
	if (n > g_s.ring_records) {
		/* More outstanding than the ring holds: the client wrapped
		 * over records it had not yet been given back. They are gone. */
		g_s.dropped += n;
		g_s.read_cursor = write_cursor;
		return n;
	}
	index = g_s.read_cursor & (g_s.ring_records - 1U);
	first = g_s.ring_records - index;
	if (first > n)
		first = n;
	Xil_DCacheInvalidateRange((INTPTR)(g_s.res.ring +
		AUDIO_VOICE_RING_HEADER_BYTES +
		index * AUDIO_VOICE_RING_RECORD_BYTES),
		first * AUDIO_VOICE_RING_RECORD_BYTES);
	if (n > first)
		Xil_DCacheInvalidateRange((INTPTR)(g_s.res.ring +
			AUDIO_VOICE_RING_HEADER_BYTES),
			(n - first) * AUDIO_VOICE_RING_RECORD_BYTES);
	for (i = 0U; i < n; i++)
		decode_record(g_s.res.ring + AUDIO_VOICE_RING_HEADER_BYTES +
			((g_s.read_cursor + i) & (g_s.ring_records - 1U)) *
			AUDIO_VOICE_RING_RECORD_BYTES);
	g_s.read_cursor = write_cursor;
	return n;
}

static uint16_t ring_write_cursor(void)
{
	Xil_DCacheInvalidateRange((INTPTR)g_s.res.ring,
		AUDIO_VOICE_RING_HEADER_BYTES);
	return rd16(g_s.res.ring + AUDIO_VOICE_RING_OFF_WRITE);
}

/* ---- session lifecycle ---- */

static void session_end(void)
{
	uint32_t irq_state;
	void (*release)(void *);
	void *ctx;

	if (g_s.state == AUDIO_VOICE_STATE_CLOSED)
		return;
	if (g_s.slot_live)
		g_s.underruns = audio_fabric_slot_underruns(VOICE_SLOT) -
			g_s.underrun_base;
	/* Give the fabric its default fill-ahead back before the slot goes:
	 * every end path (close, heartbeat loss, client release, warm reset)
	 * runs through here. */
	if (g_s.low_latency) {
		audio_fabric_fill_ahead_set(0U);
		g_s.low_latency = 0U;
	}
	/* Arm the queued-period rebuild while the slot's tags still exist,
	 * then detach: only the period the DMA is in may still carry voice
	 * audio. Both under one critical section so the compositor ISR
	 * cannot rebuild in between. */
	irq_state = smp_local_irq_save();
	audio_fabric_request_rebuild(VOICE_SLOT);
	audio_fabric_producer_detach(VOICE_SLOT);
	smp_local_irq_restore(irq_state);
	audio_voice_silence(&g_engine);
	g_s.state = AUDIO_VOICE_STATE_CLOSED;
	g_s.slot_live = 0U;
	g_s.queue_head = 0U;
	g_s.queue_count = 0U;
	publish_status();
	release = g_s.res.release;
	ctx = g_s.res.ctx;
	g_s.res.release = NULL;
	if (release != NULL)
		release(ctx);
}

int audio_voice_session_active(void)
{
	return g_s.state != AUDIO_VOICE_STATE_CLOSED;
}

static uint32_t largest_power_of_two_at_most(uint32_t n)
{
	uint32_t p = 1U;

	while (p <= n / 2U)
		p <<= 1;
	return p;
}

uint16_t audio_voice_session_check(uint32_t ring_len,
	uintptr_t status_address, uint32_t status_len, uint32_t bank_len,
	uint32_t voices, uint32_t flags)
{
	/*
	 * The firmware writes and flushes the status block: a block that
	 * shares a cache line with another buffer would write that
	 * buffer's stale line back over the client's data. The ring and
	 * staging buffers are only read (invalidated), so they need no
	 * alignment.
	 */
	if ((status_address & (AUDIO_VOICE_STATUS_ALIGN - 1U)) != 0U ||
	    status_len < AUDIO_VOICE_STATUS_BYTES ||
	    ring_len < AUDIO_VOICE_RING_HEADER_BYTES +
		AUDIO_VOICE_RING_MIN_RECORDS * AUDIO_VOICE_RING_RECORD_BYTES)
		return SDK_STATUS_BAD_REQUEST;
	if (bank_len == 0U || bank_len > AUDIO_VOICE_BANK_MAX_BYTES ||
	    voices == 0U || voices > AUDIO_VOICE_MAX_VOICES ||
	    (flags & ~AUDIO_VOICE_OPEN_KNOWN) != 0U)
		return SDK_STATUS_BAD_REQUEST;
	return SDK_STATUS_OK;
}

uint16_t audio_voice_session_open(const struct audio_voice_session_open *open,
	struct audio_voice_session_grant *grant)
{
	uint32_t records;
	uint32_t lead;
	uint32_t lead_floor;
	uint16_t rc;

	if (g_s.state != AUDIO_VOICE_STATE_CLOSED)
		return SDK_STATUS_BUSY;
	if (open->ring == NULL || open->status == NULL ||
	    open->staging == NULL || open->bank == NULL)
		return SDK_STATUS_BAD_REQUEST;
	rc = audio_voice_session_check(open->ring_len,
		(uintptr_t)open->status, open->status_len,
		open->bank_len, open->voices, open->flags);
	if (rc != SDK_STATUS_OK)
		return rc;
	records = largest_power_of_two_at_most(
		(open->ring_len - AUDIO_VOICE_RING_HEADER_BYTES) /
		AUDIO_VOICE_RING_RECORD_BYTES);
	if (records > AUDIO_VOICE_RING_MAX_RECORDS)
		records = AUDIO_VOICE_RING_MAX_RECORDS;
	/* Floor, cap, and a whole number of periods: the engine renders whole
	 * periods, so a lead that is not one would be overshot by the poll. */
	lead_floor = (open->flags & AUDIO_VOICE_OPEN_LOW_LATENCY) != 0U ?
		AUDIO_VOICE_LL_LEAD_FLOOR_FRAMES :
		AUDIO_VOICE_LEAD_FLOOR_FRAMES;
	lead = open->lead_frames;
	if (lead < lead_floor)
		lead = lead_floor;
	if (lead > AUDIO_VOICE_LEAD_MAX_FRAMES)
		lead = AUDIO_VOICE_LEAD_MAX_FRAMES;
	lead = (lead + AUDIO_VOICE_PERIOD_FRAMES - 1U) /
		AUDIO_VOICE_PERIOD_FRAMES * AUDIO_VOICE_PERIOD_FRAMES;
	if (lead > AUDIO_VOICE_LEAD_MAX_FRAMES)
		lead = AUDIO_VOICE_LEAD_MAX_FRAMES;

	if (!audio_fabric_producer_attach(VOICE_SLOT, &g_voice_ops))
		return SDK_STATUS_BUSY;
	audio_fabric_producer_rate_set(VOICE_SLOT, AUDIO_VOICE_OUT_HZ);

	memset(&g_s, 0, sizeof(g_s));
	g_s.margin_min_ticks = AUDIO_VOICE_STATUS_MARGIN_NONE;
	g_s.res = *open;
	g_s.voices = open->voices;
	g_s.lead = lead;
	g_s.ring_records = records;
	g_next_id++;
	if (g_next_id == 0U)
		g_next_id = 1U;
	g_s.id = g_next_id;
	g_s.underrun_base = audio_fabric_slot_underruns(VOICE_SLOT);
	audio_voice_init(&g_engine, open->bank, open->bank_len,
		(open->flags & AUDIO_VOICE_OPEN_NTSC) != 0U ?
			AUDIO_VOICE_CLOCK_NTSC : AUDIO_VOICE_CLOCK_PAL);
	g_engine.interp = (open->flags & AUDIO_VOICE_OPEN_LINEAR) != 0U ?
		AUDIO_VOICE_INTERP_LINEAR : AUDIO_VOICE_INTERP_NEAREST;
	/* Records and heartbeat the client wrote before the open are stale:
	 * start from where its cursors stand. */
	g_s.read_cursor = ring_write_cursor();
	g_s.heartbeat = rd32(open->ring + AUDIO_VOICE_RING_OFF_HEARTBEAT);
	g_s.heartbeat_ms = now_ms();
	g_s.state = AUDIO_VOICE_STATE_OPEN;
	/* Only now, with the slot attached and the session built: a refused
	 * open leaves the fabric alone. */
	if ((open->flags & AUDIO_VOICE_OPEN_LOW_LATENCY) != 0U) {
		audio_fabric_fill_ahead_set(AUDIO_VOICE_LL_FILL_AHEAD_PERIODS);
		g_s.low_latency = 1U;
	}
	publish_status();

	memset(grant, 0, sizeof(*grant));
	grant->session = g_s.id;
	grant->voices = open->voices;
	grant->bank_bytes = open->bank_len;
	grant->lead_frames = lead;
	grant->lead_floor_frames = lead_floor;
	grant->lead_max_frames = AUDIO_VOICE_LEAD_MAX_FRAMES;
	grant->ring_records = records;
	grant->output_rate = AUDIO_VOICE_OUT_HZ;
	grant->max_samples = AUDIO_VOICE_MAX_SAMPLES;
	return SDK_STATUS_OK;
}

uint16_t audio_voice_session_close(uint32_t session)
{
	if (g_s.state == AUDIO_VOICE_STATE_CLOSED || session != g_s.id)
		return SDK_STATUS_BAD_HANDLE;
	session_end();
	return SDK_STATUS_OK;
}

void audio_voice_session_shutdown(void)
{
	session_end();
}

uint16_t audio_voice_session_reset(uint32_t session)
{
	uint64_t consumed;
	uint32_t irq_state;

	if (g_s.state == AUDIO_VOICE_STATE_CLOSED || session != g_s.id)
		return SDK_STATUS_BAD_HANDLE;
	client_alive();
	/* Every record the client has written so far, and everything queued. */
	g_s.read_cursor = ring_write_cursor();
	g_s.queue_head = 0U;
	g_s.queue_count = 0U;
	audio_voice_silence(&g_engine);
	if (g_s.slot_live) {
		/*
		 * Drop the periods already queued in the TX ring (the rebuild
		 * needs the slot's tags, so arm it first) and the rendered
		 * periods the ISR has not taken yet. Those frames count as
		 * played: the timeline and the playing frame resume at the
		 * staged cursor, and the next periods are rendered silent
		 * from there, so the frame counter never goes backwards past
		 * a frame the client was told was rendered and still agrees
		 * with produced / 4.
		 *
		 * The first silent period is rendered before the interrupts
		 * come back: the formatter ISR runs on this core and takes
		 * one period per tick, so a tick landing between the cursor
		 * reset and the first render would find nothing staged and
		 * count a voice underrun for a stream the client just
		 * flushed. The rest of the lead is rendered with interrupts
		 * on; a tick needs one period at a time.
		 */
		irq_state = smp_local_irq_save();
		audio_fabric_request_rebuild(VOICE_SLOT);
		audio_fabric_producer_clear(VOICE_SLOT);
		consumed = g_s.consumed;
		g_s.produced = consumed;
		g_s.retired = consumed;
		g_engine.frame = (uint32_t)(consumed / 4U);
		render_period();
		smp_local_irq_restore(irq_state);
		render_to_lead();
	}
	publish_status();
	return SDK_STATUS_OK;
}

uint16_t audio_voice_session_upload(uint32_t session,
	const struct audio_voice_session_upload *up)
{
	if (g_s.state == AUDIO_VOICE_STATE_CLOSED || session != g_s.id)
		return SDK_STATUS_BAD_HANDLE;
	client_alive();
	if ((up->flags & ~AUDIO_VOICE_UPLOAD_KNOWN) != 0U || up->flags == 0U)
		return SDK_STATUS_BAD_REQUEST;
	/* Validate both halves before changing anything. */
	if ((up->flags & AUDIO_VOICE_UPLOAD_COPY) != 0U) {
		if (up->staging_offset > g_s.res.staging_len ||
		    up->length > g_s.res.staging_len - up->staging_offset ||
		    up->bank_offset > g_s.res.bank_len ||
		    up->length > g_s.res.bank_len - up->bank_offset)
			return SDK_STATUS_BAD_REQUEST;
	}
	if ((up->flags & AUDIO_VOICE_UPLOAD_DEFINE) != 0U) {
		if (up->sample_id >= AUDIO_VOICE_MAX_SAMPLES ||
		    up->format >= AUDIO_VOICE_FMT_COUNT ||
		    up->sample_offset > g_s.res.bank_len ||
		    up->sample_length > g_s.res.bank_len - up->sample_offset)
			return SDK_STATUS_BAD_REQUEST;
	}
	if ((up->flags & AUDIO_VOICE_UPLOAD_COPY) != 0U && up->length != 0U) {
		/* The client wrote the staging bytes through the host window. */
		Xil_DCacheInvalidateRange(
			(INTPTR)(g_s.res.staging + up->staging_offset),
			up->length);
		memcpy(g_s.res.bank + up->bank_offset,
		       g_s.res.staging + up->staging_offset, up->length);
	}
	if ((up->flags & AUDIO_VOICE_UPLOAD_DEFINE) != 0U &&
	    audio_voice_sample_set(&g_engine, up->sample_id,
		    up->sample_offset, up->sample_length, up->format) != 0)
		return SDK_STATUS_BAD_REQUEST;
	return SDK_STATUS_OK;
}

/* ---- main-loop poll ---- */

void audio_voice_session_poll(void)
{
	uint16_t write_cursor;
	uint32_t heartbeat;
	uint64_t now;

	if (g_s.state == AUDIO_VOICE_STATE_CLOSED)
		return;
	{
		uint64_t ticks = now_ticks();

		if (g_s.poll_seen && ticks - g_s.last_poll_ticks >
		    g_s.poll_gap_max_ticks)
			g_s.poll_gap_max_ticks = ticks - g_s.last_poll_ticks;
		g_s.last_poll_ticks = ticks;
		g_s.poll_seen = 1U;
	}
	write_cursor = ring_write_cursor();
	heartbeat = rd32(g_s.res.ring + AUDIO_VOICE_RING_OFF_HEARTBEAT);
	now = now_ms();
	if (heartbeat != g_s.heartbeat) {
		g_s.heartbeat = heartbeat;
		g_s.heartbeat_ms = now;
	} else if (now - g_s.heartbeat_ms >=
		   AUDIO_VOICE_HEARTBEAT_TIMEOUT_MS) {
		session_end();
		return;
	}
	if (drain_ring(write_cursor) != 0U &&
	    g_s.state == AUDIO_VOICE_STATE_OPEN)
		g_s.state = AUDIO_VOICE_STATE_LIVE;
	if (g_s.state == AUDIO_VOICE_STATE_LIVE) {
		render_to_lead();
		if (!g_s.slot_live) {
			/* The first record fixes frame 0: the slot goes live
			 * with the first periods already staged. */
			g_s.slot_live = 1U;
			(void)audio_fabric_producer_go_live_when_ready(
				VOICE_SLOT, 1);
		}
	}
	publish_status();
}
