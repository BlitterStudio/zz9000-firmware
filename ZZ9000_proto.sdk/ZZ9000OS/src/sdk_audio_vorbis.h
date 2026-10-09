/*
 * Ogg Vorbis backend for the codec-aware audio stream (U8).
 *
 * The client feeds the whole .ogg/.oga file from byte 0. libogg's sync layer
 * captures and CRC-checks pages (input is copied into its buffer, so a page
 * larger than the session's input ring is fine); the backend reassembles
 * packets from the lacing values itself, so every page can be checked
 * against the single logical stream (one serial, consecutive sequence
 * numbers, consistent continuation flags) and no packet is buffered beyond
 * SDK_VORBIS_MAX_PACKET_BYTES. Packets go to fixed-point Tremor; its
 * 32-bit PCM is narrowed exactly like Tremor's ov_read (>> 9, clip) into
 * interleaved S16BE at the native rate and channel count. PCM can stop at
 * any frame boundary when the ring or the per-call budget is full.
 *
 * Header packets: the identification header must be alone on the BOS page
 * (non-Vorbis BOS packets such as Opus, Ogg-FLAC or Theora are UNSUPPORTED;
 * a Vorbis comment/setup header there is IO_ERROR). The comment header is
 * validated by a streaming parser and never buffered, however large its
 * fields (embedded cover art): Tremor is handed an equivalent empty comment
 * header. The setup header is bounded by SDK_VORBIS_MAX_PACKET_BYTES, and
 * its codebooks by SDK_VORBIS_MAX_BOOK_USED (Tremor's decode-table build
 * uses alloca() proportional to a book's used entries) before
 * vorbis_synthesis_init() runs.
 *
 * Exactly one logical stream: a second BOS page (multiplexed or chained)
 * stops decoding and, once every PCM byte of the first stream has been
 * published and read by the client, faults the stream with UNSUPPORTED.
 *
 * Memory: every libogg/Tremor/backend allocation comes from the stream's
 * tracked arena (sdk_vorbis_alloc.h), bounded by SDK_VORBIS_ALLOC_LIMIT and
 * SDK_VORBIS_HEAP_MAX_REGIONS tracker slots. The arena is released as soon
 * as the stream completes or faults, and otherwise on close.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_AUDIO_VORBIS_H
#define SDK_AUDIO_VORBIS_H

#include <stddef.h>
#include <stdint.h>

#include "sdk_audio_codec_io.h"
#include "sdk_vorbis_alloc.h"

#define SDK_VORBIS_MIN_RATE        8000U
#define SDK_VORBIS_MAX_RATE        192000U
#define SDK_VORBIS_MAX_CHANNELS    2U
/* Largest setup or audio packet the backend will reassemble. libvorbis
 * 1.3.7 setup headers measure 2.4-4.2 KiB and audio packets at most
 * 1.5 KiB (q10 stereo); a whole Ogg page body is at most 65025 bytes.
 * Larger packets are UNSUPPORTED. */
#define SDK_VORBIS_MAX_PACKET_BYTES (64U * 1024U)
/* Most used entries in one codebook. Tremor's vorbis_book_init_decode()
 * puts 8 bytes per used entry on the stack (64 KiB here, against the 1 MiB
 * core-1 stack); libvorbis 1.3.7 presets (q-1..10, 8-96 kHz) use at most
 * 625. Larger books are UNSUPPORTED. */
#define SDK_VORBIS_MAX_BOOK_USED   8192U
/* Arena region size and per-stream ceiling. Peak tracked bytes measured in
 * test/vorbis: 128 KiB mono and 256 KiB stereo (whole 128 KiB regions;
 * the stereo payload alone fits 192 KiB of 64 KiB regions). The legal
 * worst case adds 8192-sample blocks (+~112 KiB of stereo DSP/block
 * buffers over the measured 2048), a maximum-size page in libogg's sync
 * buffer (~70 KiB) and a full reassembly buffer (64 KiB): about 450 KiB of
 * payload, so 1 MiB (= SDK_VORBIS_HEAP_MAX_REGIONS regions, i.e. at most 8
 * of the 64 decode-tracker slots) leaves 2x headroom while bounding
 * hostile codebooks, which fail NO_MEMORY at the ceiling. */
#define SDK_VORBIS_REGION_BYTES    (128U * 1024U)
#define SDK_VORBIS_ALLOC_LIMIT     (1024U * 1024U)

struct sdk_vorbis_state {
	struct sdk_vorbis_heap heap;
	void *codec;                /* arena-resident decoder state, NULL until
	                             * the first feed and after release */
	uint32_t sample_rate;       /* 0 until the identification header */
	uint32_t channels;
	uint16_t status;            /* sticky failure status, 0 while healthy */
	uint16_t pending_status;    /* raised once published PCM is all read */
	uint8_t done;               /* end of stream validated */
};

void sdk_vorbis_init(struct sdk_vorbis_state *st);
/* Decode as much as input, PCM room and budget allow. Returns
 * SDK_STATUS_OK or the sticky failure status (UNSUPPORTED for a valid
 * stream outside the envelope, IO_ERROR for malformed/corrupt/truncated
 * input, NO_MEMORY for allocation failure). Must run on the core that owns
 * the arena (core 1 for core-1-affine streams). */
uint16_t sdk_vorbis_decode(struct sdk_vorbis_state *st,
                           struct sdk_audio_codec_io *io);
/* Nonzero while the arena holds tracked regions. */
int sdk_vorbis_holds_memory(const struct sdk_vorbis_state *st);
/* Free the arena on the core that allocated it. */
void sdk_vorbis_release(struct sdk_vorbis_state *st);
/* Drop arena pointers after a core-1 reclaim already freed them. */
void sdk_vorbis_forget(struct sdk_vorbis_state *st);

#endif /* SDK_AUDIO_VORBIS_H */
