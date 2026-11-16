/*
 * Codec-aware audio stream session state and the MP3 ring-decode pass.
 *
 * The session struct and the ring bookkeeping the decoders share live in
 * their own translation unit so the host tests can drive the exact
 * production decode pass (test/audio/audio_stream_decode_test.c) --
 * sdk_mailbox.c itself is firmware-only. Nothing here touches the
 * mailbox, the shared-buffer registry or Zorro: the struct's ring
 * addresses are already resolved at BEGIN.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_AUDIO_STREAM_H
#define SDK_AUDIO_STREAM_H

#include <stdint.h>

#include "sdk_mailbox.h"
#include "sdk_audio_flac.h"
#include "sdk_audio_vorbis.h"
#include "audio_stream_drain.h"
#include "mp3/minimp3.h"

/* The MP3 stream loop keeps a lookahead reserve of compressed input; EOF
 * and a resumable drain both relax that gate. */
#define SDK_AUDIO_STREAM_MIN_INPUT_BYTES (4U * 1024U)

struct SDKAudioStream {
	uint32_t id;
	uint32_t codec;
	struct SDKSharedBuffer *mp3_ring;
	struct SDKSharedBuffer *pcm_ring;
	uint32_t mp3_capacity;
	uint32_t pcm_capacity;
	uint32_t input_offset;
	uint32_t input_length;
	/*
	 * PCM ring cursors as monotonic byte totals, u32 wrap-safe, each with
	 * exactly ONE writer so no atomics are needed (the table is in the
	 * SCU-coherent region):
	 *  - pcm_written_total: decode side (core 1 for affine streams),
	 *    advanced per frame; producer-internal ring geometry.
	 *  - pcm_ready_total: decode side, published AFTER the PCM cache
	 *    flush (Xil_DCacheFlushRange ends in a DSB, ordering the store)
	 *    -- consumers may only read ring bytes below this total.
	 *  - pcm_consumed_total: the consumer -- the AUDIO_STREAM_READ path
	 *    for unbound streams, the AX playback pump for bound ones (a
	 *    bound stream rejects READ, so the writer is always unique).
	 * DIFFERENCES of these totals stay correct across the u32 wrap, but
	 * `total % pcm_capacity` (ring offsets) does not once a total passes
	 * 2^32 for a non-power-of-2 capacity: a single session degrades
	 * after 4 GiB of PCM (~6 h of continuous 48 kHz stereo). Accepted --
	 * sessions are per-file/per-track in every current client.
	 */
	uint32_t pcm_written_total;
	uint32_t pcm_ready_total;
	uint32_t pcm_consumed_total;
	uint32_t low_water_bytes;
	uint32_t high_water_bytes;
	uint32_t sample_rate;
	uint32_t channels;
	uint32_t sample_format;
	uint32_t frames_decoded;
	uint32_t bytes_consumed;
	uint32_t bytes_produced;
	uint32_t output_frame_limit;
	int eof;
	int drain_requested;
	int drain_input_complete;
	int initialized;
	int backpressure;
	int vbr_checked;
	int decode_complete;
	/* Native decoders can reach a terminal output boundary before they
	 * publish a deferred terminal status; the pump must drain that PCM. */
	uint8_t output_terminal;
	struct audio_stream_refill_checkpoint refill_checkpoint;
	/* Ring addresses resolved from the shared-buffer registry at BEGIN:
	 * the registry is core-0-only, so core-1 feeds/reads must never
	 * dereference mp3_ring/pcm_ring -- they use these instead. */
	/* uintptr_t so the host tests can point these at plain arrays; on
	 * the firmware (32-bit ARM) it is the same u32 address as before. */
	uintptr_t mp3_ring_addr;
	uintptr_t pcm_ring_addr;
	/* Fixed at BEGIN: nonzero = feed/read run on the core-1 worker (the
	 * mp3 staging ring is then cache-owned by core 1). */
	uint32_t core1_affine;
	/* Set when a core-1 fault may have left the embedded decoder state
	 * mid-frame; feeds/reads answer IO_ERROR, close still works. */
	uint32_t faulted;
	/* Bound-playback tail guard: the fabric compositor advances
	 * pcm_consumed_total when it STAGES PCM into the TX ring, which
	 * runs up to the fill target ahead of the DMA. Set while staged
	 * audio is still queued for the DMA so end-of-stream (DONE /
	 * PCM_READY-clear) waits for it to actually play out. Managed by
	 * the pump slot; always 0 for unbound streams (the READ consumer
	 * hears bytes immediately). */
	uint8_t pump_tail_pending;
	/* Main-loop writers update this aligned word; the fabric ISR consumes it
	 * only through the pump slot's existing Q7 gain pass. */
	uint16_t gain;
	/* Native codec sessions (FLAC, Vorbis) only: sticky Feed/Read status
	 * once faulted (0 means the legacy IO_ERROR), input starvation of the
	 * backend, and the backend state whose decoder heap lives on the
	 * decode core. The codec is fixed for the stream's life. */
	uint16_t fault_status;
	uint8_t backend_starved;
	/* Background close (audio_stream_close.h): closed by the client but
	 * the slot still holds a core-1 decoder heap; release task queued.
	 * Written by core 0 only. */
	uint8_t closing;
	uint8_t release_queued;
	uint16_t owner_id;
	union {
		struct sdk_flac_state flac;
		struct sdk_vorbis_state vorbis;
	};
	mp3dec_t decoder;
	mp3d_sample_t scratch[MINIMP3_MAX_SAMPLES_PER_FRAME];
};

/* Ring bookkeeping shared by the MP3 pass, the native codec pass in
 * sdk_mailbox.c and the feed/read/pump paths. */
uint32_t audio_stream_pcm_used(const struct SDKAudioStream *stream);
uint32_t audio_stream_pcm_free(const struct SDKAudioStream *stream);
void audio_stream_consume_input(struct SDKAudioStream *stream,
                                uint32_t bytes);
void audio_stream_discard_input(struct SDKAudioStream *stream);
void audio_stream_pcm_write(struct SDKAudioStream *stream,
                            const uint8_t *src, uint32_t bytes);
void flush_audio_pcm_written(const struct SDKAudioStream *stream,
                             uint8_t *dst, uint32_t offset, uint32_t bytes);
void byteswap_pcm16(uint8_t *data, uint32_t bytes);

/* One bounded MP3 decode pass over the stream's rings: as many frames as
 * input, PCM room, the low-water quantum and the ~2 ms service cap allow
 * (see sdk_audio_codec_io.h for the 333 MHz global-timer basis). Returns 1
 * when the pass made progress. */
uint32_t audio_stream_decode_mp3(struct SDKAudioStream *stream);

#endif /* SDK_AUDIO_STREAM_H */
