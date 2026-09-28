/*
 * ZZ9000AX audio fabric direct-ring proof client (plan U4).
 *
 * Exercises the direct-ring producer plane end to end: the capability
 * gate (mixed-version fallback -- old firmware declines cleanly),
 * RING_ACQUIRE on a caller-selected slot (1 by default; slot 2 for
 * the Zorro III second producer), grant validation and mapping, a
 * generated 48 kHz stereo S16 tone written straight into the granted
 * ring with wrapped writes, producer-cursor publication through the
 * control-block seqlock after every write, backpressure taken only
 * from firmware's consumed-credit line (never from STATE_GET and
 * never from a mailbox submit), low-rate state telemetry, Ctrl-C
 * release through RING_RELEASE, and a clean exit. A proof client,
 * not a player.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/abi.h"
#include "zz9k/audio.h"
#include "zz9k/caps.h"
#include "zz9k/host.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__amigaos__) || defined(__amiga__) || defined(__AMIGA__) || \
    defined(__VBCC__)
#define ZZ9K_FABRICLEASE_AMIGA 1
#include <exec/types.h>
#include <proto/dos.h>
#include <proto/exec.h>
#endif

/* Set by the client test, which compiles this file directly and
 * drives the session against a mock mailbox and mock ring memory. */
#ifndef ZZ9K_FABRICLEASE_NO_MAIN
#define ZZ9K_FABRICLEASE_NO_MAIN 0
#endif

typedef int (*ZZ9KFabricLeaseCancelHook)(void *user);
typedef void (*ZZ9KFabricLeaseTickHook)(void *user);

static ZZ9KFabricLeaseCancelHook g_cancel_hook;
static void *g_cancel_user;
static ZZ9KFabricLeaseTickHook g_tick_hook;
static void *g_tick_user;

#if ZZ9K_FABRICLEASE_AMIGA
static volatile sig_atomic_t g_ctrl_c_requested;

static void fabriclease_sigint(int sig)
{
  (void)sig;
  g_ctrl_c_requested = 1;
}
#endif

void zz9k_fabriclease_set_cancel_hook_for_test(
    ZZ9KFabricLeaseCancelHook hook, void *user)
{
  g_cancel_hook = hook;
  g_cancel_user = user;
}

void zz9k_fabriclease_set_tick_hook_for_test(
    ZZ9KFabricLeaseTickHook hook, void *user)
{
  g_tick_hook = hook;
  g_tick_user = user;
}

static int fabriclease_cancel_requested(void)
{
  if (g_cancel_hook && g_cancel_hook(g_cancel_user)) {
    return 1;
  }
#if ZZ9K_FABRICLEASE_AMIGA
  if (g_ctrl_c_requested ||
      (SetSignal(0L, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) != 0U) {
    g_ctrl_c_requested = 1;
    return 1;
  }
#endif
  return 0;
}

/* One credit-poll quantum. The direct ring must be refilled as soon as
 * firmware publishes a period of consumed credit. Delay(1) is not
 * suitable here: its nominal 20-ms sleep plus scheduler latency can
 * cross the next 20-ms playback deadline and permanently lose one
 * period of producer throughput. Tests use the tick hook to advance
 * mock playback deterministically; the proof client otherwise yields
 * only through a short bounded spin while polling shared memory. */
static void fabriclease_wait(void)
{
  if (g_tick_hook) {
    g_tick_hook(g_tick_user);
    return;
  }
  {
    volatile uint32_t spin;

    for (spin = 0; spin < 1024U; spin++) {
    }
  }
}

/* Four-period writes amortize Zorro cache maintenance and control-line
 * publication. Steady-state refills wait until the whole chunk is credited:
 * writing one newly credited period at a time would perform 50 cache flushes
 * and seqlock publications per second per producer. */
#define ZZ9K_FABRICLEASE_RATE_HZ 48000U
#define ZZ9K_FABRICLEASE_FRAME_BYTES 4U
#define ZZ9K_FABRICLEASE_BYTES_PER_SECOND \
  (ZZ9K_FABRICLEASE_RATE_HZ * ZZ9K_FABRICLEASE_FRAME_BYTES)
#define ZZ9K_FABRICLEASE_CHUNK_PERIODS 4U
#define ZZ9K_FABRICLEASE_CHUNK_BYTES \
  (ZZ9K_FABRICLEASE_CHUNK_PERIODS * ZZ9K_AUDIO_RING_PERIOD_BYTES)
#define ZZ9K_FABRICLEASE_DEFAULT_SECONDS 5U
/* Telemetry is observability only (R16): one STATE_GET per ~1.2 s of
 * audio, never in the pacing path. */
#define ZZ9K_FABRICLEASE_TELEMETRY_CHUNKS 15U
/* Heartbeat refresh cadence inside pure wait stretches: well under
 * the two-second revocation bound (R11-R12). */
#define ZZ9K_FABRICLEASE_HEARTBEAT_WAITS 8U
#define ZZ9K_FABRICLEASE_WAIT_LIMIT 10000U
/* 48-sample sign blocks at 48 kHz ~= 500 Hz square bursts; integer
 * only (no libm on every toolchain) and phase-continuous across
 * chunks. */
#define ZZ9K_FABRICLEASE_BURST_SAMPLES 48U
#define ZZ9K_FABRICLEASE_AMPLITUDE 12000

typedef enum ZZ9KFabricLeaseStatus {
  ZZ9K_FABRICLEASE_OK = 0,
  ZZ9K_FABRICLEASE_DECLINE_NO_FABRIC /* firmware predates the plane */
} ZZ9KFabricLeaseStatus;

typedef struct ZZ9KFabricLeaseOptions {
  uint32_t seconds; /* bounded run length; 0 runs one chunk */
  uint32_t gain;    /* requested 0..255 producer scale */
  uint32_t slot;    /* direct-ring slot 1 or 2 */
} ZZ9KFabricLeaseOptions;

static uint8_t g_chunk[ZZ9K_FABRICLEASE_CHUNK_BYTES];

const char *zz9k_fabriclease_status_name(int status)
{
  switch (status) {
  case ZZ9K_FABRICLEASE_OK:
    return "ok";
  case ZZ9K_FABRICLEASE_DECLINE_NO_FABRIC:
    return "firmware lacks the audio fabric (capability not advertised)";
  default:
    return "error";
  }
}

/*
 * The mixed-version gate, pure so the client test can drive every
 * path: a firmware without ZZ9K_CAP_AUDIO_FABRIC declines cleanly --
 * the fallback proof, not an error. Zorro II is a supported
 * single-slot bus (R9): admission is firmware's decision at acquire
 * time, not a client-side board-shape refusal.
 */
int zz9k_fabriclease_gate(uint32_t capability_bits)
{
  if ((capability_bits & ZZ9K_CAP_AUDIO_FABRIC) == 0U) {
    return ZZ9K_FABRICLEASE_DECLINE_NO_FABRIC;
  }
  return ZZ9K_FABRICLEASE_OK;
}

/* Precompute one four-period burst chunk. The square wave repeats every
 * 96 frames, while every 48-kHz 20-ms period contains 960 frames, so every
 * period and chunk begins at the same phase. Reusing this buffer avoids
 * 48,000 software 64-bit divisions per second on each m68k producer. */
static void fabriclease_fill_chunk(uint64_t first_frame, uint32_t bytes)
{
  uint32_t frames = bytes / ZZ9K_FABRICLEASE_FRAME_BYTES;
  uint32_t f;

  for (f = 0; f < frames; f++) {
    uint64_t left = first_frame + (uint64_t)f;
    int16_t v = ((left / ZZ9K_FABRICLEASE_BURST_SAMPLES) & 1U)
                    ? (int16_t)ZZ9K_FABRICLEASE_AMPLITUDE
                    : (int16_t)-ZZ9K_FABRICLEASE_AMPLITUDE;
    uint16_t u = (uint16_t)v;
    uint32_t o = f * ZZ9K_FABRICLEASE_FRAME_BYTES;

    g_chunk[o + 0U] = (uint8_t)(u & 0xffU);
    g_chunk[o + 1U] = (uint8_t)(u >> 8);
    g_chunk[o + 2U] = (uint8_t)(u & 0xffU);
    g_chunk[o + 3U] = (uint8_t)(u >> 8);
  }
}

/* Low-rate observability: one framed slot snapshot (R16). Pacing
 * never depends on this call. */
static int fabriclease_print_state(ZZ9KContext *ctx, uint32_t slot)
{
  ZZ9KAudioFabricStateDesc desc;
  ZZ9KAudioFabricStateResult state;

  if (!zz9k_audio_build_fabric_state_desc(&desc, slot, 0U)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (zz9k_audio_fabric_state_get(ctx, &desc, &state) != ZZ9K_STATUS_OK) {
    return ZZ9K_STATUS_INTERNAL_ERROR;
  }
  printf("fabric: slot %lu state=%lu gen=%lu w=%lu r=%lu "
         "starved=%lu peak=0x%06lx clip=%lu\n",
         (unsigned long)state.slot, (unsigned long)state.state,
         (unsigned long)state.generation,
         (unsigned long)state.cursor_write,
         (unsigned long)state.cursor_read,
         (unsigned long)state.starvation_count,
         (unsigned long)state.peak, (unsigned long)state.clip);
  return ZZ9K_STATUS_OK;
}

/*
 * The direct-ring lifecycle against one open context: acquire,
 * validated mapping, the credit-paced feed loop (wrapped ring writes,
 * seqlock publication after every write, consumed-cursor
 * backpressure), bounded drain, release, cleanup. Returns a
 * ZZ9K_STATUS_* word; BAD_REQUEST/BUSY mean the acquire itself was
 * refused (bus admission or occupied slot) and the caller reports a
 * clean decline.
 */
int zz9k_fabriclease_session(ZZ9KContext *ctx,
                             const ZZ9KFabricLeaseOptions *options)
{
  ZZ9KAudioRingAcquireDesc acquire;
  ZZ9KAudioRingSession session;
  uint64_t target_bytes;
  uint64_t fed = 0U;
  uint32_t wait_stretch = 0U;
  uint32_t credit_retries = 0U;
  int status;

  if (ctx == 0 || options == 0 || options->gain > 255U ||
      options->slot == 0U || options->slot > ZZ9K_AUDIO_RING_SLOT_MAX) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (fabriclease_cancel_requested()) {
    return ZZ9K_STATUS_CANCELLED;
  }
  target_bytes = options->seconds == 0U
                     ? (uint64_t)ZZ9K_FABRICLEASE_CHUNK_BYTES
                     : (uint64_t)options->seconds *
                           (uint64_t)ZZ9K_FABRICLEASE_BYTES_PER_SECOND;

  if (!zz9k_audio_build_ring_acquire_desc(&acquire, options->slot, ZZ9K_AUDIO_METER_IDENTITY_SDK_STREAM, options->gain, 0U, 0U)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  status = zz9k_audio_ring_session_begin(ctx, &acquire, &session);
  if (status != ZZ9K_STATUS_OK) {
    /* Firmware refused the slot (Zorro II second acquisition, bus
     * admission, occupied slot): a clean decline, and nothing was
     * disturbed on any active producer. */
    return status;
  }

  printf("fabriclease: slot %lu generation %lu, ring %lu bytes at "
         "0x%06lx (%lu period(s)), control 0x%06lx",
         (unsigned long)session.grant.slot,
         (unsigned long)session.grant.generation,
         (unsigned long)session.grant.ring_capacity,
         (unsigned long)session.grant.ring_offset,
         (unsigned long)(session.grant.ring_capacity /
                         ZZ9K_AUDIO_RING_PERIOD_BYTES),
         (unsigned long)session.grant.control_offset);
  if ((session.grant.flags & ZZ9K_AUDIO_RING_RESULT_BUS_ZORRO2) != 0U) {
    printf(", Zorro II compact bus (%lu slot)",
           (unsigned long)session.grant.slot_count);
  } else {
    printf(", %lu slots", (unsigned long)session.grant.slot_count);
  }
  printf(", gain %lu/255", (unsigned long)session.grant.gain_applied);
  if ((session.grant.flags & ZZ9K_AUDIO_RING_RESULT_GAIN_BOUNDED) != 0U) {
    printf(" (bounded from %lu by the ceiling composition)",
           (unsigned long)options->gain);
  }
  printf("\n");
  fabriclease_fill_chunk(0U, ZZ9K_FABRICLEASE_CHUNK_BYTES);


  while (fed < target_bytes) {
    uint64_t remaining;
    uint32_t required;
    uint32_t free_bytes;
    uint32_t length;
    uint32_t staged;

    if (fabriclease_cancel_requested()) {
      status = ZZ9K_STATUS_CANCELLED;
      goto out;
    }

    /* Backpressure comes only from the consumed-credit line (R7):
     * one tearing-safe shared-memory read, zero mailbox traffic. */
    status = zz9k_audio_ring_take_credits(&session, 2U);
    if (status == ZZ9K_AUDIO_RING_CREDIT_REVOKED) {
      printf("fabriclease: generation revoked (heartbeat expiry or "
             "cursor fault); stopping\n");
      status = ZZ9K_STATUS_BAD_HANDLE;
      goto out;
    }
    if (status == ZZ9K_AUDIO_RING_CREDIT_RETRY) {
      if (++credit_retries > 64U) {
        status = ZZ9K_STATUS_TIMEOUT;
        goto out;
      }
      fabriclease_wait();
      continue;
    }
    credit_retries = 0U;

    remaining = target_bytes - fed;
    required = ZZ9K_FABRICLEASE_CHUNK_BYTES;
    if (required > session.grant.ring_capacity) {
      required = session.grant.ring_capacity;
    }
    if (remaining < (uint64_t)required) {
      required = (uint32_t)remaining;
    }
    free_bytes = zz9k_audio_ring_free_bytes(&session);
    if (free_bytes < required) {
      if (++wait_stretch > ZZ9K_FABRICLEASE_WAIT_LIMIT) {
        status = ZZ9K_STATUS_TIMEOUT;
        goto out;
      }
      fabriclease_wait();
      if ((wait_stretch % ZZ9K_FABRICLEASE_HEARTBEAT_WAITS) == 0U) {
        /* Still live while playback drains our reserve (R11). */
        zz9k_audio_ring_publish(&session);
      }
      continue;
    }
    wait_stretch = 0U;

    length = required;
    if (length == 0U) {
      break;
    }

    staged = zz9k_audio_ring_write(&session, g_chunk, length);
    if (staged == 0U) {
      status = ZZ9K_STATUS_IO_ERROR;
      goto out;
    }
    /* PCM landed first; the cursor becomes visible only now (R6),
     * and the same commit refreshes the heartbeat (R11). */
    zz9k_audio_ring_publish(&session);
    fed += staged;

  }

  /* Bounded drain: wait for firmware to take credit for everything
   * staged (a proof client leaves no uncredited bytes), heartbeating
   * throughout. */
  for (;;) {
    if (fabriclease_cancel_requested()) {
      status = ZZ9K_STATUS_CANCELLED;
      goto out;
    }
    status = zz9k_audio_ring_take_credits(&session, 2U);
    if (status == ZZ9K_AUDIO_RING_CREDIT_REVOKED) {
      printf("fabriclease: generation revoked during drain; stopping\n");
      status = ZZ9K_STATUS_BAD_HANDLE;
      goto out;
    }
    if (status == ZZ9K_AUDIO_RING_CREDIT_RETRY) {
      if (++credit_retries > 64U) {
        status = ZZ9K_STATUS_TIMEOUT;
        goto out;
      }
      fabriclease_wait();
      continue;
    }
    credit_retries = 0U;
    if (zz9k_audio_ring_outstanding(&session) == 0U) {
      break;
    }
    if (++wait_stretch > ZZ9K_FABRICLEASE_WAIT_LIMIT) {
      status = ZZ9K_STATUS_TIMEOUT;
      goto out;
    }
    fabriclease_wait();
    if ((wait_stretch % ZZ9K_FABRICLEASE_HEARTBEAT_WAITS) == 0U) {
      zz9k_audio_ring_publish(&session);
    }
  }
  status = fabriclease_print_state(ctx, options->slot);

out:
  if (zz9k_audio_ring_session_end(ctx, &session) != ZZ9K_STATUS_OK) {
    printf("fabriclease: ring release failed\n");
    if (status == ZZ9K_STATUS_OK || status == ZZ9K_STATUS_CANCELLED) {
      status = ZZ9K_STATUS_INTERNAL_ERROR;
    }
  } else if (status == ZZ9K_STATUS_OK) {
    printf("fabriclease: released after %lu bytes\n", (unsigned long)fed);
  }
  return status;
}

#if !ZZ9K_FABRICLEASE_NO_MAIN
static void print_usage(void)
{
  printf("usage: zz9k-fabriclease [--seconds N] [--gain 0..255] "
         "[--slot 1|2] [--force]\n");
}

int main(int argc, char **argv)
{
  ZZ9KFabricLeaseOptions options;
  ZZ9KContext *ctx = 0;
  ZZ9KCaps caps;
  int status;
  int i;
  int force = 0;
#if ZZ9K_FABRICLEASE_AMIGA
  (void)signal(SIGINT, fabriclease_sigint);
#endif

  memset(&options, 0, sizeof(options));
  options.seconds = ZZ9K_FABRICLEASE_DEFAULT_SECONDS;
  options.gain = 128U;
  options.slot = 1U;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
      options.seconds = (uint32_t)strtoul(argv[++i], 0, 10);
    } else if (strcmp(argv[i], "--gain") == 0 && i + 1 < argc) {
      options.gain = (uint32_t)strtoul(argv[++i], 0, 10);
    } else if (strcmp(argv[i], "--slot") == 0 && i + 1 < argc) {
      options.slot = (uint32_t)strtoul(argv[++i], 0, 10);
    } else if (strcmp(argv[i], "--force") == 0) {
      force = 1;
    } else {
      print_usage();
      return 1;
    }
  }
  if (options.gain > 255U || options.slot == 0U ||
      options.slot > ZZ9K_AUDIO_RING_SLOT_MAX) {
    print_usage();
    return 1;
  }

  status = zz9k_open(&ctx);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-fabriclease: open failed: %s (%d)\n",
           zz9k_status_name(status), status);
    return 1;
  }
  memset(&caps, 0, sizeof(caps));
  status = zz9k_query_caps(ctx, &caps);
  if (status == ZZ9K_STATUS_OK) {
    status = zz9k_fabriclease_gate(caps.capability_bits);
  }
  if (status == ZZ9K_FABRICLEASE_DECLINE_NO_FABRIC) {
    if (!force) {
      /* Mixed-version fallback: a clean, documented decline, not an
       * error. */
      printf("zz9k-fabriclease: %s\n",
             zz9k_fabriclease_status_name(status));
      zz9k_close(ctx);
      return 0;
    }
    /* Qualification-only path: instrument firmware implements the
     * opcodes but deliberately withholds the capability until the
     * hardware session passes. Unsupported firmware still fails the
     * first ring opcode cleanly. */
    printf("zz9k-fabriclease: forcing unadvertised fabric opcodes "
           "(hardware qualification only)\n");
    status = ZZ9K_FABRICLEASE_OK;
  }
  if (status != ZZ9K_FABRICLEASE_OK) {
    printf("zz9k-fabriclease: %s (%d)\n", zz9k_status_name(status), status);
    zz9k_close(ctx);
    return 1;
  }

  status = zz9k_fabriclease_session(ctx, &options);
  zz9k_close(ctx);
  if (status == ZZ9K_STATUS_CANCELLED) {
    printf("zz9k-fabriclease: cancelled; slot released\n");
    return 0;
  }
  if (status == ZZ9K_STATUS_BAD_REQUEST || status == ZZ9K_STATUS_BUSY) {
    /* The acquire was refused (Zorro II second acquisition, bus
     * admission, occupied slot): nothing was disturbed. */
    printf("zz9k-fabriclease: slot %lu refused: %s (%d)\n",
           (unsigned long)options.slot, zz9k_status_name(status), status);
    return 0;
  }
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-fabriclease: session failed: %s (%d)\n",
           zz9k_status_name(status), status);
    return 1;
  }
  return 0;
}
#endif /* !ZZ9K_FABRICLEASE_NO_MAIN */
