/*
 * ZZ9000AX voice session: the glue between the voice engine (audio_voice.h),
 * the client's command ring in a shared buffer, and the audio fabric's
 * firmware-owned voice slot.
 *
 * One session at a time. The module is handed already-resolved ARM pointers
 * (command ring, status block, upload staging buffer, card-side sample
 * bank) by sdk_mailbox.c, which owns handle lookup, ownership checks,
 * pinning and the bank allocation; a release hook given at open gives them
 * back whenever the session ends, whatever ended it.
 *
 * Everything that renders runs from audio_voice_session_poll() on the core-0
 * main loop. The fabric ISR only copies the staged periods through the
 * producer ops defined here. Wire layouts are in sdk_mailbox.h (the OPEN,
 * UPLOAD payloads) and in the AUDIO_VOICE_RING_* / AUDIO_VOICE_STATUS_*
 * constants below.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ_AUDIO_VOICE_SESSION_H
#define ZZ_AUDIO_VOICE_SESSION_H

#include <stdint.h>

/* One fabric period: 20 ms at 48 kHz. */
#define AUDIO_VOICE_PERIOD_FRAMES 960U

/*
 * Lead: how far past the playing frame the engine may render. The floor is
 * the fabric fill-ahead plus 2 periods of main-loop jitter: 6 + 2 periods.
 * A low-latency session shortens the fabric fill-ahead while it is open and
 * its floor is the fill-ahead plus one period, the least that serves a poll
 * in every 20 ms window (the tolerated poll gap is
 * (lead - fill-ahead) * 20 ms minus the render time). Hardware qualification
 * fixes the two low-latency constants; see docs/audio-fabric.md.
 */
#define AUDIO_VOICE_LEAD_FLOOR_FRAMES (8U * AUDIO_VOICE_PERIOD_FRAMES)
#define AUDIO_VOICE_LEAD_MAX_FRAMES   24000U
#define AUDIO_VOICE_LL_FILL_AHEAD_PERIODS 2U
#define AUDIO_VOICE_LL_LEAD_FLOOR_FRAMES \
	((AUDIO_VOICE_LL_FILL_AHEAD_PERIODS + 1U) * AUDIO_VOICE_PERIOD_FRAMES)

/* Sample bank, 1 byte .. 32 MiB. */
#define AUDIO_VOICE_BANK_MAX_BYTES (32U * 1024U * 1024U)

/* Open flags. */
#define AUDIO_VOICE_OPEN_NTSC   (1U << 0)
#define AUDIO_VOICE_OPEN_LINEAR (1U << 1)
/* Low-latency session: fabric fill-ahead of AUDIO_VOICE_LL_FILL_AHEAD_PERIODS
 * while open, lead floor AUDIO_VOICE_LL_LEAD_FLOOR_FRAMES. */
#define AUDIO_VOICE_OPEN_LOW_LATENCY (1U << 2)
#define AUDIO_VOICE_OPEN_KNOWN \
	(AUDIO_VOICE_OPEN_NTSC | AUDIO_VOICE_OPEN_LINEAR | \
	 AUDIO_VOICE_OPEN_LOW_LATENCY)

/* Command ring: 16-byte header, then 16-byte records. */
#define AUDIO_VOICE_RING_HEADER_BYTES 16U
#define AUDIO_VOICE_RING_RECORD_BYTES 16U
#define AUDIO_VOICE_RING_MIN_RECORDS  16U
#define AUDIO_VOICE_RING_MAX_RECORDS  4096U
#define AUDIO_VOICE_RING_OFF_WRITE      0U /* u16 write cursor */
#define AUDIO_VOICE_RING_OFF_HEARTBEAT  4U /* u32 heartbeat */

/* Status block (written by the firmware poll only). */
#define AUDIO_VOICE_STATUS_BYTES      64U
/* Start address: the firmware flushes the block, so it owns its lines. */
#define AUDIO_VOICE_STATUS_ALIGN      32U
#define AUDIO_VOICE_STATUS_OFF_PLAYING   0U  /* u32 */
#define AUDIO_VOICE_STATUS_OFF_HORIZON   4U  /* u32 */
#define AUDIO_VOICE_STATUS_OFF_READ      8U  /* u16 */
#define AUDIO_VOICE_STATUS_OFF_STATE     10U /* u16 */
#define AUDIO_VOICE_STATUS_OFF_LATE      12U /* u32 */
#define AUDIO_VOICE_STATUS_OFF_REJECTED  16U /* u32 */
#define AUDIO_VOICE_STATUS_OFF_UNDERRUNS 20U /* u32 */
#define AUDIO_VOICE_STATUS_OFF_DROPPED   24U /* u32 */
#define AUDIO_VOICE_STATUS_OFF_SESSION   28U /* u32 */
/*
 * Jitter telemetry, since open. MARGIN is the smallest time a rendered
 * period waited for the fabric ISR to take it, in microseconds (0xFFFFFFFF
 * until the first period is taken). POLL_GAP is the largest gap between two
 * voice polls, in microseconds.
 */
#define AUDIO_VOICE_STATUS_OFF_MARGIN   32U /* u32 */
#define AUDIO_VOICE_STATUS_OFF_POLL_GAP 36U /* u32 */
#define AUDIO_VOICE_STATUS_MARGIN_NONE  0xFFFFFFFFU

/*
 * Extended status. FEATURES is the features word, written in every status
 * write; the SDK reads it as ZZ9KVoiceExtStatus. ACTIVE_MASK, ENDS and
 * OUTPUT_DELAY are reserved for later units and stay zero.
 */
#define AUDIO_VOICE_STATUS_OFF_FEATURES     40U /* u32 */
#define AUDIO_VOICE_STATUS_OFF_ACTIVE_MASK  44U /* u32 */
#define AUDIO_VOICE_STATUS_OFF_ENDS         48U /* u32 */
#define AUDIO_VOICE_STATUS_OFF_OUTPUT_DELAY 52U /* u16 */

/* Feature bits, as ZZ9K_VOICE_FEATURE_* in the SDK's zz9k/voice.h. */
#define AUDIO_VOICE_FEATURE_RAMP         0x0001U
#define AUDIO_VOICE_FEATURE_LIMITER      0x0002U
#define AUDIO_VOICE_FEATURE_CUBIC        0x0004U
#define AUDIO_VOICE_FEATURE_VOICE_INTERP 0x0008U
#define AUDIO_VOICE_FEATURE_OVERSAMPLE   0x0010U
#define AUDIO_VOICE_FEATURE_ENVELOPE     0x0020U
#define AUDIO_VOICE_FEATURE_EVENTS       0x0040U
#define AUDIO_VOICE_FEATURE_CONFIG       0x0080U

/*
 * The features this firmware advertises. CONFIG is the record the engine
 * handles. RAMP passed the U2 card gate; the measured loads and render
 * times are recorded in sdk/docs/zz9k-voice.md, Hardware measurements.
 */
#define AUDIO_VOICE_FEATURES \
	(AUDIO_VOICE_FEATURE_CONFIG | AUDIO_VOICE_FEATURE_RAMP)

#define AUDIO_VOICE_STATE_CLOSED 0U
#define AUDIO_VOICE_STATE_OPEN   1U /* open, no record yet */
#define AUDIO_VOICE_STATE_LIVE   2U

/* Card-side event queue and the heartbeat budget. */
#define AUDIO_VOICE_QUEUE_RECORDS 4096U
#define AUDIO_VOICE_HEARTBEAT_TIMEOUT_MS 2000U

/* Upload flags. */
#define AUDIO_VOICE_UPLOAD_COPY   (1U << 0)
#define AUDIO_VOICE_UPLOAD_DEFINE (1U << 1)
#define AUDIO_VOICE_UPLOAD_KNOWN \
	(AUDIO_VOICE_UPLOAD_COPY | AUDIO_VOICE_UPLOAD_DEFINE)

struct audio_voice_session_open {
	uint8_t *ring;        /* command ring (host-visible shared buffer) */
	uint32_t ring_len;
	uint8_t *status;      /* status block */
	uint32_t status_len;
	uint8_t *staging;     /* upload staging buffer */
	uint32_t staging_len;
	uint8_t *bank;        /* card-side sample bank, already allocated */
	uint32_t bank_len;
	uint32_t lead_frames; /* requested lead */
	uint32_t flags;       /* AUDIO_VOICE_OPEN_* */
	uint32_t voices;      /* 1..32 */
	/* Called once when the session ends (close, client release,
	 * heartbeat loss, warm reset), after the slot is detached and the
	 * final status is written: unpin the buffers, free the bank. */
	void (*release)(void *ctx);
	void *ctx;
};

struct audio_voice_session_grant {
	uint32_t session;
	uint32_t voices;
	uint32_t bank_bytes;
	uint32_t lead_frames;
	uint32_t lead_floor_frames;
	uint32_t lead_max_frames;
	uint32_t ring_records;
	uint32_t output_rate;
	uint32_t max_samples;
};

struct audio_voice_session_upload {
	uint32_t flags;          /* AUDIO_VOICE_UPLOAD_* */
	uint32_t staging_offset;
	uint32_t bank_offset;
	uint32_t length;
	uint32_t sample_id;
	uint32_t sample_offset;
	uint32_t sample_length;
	uint32_t format;
};

/*
 * The size, voice and flag checks of an open, with no side effects, so a
 * caller can refuse before it allocates the bank. Returns OK or
 * SDK_STATUS_BAD_REQUEST.
 */
uint16_t audio_voice_session_check(uint32_t ring_len,
	uintptr_t status_address, uint32_t status_len, uint32_t bank_len,
	uint32_t voices, uint32_t flags);

/* Nonzero while a session is open. */
int audio_voice_session_active(void);

/*
 * Open the session. Returns an SDK_STATUS_* value: OK (grant filled), BUSY
 * (a session is open, or the fabric output is held by a legacy client),
 * BAD_REQUEST (sizes, flags, voices, a ring below 16 records, a status
 * block under 64 bytes). A refused open leaves nothing attached; the
 * caller still owns the bank and the pins and releases them itself.
 */
uint16_t audio_voice_session_open(const struct audio_voice_session_open *open,
	struct audio_voice_session_grant *grant);

/* Close / reset by session id; SDK_STATUS_BAD_HANDLE for a wrong id. */
uint16_t audio_voice_session_close(uint32_t session);
uint16_t audio_voice_session_reset(uint32_t session);
uint16_t audio_voice_session_upload(uint32_t session,
	const struct audio_voice_session_upload *upload);

/*
 * End whatever session is open, without an id (client release, warm reset).
 * Safe when nothing is open.
 */
void audio_voice_session_shutdown(void);

/*
 * Main-loop pass: heartbeat, ring drain, render, status. Renders at most
 * one period per call, so the main loop can serve a Zorro request between
 * periods; a lead that is short (after OPEN, START, RESET or a stall) is
 * rebuilt over as many calls. The slot goes live once the full lead is
 * staged.
 */
void audio_voice_session_poll(void);

/*
 * For the main loop's request branch, where audio_voice_session_poll() does
 * not run: polls when at least 2 ms have passed since the last poll, so
 * Zorro traffic that never pauses cannot starve the engine. No-op without
 * a session.
 */
void audio_voice_session_poll_busy(void);

#endif /* ZZ_AUDIO_VOICE_SESSION_H */
