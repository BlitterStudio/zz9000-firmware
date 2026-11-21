/*
 * PCM handling of the ZZ9000 mpega.library: reading the card's PCM ring,
 * turning it into the caller's buffers, choosing what the card reduces and
 * feeding the card's stream. No AmigaOS calls, so the host tests compile
 * this file as it is.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MPEGA_CONVERT_H
#define MPEGA_CONVERT_H

#include <stdint.h>

#include <libraries/mpega.h>

/*
 * Copy `bytes` from the ring (a window onto card memory, so it is read with
 * aligned longword accesses where the address allows) to dst, wrapping at
 * ring_length. offset must be below ring_length and bytes at most
 * ring_length. The byte order is kept: the result is what a byte copy would
 * give.
 */
void mpega_ring_read(uint8_t *dst, const volatile uint8_t *ring,
                     uint32_t ring_length, uint32_t offset, uint32_t bytes);

/* The ring offset after `bytes` were read from `offset`. */
uint32_t mpega_ring_advance(uint32_t offset, uint32_t bytes,
                            uint32_t ring_length);

/*
 * Turn src_frames frames of big-endian signed 16-bit PCM (src_channels 1 or
 * 2 interleaved, src 2-byte aligned) into out_channels (1 or 2) native
 * int16_t channels dst0 and dst1 (dst1 is unused for one channel):
 *   - two channels to one are mixed as (L + R) / 2, rounded toward zero;
 *   - one channel to two is duplicated;
 *   - every div-th frame is kept (div 1, 2 or 4; no filtering, this is the
 *     path for cards that cannot reduce the rate themselves);
 *   - each sample is scaled by scale_percent / 100 (truncating toward zero)
 *     and clamped to 16 bits, and *clipped is increased by the number of
 *     samples the clamp changed.
 * Returns the frames written to each channel: (src_frames + div - 1) / div.
 */
uint32_t mpega_pcm_prepare(const uint8_t *src, uint32_t src_frames,
                           uint32_t src_channels, uint32_t out_channels,
                           uint32_t div, int32_t scale_percent,
                           int16_t *dst0, int16_t *dst1, uint32_t *clipped);

/* The largest size of the samples, 0 to 32767 (-32768 counts as 32767). */
uint16_t mpega_pcm_peak(const int16_t *samples, uint32_t count);

/* Whether `out` names a known format and the buffers it needs for a stream
 * of `channels` decoded channels. */
int mpega_out_valid(const struct MpgaOut *out, uint32_t channels);

/*
 * MPGA_decode_to's writer: frames samples of each channel (left, right) go
 * into out's rings in out->mo_Format, starting at out->mo_Index. Sets
 * out->mo_Peak and advances out->mo_Index by frames. *dither is the state of
 * the S8D noise. Any start value will do; carry it from call to call.
 */
void mpega_pcm_write_out(struct MpgaOut *out, const int16_t *left,
                         const int16_t *right, uint32_t frames,
                         uint32_t channels, uint32_t *dither);

/* What the card is asked to reduce (see ZZ9K_SERVICE_FLAG_AUDIO_STREAM_REDUCE
 * in zz9k/abi.h). Zero fields mean the stream's own layout. */
typedef struct MPEGAReducePlan {
  uint32_t output_hz;
  uint32_t output_channels;
} MPEGAReducePlan;

/*
 * Choose the reduction for a stream whose header was read (header_probed):
 * mono when force_mono applies to a stereo stream, and probed_rate / freq_div
 * as the maximum rate when freq_div is above 1. Nothing is requested unless
 * the audio service reports the reduce flag.
 */
void mpega_reduce_plan(MPEGAReducePlan *plan, uint32_t service_flags,
                       int header_probed, uint32_t stream_channels,
                       int force_mono, uint32_t probed_rate,
                       uint32_t freq_div);

/* Starts the card stream with the given reduction and returns the status. */
typedef int (*MPEGAReduceBeginFn)(void *context, uint32_t output_hz,
                                  uint32_t output_channels);

/*
 * Begin through `begin`. A card that rejects a non-zero reduction with
 * ZZ9K_STATUS_UNSUPPORTED is asked again with zeros. On return *plan holds
 * the reduction in force (zeros after a retry), and the result is the last
 * status.
 */
int mpega_reduce_begin(MPEGAReducePlan *plan, MPEGAReduceBeginFn begin,
                       void *context);

/* What the feed loop needs from a card stream. Every call except
 * pcm_available returns 0 on failure. The card fields are those of the
 * latest ZZ9KAudioStreamResult. */
typedef struct MPEGAFeedOps {
  void *context;
  /* PCM the caller has not read yet is waiting in the ring. */
  int (*pcm_available)(void *context);
  /* Return all PCM credit (a READ of what was read). */
  int (*ack)(void *context);
  /* Read the next chunk of the source into the staging buffer; *got is
   * its length, 0 at the end of the source. */
  int (*read_stage)(void *context, uint32_t *got);
  /* FEED length staged bytes with ZZ9K_AUDIO_STREAM_FEED_* flags. */
  int (*feed)(void *context, uint32_t length, uint32_t flags);
  /* A zero-byte READ: one decode pass, no credit returned. */
  int (*kick)(void *context);
  /* ZZ9K_AUDIO_STREAM_RESULT_* flags, bytes_consumed and bytes_produced. */
  uint32_t (*result_flags)(void *context);
  uint32_t (*bytes_consumed)(void *context);
  uint32_t (*bytes_produced)(void *context);
} MPEGAFeedOps;

/* Feed progress of one card stream; zero it when a stream (re)starts. */
typedef struct MPEGAFeedState {
  uint32_t staged_pending; /* staged bytes a BACKPRESSURE feed did not take */
  uint8_t eof_sent;
  uint8_t eof_stalls;
} MPEGAFeedState;

#define MPEGA_FEED_NO_PCM 1

/*
 * Feed the card until it has PCM. A feed the input ring has no room for comes
 * back with BACKPRESSURE and keeps none of the chunk; the chunk stays staged
 * and is offered again after a decode pass has used up input. After the EOF
 * feed the decoder still works in bounded passes, so passes run until the
 * card reports DONE, or until a few passes in a row make no progress.
 * Returns MPEGA_ERR_NONE (PCM is ready), MPEGA_ERR_EOF, MPEGA_ERR_BADFRAME,
 * or MPEGA_FEED_NO_PCM after max_attempts rounds without PCM.
 */
int mpega_feed_until_pcm(MPEGAFeedState *state, const MPEGAFeedOps *ops,
                         uint32_t max_attempts);

#endif /* MPEGA_CONVERT_H */
