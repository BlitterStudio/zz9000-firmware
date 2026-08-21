/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * ZZ9000 SDK v2 mailbox dispatcher.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <string.h>
#include "xil_cache.h"
#include "xtime_l.h"
#include "sdk_palette.h"
#include "sdk_mailbox.h"
#include "sdk_compression.h"
#include "lzh/zz9k_lzh.h"
#include "sdk_crypto.h"
#include "sdk_offload_params.h"
#include "sdk_image_stream.h"
#include "sdk_video_stream.h"
#include "sdk_media_session.h"
#include "audio_playback_frontier.h"
#include "audio_stream_drain.h"
#include "audio_convert.h"
#include "sdk_jpeg.h"
#include "sdk_surface.h"
#include "sdk_aperture_layout.h"
#include "sdk_smp_lock.h"
#include "memorymap.h"
#include "scheduler.h"
#include "core2.h"
#include "ax.h"
#include "interrupt.h"
#include "video.h"
#include "video_scale.h"
#include "overlay.h"
#include "mp3/mp3.h"
#include "mp3/minimp3.h"

#define SDK_MAILBOX_REQUEST_OFFSET     SDK_MAILBOX_DESCRIPTOR_SIZE
#define SDK_MAILBOX_COMPLETION_OFFSET  \
	(SDK_MAILBOX_REQUEST_OFFSET + \
	 (SDK_MAILBOX_RING_ENTRIES * SDK_MAILBOX_ENTRY_SIZE))
#define SDK_MAILBOX_TOTAL_SIZE         \
	(SDK_MAILBOX_COMPLETION_OFFSET + \
	 (SDK_MAILBOX_RING_ENTRIES * SDK_MAILBOX_ENTRY_SIZE))
#define SDK_MAILBOX_BASE_CAPABILITY_BITS \
	(SDK_CAP_MAILBOX | SDK_TRANSPORT_CAPABILITY_BITS | \
	 SDK_CAP_SERVICE_DISCOVERY | \
	 SDK_CAP_SHARED_ALLOC | SDK_CAP_SURFACES | \
	 SDK_CAP_FRAMEBUFFER_SURFACE | SDK_CAP_IMAGE_DECODE | \
	 SDK_CAP_IMAGE_SCALE | SDK_CAP_AUDIO_DECODE | \
	 SDK_CAP_AUDIO_PLAYBACK | \
	 SDK_CAP_MEMORY_OPS | SDK_CAP_CRYPTO | \
	 SDK_CAP_DIAGNOSTICS | SDK_CAP_SURFACE_OPS | SDK_CAP_COMPRESSION | \
	 SDK_CAP_VIDEO_DECODE | \
	 SDK_CAP_MEDIA_SESSION | SDK_CAP_AUDIO_STREAM_DRAIN)
/* Decode proceeds only with at least this much undecoded input (unless
 * EOF): enough for the largest legal MP3 frame (~1.4K, 2.3K free-format)
 * plus sync lookahead. It must stay WELL below any client's input-ring
 * size -- at the old 16K, a client with a 16K input ring could only
 * decode with the ring full to the byte, and once decode freed a
 * non-chunk-multiple of space the client's fixed-size feeds never fit
 * again: a permanent stall. */
#define SDK_AUDIO_STREAM_MIN_INPUT_BYTES (4U * 1024U)

typedef char SDKMailbox_must_fit_legacy_io_window[
	((SDK_MAILBOX_WINDOW_OFFSET + SDK_MAILBOX_TOTAL_SIZE) <= 0x00010000U) ?
	1 : -1
];

struct SDKMailboxDescriptor {
	uint8_t magic[4];
	uint8_t abi_major[2];
	uint8_t abi_minor[2];
	uint8_t descriptor_size[4];
	uint8_t request_ring_offset[4];
	uint8_t request_ring_entries[4];
	uint8_t request_head[4];
	uint8_t request_tail[4];
	uint8_t completion_ring_offset[4];
	uint8_t completion_ring_entries[4];
	uint8_t completion_head[4];
	uint8_t completion_tail[4];
	uint8_t capability_bits[4];
	uint8_t reserved[80];
};

struct SDKMailboxEntry {
	uint8_t request_id[4];
	uint8_t opcode[2];
	uint8_t status[2];
	uint8_t flags[2];
	uint8_t payload_len[2];
	uint8_t user_cookie[4];
	uint8_t payload[48];
};

struct SDKCapsPayload {
	uint8_t magic[4];
	uint8_t abi_major[2];
	uint8_t abi_minor[2];
	uint8_t capability_bits[4];
	uint8_t max_inline_payload[4];
	uint8_t max_shared_buffers[4];
	uint8_t max_surfaces[4];
	uint8_t firmware_version[4];
	uint8_t request_ring_entries[4];
	uint8_t completion_ring_entries[4];
	uint8_t host_window_heap_size[4];
	uint8_t reserved[8];
};

struct SDKQueryServicePayload {
	uint8_t service_id[4];
	uint8_t reserved[44];
};

struct SDKQueryApertureLayoutPayload {
	uint8_t profile[4];
	uint8_t aperture_size[4];
	uint8_t framebuffer_base[4];
	uint8_t framebuffer_size[4];
	uint8_t pip_base[4];
	uint8_t pip_size[4];
	uint8_t template_base[4];
	uint8_t template_size[4];
	uint8_t host_window_base[4];
	uint8_t host_window_size[4];
	uint8_t audio_base[4];
	uint8_t audio_size[4];
};

struct SDKServiceInfoPayload {
	uint8_t service_id[4];
	uint8_t version[4];
	uint8_t capability_bits[4];
	uint8_t flags[4];
	uint8_t opcode_base[4];
	uint8_t opcode_count[4];
	uint8_t max_inline_payload[4];
	uint8_t name[20];
};

struct SDKServiceDescriptor {
	uint32_t service_id;
	uint32_t version;
	uint32_t capability_bits;
	uint32_t flags;
	uint32_t opcode_base;
	uint32_t opcode_count;
	const char *name;
};

struct SDKAllocSharedPayload {
	uint8_t length[4];
	uint8_t alignment[4];
	uint8_t flags[4];
	uint8_t reserved[36];
};

struct SDKSharedBufferInfoPayload {
	uint8_t handle[4];
	uint8_t arm_addr[4];
	uint8_t length[4];
	uint8_t flags[4];
	uint8_t reserved[32];
};

struct SDKFreeSharedPayload {
	uint8_t handle[4];
	uint8_t reserved[44];
};

struct SDKMemFillPayload {
	uint8_t handle[4];
	uint8_t offset[4];
	uint8_t length[4];
	uint8_t value;
	uint8_t reserved[35];
};

struct SDKMemCopyPayload {
	uint8_t dst_handle[4];
	uint8_t dst_offset[4];
	uint8_t src_handle[4];
	uint8_t src_offset[4];
	uint8_t length[4];
	uint8_t flags[4];
	uint8_t reserved[24];
};

struct SDKDiagPayload {
	uint8_t requests_completed[4];
	uint8_t requests_failed[4];
	uint8_t last_status[4];
	uint8_t pending_requests[4];
	uint8_t shared_buffers_used[4];
	uint8_t shared_heap_total[4];
	uint8_t shared_heap_free[4];
	uint8_t shared_heap_largest_free[4];
	uint8_t mailbox_arm_addr[4];
	uint8_t mailbox_ring_entries[4];
	uint8_t surfaces_used[4];
	uint8_t allocator_invalid_slots[4];
};

struct SDKDiagTimingPayload {
	uint8_t version[4];
	uint8_t timer_hz[4];
	uint8_t requests_timed[4];
	uint8_t total_us[4];
	uint8_t surface_requests[4];
	uint8_t surface_us[4];
	uint8_t audio_requests[4];
	uint8_t audio_us[4];
	uint8_t last_opcode[4];
	uint8_t last_us[4];
	uint8_t max_opcode[4];
	uint8_t max_us[4];
};

struct SDKDiagSchedPayload {
	uint8_t version[4];
	uint8_t core1_online[4];
	uint8_t tasks_on_core1[4];
	uint8_t tasks_on_core0[4];
	/* version 2: decode-only timing, see timing_decode_requests/_us. */
	uint8_t decode_requests[4];
	uint8_t decode_us[4];
};

struct SDKDiagMemoryPayload {
	uint8_t version[4];
	uint8_t layout_state[4];
	uint8_t aperture_size[4];
	uint8_t aperture_info[4];
	uint8_t host_window_board_base[4];
	uint8_t host_window_arm_base[4];
	uint8_t host_window_total[4];
	uint8_t host_window_free[4];
	uint8_t host_window_largest_free[4];
	uint8_t host_window_allocations[4];
	uint8_t allocator_invalid_slots[4];
	uint8_t reserved[4];
};

struct SDKSurfaceInfoPayload {
	uint8_t handle[4];
	uint8_t arm_addr[4];
	uint8_t width[4];
	uint8_t height[4];
	uint8_t pitch[4];
	uint8_t format[4];
	uint8_t flags[4];
	uint8_t length[4];
	uint8_t reserved[16];
};

struct SDKAllocSurfacePayload {
	uint8_t width[4];
	uint8_t height[4];
	uint8_t format[4];
	uint8_t flags[4];
	uint8_t pitch[4];
	uint8_t reserved[28];
};

struct SDKFreeSurfacePayload {
	uint8_t handle[4];
	uint8_t reserved[44];
};

struct SDKScaleImagePayload {
	uint8_t src_surface[4];
	uint8_t dst_surface[4];
	uint8_t src_x[4];
	uint8_t src_y[4];
	uint8_t src_w[4];
	uint8_t src_h[4];
	uint8_t dst_x[4];
	uint8_t dst_y[4];
	uint8_t dst_w[4];
	uint8_t dst_h[4];
	uint8_t filter[4];
	uint8_t flags[4];
};

struct SDKScaleImageClippedPayload {
	uint8_t src_surface[4];
	uint8_t dst_surface[4];
	uint8_t src_x[2];
	uint8_t src_y[2];
	uint8_t src_w[2];
	uint8_t src_h[2];
	uint8_t dst_x[2];
	uint8_t dst_y[2];
	uint8_t dst_w[2];
	uint8_t dst_h[2];
	uint8_t clip_x[2];
	uint8_t clip_y[2];
	uint8_t clip_w[2];
	uint8_t clip_h[2];
	uint8_t filter[4];
	uint8_t flags[4];
	uint8_t reserved[8];
};

struct SDKImageDecodePayload {
	uint8_t src_handle[4];
	uint8_t src_offset[4];
	uint8_t src_length[4];
	uint8_t dst_surface[4];
	uint8_t dst_x[4];
	uint8_t dst_y[4];
	uint8_t dst_width[4];
	uint8_t dst_height[4];
	uint8_t output_format[4];
	uint8_t flags[4];
	uint8_t reserved[8];
};

struct SDKImageDecodeResultPayload {
	uint8_t width[4];
	uint8_t height[4];
	uint8_t output_format[4];
	uint8_t flags[4];
	uint8_t bytes_written[4];
	uint8_t reserved[28];
};

struct SDKImageSessionBeginPayload {
	uint8_t codec[4];
	uint8_t output_mode[4];
	uint8_t dst_surface[4];
	uint8_t dst_x[4];
	uint8_t dst_y[4];
	uint8_t dst_width[4];
	uint8_t dst_height[4];
	uint8_t output_format[4];
	uint8_t tile_handle[4];
	uint8_t tile_stride[4];
	uint8_t tile_rows[4];
	uint8_t flags[4];
};

struct SDKImageSessionFeedPayload {
	uint8_t session[4];
	uint8_t src_handle[4];
	uint8_t src_offset[4];
	uint8_t src_length[4];
	uint8_t flags[4];
	uint8_t reserved[28];
};

struct SDKImageSessionResultPayload {
	uint8_t session[4];
	uint8_t state[4];
	uint8_t image_width[4];
	uint8_t image_height[4];
	uint8_t output_format[4];
	uint8_t tile_x[4];
	uint8_t tile_y[4];
	uint8_t tile_width[4];
	uint8_t tile_height[4];
	uint8_t bytes_consumed[4];
	uint8_t bytes_written[4];
	uint8_t flags[4];
};

struct SDKImageSessionClosePayload {
	uint8_t session[4];
	uint8_t flags[4];
	uint8_t reserved[40];
};

struct SDKAudioDecodePayload {
	uint8_t src_handle[4];
	uint8_t src_offset[4];
	uint8_t src_length[4];
	uint8_t dst_handle[4];
	uint8_t dst_offset[4];
	uint8_t dst_capacity[4];
	uint8_t output_hz[4];
	uint8_t output_channels[4];
	uint8_t output_format[4];
	uint8_t flags[4];
	uint8_t reserved[8];
};

struct SDKAudioDecodeResultPayload {
	uint8_t bytes_consumed[4];
	uint8_t bytes_written[4];
	uint8_t sample_rate[4];
	uint8_t channels[4];
	uint8_t sample_format[4];
	uint8_t frames_written[4];
	uint8_t flags[4];
	uint8_t reserved[20];
};

struct SDKAudioStreamBeginPayload {
	uint8_t mp3_ring_handle[4];
	uint8_t mp3_ring_capacity[4];
	uint8_t pcm_ring_handle[4];
	uint8_t pcm_ring_capacity[4];
	uint8_t output_hz[4];
	uint8_t output_channels[4];
	uint8_t output_format[4];
	uint8_t low_water_bytes[4];
	uint8_t high_water_bytes[4];
	uint8_t flags[4];
	uint8_t reserved[8];
};

struct SDKVideoSessionBeginPayload {
	uint8_t codec[4];
	uint8_t container[4];
	uint8_t width[4];
	uint8_t height[4];
	uint8_t output_format[4];
	uint8_t flags[4];
	uint8_t reserved[24];
};

struct SDKVideoSessionWritePayload {
	uint8_t session[4];
	uint8_t src_handle[4];
	uint8_t src_offset[4];
	uint8_t src_length[4];
	uint8_t flags[4];
	uint8_t reserved[28];
};

struct SDKVideoSessionDecodePayload {
	uint8_t session[4];
	uint8_t flags[4];
	uint8_t reserved[40];
};

struct SDKVideoSessionClosePayload {
	uint8_t session[4];
	uint8_t flags[4];
	uint8_t reserved[40];
};

struct SDKVideoSessionResultPayload {
	uint8_t session[4];
	uint8_t state[4];
	uint8_t width[4];
	uint8_t height[4];
	uint8_t frame_rate_milli[4];
	uint8_t frame_number[4];
	uint8_t frame_time_millis[4];
	uint8_t bytes_accepted[4];
	uint8_t bytes_written[4];
	uint8_t flags[4];
	uint8_t reserved[8];
};

struct SDKAudioStreamFeedPayload {
	uint8_t session[4];
	uint8_t src_handle[4];
	uint8_t src_offset[4];
	uint8_t src_length[4];
	uint8_t flags[4];
	uint8_t reserved[28];
};

struct SDKAudioStreamReadPayload {
	uint8_t session[4];
	uint8_t pcm_read[4];
	uint8_t flags[4];
	uint8_t reserved[36];
};

struct SDKAudioStreamClosePayload {
	uint8_t session[4];
	uint8_t flags[4];
	uint8_t reserved[40];
};

struct SDKAudioStreamResultPayload {
	uint8_t session[4];
	uint8_t state[4];
	uint8_t sample_rate[4];
	uint8_t channels[4];
	uint8_t sample_format[4];
	uint8_t mp3_read[4];
	uint8_t pcm_write[4];
	uint8_t pcm_read[4];
	uint8_t frames_decoded[4];
	uint8_t bytes_consumed[4];
	uint8_t bytes_produced[4];
	uint8_t flags[4];
};

struct SDKSurfaceFillPayload {
	uint8_t surface[4];
	uint8_t x[4];
	uint8_t y[4];
	uint8_t width[4];
	uint8_t height[4];
	uint8_t color[4];
	uint8_t flags[4];
	uint8_t reserved[20];
};

struct SDKSurfaceCopyPayload {
	uint8_t src_surface[4];
	uint8_t dst_surface[4];
	uint8_t src_x[4];
	uint8_t src_y[4];
	uint8_t dst_x[4];
	uint8_t dst_y[4];
	uint8_t width[4];
	uint8_t height[4];
	uint8_t flags[4];
	uint8_t reserved[12];
};

struct SDKCryptoHashPayload {
	uint8_t src_handle[4];
	uint8_t src_offset[4];
	uint8_t src_length[4];
	uint8_t dst_handle[4];
	uint8_t dst_offset[4];
	uint8_t key_handle[4];
	uint8_t key_offset[4];
	uint8_t key_length[4];
	uint8_t algorithm[4];
	uint8_t flags[4];
	uint8_t reserved[8];
};

struct SDKCryptoStreamPayload {
	uint8_t src_handle[4];
	uint8_t src_offset[4];
	uint8_t src_length[4];
	uint8_t dst_handle[4];
	uint8_t dst_offset[4];
	uint8_t key_handle[4];
	uint8_t key_offset[4];
	uint8_t nonce_handle[4];
	uint8_t nonce_offset[4];
	uint8_t counter[4];
	uint8_t algorithm[4];
	uint8_t flags[4];
};

struct SDKCryptoAeadPayload {
	uint8_t src_handle[4];
	uint8_t src_offset[4];
	uint8_t src_length[4];
	uint8_t dst_handle[4];
	uint8_t dst_offset[4];
	uint8_t aad_handle[4];
	uint8_t aad_offset[4];
	uint8_t aad_length[4];
	uint8_t key_handle[4];
	uint8_t key_offset[4];
	uint8_t nonce_handle[4];
	uint8_t flags[4];
};

struct SDKCryptoResultPayload {
	uint8_t bytes_written[4];
	uint8_t algorithm[4];
	uint8_t flags[4];
	uint8_t reserved[36];
};

struct SDKCryptoKXPayload {
	uint8_t scalar_handle[4];
	uint8_t scalar_offset[4];
	uint8_t point_handle[4];
	uint8_t point_offset[4];
	uint8_t dst_handle[4];
	uint8_t dst_offset[4];
	uint8_t algorithm[4];
	uint8_t flags[4];
	uint8_t reserved[16];
};

struct SDKDecompressPayload {
	uint8_t src_handle[4];
	uint8_t src_offset[4];
	uint8_t src_length[4];
	uint8_t dst_handle[4];
	uint8_t dst_offset[4];
	uint8_t dst_capacity[4];
	uint8_t algorithm[4];
	uint8_t flags[4];
	uint8_t reserved[16];
};

struct SDKDecompressBatchPayload {
	uint8_t arena_handle[4];
	uint8_t arena_offset[4];
	uint8_t arena_length[4];
	uint8_t reserved[36];
};

struct SDKDecompressBatchResultPayload {
	uint8_t members_total[4];
	uint8_t members_ok[4];
	uint8_t members_failed[4];
	uint8_t flags[4];
	uint8_t reserved[32];
};

struct SDKDecompressTestPayload {
	uint8_t src_handle[4];
	uint8_t src_offset[4];
	uint8_t src_length[4];
	uint8_t output_limit[4];
	uint8_t algorithm[4];
	uint8_t flags[4];
	uint8_t reserved[24];
};

struct SDKDecompressStreamBeginPayload {
	uint8_t src_handle[4];
	uint8_t src_offset[4];
	uint8_t src_length[4];
	uint8_t output_limit[4];
	uint8_t algorithm[4];
	uint8_t flags[4];
	uint8_t reserved[24];
};

struct SDKDecompressStreamReadPayload {
	uint8_t session[4];
	uint8_t dst_handle[4];
	uint8_t dst_offset[4];
	uint8_t dst_capacity[4];
	uint8_t flags[4];
	uint8_t reserved[28];
};

struct SDKDecompressStreamFeedPayload {
	uint8_t session[4];
	uint8_t src_handle[4];
	uint8_t src_offset[4];
	uint8_t src_length[4];
	uint8_t flags[4];
	uint8_t reserved[28];
};

struct SDKDecompressStreamClosePayload {
	uint8_t session[4];
	uint8_t flags[4];
	uint8_t reserved[40];
};

struct SDKDecompressResultPayload {
	uint8_t bytes_consumed[4];
	uint8_t bytes_written[4];
	uint8_t checksum[4];
	uint8_t algorithm[4];
	uint8_t flags[4];
	uint8_t reserved[28];
};

struct SDKDecompressStreamResultPayload {
	uint8_t session[4];
	uint8_t bytes_consumed[4];
	uint8_t bytes_written[4];
	uint8_t checksum[4];
	uint8_t algorithm[4];
	uint8_t flags[4];
	uint8_t reserved[24];
};

struct SDKSharedBuffer {
	uint32_t handle;
	uint32_t address;
	uint32_t length;
	uint32_t flags;
	uint32_t pin_count;
	uint8_t in_use;
};

struct SDKSurface {
	uint32_t handle;
	uint32_t address;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t format;
	uint32_t flags;
	uint32_t length;
	uint8_t in_use;
};

/*
 * Native-endian packed parameters for a deferred SCALE_IMAGE(_CLIPPED)
 * task (op_params are core-local, like decompress_op_params). Both
 * surfaces are resolved to raw geometry on core 0 at enqueue time --
 * core 1 must never read the surface registry or video state.
 * Framebuffer-backed surfaces are never deferred (their geometry tracks
 * live video state that a pending mode switch would invalidate), and
 * every field is checked to fit u16 before packing
 * (scale_defer_eligible). Exactly fills TASKQ_OP_PARAM_BYTES.
 */
struct scale_op_params {
	uint32_t src_addr;
	uint32_t dst_addr;
	uint16_t src_w, src_h, dst_w, dst_h;
	uint16_t src_pitch, dst_pitch;
	uint16_t format;
	uint16_t filter_flags;   /* filter in the low byte; flags above */
	uint16_t src_x, src_y, src_rw, src_rh;
	uint16_t dst_x, dst_y, dst_rw, dst_rh;
	uint16_t clip_x, clip_y, clip_rw, clip_rh;
};

typedef char scale_op_params_size_check[
    (sizeof(struct scale_op_params) <= TASKQ_OP_PARAM_BYTES) ? 1 : -1];

/*
 * Native-endian packed parameters for a deferred DECODE_JPEG task. The
 * source shared buffer and destination surface are resolved and
 * range-checked on core 0 at enqueue time; lifetime is protected by the
 * same batch-tail gate + blocking-QUEUED-client convention that covers
 * SDK_OP_DECOMPRESS's shared buffers. Framebuffer-backed destinations
 * are never deferred.
 */
struct jpeg_op_params {
	uint32_t src_addr;      /* src buffer address + offset, resolved */
	uint32_t src_len;
	uint32_t dst_addr;
	uint32_t output_format;
	uint16_t dst_pitch;
	uint16_t dst_x, dst_y;
	uint16_t dst_width, dst_height;   /* decode-bounds output rect */
	uint16_t dst_surf_w, dst_surf_h;  /* surface dims (flush bounds) */
	uint16_t flags;                   /* reserved, 0 */
};

typedef char jpeg_op_params_size_check[
    (sizeof(struct jpeg_op_params) <= TASKQ_OP_PARAM_BYTES) ? 1 : -1];

/*
 * Native-endian packed parameters for a deferred DECODE_MP3 task. Source
 * and destination are shared buffers, resolved and range-checked on
 * core 0; lifetime protection is the DECOMPRESS convention (batch-tail
 * gate + blocking-QUEUED client). The op is one-shot (open_buf with
 * MP3D_DO_NOT_SCAN allocates nothing), so the decoder context does not
 * cross request boundaries.
 */
struct mp3_op_params {
	uint32_t src_addr;         /* src buffer address + offset, resolved */
	uint32_t src_len;
	uint32_t dst_addr;         /* dst buffer address + offset, resolved */
	uint32_t dst_cap;          /* even, >= 2 */
	uint32_t output_format;
	uint32_t output_hz;        /* 0 = accept stream rate */
	uint32_t output_channels;  /* 0 = accept stream channels */
};

typedef char mp3_op_params_size_check[
    (sizeof(struct mp3_op_params) <= TASKQ_OP_PARAM_BYTES) ? 1 : -1];

/*
 * Native-endian packed parameters for deferred AUDIO_STREAM_FEED/READ
 * tasks. The stream table lives in the SCU-coherent region (core 1 may
 * look sessions up directly); the feed source is resolved on core 0.
 */
struct audio_feed_op_params {
	uint32_t session;
	uint32_t src_addr;      /* resolved src + offset; 0 for EOF-only */
	uint32_t src_len;
	uint32_t flags;
};

struct audio_read_op_params {
	uint32_t session;
	uint32_t pcm_read;      /* validated against consumer-visible used */
};

typedef char audio_op_params_size_check[
    (sizeof(struct audio_feed_op_params) <= TASKQ_OP_PARAM_BYTES &&
     sizeof(struct audio_read_op_params) <= TASKQ_OP_PARAM_BYTES) ? 1 : -1];

/*
 * Native-endian packed parameters for deferred IMAGE_SESSION_FEED/CLOSE
 * tasks. The session lives in the SCU-coherent table (visible to both
 * cores); the source shared buffer is resolved on core 0. A core-1-affine
 * session's feed/close may ONLY run on core 1 -- its libjpeg/libpng
 * objects live in core 1's cache -- which TASK_LONG classification
 * guarantees (never drained by core 0).
 */
struct imgsess_op_params {
	uint32_t session;
	uint32_t src_handle;
	uint32_t src_addr;      /* resolved src + offset; 0 for EOF-only/close */
	uint32_t src_len;
	uint32_t flags;
};

typedef char imgsess_op_params_size_check[
    (sizeof(struct imgsess_op_params) <= TASKQ_OP_PARAM_BYTES) ? 1 : -1];

/* Video decoder state is always core-1-owned. WRITE carries a source range
 * resolved on core 0; DECODE carries the P96 bitmap binding for this frame. */
struct video_write_op_params {
	uint32_t session;
	uint32_t src_addr;
	uint32_t src_len;
	uint32_t flags;
};

struct video_session_op_params {
	uint32_t session;
};

struct media_audio_op_params {
	uint32_t session;
	uint32_t acknowledged_hi;
	uint32_t acknowledged_lo;
	uint32_t flags;
};

typedef char video_op_params_size_check[
    (sizeof(struct video_write_op_params) <= TASKQ_OP_PARAM_BYTES &&
     sizeof(struct video_session_op_params) <= TASKQ_OP_PARAM_BYTES &&
     sizeof(struct media_audio_op_params) <= TASKQ_OP_PARAM_BYTES) ? 1 : -1];

struct SDKAudioStream {
	uint32_t id;
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
	/* Ring addresses resolved from the shared-buffer registry at BEGIN:
	 * the registry is core-0-only, so core-1 feeds/reads must never
	 * dereference mp3_ring/pcm_ring -- they use these instead. */
	uint32_t mp3_ring_addr;
	uint32_t pcm_ring_addr;
	/* Fixed at BEGIN: nonzero = feed/read run on the core-1 worker (the
	 * mp3 staging ring is then cache-owned by core 1). */
	uint32_t core1_affine;
	/* Set when a core-1 fault may have left the embedded decoder state
	 * mid-frame; feeds/reads answer IO_ERROR, close still works. */
	uint32_t faulted;
	/* Bound-playback tail guard: the AX pump advances pcm_consumed_total
	 * when it STAGES PCM into the TX ring, which runs up to
	 * AUDIO_PUMP_TARGET_AHEAD ahead of the DMA. Set while staged audio is
	 * still queued for the DMA so end-of-stream (DONE / PCM_READY-clear)
	 * waits for it to actually play out. Managed by the pump; always 0 for
	 * unbound streams (the READ consumer hears bytes immediately). */
	uint8_t pump_tail_pending;
	mp3dec_t decoder;
	mp3d_sample_t scratch[MINIMP3_MAX_SAMPLES_PER_FRAME];
};

typedef char SDKMailboxDescriptor_must_be_128_bytes[
	(sizeof(struct SDKMailboxDescriptor) == SDK_MAILBOX_DESCRIPTOR_SIZE) ? 1 : -1
];
typedef char SDKMailboxEntry_must_be_64_bytes[
	(sizeof(struct SDKMailboxEntry) == SDK_MAILBOX_ENTRY_SIZE) ? 1 : -1
];
typedef char SDKCapsPayload_must_fit_inline[
	(sizeof(struct SDKCapsPayload) <= 48U) ? 1 : -1
];
typedef char SDKQueryServicePayload_must_be_48_bytes[
	(sizeof(struct SDKQueryServicePayload) == 48U) ? 1 : -1
];
typedef char SDKQueryApertureLayoutPayload_must_be_48_bytes[
	(sizeof(struct SDKQueryApertureLayoutPayload) == 48U) ? 1 : -1
];
typedef char SDKServiceInfoPayload_must_be_48_bytes[
	(sizeof(struct SDKServiceInfoPayload) == 48U) ? 1 : -1
];
typedef char SDKAllocSharedPayload_must_be_48_bytes[
	(sizeof(struct SDKAllocSharedPayload) == 48U) ? 1 : -1
];
typedef char SDKSharedBufferInfoPayload_must_be_48_bytes[
	(sizeof(struct SDKSharedBufferInfoPayload) == 48U) ? 1 : -1
];
typedef char SDKFreeSharedPayload_must_be_48_bytes[
	(sizeof(struct SDKFreeSharedPayload) == 48U) ? 1 : -1
];
typedef char SDKMemFillPayload_must_be_48_bytes[
	(sizeof(struct SDKMemFillPayload) == 48U) ? 1 : -1
];
typedef char SDKMemCopyPayload_must_be_48_bytes[
	(sizeof(struct SDKMemCopyPayload) == 48U) ? 1 : -1
];
typedef char SDKDiagPayload_must_be_48_bytes[
	(sizeof(struct SDKDiagPayload) == 48U) ? 1 : -1
];
typedef char SDKDiagTimingPayload_must_be_48_bytes[
	(sizeof(struct SDKDiagTimingPayload) == 48U) ? 1 : -1
];
typedef char SDKDiagSchedPayload_must_be_24_bytes[
	(sizeof(struct SDKDiagSchedPayload) == 24U) ? 1 : -1
];
typedef char SDKDiagMemoryPayload_must_be_48_bytes[
	(sizeof(struct SDKDiagMemoryPayload) == 48U) ? 1 : -1
];
typedef char SDKSurfaceInfoPayload_must_be_48_bytes[
	(sizeof(struct SDKSurfaceInfoPayload) == 48U) ? 1 : -1
];
typedef char SDKAllocSurfacePayload_must_be_48_bytes[
	(sizeof(struct SDKAllocSurfacePayload) == 48U) ? 1 : -1
];
typedef char SDKFreeSurfacePayload_must_be_48_bytes[
	(sizeof(struct SDKFreeSurfacePayload) == 48U) ? 1 : -1
];
typedef char SDKScaleImagePayload_must_be_48_bytes[
	(sizeof(struct SDKScaleImagePayload) == 48U) ? 1 : -1
];
typedef char SDKScaleImageClippedPayload_must_be_48_bytes[
	(sizeof(struct SDKScaleImageClippedPayload) == 48U) ? 1 : -1
];
typedef char SDKImageDecodePayload_must_be_48_bytes[
	(sizeof(struct SDKImageDecodePayload) == 48U) ? 1 : -1
];
typedef char SDKImageDecodeResultPayload_must_be_48_bytes[
	(sizeof(struct SDKImageDecodeResultPayload) == 48U) ? 1 : -1
];
typedef char SDKImageSessionBeginPayload_must_be_48_bytes[
	(sizeof(struct SDKImageSessionBeginPayload) == 48U) ? 1 : -1
];
typedef char SDKImageSessionFeedPayload_must_be_48_bytes[
	(sizeof(struct SDKImageSessionFeedPayload) == 48U) ? 1 : -1
];
typedef char SDKImageSessionResultPayload_must_be_48_bytes[
	(sizeof(struct SDKImageSessionResultPayload) == 48U) ? 1 : -1
];
typedef char SDKImageSessionClosePayload_must_be_48_bytes[
	(sizeof(struct SDKImageSessionClosePayload) == 48U) ? 1 : -1
];
typedef char SDKAudioDecodePayload_must_be_48_bytes[
	(sizeof(struct SDKAudioDecodePayload) == 48U) ? 1 : -1
];
typedef char SDKAudioDecodeResultPayload_must_be_48_bytes[
	(sizeof(struct SDKAudioDecodeResultPayload) == 48U) ? 1 : -1
];
typedef char SDKAudioStreamBeginPayload_must_be_48_bytes[
	(sizeof(struct SDKAudioStreamBeginPayload) == 48U) ? 1 : -1
];
typedef char SDKAudioStreamFeedPayload_must_be_48_bytes[
	(sizeof(struct SDKAudioStreamFeedPayload) == 48U) ? 1 : -1
];
typedef char SDKAudioStreamReadPayload_must_be_48_bytes[
	(sizeof(struct SDKAudioStreamReadPayload) == 48U) ? 1 : -1
];
typedef char SDKAudioStreamClosePayload_must_be_48_bytes[
	(sizeof(struct SDKAudioStreamClosePayload) == 48U) ? 1 : -1
];
typedef char SDKAudioStreamResultPayload_must_be_48_bytes[
	(sizeof(struct SDKAudioStreamResultPayload) == 48U) ? 1 : -1
];
typedef char SDKVideoSessionBeginPayload_must_be_48_bytes[
	(sizeof(struct SDKVideoSessionBeginPayload) == 48U) ? 1 : -1
];
typedef char SDKVideoSessionWritePayload_must_be_48_bytes[
	(sizeof(struct SDKVideoSessionWritePayload) == 48U) ? 1 : -1
];
typedef char SDKVideoSessionDecodePayload_must_be_48_bytes[
	(sizeof(struct SDKVideoSessionDecodePayload) == 48U) ? 1 : -1
];
typedef char SDKVideoSessionClosePayload_must_be_48_bytes[
	(sizeof(struct SDKVideoSessionClosePayload) == 48U) ? 1 : -1
];
typedef char SDKVideoSessionResultPayload_must_be_48_bytes[
	(sizeof(struct SDKVideoSessionResultPayload) == 48U) ? 1 : -1
];
typedef char SDKSurfaceFillPayload_must_be_48_bytes[
	(sizeof(struct SDKSurfaceFillPayload) == 48U) ? 1 : -1
];
typedef char SDKSurfaceCopyPayload_must_be_48_bytes[
	(sizeof(struct SDKSurfaceCopyPayload) == 48U) ? 1 : -1
];
typedef char SDKCryptoHashPayload_must_be_48_bytes[
	(sizeof(struct SDKCryptoHashPayload) == 48U) ? 1 : -1
];
typedef char SDKCryptoStreamPayload_must_be_48_bytes[
	(sizeof(struct SDKCryptoStreamPayload) == 48U) ? 1 : -1
];
typedef char SDKCryptoAeadPayload_must_be_48_bytes[
	(sizeof(struct SDKCryptoAeadPayload) == 48U) ? 1 : -1
];
typedef char SDKCryptoResultPayload_must_be_48_bytes[
	(sizeof(struct SDKCryptoResultPayload) == 48U) ? 1 : -1
];
typedef char SDKDecompressPayload_must_be_48_bytes[
	(sizeof(struct SDKDecompressPayload) == 48U) ? 1 : -1
];
typedef char SDKDecompressBatchPayload_must_be_48_bytes[
	(sizeof(struct SDKDecompressBatchPayload) == 48U) ? 1 : -1
];
typedef char SDKDecompressBatchResultPayload_must_be_48_bytes[
	(sizeof(struct SDKDecompressBatchResultPayload) == 48U) ? 1 : -1
];
typedef char SDKDecompressTestPayload_must_be_48_bytes[
	(sizeof(struct SDKDecompressTestPayload) == 48U) ? 1 : -1
];
typedef char SDKDecompressResultPayload_must_be_48_bytes[
	(sizeof(struct SDKDecompressResultPayload) == 48U) ? 1 : -1
];
typedef char SDKDecompressStreamBeginPayload_must_be_48_bytes[
	(sizeof(struct SDKDecompressStreamBeginPayload) == 48U) ? 1 : -1
];
typedef char SDKDecompressStreamReadPayload_must_be_48_bytes[
	(sizeof(struct SDKDecompressStreamReadPayload) == 48U) ? 1 : -1
];
typedef char SDKDecompressStreamFeedPayload_must_be_48_bytes[
	(sizeof(struct SDKDecompressStreamFeedPayload) == 48U) ? 1 : -1
];
typedef char SDKDecompressStreamClosePayload_must_be_48_bytes[
	(sizeof(struct SDKDecompressStreamClosePayload) == 48U) ? 1 : -1
];
typedef char SDKDecompressStreamResultPayload_must_be_48_bytes[
	(sizeof(struct SDKDecompressStreamResultPayload) == 48U) ? 1 : -1
];

static volatile int sdk_mailbox_pending;
static volatile int sdk_mailbox_active;
static volatile int sdk_completion_irq_enabled;

/*
 * Mailbox generation, bumped on every sdk_mailbox_init (Amiga reset / fw-update).
 * Each offloaded task's slot is stamped with the generation live when it was
 * enqueued; scheduler_core0_poll drops a harvested task whose stamp no longer
 * matches, so a task that outlives the mailbox it was submitted under can never
 * post its stale request_id/user_cookie into the next mailbox's completion ring.
 * These are read/written only by core 0 (crypto_dispatch stamps, core0_poll
 * checks, sdk_mailbox_init bumps), so they need no cross-core barrier. NOTE: any
 * future non-crypto queue producer must stamp g_task_generation[slot] too, or
 * its completions will be dropped as stale.
 */
static uint32_t sdk_mailbox_generation;
static uint32_t g_task_generation[TASKQ_CAPACITY];

/* Enqueue an internal core-1 task (request_id 0, no client completion).
 * Core-0 main-loop context ONLY: the task queue is single-producer.
 * The caller owns its own in-flight marker; retirement is dispatched by
 * opcode in sdk_mailbox_post_deferred. */
int sdk_mailbox_enqueue_internal(uint32_t opcode, const void *params,
                                 uint32_t params_len)
{
	taskq_shared_t *sh = scheduler_shared();
	int slot;

	if (!scheduler_core1_available())
		return 0;

	slot = taskq_enqueue(&sh->queue, opcode, TASK_LONG,
	                     0u, 0u, 0u, 0u, 0u, 0u, params, params_len);
	if (slot < 0)
		return 0;

	g_task_generation[slot] = sdk_mailbox_generation;
	__asm__ __volatile__("dsb ish\n\tsev" ::: "memory");
	return 1;
}

/*
 * Batch-tail gate for crypto offload. The request loop processes a FIFO batch
 * (the requests present when it started); the synchronous dispatcher completed
 * each request -- including its shared-buffer writes -- before the next ran, so
 * a batched dependent op (e.g. CRYPTO_HASH -> H followed by MEM_COPY from H or
 * FREE_SHARED H) always observed finished side effects. Deferring a crypto op
 * to core 1 breaks that: core 0 would run the next request while core 1 is
 * still writing H. So crypto_dispatch only defers when this flag says the
 * current request is the LAST one in the batch (nothing behind it can depend on
 * it); any crypto op with requests queued after it runs inline, preserving
 * in-order side effects. Cross-batch ordering remains the client's job via the
 * completion-is-a-barrier contract. Core-0-only, set immediately before each
 * handle_request(); no cross-core barrier needed.
 */
static int g_request_is_batch_tail;
static uint16_t sdk_status;
static struct SDKSharedBuffer shared_buffers[SDK_MAX_SHARED_BUFFERS];
static struct SDKSurface surfaces[SDK_MAX_SURFACES];
static struct SDKSharedBuffer *media_pcm_ring;
static uint32_t media_pcm_ring_session;

/*
 * The audio-stream table lives in the SCU-coherent scheduler region
 * (SDK_AUDIO_STREAMS_ADDRESS, memorymap.h): core-1-affine streams decode
 * on the worker while core 0 begins/closes them, and the coherent section
 * makes the embedded decoder/cursor state visible to both cores without
 * manual cache maintenance. No heap objects hang off a stream.
 */
static struct SDKAudioStream *const audio_streams =
    (struct SDKAudioStream *)SDK_AUDIO_STREAMS_ADDRESS;

typedef char audio_streams_fit_check[
    (SDK_MAX_AUDIO_STREAMS * sizeof(struct SDKAudioStream) <=
     SDK_AUDIO_STREAMS_MAX_BYTES) ? 1 : -1];
static uint32_t next_shared_handle;
static uint32_t next_surface_handle;
static uint32_t next_audio_stream_id;
static uint32_t requests_completed;
static uint32_t requests_failed;
static uint32_t timing_requests_timed;
static uint32_t timing_total_us;
static uint32_t timing_surface_requests;
static uint32_t timing_surface_us;
static uint32_t timing_audio_requests;
static uint32_t timing_audio_us;
static uint32_t timing_last_opcode;
static uint32_t timing_last_us;
static uint32_t timing_max_opcode;
static uint32_t timing_max_us;
/*
 * Decode-only timing (diagnostic, not functional). Brackets just the
 * sdk_decompress_buffer() compute inside sdk_mailbox_run_offload_task, which
 * is shared by the core-1 worker and the core-0 inline fallback. Unlike
 * timing_total_us (whole-request time -- for a core-1-deferred decompress
 * that is only the enqueue latency, not the decode itself), this isolates
 * actual decode compute time so archive transfer-vs-decode can be split on
 * hardware. Counts every attempt, success or failure, like
 * record_request_timing does for the whole-request counters above.
 */
static uint32_t timing_decode_requests;
static uint32_t timing_decode_us;

static uint32_t surface_format_bytes(uint32_t format);

static int aperture_contract_present(void)
{
	return (sdk_aperture_runtime_flags() & SDK_APERTURE_FLAG_VALID) != 0U;
}

static uint32_t effective_host_window_address(void)
{
	if (aperture_contract_present())
		return sdk_aperture_host_window_address();
	/* Compatibility for Z3 and old 4 MB Z2 bitstreams which expose no
	 * generation-tagged aperture register. The existing host library rejects
	 * this legacy address on sub-4 MB boards. New 2 MB bitstreams expose the
	 * exact size and therefore take the acknowledged dynamic path instead. */
	if (sdk_aperture_runtime_is_zorro3() ||
	    sdk_aperture_runtime_is_legacy())
		return SDK_HOST_WINDOW_HEAP_ADDRESS;
	/* A nonzero but unsupported FPGA-reported size is not a legacy image.
	 * Keep the host heap disabled instead of guessing at a 4 MB address. */
	return 0U;
}

static uint32_t effective_host_window_size(void)
{
	if (aperture_contract_present())
		return sdk_aperture_host_window_size();
	if (sdk_aperture_runtime_is_zorro3() ||
	    sdk_aperture_runtime_is_legacy())
		return SDK_HOST_WINDOW_HEAP_SIZE;
	return 0U;
}

static uint32_t mailbox_capability_bits(void)
{
	uint32_t capabilities = SDK_MAILBOX_BASE_CAPABILITY_BITS;

	if (aperture_contract_present())
		capabilities |= SDK_CAP_APERTURE_LAYOUT;
	if (effective_host_window_size() != 0U)
		capabilities |= SDK_CAP_HOST_WINDOW_HEAP;
	return capabilities;
}

static const struct SDKServiceDescriptor sdk_services[] = {
	{
		SDK_SERVICE_CORE,
		0x00020000U,
		SDK_CAP_MAILBOX | SDK_CAP_POLLING_COMPLETION |
			SDK_CAP_SERVICE_DISCOVERY,
		SDK_SERVICE_FLAG_FIRMWARE,
		SDK_SERVICE_CORE,
		6,
		"core"
	},
	{
		SDK_SERVICE_MEMORY,
		0x00020000U,
		SDK_CAP_SHARED_ALLOC | SDK_CAP_MEMORY_OPS,
		SDK_SERVICE_FLAG_FIRMWARE,
		SDK_SERVICE_MEMORY,
		4,
		"memory"
	},
	{
		SDK_SERVICE_SURFACE,
		0x00020000U,
		SDK_CAP_SURFACES | SDK_CAP_FRAMEBUFFER_SURFACE |
			SDK_CAP_SURFACE_OPS,
		SDK_SERVICE_FLAG_FIRMWARE | SDK_SERVICE_FLAG_ZERO_COPY |
			SDK_SERVICE_FLAG_SURFACE_PALETTE_QUERY,
		SDK_SERVICE_SURFACE,
		6,
		"surface"
	},
	{
		SDK_SERVICE_IMAGE,
		0x00020000U,
		SDK_CAP_IMAGE_SCALE | SDK_CAP_IMAGE_DECODE,
		SDK_SERVICE_FLAG_FIRMWARE |
			SDK_SERVICE_FLAG_IMAGE_STREAMING_INPUT |
			SDK_SERVICE_FLAG_IMAGE_TILE_OUTPUT |
			SDK_SERVICE_FLAG_IMAGE_FRAMEBUFFER_OUTPUT |
			SDK_SERVICE_FLAG_IMAGE_SCALE_BILINEAR |
			SDK_SERVICE_FLAG_IMAGE_SCALE_CLIPPED |
			SDK_SERVICE_FLAG_IMAGE_PNG_DIRECT_BGRA |
			SDK_SERVICE_FLAG_IMAGE_RGB888_OUTPUT |
			SDK_SERVICE_FLAG_IMAGE_SCALE_BGRA_TO_RGB555_RGB565,
		SDK_SERVICE_IMAGE,
		8,
		"image"
	},
	{
		SDK_SERVICE_CODEC,
		0x00020000U,
		SDK_CAP_COMPRESSION,
		SDK_SERVICE_FLAG_FIRMWARE |
			SDK_SERVICE_FLAG_CODEC_DEFLATE_RAW |
			SDK_SERVICE_FLAG_CODEC_ZLIB |
			SDK_SERVICE_FLAG_CODEC_GZIP |
			SDK_SERVICE_FLAG_CODEC_LZMA_ALONE |
			SDK_SERVICE_FLAG_CODEC_LZMA2 |
			SDK_SERVICE_FLAG_CODEC_CHECKSUM |
			SDK_SERVICE_FLAG_CODEC_DECOMPRESS_TEST |
			SDK_SERVICE_FLAG_CODEC_DECOMPRESS_STREAM |
			SDK_SERVICE_FLAG_CODEC_DECOMPRESS_FEED |
			SDK_SERVICE_FLAG_CODEC_DEFLATE_FEED |
			SDK_SERVICE_FLAG_CODEC_ZLIB_FEED |
			SDK_SERVICE_FLAG_CODEC_GZIP_FEED |
			SDK_SERVICE_FLAG_CODEC_LZH |
			SDK_SERVICE_FLAG_CODEC_DECOMPRESS_BATCH,
		SDK_SERVICE_CODEC,
		7,	/* 0x0600..0x0606 incl. SDK_OP_DECOMPRESS_BATCH */
		"codec"
	},
	{
		SDK_SERVICE_AUDIO,
		0x00020001U,
		SDK_CAP_AUDIO_DECODE | SDK_CAP_AUDIO_PLAYBACK,
		SDK_SERVICE_FLAG_FIRMWARE |
			SDK_SERVICE_FLAG_AUDIO_MP3_DECODE |
			SDK_SERVICE_FLAG_AUDIO_MP3_STREAM,
		SDK_SERVICE_AUDIO,
		9,	/* 0x0500..0x0508 incl. AUDIO_STREAM_PLAY/STOP */
		"audio"
	},
	{
		SDK_SERVICE_CRYPTO,
		0x00020000U,
		SDK_CAP_CRYPTO,
		SDK_SERVICE_FLAG_FIRMWARE | SDK_SERVICE_FLAG_CRYPTO_X25519 |
			SDK_SERVICE_FLAG_CRYPTO_P256 |
			SDK_SERVICE_FLAG_CRYPTO_P256_KEYGEN |
			SDK_SERVICE_FLAG_CRYPTO_ECDSA_P256 |
			SDK_SERVICE_FLAG_CRYPTO_RSA_2048 |
			SDK_SERVICE_FLAG_CRYPTO_AES_GCM,
		SDK_SERVICE_CRYPTO,
		5,
		"crypto"
	},
	{
		SDK_SERVICE_DIAG,
		0x00020000U,
		SDK_CAP_DIAGNOSTICS,
		SDK_SERVICE_FLAG_FIRMWARE,
		SDK_SERVICE_DIAG,
		4,
		"diag"
	},
	{
		SDK_SERVICE_VIDEO,
		0x00020000U,
		SDK_CAP_VIDEO_DECODE | SDK_CAP_MEDIA_SESSION,
		SDK_SERVICE_FLAG_FIRMWARE |
			SDK_SERVICE_FLAG_ASYNC |
			SDK_SERVICE_FLAG_VIDEO_MPEG1 |
			SDK_SERVICE_FLAG_VIDEO_MPEG_PS |
			SDK_SERVICE_FLAG_VIDEO_DIRECT_OVERLAY |
			SDK_SERVICE_FLAG_VIDEO_STREAMING_INPUT |
			SDK_SERVICE_FLAG_VIDEO_CORE1 |
			SDK_SERVICE_FLAG_VIDEO_MEDIA_SESSION |
			SDK_SERVICE_FLAG_VIDEO_MEDIA_MP2 |
			SDK_SERVICE_FLAG_VIDEO_EXPLICIT_PRESENT |
			SDK_SERVICE_FLAG_VIDEO_TIMELINE_90KHZ |
			SDK_SERVICE_FLAG_VIDEO_PCM_RING_STATUS,
		SDK_SERVICE_VIDEO,
		14,
		"video"
	}
};

static inline uint16_t get_be16(const volatile void *p)
{
	const volatile uint8_t *b = (const volatile uint8_t *)p;
	return ((uint16_t)b[0] << 8) | b[1];
}

static inline uint32_t get_be32(const volatile void *p)
{
	const volatile uint8_t *b = (const volatile uint8_t *)p;
	return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
	       ((uint32_t)b[2] << 8) | b[3];
}

static int bytes_are_zero(const volatile uint8_t *bytes, uint32_t length)
{
	uint32_t i;

	for (i = 0U; i < length; i++) {
		if (bytes[i] != 0U)
			return 0;
	}
	return 1;
}

static inline void put_be16(volatile void *p, uint16_t value)
{
	volatile uint8_t *b = (volatile uint8_t *)p;
	b[0] = (value >> 8) & 0xff;
	b[1] = value & 0xff;
}

static inline void put_be32(volatile void *p, uint32_t value)
{
	volatile uint8_t *b = (volatile uint8_t *)p;
	b[0] = (value >> 24) & 0xff;
	b[1] = (value >> 16) & 0xff;
	b[2] = (value >> 8) & 0xff;
	b[3] = value & 0xff;
}

static uint32_t saturated_add_u32(uint32_t value, uint32_t increment)
{
	if (0xffffffffU - value < increment)
		return 0xffffffffU;
	return value + increment;
}

static uint32_t timing_delta_us(XTime start, XTime end)
{
	uint64_t delta = (uint64_t)(end - start);
	uint64_t us;

	if (COUNTS_PER_SECOND == 0U)
		return 0U;
	us = (delta * 1000000ULL) / (uint64_t)COUNTS_PER_SECOND;
	if (us > 0xffffffffULL)
		return 0xffffffffU;
	return (uint32_t)us;
}

static void record_request_timing(uint32_t opcode, uint32_t elapsed_us)
{
	uint32_t service = opcode & 0xff00U;

	timing_requests_timed = saturated_add_u32(timing_requests_timed, 1U);
	timing_total_us = saturated_add_u32(timing_total_us, elapsed_us);
	timing_last_opcode = opcode;
	timing_last_us = elapsed_us;
	if (elapsed_us >= timing_max_us) {
		timing_max_opcode = opcode;
		timing_max_us = elapsed_us;
	}
	if (service == SDK_SERVICE_SURFACE) {
		timing_surface_requests =
			saturated_add_u32(timing_surface_requests, 1U);
		timing_surface_us =
			saturated_add_u32(timing_surface_us, elapsed_us);
	} else if (service == SDK_SERVICE_AUDIO) {
		timing_audio_requests =
			saturated_add_u32(timing_audio_requests, 1U);
		timing_audio_us =
			saturated_add_u32(timing_audio_us, elapsed_us);
	}
}

static inline volatile struct SDKMailboxDescriptor *descriptor(void)
{
	return (volatile struct SDKMailboxDescriptor *)SDK_MAILBOX_ADDRESS;
}

void sdk_mailbox_refresh_capabilities(void)
{
	volatile struct SDKMailboxDescriptor *desc = descriptor();

	put_be32(desc->capability_bits, mailbox_capability_bits());
	Xil_DCacheFlushRange((INTPTR)desc, sizeof(*desc));
	__asm__ __volatile__("dsb" ::: "memory");
}

static inline volatile struct SDKMailboxEntry *request_ring(void)
{
	return (volatile struct SDKMailboxEntry *)
		(SDK_MAILBOX_ADDRESS + SDK_MAILBOX_REQUEST_OFFSET);
}

static inline volatile struct SDKMailboxEntry *completion_ring(void)
{
	return (volatile struct SDKMailboxEntry *)
		(SDK_MAILBOX_ADDRESS + SDK_MAILBOX_COMPLETION_OFFSET);
}

static uint32_t next_index(uint32_t index)
{
	index++;
	if (index >= SDK_MAILBOX_RING_ENTRIES)
		index = 0;
	return index;
}

static int descriptor_valid(volatile struct SDKMailboxDescriptor *desc)
{
	uint32_t request_head;
	uint32_t request_tail;
	uint32_t completion_head;
	uint32_t completion_tail;

	if (get_be32(desc->magic) != SDK_MAILBOX_MAGIC)
		return 0;
	if (get_be16(desc->abi_major) != SDK_MAILBOX_ABI_MAJOR)
		return 0;
	if (get_be32(desc->descriptor_size) != sizeof(*desc))
		return 0;
	if (get_be32(desc->request_ring_offset) != SDK_MAILBOX_REQUEST_OFFSET)
		return 0;
	if (get_be32(desc->completion_ring_offset) != SDK_MAILBOX_COMPLETION_OFFSET)
		return 0;
	if (get_be32(desc->request_ring_entries) != SDK_MAILBOX_RING_ENTRIES)
		return 0;
	if (get_be32(desc->completion_ring_entries) != SDK_MAILBOX_RING_ENTRIES)
		return 0;
	if ((get_be32(desc->capability_bits) & SDK_CAP_MAILBOX) == 0)
		return 0;

	request_head = get_be32(desc->request_head);
	request_tail = get_be32(desc->request_tail);
	completion_head = get_be32(desc->completion_head);
	completion_tail = get_be32(desc->completion_tail);
	if (request_head >= SDK_MAILBOX_RING_ENTRIES ||
	    request_tail >= SDK_MAILBOX_RING_ENTRIES ||
	    completion_head >= SDK_MAILBOX_RING_ENTRIES ||
	    completion_tail >= SDK_MAILBOX_RING_ENTRIES) {
		return 0;
	}

	return 1;
}

static void copy_payload(volatile uint8_t *dst, const volatile uint8_t *src,
                         uint32_t length)
{
	uint32_t i;
	for (i = 0; i < length; i++)
		dst[i] = src[i];
}

static void copy_name(volatile uint8_t *dst, const char *src)
{
	uint32_t i;

	for (i = 0; i < 20U; i++) {
		if (src && src[i] != '\0')
			dst[i] = (uint8_t)src[i];
		else
			dst[i] = 0;
	}
}

static const struct SDKServiceDescriptor *find_service(uint32_t service_id)
{
	uint32_t i;

	for (i = 0; i < sizeof(sdk_services) / sizeof(sdk_services[0]); i++) {
		if (sdk_services[i].service_id == service_id)
			return &sdk_services[i];
	}

	return 0;
}

static uint32_t service_flags(const struct SDKServiceDescriptor *service)
{
	uint32_t flags;

	if (!service)
		return 0;

	flags = service->flags;
	if (service->service_id == SDK_SERVICE_IMAGE) {
		flags |= sdk_jpeg_service_flags();
		flags |= SDK_SERVICE_FLAG_IMAGE_PNG_DIRECT_BGRA;
	}
	if (service->service_id == SDK_SERVICE_VIDEO &&
	    audio_codec_present())
		flags |= SDK_SERVICE_FLAG_VIDEO_AUDIO_BIND;

	return flags;
}

static void write_completion(volatile struct SDKMailboxEntry *dst,
                             volatile struct SDKMailboxEntry *src,
                             uint16_t status, uint16_t payload_len)
{
	put_be32(dst->request_id, get_be32(src->request_id));
	put_be16(dst->opcode, get_be16(src->opcode));
	put_be16(dst->status, status);
	put_be16(dst->flags, get_be16(src->flags));
	put_be16(dst->payload_len, payload_len);
	put_be32(dst->user_cookie, get_be32(src->user_cookie));
}

static uint16_t complete_status(volatile struct SDKMailboxEntry *req,
                                volatile struct SDKMailboxEntry *comp,
                                uint16_t status)
{
	write_completion(comp, req, status, 0);
	return status;
}

static uint16_t complete_image_session_result(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp,
	uint16_t status,
	const struct SDKImageStreamResult *result)
{
	volatile struct SDKImageSessionResultPayload *payload;

	if (status != SDK_STATUS_OK)
		return complete_status(req, comp, status);

	write_completion(comp, req, SDK_STATUS_OK, sizeof(*payload));
	memset((void *)comp->payload, 0, sizeof(comp->payload));
	payload = (volatile struct SDKImageSessionResultPayload *)comp->payload;
	put_be32(payload->session, result->session);
	put_be32(payload->state, result->state);
	put_be32(payload->image_width, result->image_width);
	put_be32(payload->image_height, result->image_height);
	put_be32(payload->output_format, result->output_format);
	put_be32(payload->tile_x, result->tile_x);
	put_be32(payload->tile_y, result->tile_y);
	put_be32(payload->tile_width, result->tile_width);
	put_be32(payload->tile_height, result->tile_height);
	put_be32(payload->bytes_consumed, result->bytes_consumed);
	put_be32(payload->bytes_written, result->bytes_written);
	put_be32(payload->flags, result->flags);
	return SDK_STATUS_OK;
}

static void encode_video_session_result(
	volatile struct SDKVideoSessionResultPayload *payload,
	const struct SDKVideoStreamResult *result)
{
	put_be32(payload->session, result->session);
	put_be32(payload->state, result->state);
	put_be32(payload->width, result->width);
	put_be32(payload->height, result->height);
	put_be32(payload->frame_rate_milli, result->frame_rate_milli);
	put_be32(payload->frame_number, result->frame_number);
	put_be32(payload->frame_time_millis, result->frame_time_millis);
	put_be32(payload->bytes_accepted, result->bytes_accepted);
	put_be32(payload->bytes_written, result->bytes_written);
	put_be32(payload->flags, result->flags);
}

static uint16_t complete_video_session_result(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp,
	uint16_t status,
	const struct SDKVideoStreamResult *result)
{
	volatile struct SDKVideoSessionResultPayload *payload;

	if (status != SDK_STATUS_OK)
		return complete_status(req, comp, status);
	write_completion(comp, req, SDK_STATUS_OK, sizeof(*payload));
	memset((void *)comp->payload, 0, sizeof(comp->payload));
	payload = (volatile struct SDKVideoSessionResultPayload *)comp->payload;
	encode_video_session_result(payload, result);
	return SDK_STATUS_OK;
}

static void put_be64_parts(volatile uint8_t hi[4],
	                       volatile uint8_t lo[4], uint64_t value)
{
	put_be32(hi, (uint32_t)(value >> 32));
	put_be32(lo, (uint32_t)value);
}

static void encode_media_session_main_result(
	volatile struct SDKMediaSessionMainResultPayload *payload,
	const struct SDKMediaSessionMainResult *result)
{
	put_be32(payload->session, result->session);
	put_be32(payload->state, result->state);
	put_be32(payload->width, result->width);
	put_be32(payload->height, result->height);
	put_be32(payload->frame_rate_num, result->frame_rate_num);
	put_be32(payload->frame_rate_den, result->frame_rate_den);
	put_be32(payload->frame_number, result->frame_number);
	put_be64_parts(payload->video_pts_hi, payload->video_pts_lo,
	               result->video_pts);
	put_be32(payload->bytes_accepted, result->bytes_accepted);
	put_be32(payload->bytes_written, result->bytes_written);
	put_be32(payload->flags, result->flags);
}

static uint16_t complete_media_session_main_result(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp,
	uint16_t status,
	const struct SDKMediaSessionMainResult *result)
{
	volatile struct SDKMediaSessionMainResultPayload *payload;

	if (status != SDK_STATUS_OK)
		return complete_status(req, comp, status);
	write_completion(comp, req, SDK_STATUS_OK, sizeof(*payload));
	memset((void *)comp->payload, 0, sizeof(comp->payload));
	payload =
		(volatile struct SDKMediaSessionMainResultPayload *)comp->payload;
	encode_media_session_main_result(payload, result);
	return SDK_STATUS_OK;
}

static uint16_t complete_media_session_status_result(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp,
	uint16_t status,
	const struct SDKMediaSessionStatusResult *result)
{
	volatile struct SDKMediaSessionStatusResultPayload *payload;

	if (status != SDK_STATUS_OK)
		return complete_status(req, comp, status);
	write_completion(comp, req, SDK_STATUS_OK, sizeof(*payload));
	memset((void *)comp->payload, 0, sizeof(comp->payload));
	payload =
		(volatile struct SDKMediaSessionStatusResultPayload *)comp->payload;
	put_be32(payload->session, result->session);
	put_be32(payload->state, result->state);
	put_be32(payload->page, result->page);
	put_be32(payload->flags, result->flags);
	put_be64_parts(payload->value0_hi, payload->value0_lo,
	               result->value[0]);
	put_be64_parts(payload->value1_hi, payload->value1_lo,
	               result->value[1]);
	put_be64_parts(payload->value2_hi, payload->value2_lo,
	               result->value[2]);
	put_be64_parts(payload->value3_hi, payload->value3_lo,
	               result->value[3]);
	return SDK_STATUS_OK;
}

static void encode_media_session_audio_result(
	volatile struct SDKMediaSessionAudioResultPayload *payload,
	const struct SDKMediaSessionAudioResult *result)
{
	put_be32(payload->session, result->session);
	put_be32(payload->state, result->state);
	put_be32(payload->sample_rate, result->sample_rate);
	put_be32(payload->channels, result->channels);
	put_be32(payload->sample_format, result->sample_format);
	put_be64_parts(payload->pcm_produced_hi, payload->pcm_produced_lo,
	               result->pcm_produced);
	put_be64_parts(payload->pcm_acknowledged_hi,
	               payload->pcm_acknowledged_lo,
	               result->pcm_acknowledged);
	put_be64_parts(payload->audio_pts_hi, payload->audio_pts_lo,
	               result->audio_pts);
	put_be32(payload->flags, result->flags);
}

static uint16_t complete_media_session_audio_result(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp,
	uint16_t status,
	const struct SDKMediaSessionAudioResult *result)
{
	volatile struct SDKMediaSessionAudioResultPayload *payload;

	if (status != SDK_STATUS_OK)
		return complete_status(req, comp, status);
	write_completion(comp, req, SDK_STATUS_OK, sizeof(*payload));
	memset((void *)comp->payload, 0, sizeof(comp->payload));
	payload =
		(volatile struct SDKMediaSessionAudioResultPayload *)comp->payload;
	encode_media_session_audio_result(payload, result);
	return SDK_STATUS_OK;
}

static int is_power_of_two(uint32_t value)
{
	return value != 0 && (value & (value - 1U)) == 0;
}

static uint32_t align_up(uint32_t value, uint32_t alignment)
{
	if (alignment == 0)
		alignment = 16U;
	if (!is_power_of_two(alignment))
		return 0;
	if (value > (uint32_t)(0xffffffffU - (alignment - 1U)))
		return 0;
	return (value + alignment - 1U) & ~(alignment - 1U);
}

static int range_valid(uint32_t address, uint32_t length,
                       uint32_t base, uint32_t size)
{
	uint32_t end = base + size;

	if (length == 0 || length > size)
		return 0;
	if (address < base || address >= end)
		return 0;
	if (length > (end - address))
		return 0;
	return 1;
}

static int heap_range_valid(uint32_t address, uint32_t length)
{
	return range_valid(address, length,
	                   SDK_SHARED_HEAP_ADDRESS, SDK_SHARED_HEAP_SIZE);
}

static int local_surface_range_valid(uint32_t address, uint32_t length)
{
	return range_valid(address, length,
	                   SDK_LOCAL_SURFACE_HEAP_ADDRESS,
	                   SDK_LOCAL_SURFACE_HEAP_SIZE);
}

static int host_window_range_valid(uint32_t address, uint32_t length)
{
	uint32_t base = effective_host_window_address();
	uint32_t size = effective_host_window_size();

	if (base == 0U || size == 0U)
		return 0;
	return range_valid(address, length,
	                   base, size);
}

static int shared_buffer_live(const struct SDKSharedBuffer *buffer)
{
	if (!buffer || !buffer->in_use)
		return 0;
	if (buffer->handle == 0 || buffer->handle == SDK_INVALID_HANDLE)
		return 0;
	if ((buffer->flags & SDK_ALLOC_HOST_WINDOW) != 0U)
		return host_window_range_valid(buffer->address, buffer->length);
	return heap_range_valid(buffer->address, buffer->length);
}

static int surface_is_arm_local(const struct SDKSurface *surface)
{
	return surface && ((surface->flags & SDK_SURFACE_FLAG_ARM_LOCAL) != 0U);
}

static void prepare_surface_for_arm_read(const struct SDKSurface *surface)
{
	if (!surface || surface_is_arm_local(surface))
		return;
	Xil_DCacheInvalidateRange((INTPTR)surface->address, surface->length);
}

static void flush_surface_rect(const struct SDKSurface *surface,
                               uint32_t x, uint32_t y,
                               uint32_t width, uint32_t height)
{
	uint32_t bytes_per_pixel;
	uint32_t row_bytes;
	uint32_t row;

	if (!surface || width == 0U || height == 0U ||
	    x >= surface->width || y >= surface->height ||
	    width > (surface->width - x) ||
	    height > (surface->height - y)) {
		return;
	}

	bytes_per_pixel = surface_format_bytes(surface->format);
	if (bytes_per_pixel == 0U ||
	    x > (0xffffffffU / bytes_per_pixel) ||
	    width > (0xffffffffU / bytes_per_pixel)) {
		return;
	}

	row_bytes = width * bytes_per_pixel;
	if (row_bytes == 0U ||
	    (x * bytes_per_pixel) > surface->pitch ||
	    row_bytes > (surface->pitch - (x * bytes_per_pixel))) {
		return;
	}

	if (x == 0U && row_bytes == surface->pitch) {
		uint64_t offset = (uint64_t)y * surface->pitch;
		uint64_t length = (uint64_t)height * surface->pitch;
		if (offset > 0xffffffffULL || length > 0xffffffffULL ||
		    length > (0xffffffffULL - offset)) {
			return;
		}
		Xil_DCacheFlushRange((INTPTR)(surface->address + (uint32_t)offset),
		                     (uint32_t)length);
		return;
	}

	for (row = 0U; row < height; row++) {
		uint64_t offset = ((uint64_t)y + row) * surface->pitch;
		offset += (uint64_t)x * bytes_per_pixel;
		if (offset > 0xffffffffULL) {
			return;
		}
		Xil_DCacheFlushRange((INTPTR)(surface->address + (uint32_t)offset),
		                     row_bytes);
	}
}

static int surface_live(const struct SDKSurface *surface)
{
	uint32_t bytes_per_pixel;

	if (!surface || !surface->in_use)
		return 0;
	if (surface->handle == 0 || surface->handle == SDK_INVALID_HANDLE ||
	    surface->handle == SDK_SURFACE_HANDLE_FRAMEBUFFER)
		return 0;
	if (surface->width == 0 || surface->height == 0 || surface->pitch == 0)
		return 0;
	bytes_per_pixel = surface_format_bytes(surface->format);
	if (bytes_per_pixel == 0 ||
	    surface->width > (0xffffffffU / bytes_per_pixel))
		return 0;
	if (surface->pitch < surface->width * bytes_per_pixel)
		return 0;
	if (surface->height > (0xffffffffU / surface->pitch))
		return 0;
	if (surface->length != surface->pitch * surface->height)
		return 0;
	if (surface_is_arm_local(surface))
		return local_surface_range_valid(surface->address,
		                                 surface->length);
	return heap_range_valid(surface->address, surface->length);
}

static uint32_t sanitize_allocator_metadata(void)
{
	uint32_t i;
	uint32_t invalid = 0;

	for (i = 0; i < SDK_MAX_SHARED_BUFFERS; i++) {
		if (shared_buffers[i].in_use &&
		    !shared_buffer_live(&shared_buffers[i])) {
			memset(&shared_buffers[i], 0, sizeof(shared_buffers[i]));
			invalid++;
		}
	}

	for (i = 0; i < SDK_MAX_SURFACES; i++) {
		if (surfaces[i].in_use && !surface_live(&surfaces[i])) {
			memset(&surfaces[i], 0, sizeof(surfaces[i]));
			invalid++;
		}
	}

	return invalid;
}

static struct SDKSharedBuffer *find_shared_buffer(uint32_t handle)
{
	uint32_t i;

	for (i = 0; i < SDK_MAX_SHARED_BUFFERS; i++) {
		if (shared_buffer_live(&shared_buffers[i]) &&
		    shared_buffers[i].handle == handle)
			return &shared_buffers[i];
	}

	return 0;
}

static struct SDKSharedBuffer *find_free_buffer_slot(void)
{
	uint32_t i;

	for (i = 0; i < SDK_MAX_SHARED_BUFFERS; i++) {
		if (!shared_buffer_live(&shared_buffers[i]))
			return &shared_buffers[i];
	}

	return 0;
}

static int shared_buffer_pin(struct SDKSharedBuffer *buffer)
{
	if (!shared_buffer_live(buffer) || buffer->pin_count == 0xffffffffU)
		return 0;
	buffer->pin_count++;
	return 1;
}

static void shared_buffer_unpin(struct SDKSharedBuffer *buffer)
{
	if (buffer && buffer->pin_count != 0U)
		buffer->pin_count--;
}

static void release_media_pcm_ring(uint32_t session)
{
	if (media_pcm_ring && media_pcm_ring_session == session) {
		shared_buffer_unpin(media_pcm_ring);
		media_pcm_ring = 0;
		media_pcm_ring_session = 0U;
	}
}

static struct SDKSurface *find_surface(uint32_t handle)
{
	uint32_t i;

	for (i = 0; i < SDK_MAX_SURFACES; i++) {
		if (surface_live(&surfaces[i]) && surfaces[i].handle == handle)
			return &surfaces[i];
	}

	return 0;
}

static struct SDKSurface *find_free_surface_slot(void)
{
	uint32_t i;

	for (i = 0; i < SDK_MAX_SURFACES; i++) {
		if (!surface_live(&surfaces[i]))
			return &surfaces[i];
	}

	return 0;
}

static uint32_t count_used_shared_buffers(void)
{
	uint32_t i;
	uint32_t used = 0;

	for (i = 0; i < SDK_MAX_SHARED_BUFFERS; i++) {
		if (shared_buffer_live(&shared_buffers[i]))
			used++;
	}

	return used;
}

static uint32_t count_used_surfaces(void)
{
	uint32_t i;
	uint32_t used = 0;

	for (i = 0; i < SDK_MAX_SURFACES; i++) {
		if (surface_live(&surfaces[i]))
			used++;
	}

	return used;
}

static uint32_t find_allocation_end_containing(uint32_t address,
                                               int local_surfaces)
{
	uint32_t i;

	if (!local_surfaces) {
		for (i = 0; i < SDK_MAX_SHARED_BUFFERS; i++) {
			uint32_t start;
			uint32_t end;

			if (!shared_buffer_live(&shared_buffers[i]))
				continue;

			start = shared_buffers[i].address;
			end = start + shared_buffers[i].length;
			if (address >= start && address < end)
				return end;
		}
	}

	for (i = 0; i < SDK_MAX_SURFACES; i++) {
		uint32_t start;
		uint32_t end;

		if (!surface_live(&surfaces[i]))
			continue;
		if (surface_is_arm_local(&surfaces[i]) != local_surfaces)
			continue;

		start = surfaces[i].address;
		end = start + surfaces[i].length;
		if (address >= start && address < end)
			return end;
	}

	return 0;
}

static uint32_t find_next_allocation_start(uint32_t address, uint32_t heap_end,
                                           int local_surfaces)
{
	uint32_t i;
	uint32_t next = heap_end;

	if (!local_surfaces) {
		for (i = 0; i < SDK_MAX_SHARED_BUFFERS; i++) {
			if (shared_buffer_live(&shared_buffers[i]) &&
			    shared_buffers[i].address >= address &&
			    shared_buffers[i].address < next) {
				next = shared_buffers[i].address;
			}
		}
	}

	for (i = 0; i < SDK_MAX_SURFACES; i++) {
		if (surface_live(&surfaces[i]) &&
		    surface_is_arm_local(&surfaces[i]) == local_surfaces &&
		    surfaces[i].address >= address &&
		    surfaces[i].address < next) {
			next = surfaces[i].address;
		}
	}

	return next;
}

static void heap_free_stats_in(uint32_t base, uint32_t size,
			       uint32_t *free_total, uint32_t *largest_free)
{
	uint32_t cursor = base;
	uint32_t heap_end;
	uint32_t total = 0;
	uint32_t largest = 0;

	if (base > UINT32_MAX - size) {
		*free_total = 0U;
		*largest_free = 0U;
		return;
	}
	heap_end = base + size;

	while (cursor < heap_end) {
		uint32_t alloc_end = find_allocation_end_containing(cursor, 0);
		uint32_t next_alloc;
		uint32_t free_len;

		if (alloc_end != 0) {
			cursor = alloc_end;
			continue;
		}

		next_alloc = find_next_allocation_start(cursor, heap_end, 0);
		free_len = next_alloc - cursor;
		total += free_len;
		if (free_len > largest)
			largest = free_len;
		cursor = next_alloc;
	}

	*free_total = total;
	*largest_free = largest;
}

static void heap_free_stats(uint32_t *free_total, uint32_t *largest_free)
{
	heap_free_stats_in(SDK_SHARED_HEAP_ADDRESS, SDK_SHARED_HEAP_SIZE,
			   free_total, largest_free);
}

static uint32_t count_host_window_allocations(void)
{
	uint32_t i;
	uint32_t used = 0U;

	for (i = 0; i < SDK_MAX_SHARED_BUFFERS; i++) {
		if (shared_buffer_live(&shared_buffers[i]) &&
		    (shared_buffers[i].flags & SDK_ALLOC_HOST_WINDOW) != 0U)
			used++;
	}
	return used;
}

static int buffer_range_valid(const struct SDKSharedBuffer *buffer,
                              uint32_t offset, uint32_t length)
{
	if (!buffer)
		return 0;
	if (offset > buffer->length)
		return 0;
	if (length > (buffer->length - offset))
		return 0;
	return 1;
}

static int ranges_overlap(uint32_t offset_a, uint32_t length_a,
                          uint32_t offset_b, uint32_t length_b)
{
	if (length_a == 0U || length_b == 0U)
		return 0;
	if (offset_a > (0xffffffffU - length_a) ||
	    offset_b > (0xffffffffU - length_b)) {
		return 1;
	}
	return offset_a < (offset_b + length_b) &&
	       offset_b < (offset_a + length_a);
}

/* offset/length window fits within total (overflow-safe). */
static int batch_range_ok(uint32_t total, uint32_t offset, uint32_t length)
{
	return offset <= total && length <= total - offset;
}

static uint32_t next_handle(void)
{
	uint32_t handle = next_shared_handle++;

	if (next_shared_handle == 0 || next_shared_handle == 0xffffffffU)
		next_shared_handle = 1;
	if (handle == 0 || handle == 0xffffffffU)
		handle = next_handle();

	return handle;
}

static uint32_t find_free_region_in(uint32_t base, uint32_t size,
                                    uint32_t length, uint32_t alignment,
                                    int local_surfaces)
{
	uint32_t heap_end = base + size;
	uint32_t candidate = align_up(base, alignment);
	uint32_t i;

	while (candidate != 0 && candidate < heap_end &&
	       length <= (heap_end - candidate)) {
		int collision = 0;

		if (!local_surfaces) {
			for (i = 0; i < SDK_MAX_SHARED_BUFFERS; i++) {
				uint32_t block_start;
				uint32_t block_end;

				if (!shared_buffer_live(&shared_buffers[i]))
					continue;

				block_start = shared_buffers[i].address;
				block_end = block_start + shared_buffers[i].length;
				if (candidate + length <= block_start ||
				    candidate >= block_end)
					continue;

				candidate = align_up(block_end, alignment);
				collision = 1;
				break;
			}
		}

		if (collision)
			continue;

		for (i = 0; i < SDK_MAX_SURFACES; i++) {
			uint32_t block_start;
			uint32_t block_end;

			if (!surface_live(&surfaces[i]))
				continue;
			if (surface_is_arm_local(&surfaces[i]) != local_surfaces)
				continue;

			block_start = surfaces[i].address;
			block_end = block_start + surfaces[i].length;
			if (candidate + length <= block_start || candidate >= block_end)
				continue;

			candidate = align_up(block_end, alignment);
			collision = 1;
			break;
		}

		if (!collision)
			return candidate;
	}

	return 0;
}

static uint32_t find_free_region(uint32_t length, uint32_t alignment)
{
	return find_free_region_in(SDK_SHARED_HEAP_ADDRESS,
	                           SDK_SHARED_HEAP_SIZE,
	                           length, alignment, 0);
}

static uint32_t find_free_local_surface_region(uint32_t length,
                                               uint32_t alignment)
{
	return find_free_region_in(SDK_LOCAL_SURFACE_HEAP_ADDRESS,
	                           SDK_LOCAL_SURFACE_HEAP_SIZE,
	                           length, alignment, 1);
}

static uint32_t find_free_host_window_region(uint32_t length,
                                             uint32_t alignment)
{
	uint32_t base = effective_host_window_address();
	uint32_t size = effective_host_window_size();

	if (base == 0U || size == 0U)
		return 0U;
	/* Shared-heap blocks can never overlap a candidate here (disjoint
	 * regions), so the region-agnostic overlap scan is reusable. */
	return find_free_region_in(base, size,
	                           length, alignment, 0);
}

static uint16_t handle_alloc_shared(volatile struct SDKMailboxEntry *req,
                                    volatile struct SDKMailboxEntry *comp,
                                    uint16_t payload_len)
{
	volatile struct SDKAllocSharedPayload *payload;
	volatile struct SDKSharedBufferInfoPayload *info;
	struct SDKSharedBuffer *slot;
	uint32_t length;
	uint32_t alignment;
	uint32_t flags;
	uint32_t address;

	if (payload_len < 12U)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	payload = (volatile struct SDKAllocSharedPayload *)req->payload;
	length = get_be32(payload->length);
	alignment = get_be32(payload->alignment);
	flags = get_be32(payload->flags);
	if (alignment == 0)
		alignment = 16U;
	if (length == 0 || !is_power_of_two(alignment))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	if ((flags & SDK_ALLOC_HOST_WINDOW) != 0U) {
		uint32_t host_window_size = effective_host_window_size();

		if (host_window_size == 0U)
			return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);
		if (length > host_window_size)
			return complete_status(req, comp,
			                       SDK_STATUS_BAD_REQUEST);
	} else if (length > SDK_SHARED_HEAP_SIZE) {
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	}

	sanitize_allocator_metadata();
	slot = find_free_buffer_slot();
	if (!slot)
		return complete_status(req, comp, SDK_STATUS_NO_MEMORY);
	memset(slot, 0, sizeof(*slot));

	if ((flags & SDK_ALLOC_HOST_WINDOW) != 0U)
		address = find_free_host_window_region(length, alignment);
	else
		address = find_free_region(length, alignment);
	if (address == 0)
		return complete_status(req, comp, SDK_STATUS_NO_MEMORY);

	slot->handle = next_handle();
	slot->address = address;
	slot->length = length;
	slot->flags = flags;
	slot->in_use = 1;

	write_completion(comp, req, SDK_STATUS_OK, sizeof(*info));
	memset((void *)comp->payload, 0, sizeof(comp->payload));
	info = (volatile struct SDKSharedBufferInfoPayload *)comp->payload;
	put_be32(info->handle, slot->handle);
	put_be32(info->arm_addr, slot->address);
	put_be32(info->length, slot->length);
	put_be32(info->flags, slot->flags);
	return SDK_STATUS_OK;
}

static uint16_t handle_free_shared(volatile struct SDKMailboxEntry *req,
                                   volatile struct SDKMailboxEntry *comp,
                                   uint16_t payload_len)
{
	volatile struct SDKFreeSharedPayload *payload;
	struct SDKSharedBuffer *buffer;
	uint32_t handle;

	if (payload_len < 4U)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	payload = (volatile struct SDKFreeSharedPayload *)req->payload;
	handle = get_be32(payload->handle);
	buffer = find_shared_buffer(handle);
	if (!buffer)
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	if (buffer->pin_count != 0U)
		return complete_status(req, comp, SDK_STATUS_BUSY);

	memset(buffer, 0, sizeof(*buffer));
	return complete_status(req, comp, SDK_STATUS_OK);
}

static uint16_t handle_mem_fill(volatile struct SDKMailboxEntry *req,
                                volatile struct SDKMailboxEntry *comp,
                                uint16_t payload_len)
{
	volatile struct SDKMemFillPayload *payload;
	struct SDKSharedBuffer *buffer;
	uint32_t handle;
	uint32_t offset;
	uint32_t length;

	if (payload_len < 13U)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	payload = (volatile struct SDKMemFillPayload *)req->payload;
	handle = get_be32(payload->handle);
	offset = get_be32(payload->offset);
	length = get_be32(payload->length);
	buffer = find_shared_buffer(handle);
	if (!buffer)
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	if (!buffer_range_valid(buffer, offset, length))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	if (length != 0) {
		memset((void *)(uintptr_t)(buffer->address + offset),
		       payload->value, length);
		Xil_DCacheFlushRange((INTPTR)(buffer->address + offset), length);
	}

	return complete_status(req, comp, SDK_STATUS_OK);
}

static uint16_t handle_mem_copy(volatile struct SDKMailboxEntry *req,
                                volatile struct SDKMailboxEntry *comp,
                                uint16_t payload_len)
{
	volatile struct SDKMemCopyPayload *payload;
	struct SDKSharedBuffer *dst;
	struct SDKSharedBuffer *src;
	uint32_t dst_offset;
	uint32_t src_offset;
	uint32_t length;

	if (payload_len < 20U)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	payload = (volatile struct SDKMemCopyPayload *)req->payload;
	dst = find_shared_buffer(get_be32(payload->dst_handle));
	src = find_shared_buffer(get_be32(payload->src_handle));
	dst_offset = get_be32(payload->dst_offset);
	src_offset = get_be32(payload->src_offset);
	length = get_be32(payload->length);
	if (!dst || !src)
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	if (!buffer_range_valid(dst, dst_offset, length) ||
	    !buffer_range_valid(src, src_offset, length))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	if (length != 0) {
		Xil_DCacheInvalidateRange((INTPTR)(src->address + src_offset),
		                          length);
		memmove((void *)(uintptr_t)(dst->address + dst_offset),
		        (const void *)(uintptr_t)(src->address + src_offset),
		        length);
		Xil_DCacheFlushRange((INTPTR)(dst->address + dst_offset), length);
	}

	return complete_status(req, comp, SDK_STATUS_OK);
}

static uint32_t color_mode_surface_format(uint32_t color_mode)
{
	switch (color_mode) {
	case MNTVA_COLOR_8BIT:
		return SDK_SURFACE_FORMAT_INDEX8;
	case MNTVA_COLOR_16BIT565:
		return SDK_SURFACE_FORMAT_RGB565;
	case MNTVA_COLOR_15BIT:
		return SDK_SURFACE_FORMAT_RGB555;
	case MNTVA_COLOR_32BIT:
		return SDK_SURFACE_FORMAT_BGRA8888;
	default:
		return SDK_SURFACE_FORMAT_UNKNOWN;
	}
}

static uint32_t surface_format_bytes(uint32_t format)
{
	return sdk_surface_format_bytes(format);
}

static uint32_t next_surface_id(void)
{
	uint32_t handle = SDK_SURFACE_HANDLE_BASE | next_surface_handle++;

	if (next_surface_handle == 0)
		next_surface_handle = 1;

	return handle;
}

static uint16_t write_surface_completion(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp,
	const struct SDKSurface *surface_info)
{
	volatile struct SDKSurfaceInfoPayload *payload;

	write_completion(comp, req, SDK_STATUS_OK, sizeof(*payload));
	memset((void *)comp->payload, 0, sizeof(comp->payload));
	payload = (volatile struct SDKSurfaceInfoPayload *)comp->payload;
	put_be32(payload->handle, surface_info->handle);
	put_be32(payload->arm_addr, surface_info->address);
	put_be32(payload->width, surface_info->width);
	put_be32(payload->height, surface_info->height);
	put_be32(payload->pitch, surface_info->pitch);
	put_be32(payload->format, surface_info->format);
	put_be32(payload->flags, surface_info->flags);
	put_be32(payload->length, surface_info->length);
	return SDK_STATUS_OK;
}

static int fill_framebuffer_surface(struct SDKSurface *surface_info)
{
	struct ZZ_VIDEO_STATE *state = video_get_state();
	struct zz_video_mode *mode;
	uint32_t mode_index;
	uint32_t bytes_per_pixel;
	uint32_t pitch_pixels;

	if (!state)
		return 0;

	mode_index = (uint32_t)(state->video_mode & 0xff);
	if (mode_index >= ZZVMODE_NUM)
		mode_index = ZZVMODE_800x600;
	mode = get_custom_video_mode_ptr(mode_index);

	memset(surface_info, 0, sizeof(*surface_info));
	surface_info->handle = SDK_SURFACE_HANDLE_FRAMEBUFFER;
	surface_info->address = (uint32_t)(uintptr_t)state->framebuffer +
	                        state->framebuffer_pan_offset;
	surface_info->width = state->vmode_hsize ?
	                      state->vmode_hsize : (uint32_t)mode->hres;
	surface_info->height = state->vmode_vsize ?
	                       state->vmode_vsize : (uint32_t)mode->vres;
	if (state->scalemode & 1)
		surface_info->width /= 2U;
	surface_info->height /= video_vertical_scale_factor(state->scalemode);
	if (surface_info->width == 0 || surface_info->height == 0)
		return 0;

	surface_info->format =
		color_mode_surface_format((uint32_t)state->colormode);
	bytes_per_pixel = surface_format_bytes(surface_info->format);
	if (bytes_per_pixel == 0)
		return 0;

	pitch_pixels = state->framebuffer_pan_width ?
	               state->framebuffer_pan_width : surface_info->width;
	surface_info->pitch = pitch_pixels * bytes_per_pixel;
	surface_info->flags = SDK_SURFACE_FLAG_CPU_VISIBLE |
	                      SDK_SURFACE_FLAG_FRAMEBUFFER |
	                      SDK_SURFACE_FLAG_DISPLAYED;
	surface_info->length = surface_info->pitch * surface_info->height;
	surface_info->in_use = 1;
	return 1;
}

static uint16_t handle_map_framebuffer_surface(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp)
{
	struct SDKSurface surface_info;

	if (!fill_framebuffer_surface(&surface_info))
		return complete_status(req, comp, SDK_STATUS_INTERNAL_ERROR);

	return write_surface_completion(req, comp, &surface_info);
}

static int get_surface_info(uint32_t handle, struct SDKSurface *surface_info)
{
	struct SDKSurface *surface;

	if (handle == SDK_SURFACE_HANDLE_FRAMEBUFFER)
		return fill_framebuffer_surface(surface_info);

	surface = find_surface(handle);
	if (!surface)
		return 0;

	*surface_info = *surface;
	return 1;
}

static int surface_range_valid(const struct SDKSurface *surface_info,
                               uint32_t x, uint32_t y,
                               uint32_t width, uint32_t height)
{
	if (!surface_info || width == 0 || height == 0)
		return 0;
	if (x > surface_info->width || y > surface_info->height)
		return 0;
	if (width > (surface_info->width - x))
		return 0;
	if (height > (surface_info->height - y))
		return 0;
	return 1;
}

static uint16_t handle_alloc_surface(volatile struct SDKMailboxEntry *req,
                                     volatile struct SDKMailboxEntry *comp,
                                     uint16_t payload_len)
{
	volatile struct SDKAllocSurfacePayload *payload;
	struct SDKSurface *slot;
	uint32_t width;
	uint32_t height;
	uint32_t format;
	uint32_t flags;
	uint32_t pitch;
	uint32_t bytes_per_pixel;
	uint32_t length;
	uint32_t address;
	int arm_local;

	if (payload_len < 20U)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	payload = (volatile struct SDKAllocSurfacePayload *)req->payload;
	width = get_be32(payload->width);
	height = get_be32(payload->height);
	format = get_be32(payload->format);
	flags = get_be32(payload->flags);
	pitch = get_be32(payload->pitch);
	bytes_per_pixel = surface_format_bytes(format);
	arm_local = (flags & SDK_SURFACE_FLAG_ARM_LOCAL) != 0U;

	if (width == 0 || height == 0 || bytes_per_pixel == 0)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	if (pitch == 0)
		pitch = width * bytes_per_pixel;
	if (pitch < width * bytes_per_pixel)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	if (height > (0xffffffffU / pitch))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	length = pitch * height;
	if (length == 0)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	if (arm_local && length > SDK_LOCAL_SURFACE_HEAP_SIZE)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	if (!arm_local && length > SDK_SHARED_HEAP_SIZE)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	sanitize_allocator_metadata();
	slot = find_free_surface_slot();
	if (!slot)
		return complete_status(req, comp, SDK_STATUS_NO_MEMORY);

	address = arm_local ? find_free_local_surface_region(length, 64U) :
	                      find_free_region(length, 64U);
	if (address == 0)
		return complete_status(req, comp, SDK_STATUS_NO_MEMORY);

	memset(slot, 0, sizeof(*slot));
	slot->handle = next_surface_id();
	slot->address = address;
	slot->width = width;
	slot->height = height;
	slot->pitch = pitch;
	slot->format = format;
	if (arm_local)
		slot->flags = (flags | SDK_SURFACE_FLAG_ARM_LOCAL) &
		              ~SDK_SURFACE_FLAG_CPU_VISIBLE;
	else
		slot->flags = flags | SDK_SURFACE_FLAG_CPU_VISIBLE;
	slot->length = length;
	slot->in_use = 1;

	memset((void *)(uintptr_t)address, 0, length);
	Xil_DCacheFlushRange((INTPTR)address, length);

	return write_surface_completion(req, comp, slot);
}

static uint16_t handle_free_surface(volatile struct SDKMailboxEntry *req,
                                    volatile struct SDKMailboxEntry *comp,
                                    uint16_t payload_len)
{
	volatile struct SDKFreeSurfacePayload *payload;
	struct SDKSurface *surface;
	uint32_t handle;

	if (payload_len < 4U)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	payload = (volatile struct SDKFreeSurfacePayload *)req->payload;
	handle = get_be32(payload->handle);
	if (handle == SDK_SURFACE_HANDLE_FRAMEBUFFER)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	surface = find_surface(handle);
	if (!surface)
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);

	memset(surface, 0, sizeof(*surface));
	return complete_status(req, comp, SDK_STATUS_OK);
}

static uint16_t service_try_defer(uint16_t opcode,
                                  volatile struct SDKMailboxEntry *req,
                                  const void *params, uint32_t param_len,
                                  uint32_t in_len);

/*
 * A scale request may be deferred to core 1 only when neither surface is
 * the live framebuffer (its address/geometry come from video state that
 * can change while the task waits in the queue), neither surface is
 * ARM-local, and all geometry fits the u16 fields of scale_op_params.
 * Anything else computes inline, byte-identically to the pre-scheduler
 * firmware.
 *
 * ARM-local surfaces are excluded because their cache contract is
 * single-core: they have no Zorro-side writer, so reads skip
 * invalidation (prepare_surface_for_arm_read) and rely on the reading
 * core's cache being authoritative. The local surface heap is NOT in
 * the SCU-coherent region, so letting these ops bounce between cores
 * would need invalidate/flush in both directions on every hop --
 * cheaper and safer to keep ARM-local work on core 0.
 */
static int scale_defer_eligible(uint32_t src_handle, uint32_t dst_handle,
                                const struct SDKSurface *src,
                                const struct SDKSurface *dst)
{
	if (src_handle == SDK_SURFACE_HANDLE_FRAMEBUFFER ||
	    dst_handle == SDK_SURFACE_HANDLE_FRAMEBUFFER)
		return 0;
	if (surface_is_arm_local(src) || surface_is_arm_local(dst))
		return 0;
	if (src->format != dst->format)
		return 0;
	if (src->width > 0xffffU || src->height > 0xffffU ||
	    src->pitch > 0xffffU || src->format > 0xffffU ||
	    dst->width > 0xffffU || dst->height > 0xffffU ||
	    dst->pitch > 0xffffU)
		return 0;
	return 1;
}

static void scale_pack_params(struct scale_op_params *p,
                              const struct SDKSurface *src,
                              const struct SDKSurface *dst,
                              uint32_t filter)
{
	memset(p, 0, sizeof(*p));
	p->src_addr = src->address;
	p->dst_addr = dst->address;
	p->src_w = (uint16_t)src->width;
	p->src_h = (uint16_t)src->height;
	p->dst_w = (uint16_t)dst->width;
	p->dst_h = (uint16_t)dst->height;
	p->src_pitch = (uint16_t)src->pitch;
	p->dst_pitch = (uint16_t)dst->pitch;
	p->format = (uint16_t)src->format;
	p->filter_flags = (uint16_t)(filter & 0xffU);
}

/* Destination-rect byte count for SHORT/LONG classification, saturated
 * (only the <= TASKQ_SHORT_MAX_BYTES comparison matters). */
static uint32_t scale_rect_bytes(uint32_t w, uint32_t h, uint32_t bpp)
{
	uint32_t px = w * h;   /* both <= 0xffff, cannot overflow u32 */

	if (px > TASKQ_SHORT_MAX_BYTES)
		return 0xffffffffU;
	return px * bpp;
}

static uint16_t handle_scale_image(volatile struct SDKMailboxEntry *req,
                                   volatile struct SDKMailboxEntry *comp,
                                   uint16_t payload_len)
{
	volatile struct SDKScaleImagePayload *payload;
	struct SDKSurface src;
	struct SDKSurface dst;
	uint32_t src_x;
	uint32_t src_y;
	uint32_t src_w;
	uint32_t src_h;
	uint32_t dst_x;
	uint32_t dst_y;
	uint32_t dst_w;
	uint32_t dst_h;
	uint32_t filter;
	uint32_t bytes_per_pixel;

	if (payload_len < sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	payload = (volatile struct SDKScaleImagePayload *)req->payload;
	if (!get_surface_info(get_be32(payload->src_surface), &src) ||
	    !get_surface_info(get_be32(payload->dst_surface), &dst)) {
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	}

	src_x = get_be32(payload->src_x);
	src_y = get_be32(payload->src_y);
	src_w = get_be32(payload->src_w);
	src_h = get_be32(payload->src_h);
	dst_x = get_be32(payload->dst_x);
	dst_y = get_be32(payload->dst_y);
	dst_w = get_be32(payload->dst_w);
	dst_h = get_be32(payload->dst_h);
	filter = get_be32(payload->filter);

	if (filter != SDK_SCALE_NEAREST && filter != SDK_SCALE_BILINEAR)
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);
	if (!sdk_surface_scale_formats_supported(src.format, dst.format,
	                                         filter))
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);
	if (!surface_range_valid(&src, src_x, src_y, src_w, src_h) ||
	    !surface_range_valid(&dst, dst_x, dst_y, dst_w, dst_h)) {
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	}

	bytes_per_pixel = surface_format_bytes(dst.format);
	if (bytes_per_pixel == 0)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	if (scale_defer_eligible(get_be32(payload->src_surface),
	                         get_be32(payload->dst_surface), &src, &dst)) {
		struct scale_op_params p;

		scale_pack_params(&p, &src, &dst, filter);
		p.src_x = (uint16_t)src_x;
		p.src_y = (uint16_t)src_y;
		p.src_rw = (uint16_t)src_w;
		p.src_rh = (uint16_t)src_h;
		p.dst_x = (uint16_t)dst_x;
		p.dst_y = (uint16_t)dst_y;
		p.dst_rw = (uint16_t)dst_w;
		p.dst_rh = (uint16_t)dst_h;
		if (service_try_defer(SDK_OP_SCALE_IMAGE, req, &p, sizeof(p),
		                      scale_rect_bytes(dst_w, dst_h,
		                                       bytes_per_pixel)) ==
		    SDK_STATUS_QUEUED)
			return SDK_STATUS_QUEUED;
		/* defer unavailable -> compute inline below, as before */
	}

	prepare_surface_for_arm_read(&src);
	if (!sdk_surface_scale_rect_formats(
		    (uint8_t *)(uintptr_t)dst.address,
		    dst.width, dst.height, dst.pitch, dst.format,
		    (const uint8_t *)(uintptr_t)src.address,
		    src.width, src.height, src.pitch, src.format,
		    src_x, src_y, src_w, src_h,
		    dst_x, dst_y, dst_w, dst_h, filter)) {
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	}
	scheduler_shared()->tasks_on_core0++;   /* offload-class op run inline */
	flush_surface_rect(&dst, dst_x, dst_y, dst_w, dst_h);

	return complete_status(req, comp, SDK_STATUS_OK);
}

static uint16_t handle_scale_image_clipped(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp,
	uint16_t payload_len)
{
	volatile struct SDKScaleImageClippedPayload *payload;
	struct SDKSurface src;
	struct SDKSurface dst;
	uint32_t src_x;
	uint32_t src_y;
	uint32_t src_w;
	uint32_t src_h;
	uint32_t dst_x;
	uint32_t dst_y;
	uint32_t dst_w;
	uint32_t dst_h;
	uint32_t clip_x;
	uint32_t clip_y;
	uint32_t clip_w;
	uint32_t clip_h;
	uint32_t filter;
	uint32_t bytes_per_pixel;

	if (payload_len < sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	payload = (volatile struct SDKScaleImageClippedPayload *)req->payload;
	if (!get_surface_info(get_be32(payload->src_surface), &src) ||
	    !get_surface_info(get_be32(payload->dst_surface), &dst)) {
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	}

	src_x = get_be16(payload->src_x);
	src_y = get_be16(payload->src_y);
	src_w = get_be16(payload->src_w);
	src_h = get_be16(payload->src_h);
	dst_x = get_be16(payload->dst_x);
	dst_y = get_be16(payload->dst_y);
	dst_w = get_be16(payload->dst_w);
	dst_h = get_be16(payload->dst_h);
	clip_x = get_be16(payload->clip_x);
	clip_y = get_be16(payload->clip_y);
	clip_w = get_be16(payload->clip_w);
	clip_h = get_be16(payload->clip_h);
	filter = get_be32(payload->filter);

	if (filter != SDK_SCALE_NEAREST && filter != SDK_SCALE_BILINEAR)
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);
	if (!sdk_surface_scale_formats_supported(src.format, dst.format,
	                                         filter))
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);
	if (!surface_range_valid(&src, src_x, src_y, src_w, src_h) ||
	    !surface_range_valid(&dst, dst_x, dst_y, dst_w, dst_h) ||
	    clip_w == 0U || clip_h == 0U) {
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	}

	bytes_per_pixel = surface_format_bytes(dst.format);
	if (bytes_per_pixel == 0)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	if (scale_defer_eligible(get_be32(payload->src_surface),
	                         get_be32(payload->dst_surface), &src, &dst)) {
		struct scale_op_params p;

		scale_pack_params(&p, &src, &dst, filter);
		p.src_x = (uint16_t)src_x;
		p.src_y = (uint16_t)src_y;
		p.src_rw = (uint16_t)src_w;
		p.src_rh = (uint16_t)src_h;
		p.dst_x = (uint16_t)dst_x;
		p.dst_y = (uint16_t)dst_y;
		p.dst_rw = (uint16_t)dst_w;
		p.dst_rh = (uint16_t)dst_h;
		p.clip_x = (uint16_t)clip_x;
		p.clip_y = (uint16_t)clip_y;
		p.clip_rw = (uint16_t)clip_w;
		p.clip_rh = (uint16_t)clip_h;
		if (service_try_defer(SDK_OP_SCALE_IMAGE_CLIPPED, req, &p,
		                      sizeof(p),
		                      scale_rect_bytes(clip_w, clip_h,
		                                       bytes_per_pixel)) ==
		    SDK_STATUS_QUEUED)
			return SDK_STATUS_QUEUED;
		/* defer unavailable -> compute inline below, as before */
	}

	prepare_surface_for_arm_read(&src);
	if (!sdk_surface_scale_rect_formats_clipped(
		    (uint8_t *)(uintptr_t)dst.address,
		    dst.width, dst.height, dst.pitch, dst.format,
		    (const uint8_t *)(uintptr_t)src.address,
		    src.width, src.height, src.pitch, src.format,
		    src_x, src_y, src_w, src_h,
		    dst_x, dst_y, dst_w, dst_h,
		    clip_x, clip_y, clip_w, clip_h, filter)) {
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	}
	scheduler_shared()->tasks_on_core0++;   /* offload-class op run inline */
	flush_surface_rect(&dst, dst_x, dst_y, dst_w, dst_h);

	return complete_status(req, comp, SDK_STATUS_OK);
}

static int decode_bounds(uint32_t surface_limit, uint32_t offset,
                         uint32_t requested, uint32_t *bounds)
{
	if (offset > surface_limit)
		return 0;
	if (requested == 0U) {
		*bounds = surface_limit;
		return 1;
	}
	if (requested > (0xffffffffU - offset))
		return 0;
	if ((offset + requested) > surface_limit)
		return 0;
	*bounds = offset + requested;
	return 1;
}

static uint16_t handle_decode_jpeg(volatile struct SDKMailboxEntry *req,
                                   volatile struct SDKMailboxEntry *comp,
                                   uint16_t payload_len)
{
	volatile struct SDKImageDecodePayload *payload;
	volatile struct SDKImageDecodeResultPayload *result;
	struct SDKSharedBuffer *src;
	struct SDKSurface dst;
	uint32_t src_offset;
	uint32_t src_length;
	uint32_t dst_x;
	uint32_t dst_y;
	uint32_t dst_width;
	uint32_t dst_height;
	uint32_t output_format;
	uint32_t flags;
	uint32_t bytes_written = 0;
	uint32_t image_width = 0;
	uint32_t image_height = 0;

	if (payload_len < sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	payload = (volatile struct SDKImageDecodePayload *)req->payload;
	src = find_shared_buffer(get_be32(payload->src_handle));
	if (!src || !get_surface_info(get_be32(payload->dst_surface), &dst))
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);

	src_offset = get_be32(payload->src_offset);
	src_length = get_be32(payload->src_length);
	dst_x = get_be32(payload->dst_x);
	dst_y = get_be32(payload->dst_y);
	output_format = get_be32(payload->output_format);
	flags = get_be32(payload->flags);

	if (flags != 0U)
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);
	if (output_format != dst.format)
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);
	if (output_format != SDK_SURFACE_FORMAT_ARGB8888 &&
	    output_format != SDK_SURFACE_FORMAT_RGBA8888 &&
	    output_format != SDK_SURFACE_FORMAT_BGRA8888 &&
	    output_format != SDK_SURFACE_FORMAT_RGB888) {
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);
	}
	if (!buffer_range_valid(src, src_offset, src_length) ||
	    src_length == 0U) {
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	}
	if (!decode_bounds(dst.width, dst_x, get_be32(payload->dst_width),
	                   &dst_width) ||
	    !decode_bounds(dst.height, dst_y, get_be32(payload->dst_height),
	                   &dst_height)) {
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	}

	/* Defer to core 1 when eligible: never a framebuffer-backed dst
	 * (its geometry tracks live video state), never an ARM-local dst
	 * (single-core cache contract -- see scale_defer_eligible), and
	 * everything must fit the u16 op_params fields. Decoder memory is
	 * core-1-reclaimable (jmem_zz9k.c), so a worker fault mid-decode
	 * cannot leak. */
	if (get_be32(payload->dst_surface) != SDK_SURFACE_HANDLE_FRAMEBUFFER &&
	    !surface_is_arm_local(&dst) &&
	    dst.width <= 0xffffU && dst.height <= 0xffffU &&
	    dst.pitch <= 0xffffU && dst_x <= 0xffffU && dst_y <= 0xffffU &&
	    dst_width <= 0xffffU && dst_height <= 0xffffU) {
		struct jpeg_op_params p;

		memset(&p, 0, sizeof(p));
		p.src_addr = src->address + src_offset;
		p.src_len = src_length;
		p.dst_addr = dst.address;
		p.output_format = output_format;
		p.dst_pitch = (uint16_t)dst.pitch;
		p.dst_x = (uint16_t)dst_x;
		p.dst_y = (uint16_t)dst_y;
		p.dst_width = (uint16_t)dst_width;
		p.dst_height = (uint16_t)dst_height;
		p.dst_surf_w = (uint16_t)dst.width;
		p.dst_surf_h = (uint16_t)dst.height;
		if (service_try_defer(SDK_OP_DECODE_JPEG, req, &p, sizeof(p),
		                      src_length) == SDK_STATUS_QUEUED)
			return SDK_STATUS_QUEUED;
		/* defer unavailable -> decode inline below, as before */
	}

	Xil_DCacheInvalidateRange((INTPTR)(src->address + src_offset),
	                          src_length);
	if (!sdk_jpeg_decode_to_surface(
	            (const uint8_t *)(uintptr_t)(src->address + src_offset),
	            src_length, (uint8_t *)(uintptr_t)dst.address,
	            dst_width, dst_height, dst.pitch, output_format,
	            dst_x, dst_y, &image_width, &image_height,
	            &bytes_written)) {
		return complete_status(req, comp, SDK_STATUS_IO_ERROR);
	}
	scheduler_shared()->tasks_on_core0++;   /* offload-class op run inline */

	flush_surface_rect(&dst, dst_x, dst_y, dst_width, dst_height);

	write_completion(comp, req, SDK_STATUS_OK, sizeof(*result));
	memset((void *)comp->payload, 0, sizeof(comp->payload));
	result = (volatile struct SDKImageDecodeResultPayload *)comp->payload;
	put_be32(result->width, image_width);
	put_be32(result->height, image_height);
	put_be32(result->output_format, output_format);
	put_be32(result->flags, 0U);
	put_be32(result->bytes_written, bytes_written);
	return SDK_STATUS_OK;
}

static void byteswap_pcm16(uint8_t *data, uint32_t bytes)
{
	uint32_t i;
	uint8_t tmp;

	for (i = 0U; i + 1U < bytes; i += 2U) {
		tmp = data[i];
		data[i] = data[i + 1U];
		data[i + 1U] = tmp;
	}
}

/*
 * MP3 decoder contexts: one for the core-0 inline path, one for the
 * core-1 runner. Each is touched by exactly one core (the single core-1
 * worker serializes its own tasks), and the legacy register-driven FIFO
 * path in main.c owns a third -- eliminating the shared-global decoder
 * state that used to let the two paths corrupt each other.
 */
static struct mp3_decode_ctx g_mp3_inline_ctx;
static struct mp3_decode_ctx g_mp3_core1_ctx;

/*
 * One-shot MP3 decode compute, shared by the core-0 inline path and the
 * core-1 runner: cache maintenance on the data buffers lives here so it
 * runs correctly on whichever core executes. Fills a big-endian
 * SDKAudioDecodeResultPayload into result_payload on success.
 */
static uint16_t mp3_decode_compute(struct mp3_decode_ctx *ctx,
                                   const struct mp3_op_params *p,
                                   uint8_t *result_payload,
                                   uint32_t *result_len)
{
	volatile struct SDKAudioDecodeResultPayload *reply;
	uint32_t sample_rate;
	uint32_t channels;
	uint32_t bytes_written;
	uint32_t bytes_consumed;
	uint8_t *dst_ptr = (uint8_t *)(uintptr_t)p->dst_addr;

	*result_len = 0;

	Xil_DCacheInvalidateRange((INTPTR)p->src_addr, p->src_len);
	if (decode_mp3_init(ctx, (uint8_t *)(uintptr_t)p->src_addr,
	                    p->src_len) != 0)
		return SDK_STATUS_BAD_REQUEST;

	sample_rate = (uint32_t)mp3_get_hz(ctx);
	channels = (uint32_t)mp3_get_channels(ctx);
	if (sample_rate == 0U || channels == 0U)
		return SDK_STATUS_BAD_REQUEST;
	if ((p->output_hz != 0U && p->output_hz != sample_rate) ||
	    (p->output_channels != 0U && p->output_channels != channels))
		return SDK_STATUS_UNSUPPORTED;

	bytes_written = (uint32_t)decode_mp3_samples(ctx, dst_ptr,
	                                             (int)(p->dst_cap / 2U));
	bytes_consumed = (uint32_t)mp3_get_bytes_consumed(ctx);
	if (bytes_consumed > p->src_len)
		bytes_consumed = p->src_len;
	if (p->output_format == SDK_AUDIO_SAMPLE_FORMAT_S16BE)
		byteswap_pcm16(dst_ptr, bytes_written);
	Xil_DCacheFlushRange((INTPTR)dst_ptr, p->dst_cap);

	memset(result_payload, 0, sizeof(struct SDKAudioDecodeResultPayload));
	reply = (volatile struct SDKAudioDecodeResultPayload *)result_payload;
	put_be32(reply->bytes_consumed, bytes_consumed);
	put_be32(reply->bytes_written, bytes_written);
	put_be32(reply->sample_rate, sample_rate);
	put_be32(reply->channels, channels);
	put_be32(reply->sample_format, p->output_format);
	put_be32(reply->frames_written, bytes_written / (2U * channels));
	put_be32(reply->flags,
	         (bytes_consumed >= p->src_len) ?
	             SDK_AUDIO_DECODE_RESULT_END : 0U);
	*result_len = sizeof(struct SDKAudioDecodeResultPayload);
	return SDK_STATUS_OK;
}

static uint16_t handle_decode_mp3(volatile struct SDKMailboxEntry *req,
                                  volatile struct SDKMailboxEntry *comp,
                                  uint16_t payload_len)
{
	volatile struct SDKAudioDecodePayload *payload;
	struct SDKSharedBuffer *src;
	struct SDKSharedBuffer *dst;
	struct mp3_op_params p;
	uint8_t result_local[TASKQ_RESULT_PAYLOAD];
	uint32_t result_local_len = 0;
	uint16_t status;
	uint32_t src_offset;
	uint32_t src_length;
	uint32_t dst_offset;
	uint32_t dst_capacity;
	uint32_t output_hz;
	uint32_t output_channels;
	uint32_t output_format;
	uint32_t flags;

	if (payload_len < sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	payload = (volatile struct SDKAudioDecodePayload *)req->payload;
	src = find_shared_buffer(get_be32(payload->src_handle));
	dst = find_shared_buffer(get_be32(payload->dst_handle));
	if (!src || !dst)
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);

	src_offset = get_be32(payload->src_offset);
	src_length = get_be32(payload->src_length);
	dst_offset = get_be32(payload->dst_offset);
	dst_capacity = get_be32(payload->dst_capacity);
	output_hz = get_be32(payload->output_hz);
	output_channels = get_be32(payload->output_channels);
	output_format = get_be32(payload->output_format);
	flags = get_be32(payload->flags);

	if (!buffer_range_valid(src, src_offset, src_length) ||
	    !buffer_range_valid(dst, dst_offset, dst_capacity) ||
	    src_length == 0U || dst_capacity < 2U) {
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	}
	if ((flags & ~SDK_AUDIO_DECODE_FLAG_EXPECT_END) != 0U)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	if (output_format != SDK_AUDIO_SAMPLE_FORMAT_S16LE &&
	    output_format != SDK_AUDIO_SAMPLE_FORMAT_S16BE) {
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);
	}

	p.src_addr = src->address + src_offset;
	p.src_len = src_length;
	p.dst_addr = dst->address + dst_offset;
	p.dst_cap = dst_capacity & ~1UL;
	p.output_format = output_format;
	p.output_hz = output_hz;
	p.output_channels = output_channels;

	if (service_try_defer(SDK_OP_DECODE_MP3, req, &p, sizeof(p),
	                      src_length) == SDK_STATUS_QUEUED)
		return SDK_STATUS_QUEUED;
	/* defer unavailable -> decode inline below, as before */

	status = mp3_decode_compute(&g_mp3_inline_ctx, &p, result_local,
	                            &result_local_len);
	scheduler_shared()->tasks_on_core0++;   /* offload-class op run inline */
	if (status != SDK_STATUS_OK)
		return complete_status(req, comp, status);
	write_completion(comp, req, SDK_STATUS_OK, (uint16_t)result_local_len);
	memset((void *)comp->payload, 0, sizeof(comp->payload));
	memcpy((void *)comp->payload, result_local, result_local_len);
	return SDK_STATUS_OK;
}

static struct SDKAudioStream *find_audio_stream(uint32_t session)
{
	uint32_t i;

	if (session == 0U)
		return 0;
	for (i = 0; i < SDK_MAX_AUDIO_STREAMS; i++) {
		if (audio_streams[i].id == session)
			return &audio_streams[i];
	}
	return 0;
}

static struct SDKAudioStream *alloc_audio_stream(void)
{
	uint32_t i;

	for (i = 0; i < SDK_MAX_AUDIO_STREAMS; i++) {
		if (audio_streams[i].id == 0U) {
			uint32_t id = next_audio_stream_id++;

			if (next_audio_stream_id == 0U)
				next_audio_stream_id = 1U;
			memset(&audio_streams[i], 0, sizeof(audio_streams[i]));
			audio_streams[i].id = id;
			return &audio_streams[i];
		}
	}
	return 0;
}

static void free_audio_stream(struct SDKAudioStream *stream)
{
	if (stream)
		memset(stream, 0, sizeof(*stream));
}

void sdk_mailbox_poison_core1_audio_streams(void)
{
	uint32_t i;

	for (i = 0; i < SDK_MAX_AUDIO_STREAMS; i++) {
		if (audio_streams[i].id != 0U && audio_streams[i].core1_affine)
			audio_streams[i].faulted = 1U;
	}
}

/* Consumer-visible bytes in the ring (flushed and safe to read). */
static uint32_t audio_stream_pcm_used(const struct SDKAudioStream *stream)
{
	return stream->pcm_ready_total - stream->pcm_consumed_total;
}

/* Producer's free space: against written_total (conservative -- counts
 * frames not yet published by the end-of-decode flush too). */
static uint32_t audio_stream_pcm_free(const struct SDKAudioStream *stream)
{
	uint32_t used;

	if (!stream)
		return 0;
	used = stream->pcm_written_total - stream->pcm_consumed_total;
	if (used >= stream->pcm_capacity)
		return 0;
	return stream->pcm_capacity - used;
}

static void flush_audio_pcm_written(const struct SDKAudioStream *stream,
                                    uint8_t *dst, uint32_t offset,
                                    uint32_t bytes)
{
	uint32_t first;

	if (!stream || !dst || bytes == 0U || stream->pcm_capacity == 0U)
		return;
	if (offset >= stream->pcm_capacity)
		offset %= stream->pcm_capacity;
	first = stream->pcm_capacity - offset;
	if (first > bytes)
		first = bytes;
	Xil_DCacheFlushRange((INTPTR)(dst + offset), first);
	if (bytes > first)
		Xil_DCacheFlushRange((INTPTR)dst, bytes - first);
}

static void audio_stream_pcm_write(struct SDKAudioStream *stream,
                                   const uint8_t *src, uint32_t bytes)
{
	uint8_t *dst = (uint8_t *)(uintptr_t)stream->pcm_ring_addr;
	uint32_t offset = stream->pcm_written_total % stream->pcm_capacity;
	uint32_t first = stream->pcm_capacity - offset;

	if (first > bytes)
		first = bytes;
	memcpy(dst + offset, src, first);
	if (bytes > first)
		memcpy(dst, src + first, bytes - first);
	stream->pcm_written_total += bytes;
	stream->bytes_produced += bytes;
}

static void audio_stream_consume_input(struct SDKAudioStream *stream,
                                       uint32_t bytes)
{
	if (bytes >= stream->input_length) {
		stream->input_offset = 0U;
		stream->input_length = 0;
		return;
	}
	stream->input_offset += bytes;
	stream->input_length -= bytes;
}

static void audio_stream_discard_input(struct SDKAudioStream *stream)
{
	uint32_t bytes;

	if (!stream || stream->input_length == 0U)
		return;
	bytes = stream->input_length;
	audio_stream_consume_input(stream, bytes);
	stream->bytes_consumed += bytes;
}

static void audio_stream_compact_input(struct SDKAudioStream *stream)
{
	uint8_t *input;

	if (!stream || stream->input_offset == 0U || stream->input_length == 0U)
		return;
	input = (uint8_t *)(uintptr_t)stream->mp3_ring_addr;
	memmove(input, input + stream->input_offset, stream->input_length);
	stream->input_offset = 0U;
}

static uint32_t audio_stream_get_be32(const uint8_t *data)
{
	return ((uint32_t)data[0] << 24) |
	       ((uint32_t)data[1] << 16) |
	       ((uint32_t)data[2] << 8) |
	       (uint32_t)data[3];
}

static int audio_stream_check_vbr_tag(const uint8_t *frame,
                                      uint32_t frame_size,
                                      uint32_t *frame_count)
{
	uint32_t tag_off;
	uint32_t flags;

	if (!frame || !frame_count || frame_size < 12U)
		return 0;
	tag_off = 4U;
	if ((frame[1] & 1U) == 0U)
		tag_off += 2U;
	if ((frame[1] & 0x08U) != 0U)
		tag_off += ((frame[3] & 0xC0U) == 0xC0U) ? 17U : 32U;
	else
		tag_off += ((frame[3] & 0xC0U) == 0xC0U) ? 9U : 17U;
	if (tag_off > frame_size || frame_size - tag_off < 12U)
		return 0;
	if (memcmp(frame + tag_off, "Xing", 4U) != 0 &&
	    memcmp(frame + tag_off, "Info", 4U) != 0)
		return 0;
	flags = frame[tag_off + 7U];
	if ((flags & 1U) == 0U)
		return -1;
	tag_off += 8U;
	if (frame_size - tag_off < 4U)
		return 0;
	*frame_count = audio_stream_get_be32(frame + tag_off);
	if (*frame_count == 0U)
		return 0;
	return 1;
}

static int audio_stream_needs_more_input(const struct SDKAudioStream *stream)
{
	return stream &&
	       stream->input_length < SDK_AUDIO_STREAM_MIN_INPUT_BYTES &&
	       !stream->eof && !stream->drain_requested;
}

static int audio_stream_process_vbr_tag(struct SDKAudioStream *stream,
                                        const uint8_t *input,
                                        const mp3dec_frame_info_t *info)
{
	uint32_t frame_count = 0U;
	uint32_t frame_size;
	int ret;

	if (!stream || !input || !info || stream->vbr_checked ||
	    info->frame_bytes <= info->frame_offset)
		return 0;
	stream->vbr_checked = 1;
	frame_size = (uint32_t)(info->frame_bytes - info->frame_offset);
	ret = audio_stream_check_vbr_tag(
		input + info->frame_offset, frame_size, &frame_count);
	if (ret > 0)
		stream->output_frame_limit = frame_count;
	return ret != 0;
}

static uint32_t audio_stream_decode(struct SDKAudioStream *stream)
{
	const uint32_t frame_pcm_bytes =
		MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(mp3d_sample_t);
	uint32_t produced_this_call = 0U;
	uint32_t max_pcm_this_call;
	uint32_t pcm_flush_start;
	uint32_t progress = 0U;
	int drain_blocked_on_input = 0;
	uint8_t *input;
	uint8_t *pcm_dst;

	if (!stream || stream->mp3_ring_addr == 0U ||
	    stream->pcm_ring_addr == 0U)
		return 0;
	input = (uint8_t *)(uintptr_t)stream->mp3_ring_addr;
	pcm_dst = (uint8_t *)(uintptr_t)stream->pcm_ring_addr;
	if (stream->decode_complete) {
		if (stream->input_length != 0U) {
			audio_stream_discard_input(stream);
			progress = 1U;
		}
		if (audio_stream_drain_input_done(
			    stream->drain_requested, stream->decode_complete,
			    stream->input_length, 0))
			stream->drain_input_complete = 1;
		return progress;
	}
	max_pcm_this_call = stream->high_water_bytes;
	if (max_pcm_this_call == 0U ||
	    max_pcm_this_call > stream->pcm_capacity)
		max_pcm_this_call = stream->pcm_capacity;
	if (max_pcm_this_call < frame_pcm_bytes)
		max_pcm_this_call = frame_pcm_bytes;
	pcm_flush_start = stream->pcm_written_total % stream->pcm_capacity;

	while (stream->input_length > 0U &&
	       audio_stream_pcm_free(stream) >= frame_pcm_bytes) {
		mp3dec_frame_info_t info;
		int decode_input_size = (int)stream->input_length;
		int samples;
		uint32_t consumed;
		uint32_t bytes;

		if (!audio_stream_decode_may_run(
			    stream->input_length,
			    SDK_AUDIO_STREAM_MIN_INPUT_BYTES,
			    stream->eof, stream->drain_requested))
			break;
		if (produced_this_call != 0U &&
		    (max_pcm_this_call - produced_this_call) <
		        frame_pcm_bytes) {
			break;
		}
		if (stream->drain_requested && !stream->eof &&
		    !mp3_probe_complete_frame(
			    &stream->decoder, input + stream->input_offset,
			    (int)stream->input_length, &decode_input_size)) {
			drain_blocked_on_input = 1;
			break;
		}
		memset(&info, 0, sizeof(info));
		samples = mp3dec_decode_frame(
			&stream->decoder,
			input + stream->input_offset,
			decode_input_size,
			stream->scratch, &info);
		consumed = (uint32_t)info.frame_bytes;
		if (consumed == 0U) {
			drain_blocked_on_input = 1;
			break;
		}
		if (stream->eof && stream->frames_decoded != 0U &&
		    info.frame_offset != 0) {
			audio_stream_discard_input(stream);
			progress = 1U;
			break;
		}

		if (samples > 0) {
			if (stream->sample_rate == 0U) {
				stream->sample_rate = (uint32_t)info.hz;
				stream->channels = (uint32_t)info.channels;
			}
			if ((uint32_t)info.hz != stream->sample_rate ||
			    (uint32_t)info.channels != stream->channels) {
				break;
			}
			if (audio_stream_process_vbr_tag(
				    stream, input + stream->input_offset,
				    &info)) {
				bytes = 0U;
			} else if (stream->output_frame_limit != 0U &&
			           stream->frames_decoded >=
			                   stream->output_frame_limit) {
				stream->decode_complete = 1;
				bytes = 0U;
			} else {
				bytes = (uint32_t)samples *
				        (uint32_t)info.channels *
				        sizeof(mp3d_sample_t);
			}
			if (bytes != 0U) {
				if (stream->sample_format ==
				    SDK_AUDIO_SAMPLE_FORMAT_S16BE)
					byteswap_pcm16((uint8_t *)stream->scratch,
					               bytes);
				audio_stream_pcm_write(
					stream, (uint8_t *)stream->scratch, bytes);
				stream->frames_decoded++;
				produced_this_call += bytes;
				progress = 1U;
				if (stream->output_frame_limit != 0U &&
				    stream->frames_decoded >=
				            stream->output_frame_limit)
					stream->decode_complete = 1;
			}
		}

		audio_stream_consume_input(stream, consumed);
		stream->bytes_consumed += consumed;
		progress = 1U;
		if (stream->decode_complete) {
			audio_stream_discard_input(stream);
			break;
		}
	}

	if (produced_this_call != 0U) {
		flush_audio_pcm_written(stream, pcm_dst, pcm_flush_start,
		                        produced_this_call);
		/* Publish only after the flush: Xil_DCacheFlushRange ends in
		 * a DSB, so consumers that see the new ready total also see
		 * the PCM bytes in DRAM. */
		stream->pcm_ready_total = stream->pcm_written_total;
	}
	if (audio_stream_drain_input_done(
		    stream->drain_requested, stream->decode_complete,
		    stream->input_length, drain_blocked_on_input))
		stream->drain_input_complete = 1;
	return progress;
}

static uint32_t audio_stream_state(const struct SDKAudioStream *stream)
{
	if (!stream)
		return SDK_AUDIO_STREAM_STATE_ERROR;
	if (stream->eof && stream->input_length == 0U &&
	    audio_stream_pcm_used(stream) == 0U) {
		/* Source exhausted, but a bound pump may still be playing the
		 * staged tail out of the TX ring -- report STREAMING until it
		 * drains so DONE does not race ahead of the DMA. */
		if (stream->pump_tail_pending)
			return SDK_AUDIO_STREAM_STATE_STREAMING;
		return SDK_AUDIO_STREAM_STATE_DONE;
	}
	if (audio_stream_transient_drained(
	        stream->drain_requested, stream->drain_input_complete,
	        audio_stream_pcm_used(stream), stream->pump_tail_pending))
		return SDK_AUDIO_STREAM_STATE_NEED_INPUT;
	if (audio_stream_needs_more_input(stream))
		return SDK_AUDIO_STREAM_STATE_NEED_INPUT;
	if (stream->input_length == 0U &&
	    audio_stream_pcm_used(stream) < stream->pcm_capacity)
		return SDK_AUDIO_STREAM_STATE_NEED_INPUT;
	return SDK_AUDIO_STREAM_STATE_STREAMING;
}

static uint32_t audio_stream_result_flags(const struct SDKAudioStream *stream)
{
	uint32_t flags = 0U;

	if (!stream)
		return 0;
	if (audio_stream_pcm_used(stream) != 0U || stream->pump_tail_pending)
		flags |= SDK_AUDIO_STREAM_RESULT_PCM_READY;
	if (audio_stream_needs_more_input(stream))
		flags |= SDK_AUDIO_STREAM_RESULT_NEED_INPUT;
	if (stream->eof && stream->input_length == 0U &&
	    audio_stream_pcm_used(stream) == 0U && !stream->pump_tail_pending)
		flags |= SDK_AUDIO_STREAM_RESULT_DONE;
	if (stream->backpressure)
		flags |= SDK_AUDIO_STREAM_RESULT_BACKPRESSURE;
	if (audio_stream_transient_drained(
	        stream->drain_requested, stream->drain_input_complete,
	        audio_stream_pcm_used(stream), stream->pump_tail_pending))
		flags |= SDK_AUDIO_STREAM_RESULT_DRAINED;
	return flags;
}

/* Fill a big-endian SDKAudioStreamResultPayload; shared by the inline
 * completion below and the core-1 runner's deferred result. */
static void audio_stream_fill_result(uint8_t *dst,
                                     const struct SDKAudioStream *stream)
{
	struct SDKAudioStreamResultPayload *result =
	    (struct SDKAudioStreamResultPayload *)dst;

	memset(dst, 0, sizeof(*result));
	put_be32(result->session, stream->id);
	put_be32(result->state, audio_stream_state(stream));
	put_be32(result->sample_rate, stream->sample_rate);
	put_be32(result->channels, stream->channels);
	put_be32(result->sample_format, stream->sample_format);
	put_be32(result->mp3_read, stream->bytes_consumed);
	put_be32(result->pcm_write,
	         stream->pcm_ready_total % stream->pcm_capacity);
	put_be32(result->pcm_read,
	         stream->pcm_consumed_total % stream->pcm_capacity);
	put_be32(result->frames_decoded, stream->frames_decoded);
	put_be32(result->bytes_consumed, stream->bytes_consumed);
	put_be32(result->bytes_produced, stream->bytes_produced);
	put_be32(result->flags, audio_stream_result_flags(stream));
}

static uint16_t complete_audio_stream_result(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp,
	uint16_t status,
	const struct SDKAudioStream *stream)
{
	uint8_t local[sizeof(struct SDKAudioStreamResultPayload)];

	if (status != SDK_STATUS_OK)
		return complete_status(req, comp, status);

	audio_stream_fill_result(local, stream);
	write_completion(comp, req, SDK_STATUS_OK, sizeof(local));
	memset((void *)comp->payload, 0, sizeof(comp->payload));
	memcpy((void *)comp->payload, local, sizeof(local));
	return SDK_STATUS_OK;
}

/*
 * Feed/read compute, shared by the core-0 inline paths and the core-1
 * runner. Everything here operates on the coherent stream struct, the
 * stream's cached ring addresses, and the resolved feed source -- no
 * registry or video state. Cache maintenance (source invalidate, PCM
 * flush inside audio_stream_decode) runs on whichever core executes.
 */
static void audio_stream_feed_compute(struct SDKAudioStream *stream,
                                      uint32_t src_addr, uint32_t src_length,
                                      uint32_t flags)
{
	uint8_t *dst;

	if (src_length != 0U) {
		if (src_length > stream->mp3_capacity - stream->input_length) {
			/* Result reflects backpressure; EOF/decode skipped,
			 * exactly as the pre-scheduler handler behaved. */
			stream->backpressure = 1;
			return;
		}
		if (stream->input_offset + stream->input_length + src_length >
		    stream->mp3_capacity) {
			audio_stream_compact_input(stream);
		}
		Xil_DCacheInvalidateRange((INTPTR)src_addr, src_length);
		dst = (uint8_t *)(uintptr_t)stream->mp3_ring_addr;
		memcpy(dst + stream->input_offset + stream->input_length,
		       (const void *)(uintptr_t)src_addr, src_length);
		stream->input_length += src_length;
		stream->backpressure = 0;
		stream->drain_requested = 0;
		stream->drain_input_complete = 0;
	}
	if ((flags & SDK_AUDIO_STREAM_FEED_EOF) != 0U) {
		stream->eof = 1;
		stream->drain_requested = 0;
		stream->drain_input_complete = 0;
	} else if ((flags & SDK_AUDIO_STREAM_FEED_DRAIN) != 0U) {
		stream->drain_requested = 1;
		stream->drain_input_complete = 0;
	}
	audio_stream_decode(stream);
}

static void audio_stream_read_compute(struct SDKAudioStream *stream,
                                      uint32_t pcm_read)
{
	stream->pcm_consumed_total += pcm_read;
	audio_stream_decode(stream);
}

/*
 * ---- AX playback pump (MHI modernization) ----
 * One audio-stream session at a time may be bound to the AX output
 * (SDK_OP_AUDIO_STREAM_PLAY). The TX-fill half runs from the audio
 * formatter's period INTERRUPT (sdk_mailbox_audio_playback_pump_isr,
 * called by isr_audio every 20 ms) so heavy main-loop passes -- RTG
 * blits, image-decode dispatch, network bursts -- can no longer let
 * the DMA overrun the ~120 ms TX frontier and glitch the audio. Only
 * the core-1 refill kick stays in the main loop (it enqueues scheduler
 * tasks, which the ISR must not). The ISR path is integer-only: the
 * standalone BSP does not save VFP state across interrupts, so non-48k
 * sources go through the shared qualified converter kernel
 * (audio_convert.c) with exact-rational, drift-free phase; mono sources
 * are duplicated to stereo. Underrun, a faulted session or an unusable
 * stream geometry produce silence. The ISR pump is the CONSUMER of a
 * bound session -- the single writer of pcm_consumed_total -- so
 * AUDIO_STREAM_READ is rejected while bound.
 */
#define AUDIO_PUMP_PERIOD_BYTES  AUDIO_BYTES_PER_PERIOD
#define AUDIO_PUMP_RING_BYTES    AUDIO_TX_BUFFER_SIZE
#define AUDIO_PUMP_TARGET_AHEAD \
	(AUDIO_PUMP_RING_BYTES - 2U * AUDIO_PUMP_PERIOD_BYTES)
#define AUDIO_PUMP_SOURCE_NONE   0U
#define AUDIO_PUMP_SOURCE_STREAM 1U
#define AUDIO_PUMP_SOURCE_MEDIA  2U

struct SDKAudioPumpSource {
	uint8_t *ring;
	uint32_t capacity;
	uint64_t produced_bytes;
	uint64_t staged_bytes;
	uint32_t sample_rate;
	uint32_t channels;
	uint32_t sample_format;
	uint8_t done;
	uint8_t faulted;
};

static struct {
	uint32_t session;         /* 0 = unbound */
	uint32_t source_kind;
	uint32_t paused;
	uint32_t fill_offset;     /* next TX-ring byte to fill, period-aligned */
	uint32_t last_dma_offset; /* active DMA period at the last ISR */
	uint32_t period_source_bytes[AUDIO_NUM_PERIODS];
	uint32_t refill_pending;  /* internal core-1 refill task in flight */
	uint32_t refill_session;  /* session that refill targets (valid while
	                             refill_pending; survives an unbind) */
	uint32_t silence_run;     /* consecutive pump ISR periods (one DMA
	                             period each) that staged only silence;
	                             a full ring means the DMA played the tail */
} g_audio_playback;

static int16_t g_pump_src[AUDIO_PUMP_PERIOD_BYTES / 2];
static int16_t g_pump_stereo[AUDIO_PUMP_PERIOD_BYTES / 2];

/*
 * Private converter instance for the pump (stream and media bindings).
 * The qualified kernel is integer-only and ISR-safe, so the pump runs
 * the same code the host suite gates; binding start resets it, and
 * silence periods skip the kernel entirely so the rational phase
 * freezes during underrun and recovery continues phase-continuously.
 */
static struct zz_audio_convert g_pump_convert;
static uint32_t g_pump_convert_rate;

static void pump_resample_reset(void)
{
	g_pump_convert_rate = 0U;
	zz_audio_convert_reset(&g_pump_convert);
}

static void pump_convert(const int16_t *pcm, int16_t *slot,
                         uint32_t rate, uint32_t src_frames)
{
	if (rate != g_pump_convert_rate) {
		g_pump_convert_rate = rate;
		zz_audio_convert_init(&g_pump_convert, rate, 48000U);
	}
	zz_audio_convert_stream(&g_pump_convert, pcm, slot,
	                        (uint16_t)src_frames,
	                        AUDIO_PUMP_PERIOD_BYTES / 4);
}

static int audio_pump_source_snapshot(struct SDKAudioPumpSource *source)
{
	memset(source, 0, sizeof(*source));
	if (g_audio_playback.source_kind == AUDIO_PUMP_SOURCE_STREAM) {
		struct SDKAudioStream *stream =
			find_audio_stream(g_audio_playback.session);

		if (!stream)
			return 0;
		source->ring =
			(uint8_t *)(uintptr_t)stream->pcm_ring_addr;
		source->capacity = stream->pcm_capacity;
		source->produced_bytes = stream->pcm_ready_total;
		source->staged_bytes = stream->pcm_consumed_total;
		source->sample_rate = stream->sample_rate;
		source->channels = stream->channels;
		source->sample_format = stream->sample_format;
		source->done = audio_stream_source_tail_ready(
			stream->eof, stream->input_length,
			stream->drain_requested,
			stream->drain_input_complete);
		source->faulted = stream->faulted != 0U;
		return 1;
	}
	if (g_audio_playback.source_kind == AUDIO_PUMP_SOURCE_MEDIA) {
		struct SDKMediaAudioSource media_source;

		if (!sdk_media_session_audio_source(
			    g_audio_playback.session, &media_source))
			return 0;
		source->ring = media_source.ring;
		source->capacity = media_source.capacity;
		source->produced_bytes = media_source.produced_bytes;
		source->staged_bytes = media_source.staged_bytes;
		source->sample_rate = media_source.sample_rate;
		source->channels = media_source.channels;
		source->sample_format = media_source.sample_format;
		source->done = media_source.done;
		return 1;
	}
	return 0;
}

static int audio_pump_source_stage(uint32_t bytes)
{
	if (g_audio_playback.source_kind == AUDIO_PUMP_SOURCE_STREAM) {
		struct SDKAudioStream *stream =
			find_audio_stream(g_audio_playback.session);

		if (!stream ||
		    bytes > stream->pcm_ready_total -
		            stream->pcm_consumed_total)
			return 0;
		stream->pcm_consumed_total += bytes;
		return 1;
	}
	if (g_audio_playback.source_kind == AUDIO_PUMP_SOURCE_MEDIA)
		return sdk_media_session_audio_stage(
			g_audio_playback.session, bytes);
	return 0;
}

static void audio_pump_source_retire(uint32_t bytes)
{
	if (bytes != 0U &&
	    g_audio_playback.source_kind == AUDIO_PUMP_SOURCE_MEDIA)
		(void)sdk_media_session_audio_retire(
			g_audio_playback.session, bytes);
}

static void audio_pump_source_underrun(void)
{
	if (g_audio_playback.source_kind == AUDIO_PUMP_SOURCE_MEDIA)
		sdk_media_session_audio_underrun(
			g_audio_playback.session);
}

/* Returns the number of source PCM bytes staged into this DMA period, or zero
 * when the slot contains silence (temporary shortage, fault, pause, or drained
 * end-of-stream). A zero-byte slot stays at the retryable fill frontier; it is
 * not committed as future silence. Per-period metadata converts the staging
 * cursor into an actual DMA-retirement clock. */
static uint32_t audio_pump_fill_period(
	const struct SDKAudioPumpSource *source, uint8_t *slot)
{
	uint32_t rate;
	uint32_t channels;
	uint32_t src_frames;
	uint32_t src_bytes;
	uint32_t pull;
	uint32_t offset;
	uint32_t first;
	uint64_t available;
	uint8_t *ring;
	int16_t *pcm;
	uint32_t i;

	if (!source || source->faulted || !source->ring ||
	    source->capacity == 0U ||
	    source->produced_bytes < source->staged_bytes)
		goto silence;
	rate = source->sample_rate;
	channels = source->channels;
	if (rate == 0U || channels == 0U || channels > 2U)
		goto silence;
	if (source->sample_format != SDK_AUDIO_SAMPLE_FORMAT_S16LE &&
	    source->sample_format != SDK_AUDIO_SAMPLE_FORMAT_S16BE)
		goto silence;
	src_frames = rate / 50U;
	if (src_frames == 0U || src_frames > (AUDIO_PUMP_PERIOD_BYTES / 4U))
		goto silence;
	src_bytes = src_frames * channels * 2U;
	available = source->produced_bytes - source->staged_bytes;
	if (available >= src_bytes) {
		pull = src_bytes;
	} else if (!source->done || available == 0U) {
		goto silence;
	} else {
		pull = (uint32_t)available;
	}
	/* else: true end of stream (EOF fed, input fully consumed) with a
	 * final PCM tail shorter than one 20 ms period -- MP3 frames owe
	 * no alignment to rate/50. Drain it zero-padded; refusing partial
	 * pulls would pin used above zero and the stream could never
	 * report DONE. */

	/* Pull the source from the PCM ring. The decode side flushed these
	 * bytes before publishing pcm_ready_total, so a reader-side
	 * invalidate makes them visible on this core. */
	ring = source->ring;
	offset = audio_playback_source_offset(
		source->staged_bytes, source->capacity);
	first = source->capacity - offset;
	if (first > pull)
		first = pull;
	Xil_DCacheInvalidateRange((INTPTR)(ring + offset), first);
	memcpy(g_pump_src, ring + offset, first);
	if (pull > first) {
		Xil_DCacheInvalidateRange((INTPTR)ring, pull - first);
		memcpy((uint8_t *)g_pump_src + first, ring, pull - first);
	}
	if (pull < src_bytes)
		memset((uint8_t *)g_pump_src + pull, 0, src_bytes - pull);
	if (!audio_pump_source_stage(pull))
		goto silence;

	if (source->sample_format == SDK_AUDIO_SAMPLE_FORMAT_S16BE) {
		uint8_t *bytes = (uint8_t *)g_pump_src;

		for (i = 0U; i < src_bytes; i += 2U) {
			uint8_t high = bytes[i];

			bytes[i] = bytes[i + 1U];
			bytes[i + 1U] = high;
		}
	}

	pcm = g_pump_src;
	if (channels == 1U) {
		for (i = 0; i < src_frames; i++) {
			g_pump_stereo[2U * i] = g_pump_src[i];
			g_pump_stereo[2U * i + 1U] = g_pump_src[i];
		}
		pcm = g_pump_stereo;
	}
	if (rate == 48000U) {
		memcpy(slot, pcm, AUDIO_PUMP_PERIOD_BYTES);
	} else {
		pump_convert(pcm, (int16_t *)slot, rate, src_frames);
	}
	return pull;
silence:
	memset(slot, 0, AUDIO_PUMP_PERIOD_BYTES);
	return 0;
}

/* TX-fill half: called from isr_audio (audio-formatter period IRQ,
 * every 20 ms) so main-loop load cannot starve the TX frontier.
 * Integer-only; no scheduler/taskq/printf access from here. */
void sdk_mailbox_audio_playback_pump_isr(void)
{
	struct SDKAudioPumpSource source;
	uint8_t *tx = (uint8_t *)AUDIO_TX_BUFFER_ADDRESS;
	uint32_t pos_period;
	uint32_t ahead;
	uint32_t guard;
	uint32_t retired;
	uint32_t ring_periods = AUDIO_PUMP_RING_BYTES / AUDIO_PUMP_PERIOD_BYTES;
	int staged_real = 0;

	if (g_audio_playback.session == 0U ||
	    g_audio_playback.paused)
		return;

	pos_period = (audio_get_dma_transfer_count() % AUDIO_PUMP_RING_BYTES);
	pos_period -= pos_period % AUDIO_PUMP_PERIOD_BYTES;

	/* Retire every period the DMA advanced through since the preceding IRQ.
	 * Each slot records exactly how many source bytes were staged into it;
	 * silence contributes zero. This is the playback clock -- not the
	 * decoder acknowledgement or the TX-fill frontier. */
	retired = audio_playback_retire_to(
		&g_audio_playback.last_dma_offset, pos_period,
		AUDIO_PUMP_PERIOD_BYTES, AUDIO_PUMP_RING_BYTES,
		g_audio_playback.period_source_bytes, AUDIO_NUM_PERIODS);
	audio_pump_source_retire(retired);

	if (!audio_pump_source_snapshot(&source)) {
		g_audio_playback.session = 0U;
		g_audio_playback.source_kind = AUDIO_PUMP_SOURCE_NONE;
		return;
	}

	/* If the DMA caught up with (or passed) the fill frontier, it has
	 * actually reached an unfilled silence slot. Count that played
	 * underrun (not speculative attempts to fill future periods), then
	 * restart one period ahead. Circular distance: bias by the ring size
	 * BEFORE the modulo -- a plain u32 (fill - pos) % RING is wrong on
	 * wrap because 2^32 is not a multiple of the 30720-byte ring. */
	if (audio_playback_frontier_needs_rebase(
	        g_audio_playback.fill_offset, pos_period,
	        AUDIO_PUMP_TARGET_AHEAD, AUDIO_PUMP_RING_BYTES)) {
		if (!source.done && !source.faulted)
			audio_pump_source_underrun();
		g_audio_playback.fill_offset =
		    (pos_period + AUDIO_PUMP_PERIOD_BYTES) %
		    AUDIO_PUMP_RING_BYTES;
	}

	guard = AUDIO_PUMP_RING_BYTES / AUDIO_PUMP_PERIOD_BYTES;
	while (guard--) {
		uint32_t index;
		uint32_t staged;
		uint32_t next_fill;

		ahead = audio_playback_ring_distance(
			g_audio_playback.fill_offset, pos_period,
			AUDIO_PUMP_RING_BYTES);
		/* Stop AT the target, never past it: the frontier must stay
		 * inside [PERIOD, TARGET_AHEAD] so the caught-up reset above
		 * only fires on a genuine DMA overrun. Filling one period past
		 * the target (the '>' variant of this check) made every pump
		 * pass look caught-up, re-basing the frontier and re-filling
		 * the ring at main-loop speed -- consuming PCM thousands of
		 * times faster than playback, which defeated FEED
		 * backpressure entirely. */
		if (ahead >= AUDIO_PUMP_TARGET_AHEAD)
			break;   /* frontier far enough ahead */
		index = g_audio_playback.fill_offset / AUDIO_PUMP_PERIOD_BYTES;
		staged = audio_pump_fill_period(
			&source, tx + g_audio_playback.fill_offset);
		g_audio_playback.period_source_bytes[index] = staged;
		if (staged != 0U)
			staged_real = 1;
		/* The TX ring is plain cacheable DDR (no TLB override) and
		 * the audio formatter DMA does not snoop: push the period to
		 * DRAM before the frontier advances over it. ~120 lines,
		 * microseconds, once per 20 ms. */
		Xil_DCacheFlushRange(
		    (INTPTR)(tx + g_audio_playback.fill_offset),
		    AUDIO_PUMP_PERIOD_BYTES);
		next_fill = audio_playback_frontier_after_fill(
			g_audio_playback.fill_offset, staged,
			AUDIO_PUMP_PERIOD_BYTES, AUDIO_PUMP_RING_BYTES);
		if (next_fill == g_audio_playback.fill_offset)
			break;
		g_audio_playback.fill_offset = next_fill;
		/* Refresh the published source cursor before filling another
		 * period in this same IRQ. */
		if (!audio_pump_source_snapshot(&source))
			memset(&source, 0, sizeof(source));
	}

	/* Play-out tail tracking. This ISR fires once per formatter period,
	 * so each call is one DMA period elapsed. While real PCM is flowing,
	 * pump_tail_pending stays armed; once the source is exhausted the loop
	 * only stages silence, and after a whole ring of silence periods the
	 * DMA has played the last real audio out of the TX ring. Only then may
	 * end-of-stream drop (see audio_stream_result_flags / audio_stream_
	 * state), so a client that stops on DONE does not truncate the tail. */
	if (g_audio_playback.source_kind == AUDIO_PUMP_SOURCE_STREAM) {
		struct SDKAudioStream *stream =
			find_audio_stream(g_audio_playback.session);

		if (!stream)
			return;
		if (staged_real) {
			g_audio_playback.silence_run = 0U;
			stream->pump_tail_pending = 1U;
		} else if (stream->pump_tail_pending) {
			if (g_audio_playback.silence_run < ring_periods)
				g_audio_playback.silence_run++;
			if (g_audio_playback.silence_run >= ring_periods)
				stream->pump_tail_pending = 0U;
		}
	}
}

void sdk_mailbox_audio_playback_pump(void)
{
	struct SDKAudioStream *stream;

	if (g_audio_playback.session == 0U ||
	    g_audio_playback.source_kind != AUDIO_PUMP_SOURCE_STREAM ||
	    g_audio_playback.paused)
		return;
	stream = find_audio_stream(g_audio_playback.session);
	if (!stream) {
		g_audio_playback.session = 0U;   /* closed under us */
		return;
	}

	/* Keep the decoder ahead of the pump. Session FEEDs drive decode, but
	 * once the client has fed everything it just waits -- so when the PCM
	 * ring is at/below the low-water mark and undecoded input remains,
	 * kick a refill: a FEED with no new bytes. For core-1-affine streams
	 * it goes through the queue as an INTERNAL task (request_id 0 -> no
	 * client completion is posted); a core-0-affine stream (begun while
	 * the scheduler was down) refills inline, matching the legacy CPU
	 * profile of that degraded mode. */
	if (audio_stream_refill_may_run(
	        stream->input_length, stream->faulted,
	        audio_stream_needs_more_input(stream),
	        stream->drain_input_complete, audio_stream_pcm_used(stream),
	        stream->low_water_bytes)) {
		/* needs_more_input guard: a refill FEED would no-op below the
		 * decoder's minimum-input gate, and the pump runs every main
		 * loop pass -- without the guard the sub-minimum tail of a
		 * stream turns into a core-1 task storm. */
		if (stream->core1_affine && scheduler_core1_available()) {
			if (!g_audio_playback.refill_pending) {
				struct audio_feed_op_params p;
				taskq_shared_t *sh = scheduler_shared();
				int slot;

				p.session = g_audio_playback.session;
				p.src_addr = 0U;
				p.src_len = 0U;
				p.flags = 0U;
				slot = taskq_enqueue(&sh->queue,
				                     SDK_OP_AUDIO_STREAM_FEED,
				                     TASK_LONG, 0u, 0u, 0u, 0u,
				                     0u, 0u, &p, sizeof(p));
				if (slot >= 0) {
					g_task_generation[slot] =
					    sdk_mailbox_generation;
					__asm__ __volatile__("dsb ish\n\tsev"
					                     ::: "memory");
					g_audio_playback.refill_pending = 1U;
					g_audio_playback.refill_session =
					    g_audio_playback.session;
				}
			}
		} else if (!stream->core1_affine) {
			audio_stream_feed_compute(stream, 0U, 0U, 0U);
		}
	}
}

static void audio_playback_start(uint32_t source_kind, uint32_t session)
{
	uint32_t pos;

	/* Gate the ISR while every cursor and period tag is rebuilt. */
	g_audio_playback.session = 0U;
	__asm__ __volatile__("" ::: "memory");
	audio_set_tx_buffer((uint8_t *)AUDIO_TX_BUFFER_ADDRESS);
	if (audio_get_inited_tx_buffer() !=
	    (uint8_t *)AUDIO_TX_BUFFER_ADDRESS)
		audio_init_i2s();
	pump_resample_reset();
	pos = audio_get_dma_transfer_count() % AUDIO_PUMP_RING_BYTES;
	pos -= pos % AUDIO_PUMP_PERIOD_BYTES;
	g_audio_playback.fill_offset =
		(pos + AUDIO_PUMP_PERIOD_BYTES) % AUDIO_PUMP_RING_BYTES;
	g_audio_playback.last_dma_offset = pos;
	audio_playback_clear_periods(
		g_audio_playback.period_source_bytes, AUDIO_NUM_PERIODS);
	g_audio_playback.silence_run = 0U;
	g_audio_playback.source_kind = source_kind;
	g_audio_playback.paused = 0U;
	/* Publish the session LAST: the next audio IRQ may now stage PCM. */
	__asm__ __volatile__("" ::: "memory");
	g_audio_playback.session = session;
}

static void audio_playback_stop(void)
{
	g_audio_playback.session = 0U;
	g_audio_playback.source_kind = AUDIO_PUMP_SOURCE_NONE;
	g_audio_playback.paused = 0U;
	audio_playback_clear_periods(
		g_audio_playback.period_source_bytes, AUDIO_NUM_PERIODS);
	g_audio_playback.silence_run = 0U;
	audio_silence();
}

int sdk_mailbox_audio_playback_active(void)
{
	return g_audio_playback.session != 0U;
}

static uint16_t handle_audio_stream_play(volatile struct SDKMailboxEntry *req,
                                         volatile struct SDKMailboxEntry *comp,
                                         uint16_t payload_len)
{
	volatile struct SDKAudioStreamClosePayload *payload;
	struct SDKAudioStream *stream;
	uint32_t session;
	uint32_t flags;

	if (payload_len < sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	payload = (volatile struct SDKAudioStreamClosePayload *)req->payload;
	session = get_be32(payload->session);
	flags = get_be32(payload->flags);
	if (flags != 0U)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	stream = find_audio_stream(session);
	if (!stream)
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	if (stream->faulted)
		return complete_status(req, comp, SDK_STATUS_IO_ERROR);
	if (g_audio_playback.session == session &&
	    g_audio_playback.source_kind == AUDIO_PUMP_SOURCE_STREAM) {
		/* Already playing this session: idempotent, no re-init (the
		 * client may use this as a cheap status probe). */
		return complete_audio_stream_result(req, comp, SDK_STATUS_OK,
		                                    stream);
	}
	if (stream->sample_rate == 0U)   /* client must prebuffer first */
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	/* The AX DMA consumes native little-endian samples and the pump
	 * copies the PCM ring verbatim; an S16BE session (the READ-path
	 * byte order) would play byte-swapped noise. */
	if (stream->sample_format != SDK_AUDIO_SAMPLE_FORMAT_S16LE)
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);
	if (g_audio_playback.session != 0U)
		return complete_status(req, comp, SDK_STATUS_BUSY);
	/* A legacy/AHI client owning the output is just as busy as another
	 * SDK session: binding now would repoint the formatter DMA away
	 * from the buffer that client configured (AP_TX_BUF_OFFS) and
	 * steal its playback. On the Amiga side MHI and AHI already
	 * exclude each other via the interrupt-server ownership token;
	 * this enforces the same exclusion for raw SDK clients. */
	if (audio_legacy_output_active())
		return complete_status(req, comp, SDK_STATUS_BUSY);

	/* Deterministic output target: the standard TX ring. An AHI session
	 * repoints the FORMATTER DMA at its own buffer (AP_TX_BUF_OFFS +
	 * re-init) and closing AHI does not restore it -- and moving the
	 * CPU-side pointer alone moves nothing, the DMA keeps reading the
	 * buffer captured at the last audio_init_i2s(). Without the
	 * re-init below, a bind after any AHI session plays silence: the
	 * pump fills the default ring while the DMA reads AHI's dead one.
	 * Safe here: PLAY runs in the main loop, and the ISR pump stays
	 * gated off until the session publishes below. */
	stream->pump_tail_pending = 0U;
	audio_playback_start(AUDIO_PUMP_SOURCE_STREAM, session);
	return complete_audio_stream_result(req, comp, SDK_STATUS_OK, stream);
}

static uint16_t handle_audio_stream_stop(volatile struct SDKMailboxEntry *req,
                                         volatile struct SDKMailboxEntry *comp,
                                         uint16_t payload_len)
{
	volatile struct SDKAudioStreamClosePayload *payload;
	struct SDKAudioStream *stream;
	uint32_t session;
	uint32_t flags;

	if (payload_len < sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	payload = (volatile struct SDKAudioStreamClosePayload *)req->payload;
	session = get_be32(payload->session);
	flags = get_be32(payload->flags);
	if (flags != 0U)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	stream = find_audio_stream(session);
	if (!stream)
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	if (g_audio_playback.session == session &&
	    g_audio_playback.source_kind == AUDIO_PUMP_SOURCE_STREAM) {
		audio_playback_stop();
		/* audio_silence() wiped the TX ring: no tail is left to play. */
		stream->pump_tail_pending = 0U;
		g_audio_playback.silence_run = 0U;
	}
	/* Idempotent: stopping an unbound session is OK (MHI pause/stop). */
	return complete_audio_stream_result(req, comp, SDK_STATUS_OK, stream);
}

static uint16_t handle_audio_stream_begin(volatile struct SDKMailboxEntry *req,
                                          volatile struct SDKMailboxEntry *comp,
                                          uint16_t payload_len)
{
	volatile struct SDKAudioStreamBeginPayload *payload;
	struct SDKSharedBuffer *mp3_ring;
	struct SDKSharedBuffer *pcm_ring;
	struct SDKAudioStream *stream;
	uint32_t mp3_capacity;
	uint32_t pcm_capacity;
	uint32_t output_hz;
	uint32_t output_channels;
	uint32_t output_format;
	uint32_t low_water_bytes;
	uint32_t high_water_bytes;
	uint32_t flags;

	if (payload_len < sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	payload = (volatile struct SDKAudioStreamBeginPayload *)req->payload;
	mp3_ring = find_shared_buffer(get_be32(payload->mp3_ring_handle));
	pcm_ring = find_shared_buffer(get_be32(payload->pcm_ring_handle));
	if (!mp3_ring || !pcm_ring)
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);

	mp3_capacity = get_be32(payload->mp3_ring_capacity);
	pcm_capacity = get_be32(payload->pcm_ring_capacity);
	output_hz = get_be32(payload->output_hz);
	output_channels = get_be32(payload->output_channels);
	output_format = get_be32(payload->output_format);
	low_water_bytes = get_be32(payload->low_water_bytes);
	high_water_bytes = get_be32(payload->high_water_bytes);
	flags = get_be32(payload->flags);
	if (flags != 0U || output_hz != 0U || output_channels != 0U)
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);
	if (output_format != SDK_AUDIO_SAMPLE_FORMAT_S16LE &&
	    output_format != SDK_AUDIO_SAMPLE_FORMAT_S16BE)
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);
	if (mp3_capacity == 0U || pcm_capacity <
	    (MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(mp3d_sample_t)) ||
	    mp3_capacity > mp3_ring->length ||
	    pcm_capacity > pcm_ring->length ||
	    /* Both water marks are PCM-ring thresholds: low_water is the
	     * playback pump's refill trigger, high_water caps decode
	     * output per pass. (Validating low_water against the mp3 ring
	     * here rejected any BEGIN whose input ring was smaller than
	     * the PCM refill mark.) */
	    low_water_bytes >= pcm_capacity ||
	    high_water_bytes >= pcm_capacity) {
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	}

	stream = alloc_audio_stream();
	if (!stream)
		return complete_status(req, comp, SDK_STATUS_NO_MEMORY);
	stream->mp3_ring = mp3_ring;
	stream->pcm_ring = pcm_ring;
	/* Resolved once here: core-1 feeds/reads use these instead of the
	 * core-0-only shared-buffer registry entries above. */
	stream->mp3_ring_addr = mp3_ring->address;
	stream->pcm_ring_addr = pcm_ring->address;
	stream->mp3_capacity = mp3_capacity;
	stream->pcm_capacity = pcm_capacity & ~1UL;
	stream->low_water_bytes = low_water_bytes;
	stream->high_water_bytes = high_water_bytes & ~1UL;
	stream->sample_format = output_format;
	/* Affinity is fixed for the stream's whole life: the mp3 staging
	 * ring becomes cache-owned by whichever core runs the decoder. */
	stream->core1_affine = scheduler_core1_available() ? 1U : 0U;
	mp3dec_init(&stream->decoder);
	stream->initialized = 1;
	return complete_audio_stream_result(req, comp, SDK_STATUS_OK, stream);
}

static uint16_t handle_audio_stream_feed(volatile struct SDKMailboxEntry *req,
                                         volatile struct SDKMailboxEntry *comp,
                                         uint16_t payload_len)
{
	volatile struct SDKAudioStreamFeedPayload *payload;
	struct SDKAudioStream *stream;
	struct SDKSharedBuffer *src;
	uint32_t session;
	uint32_t src_offset;
	uint32_t src_length;
	uint32_t flags;

	if (payload_len < sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	payload = (volatile struct SDKAudioStreamFeedPayload *)req->payload;
	session = get_be32(payload->session);
	stream = find_audio_stream(session);
	if (!stream)
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);

	src_length = get_be32(payload->src_length);
	flags = get_be32(payload->flags);
	if ((flags & ~(SDK_AUDIO_STREAM_FEED_EOF |
	               SDK_AUDIO_STREAM_FEED_DRAIN)) != 0U ||
	    (flags & (SDK_AUDIO_STREAM_FEED_EOF |
	              SDK_AUDIO_STREAM_FEED_DRAIN)) ==
	        (SDK_AUDIO_STREAM_FEED_EOF |
	         SDK_AUDIO_STREAM_FEED_DRAIN) ||
	    ((flags & SDK_AUDIO_STREAM_FEED_DRAIN) != 0U &&
	     src_length != 0U))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	if (stream->faulted)
		return complete_status(req, comp, SDK_STATUS_IO_ERROR);
	src_offset = 0U;
	if (src_length != 0U) {
		src = find_shared_buffer(get_be32(payload->src_handle));
		src_offset = get_be32(payload->src_offset);
		if (!src || !buffer_range_valid(src, src_offset, src_length))
			return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
		src_offset += src->address;   /* resolved source address */
	}

	if (stream->core1_affine && scheduler_core1_available()) {
		struct audio_feed_op_params p;

		p.session = session;
		p.src_addr = (src_length != 0U) ? src_offset : 0U;
		p.src_len = src_length;
		p.flags = flags;
		if (service_try_defer(SDK_OP_AUDIO_STREAM_FEED, req, &p,
		                      sizeof(p), src_length) ==
		    SDK_STATUS_QUEUED)
			return SDK_STATUS_QUEUED;
		/* A core-1-affine stream cannot decode on core 0 (its mp3
		 * staging ring is cache-owned by core 1), so a refused defer
		 * answers BUSY rather than corrupt. Two refusal causes, both
		 * retryable by contract: a full task queue (transient), and a
		 * FEED that is not the batch tail -- deferring mid-batch would
		 * let a later inline op in the same batch (PLAY/STOP/CLOSE)
		 * execute before the queued decode, so ordering demands the
		 * client resubmit. Every current client (zz9k.library sync
		 * calls, mpega, the MHI feeder) sends stream ops singly and
		 * never sees the batch case. */
		return complete_status(req, comp, SDK_STATUS_BUSY);
	}
	/* Core-0-affine stream: inline, as the pre-scheduler firmware did. */
	audio_stream_feed_compute(stream, (src_length != 0U) ? src_offset : 0U,
	                          src_length, flags);
	return complete_audio_stream_result(req, comp, SDK_STATUS_OK, stream);
}

static uint16_t handle_audio_stream_read(volatile struct SDKMailboxEntry *req,
                                         volatile struct SDKMailboxEntry *comp,
                                         uint16_t payload_len)
{
	volatile struct SDKAudioStreamReadPayload *payload;
	struct SDKAudioStream *stream;
	uint32_t session;
	uint32_t pcm_read;
	uint32_t flags;

	if (payload_len < sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	payload = (volatile struct SDKAudioStreamReadPayload *)req->payload;
	session = get_be32(payload->session);
	stream = find_audio_stream(session);
	if (!stream)
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	pcm_read = get_be32(payload->pcm_read);
	flags = get_be32(payload->flags);
	if (flags != 0U || pcm_read > audio_stream_pcm_used(stream))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	if (stream->faulted)
		return complete_status(req, comp, SDK_STATUS_IO_ERROR);
	/* A bound stream's consumer is the AX playback pump. */
	if (g_audio_playback.session == session &&
	    g_audio_playback.source_kind == AUDIO_PUMP_SOURCE_STREAM)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	if (stream->core1_affine && scheduler_core1_available()) {
		struct audio_read_op_params p;

		p.session = session;
		p.pcm_read = pcm_read;
		if (service_try_defer(SDK_OP_AUDIO_STREAM_READ, req, &p,
		                      sizeof(p), 0U) == SDK_STATUS_QUEUED)
			return SDK_STATUS_QUEUED;
		/* See the feed path: never run the decoder on core 0 for a
		 * core-1-affine stream. */
		return complete_status(req, comp, SDK_STATUS_BUSY);
	}
	/* Core-0-affine stream: inline, as the pre-scheduler firmware did. */
	audio_stream_read_compute(stream, pcm_read);
	return complete_audio_stream_result(req, comp, SDK_STATUS_OK, stream);
}

/* True while a deferred FEED/READ for this session is still queued or
 * executing on the scheduler. Both op-param structs start with the
 * session word, and params are packed native-endian by core 0. A DONE
 * slot is deliberately not counted: the worker has finished with the
 * stream and its result payload already lives in the task slot, so
 * only the (harmless) completion post remains. */
static int audio_stream_tasks_inflight(uint32_t session)
{
	taskq_shared_t *sh;
	uint32_t i;

	if (!scheduler_core1_available())
		return 0;
	sh = scheduler_shared();
	for (i = 0; i < TASKQ_CAPACITY; i++) {
		volatile taskq_desc_t *d = &sh->queue.descs[i];
		uint32_t st = d->state;

		if (st != TASK_QUEUED && st != TASK_CLAIMED)
			continue;
		if (d->opcode != SDK_OP_AUDIO_STREAM_FEED &&
		    d->opcode != SDK_OP_AUDIO_STREAM_READ)
			continue;
		if (((const struct audio_feed_op_params *)
		         (uintptr_t)d->op_params)->session == session)
			return 1;
	}
	return 0;
}

static uint16_t handle_audio_stream_close(volatile struct SDKMailboxEntry *req,
                                          volatile struct SDKMailboxEntry *comp,
                                          uint16_t payload_len)
{
	volatile struct SDKAudioStreamClosePayload *payload;
	struct SDKAudioStream snapshot;
	struct SDKAudioStream *stream;
	uint32_t session;
	uint32_t flags;

	if (payload_len < sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	payload = (volatile struct SDKAudioStreamClosePayload *)req->payload;
	session = get_be32(payload->session);
	flags = get_be32(payload->flags);
	if (flags != 0U)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	stream = find_audio_stream(session);
	if (!stream)
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	if (g_audio_playback.session == session &&
	    g_audio_playback.source_kind == AUDIO_PUMP_SOURCE_STREAM) {
		/* Closing a bound stream implies stop. */
		audio_playback_stop();
	}
	/* An internal PCM-refill FEED (request_id 0) may still be queued or
	 * running on core 1 against this stream's coherent slot -- it was
	 * enqueued for the bound session, which a prior STOP may already
	 * have unbound, so refill_session is checked rather than the
	 * binding above. Freeing the slot under the worker would zero the
	 * decoder state mid-decode. Draining it HERE is not an option:
	 * that would nest scheduler_core0_poll inside a handler, and a
	 * harvested foreign task would post its deferred completion into
	 * the ring slot the outer dispatcher already reserved for THIS
	 * request. Answer BUSY instead -- the main loop drains the refill
	 * between requests within milliseconds and the client retries. */
	if (g_audio_playback.refill_pending &&
	    g_audio_playback.refill_session == session)
		return complete_status(req, comp, SDK_STATUS_BUSY);
	/* Same hazard from the client side: an async caller can enqueue
	 * CLOSE while its own deferred FEED/READ is still queued or
	 * executing against this slot. (A synchronous caller cannot -- it
	 * waits out each op -- so this only gates misuse of the async
	 * API.) */
	if (audio_stream_tasks_inflight(session))
		return complete_status(req, comp, SDK_STATUS_BUSY);
	snapshot = *stream;
	free_audio_stream(stream);
	return complete_audio_stream_result(req, comp, SDK_STATUS_OK, &snapshot);
}

static uint16_t validate_image_session_buffers(
	struct SDKImageStreamBegin *begin)
{
	struct SDKSurface dst;
	struct SDKSharedBuffer *tile;
	uint32_t tile_length;

	if (begin->output_mode == SDK_IMAGE_OUTPUT_SURFACE ||
	    begin->output_mode == SDK_IMAGE_OUTPUT_FRAMEBUFFER) {
		if (!get_surface_info(begin->dst_surface, &dst))
			return SDK_STATUS_BAD_HANDLE;
		if (begin->output_format != dst.format)
			return SDK_STATUS_UNSUPPORTED;
		if (!surface_range_valid(&dst, begin->dst_x, begin->dst_y,
		                         begin->dst_width,
		                         begin->dst_height)) {
			return SDK_STATUS_BAD_REQUEST;
		}
		begin->dst_address = (uintptr_t)dst.address;
		begin->dst_pitch = dst.pitch;
		begin->dst_length = dst.length;
		return SDK_STATUS_OK;
	}

	if (begin->output_mode == SDK_IMAGE_OUTPUT_TILE_BUFFER) {
		tile = find_shared_buffer(begin->tile_handle);
		if (!tile)
			return SDK_STATUS_BAD_HANDLE;
		if (begin->tile_rows != 0U &&
		    begin->tile_stride > (0xffffffffU / begin->tile_rows)) {
			return SDK_STATUS_BAD_REQUEST;
		}
		tile_length = begin->tile_stride * begin->tile_rows;
		if (tile_length == 0U || tile_length > tile->length)
			return SDK_STATUS_BAD_REQUEST;
		begin->tile_address = (uintptr_t)tile->address;
		begin->tile_length = tile->length;
		return SDK_STATUS_OK;
	}

	return SDK_STATUS_BAD_REQUEST;
}

static void flush_image_session_output(const struct SDKImageStreamResult *result)
{
	if (result && result->flush_address != 0U && result->flush_length != 0U)
		Xil_DCacheFlushRange((INTPTR)result->flush_address,
		                     result->flush_length);
}

static uint16_t handle_image_session_begin(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp,
	uint16_t payload_len)
{
	volatile struct SDKImageSessionBeginPayload *payload;
	struct SDKImageStreamBegin begin;
	struct SDKImageStreamResult result;
	uint16_t status;

	if (payload_len < sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	payload = (volatile struct SDKImageSessionBeginPayload *)req->payload;
	memset(&begin, 0, sizeof(begin));
	begin.codec = get_be32(payload->codec);
	begin.output_mode = get_be32(payload->output_mode);
	begin.dst_surface = get_be32(payload->dst_surface);
	begin.dst_x = get_be32(payload->dst_x);
	begin.dst_y = get_be32(payload->dst_y);
	begin.dst_width = get_be32(payload->dst_width);
	begin.dst_height = get_be32(payload->dst_height);
	begin.output_format = get_be32(payload->output_format);
	begin.tile_handle = get_be32(payload->tile_handle);
	begin.tile_stride = get_be32(payload->tile_stride);
	begin.tile_rows = get_be32(payload->tile_rows);
	begin.flags = get_be32(payload->flags);

	status = validate_image_session_buffers(&begin);
	if (status != SDK_STATUS_OK)
		return complete_status(req, comp, status);

	/* Fix the session's core affinity for its whole life: feeds/closes
	 * of a core-1-affine session run on the worker (the codec heap
	 * objects then live in core 1's cache and never migrate). */
	begin.core1_affine = scheduler_core1_available() ? 1U : 0U;
	if (begin.core1_affine &&
	    (begin.output_mode == SDK_IMAGE_OUTPUT_SURFACE ||
	     begin.output_mode == SDK_IMAGE_OUTPUT_FRAMEBUFFER)) {
		struct SDKSurface dst;

		/* ARM-local outputs keep the single-core cache contract
		 * (reads skip invalidation, so a core-1-written surface
		 * would go stale for core 0) -- see scale_defer_eligible.
		 * Keep those sessions fully inline. */
		if (get_surface_info(begin.dst_surface, &dst) &&
		    surface_is_arm_local(&dst))
			begin.core1_affine = 0U;
	}

	status = sdk_image_stream_begin(&begin, &result);
	return complete_image_session_result(req, comp, status, &result);
}

static uint16_t handle_image_session_feed(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp,
	uint16_t payload_len)
{
	volatile struct SDKImageSessionFeedPayload *payload;
	struct SDKImageStreamFeed feed;
	struct SDKImageStreamResult result;
	struct SDKSharedBuffer *src;
	const uint8_t *src_data = 0;
	uint16_t status;

	if (payload_len < sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	payload = (volatile struct SDKImageSessionFeedPayload *)req->payload;
	memset(&feed, 0, sizeof(feed));
	feed.session = get_be32(payload->session);
	feed.src_handle = get_be32(payload->src_handle);
	feed.src_offset = get_be32(payload->src_offset);
	feed.src_length = get_be32(payload->src_length);
	feed.flags = get_be32(payload->flags);

	src = find_shared_buffer(feed.src_handle);
	if (!src)
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	if (!buffer_range_valid(src, feed.src_offset, feed.src_length))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	if (sdk_image_stream_session_core1(feed.session) > 0 &&
	    scheduler_core1_available()) {
		struct imgsess_op_params p;

		p.session = feed.session;
		p.src_handle = feed.src_handle;
		p.src_addr = (feed.src_length != 0U) ?
		    (src->address + feed.src_offset) : 0U;
		p.src_len = feed.src_length;
		p.flags = feed.flags;
		if (service_try_defer(SDK_OP_IMAGE_SESSION_FEED, req, &p,
		                      sizeof(p), feed.src_length) ==
		    SDK_STATUS_QUEUED)
			return SDK_STATUS_QUEUED;
		/* A core-1-affine session cannot decode on core 0 (its codec
		 * state lives in core 1's cache). The sole synchronous client
		 * sends session ops singly (always the batch tail, one in
		 * flight), so a refused defer means a full queue -- transient
		 * and unreachable today; answer BUSY rather than corrupt. */
		return complete_status(req, comp, SDK_STATUS_BUSY);
	}
	/* Core-0-affine (or poisoned-after-fault) session: inline, as the
	 * pre-scheduler firmware did. */

	if (feed.src_length != 0U) {
		src_data = (const uint8_t *)(uintptr_t)
			(src->address + feed.src_offset);
		Xil_DCacheInvalidateRange((INTPTR)(src->address +
		                          feed.src_offset),
		                          feed.src_length);
	}

	status = sdk_image_stream_feed(&feed, src_data, &result);
	if (status == SDK_STATUS_OK)
		flush_image_session_output(&result);
	return complete_image_session_result(req, comp, status, &result);
}

static uint16_t handle_image_session_close(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp,
	uint16_t payload_len)
{
	volatile struct SDKImageSessionClosePayload *payload;
	uint32_t session;
	uint16_t status;

	if (payload_len < sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	payload = (volatile struct SDKImageSessionClosePayload *)req->payload;
	session = get_be32(payload->session);

	if (sdk_image_stream_session_core1(session) > 0 &&
	    scheduler_core1_available()) {
		struct imgsess_op_params p;

		memset(&p, 0, sizeof(p));
		p.session = session;
		if (service_try_defer(SDK_OP_IMAGE_SESSION_CLOSE, req, &p,
		                      sizeof(p), 0U) == SDK_STATUS_QUEUED)
			return SDK_STATUS_QUEUED;
		/* See the feed path: never touch core-1 codec state from
		 * core 0; refused defer = transient full queue. */
		return complete_status(req, comp, SDK_STATUS_BUSY);
	}
	/* Core-0-affine session, or a poisoned one after a core-1 fault
	 * (codec objects already reclaimed; close reduces to a slot reset). */

	status = sdk_image_stream_close(session);
	return complete_status(req, comp, status);
}

static uint16_t handle_video_session_begin(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp,
	uint16_t payload_len)
{
	volatile struct SDKVideoSessionBeginPayload *payload;
	struct SDKVideoStreamBegin begin;
	struct SDKVideoStreamResult result;
	uint16_t status;

	if (payload_len < sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	if (!scheduler_core1_available())
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);
	payload = (volatile struct SDKVideoSessionBeginPayload *)req->payload;
	memset(&begin, 0, sizeof(begin));
	begin.codec = get_be32(payload->codec);
	begin.container = get_be32(payload->container);
	begin.width = get_be32(payload->width);
	begin.height = get_be32(payload->height);
	begin.output_format = get_be32(payload->output_format);
	begin.flags = get_be32(payload->flags);
	status = sdk_video_stream_begin(&begin, &result);
	return complete_video_session_result(req, comp, status, &result);
}

static uint16_t handle_video_session_write(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp,
	uint16_t payload_len)
{
	volatile struct SDKVideoSessionWritePayload *payload;
	struct SDKSharedBuffer *src;
	struct video_write_op_params p;
	uint32_t src_offset;

	if (payload_len < sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	payload = (volatile struct SDKVideoSessionWritePayload *)req->payload;
	memset(&p, 0, sizeof(p));
	p.session = get_be32(payload->session);
	p.src_len = get_be32(payload->src_length);
	p.flags = get_be32(payload->flags);
	if (sdk_video_stream_session_core1(p.session) < 0 ||
	    sdk_video_stream_session_owner(p.session) !=
	        SDK_VIDEO_STREAM_OWNER_LEGACY)
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	/* EOF-only writes carry no source bytes and intentionally need no live
	 * shared-buffer handle. The worker validates that the EOF flag is present. */
	if (p.src_len != 0U) {
		src_offset = get_be32(payload->src_offset);
		src = find_shared_buffer(get_be32(payload->src_handle));
		if (!src)
			return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
		if (!buffer_range_valid(src, src_offset, p.src_len))
			return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
		p.src_addr = src->address + src_offset;
	}
	if (service_try_defer(SDK_OP_VIDEO_SESSION_WRITE, req, &p,
	                      sizeof(p), p.src_len) == SDK_STATUS_QUEUED)
		return SDK_STATUS_QUEUED;
	return complete_status(req, comp, SDK_STATUS_BUSY);
}

static uint16_t handle_video_session_simple(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp,
	uint16_t payload_len, uint16_t opcode)
{
	struct video_session_op_params p;
	uint32_t flags;

	memset(&p, 0, sizeof(p));
	if (opcode == SDK_OP_VIDEO_SESSION_DECODE) {
		volatile struct SDKVideoSessionDecodePayload *payload;

		if (payload_len < sizeof(*payload))
			return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
		payload = (volatile struct SDKVideoSessionDecodePayload *)req->payload;
		p.session = get_be32(payload->session);
		flags = get_be32(payload->flags);
		if (sdk_video_stream_session_core1(p.session) < 0 ||
		    sdk_video_stream_session_owner(p.session) !=
		        SDK_VIDEO_STREAM_OWNER_LEGACY)
			return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	} else {
		volatile struct SDKVideoSessionClosePayload *payload;

		if (payload_len < sizeof(*payload))
			return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
		payload = (volatile struct SDKVideoSessionClosePayload *)req->payload;
		p.session = get_be32(payload->session);
		flags = get_be32(payload->flags);
	}
	if (flags != 0U)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	if (sdk_video_stream_session_core1(p.session) < 0 ||
	    sdk_video_stream_session_owner(p.session) !=
	        SDK_VIDEO_STREAM_OWNER_LEGACY)
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	if (service_try_defer(opcode, req, &p, sizeof(p), 0U) ==
	    SDK_STATUS_QUEUED)
		return SDK_STATUS_QUEUED;
	return complete_status(req, comp, SDK_STATUS_BUSY);
}

static uint16_t handle_media_session_begin(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp,
	uint16_t payload_len)
{
	volatile struct SDKMediaSessionBeginPayload *payload;
	struct SDKMediaSessionBegin begin;
	struct SDKMediaSessionMainResult result;
	struct SDKSharedBuffer *pcm_ring = 0;
	uint16_t status;

	if (payload_len != sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	if (!scheduler_core1_available())
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);
	payload = (volatile struct SDKMediaSessionBeginPayload *)req->payload;
	memset(&begin, 0, sizeof(begin));
	begin.video_codec = get_be32(payload->video_codec);
	begin.container = get_be32(payload->container);
	begin.width = get_be32(payload->width);
	begin.height = get_be32(payload->height);
	begin.output_format = get_be32(payload->output_format);
	begin.audio_codec = get_be32(payload->audio_codec);
	begin.pcm_ring_handle = get_be32(payload->pcm_ring_handle);
	begin.pcm_ring_capacity = get_be32(payload->pcm_ring_capacity);
	begin.pcm_low_water_bytes = get_be32(payload->pcm_low_water_bytes);
	begin.pcm_high_water_bytes = get_be32(payload->pcm_high_water_bytes);
	begin.flags = get_be32(payload->flags);
	if (!bytes_are_zero(payload->reserved, sizeof(payload->reserved)))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	if (begin.audio_codec == SDK_MEDIA_AUDIO_MP2) {
		if (media_pcm_ring)
			return complete_status(req, comp, SDK_STATUS_BUSY);
		pcm_ring = find_shared_buffer(begin.pcm_ring_handle);
		if (!pcm_ring)
			return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
		if (begin.pcm_ring_capacity > pcm_ring->length)
			return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
		if (!shared_buffer_pin(pcm_ring))
			return complete_status(req, comp, SDK_STATUS_BUSY);
		begin.pcm_ring = (uint8_t *)(uintptr_t)pcm_ring->address;
	}
	status = sdk_media_session_begin(&begin, &result);
	if (begin.audio_codec == SDK_MEDIA_AUDIO_MP2) {
		if (status == SDK_STATUS_OK) {
			media_pcm_ring = pcm_ring;
			media_pcm_ring_session = result.session;
		} else {
			shared_buffer_unpin(pcm_ring);
		}
	}
	return complete_media_session_main_result(req, comp, status, &result);
}

static uint16_t handle_media_session_write(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp,
	uint16_t payload_len)
{
	volatile struct SDKMediaSessionWritePayload *payload;
	struct SDKSharedBuffer *src;
	struct video_write_op_params p;
	uint32_t src_offset;

	if (payload_len != sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	payload = (volatile struct SDKMediaSessionWritePayload *)req->payload;
	memset(&p, 0, sizeof(p));
	p.session = get_be32(payload->session);
	p.src_len = get_be32(payload->src_length);
	p.flags = get_be32(payload->flags);
	if ((p.flags & ~SDK_MEDIA_SESSION_WRITE_EOF) != 0U)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	if (sdk_media_session_core1(p.session) < 0)
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	if (p.src_len != 0U) {
		src_offset = get_be32(payload->src_offset);
		src = find_shared_buffer(get_be32(payload->src_handle));
		if (!src)
			return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
		if (!buffer_range_valid(src, src_offset, p.src_len))
			return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
		p.src_addr = src->address + src_offset;
	}
	if (service_try_defer(SDK_OP_MEDIA_SESSION_WRITE, req, &p,
	                      sizeof(p), p.src_len) == SDK_STATUS_QUEUED)
		return SDK_STATUS_QUEUED;
	return complete_status(req, comp, SDK_STATUS_BUSY);
}

static uint16_t handle_media_session_deferred_simple(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp,
	uint16_t payload_len, uint16_t opcode)
{
	volatile struct SDKMediaSessionCommandPayload *payload;
	struct video_session_op_params p;
	uint32_t flags;

	if (payload_len != sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	payload = (volatile struct SDKMediaSessionCommandPayload *)req->payload;
	memset(&p, 0, sizeof(p));
	p.session = get_be32(payload->session);
	flags = get_be32(payload->flags);
	if (p.session == 0U || flags != 0U ||
	    get_be32(payload->value_hi) != 0U ||
	    get_be32(payload->value_lo) != 0U)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	if (opcode == SDK_OP_MEDIA_SESSION_CLOSE) {
		if (!sdk_media_session_close_known(p.session))
			return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	} else if (sdk_media_session_core1(p.session) < 0) {
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	}
	if (service_try_defer(opcode, req, &p, sizeof(p), 0U) ==
	    SDK_STATUS_QUEUED)
		return SDK_STATUS_QUEUED;
	return complete_status(req, comp, SDK_STATUS_BUSY);
}

static uint16_t handle_media_session_present_or_discard(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp,
	uint16_t payload_len, uint16_t opcode)
{
	volatile struct SDKMediaSessionCommandPayload *payload;
	struct SDKMediaSessionMainResult result;
	uint32_t session;
	uint32_t flags;
	uint16_t status;

	if (payload_len != sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	payload = (volatile struct SDKMediaSessionCommandPayload *)req->payload;
	session = get_be32(payload->session);
	flags = get_be32(payload->flags);
	if (get_be32(payload->value_hi) != 0U ||
	    get_be32(payload->value_lo) != 0U)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	if (opcode == SDK_OP_MEDIA_SESSION_PRESENT)
		status = sdk_media_session_present(session, flags, &result);
	else
		status = sdk_media_session_discard(session, flags, &result);
	return complete_media_session_main_result(req, comp, status, &result);
}

static uint16_t handle_media_session_status(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp,
	uint16_t payload_len)
{
	volatile struct SDKMediaSessionStatusPayload *payload;
	struct SDKMediaSessionStatusResult result;
	uint16_t status;

	if (payload_len != sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	payload = (volatile struct SDKMediaSessionStatusPayload *)req->payload;
	status = sdk_media_session_status(
		get_be32(payload->session), get_be32(payload->page),
		get_be32(payload->flags), &result);
	return complete_media_session_status_result(
		req, comp, status, &result);
}

static uint16_t handle_media_session_audio_read(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp,
	uint16_t payload_len)
{
	volatile struct SDKMediaSessionCommandPayload *payload;
	struct media_audio_op_params p;

	if (payload_len != sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	payload = (volatile struct SDKMediaSessionCommandPayload *)req->payload;
	memset(&p, 0, sizeof(p));
	p.session = get_be32(payload->session);
	p.acknowledged_hi = get_be32(payload->value_hi);
	p.acknowledged_lo = get_be32(payload->value_lo);
	p.flags = get_be32(payload->flags);
	if (p.session == 0U || p.flags != 0U ||
	    !bytes_are_zero(payload->reserved, sizeof(payload->reserved)))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	if (sdk_media_session_core1(p.session) < 0)
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	if (service_try_defer(SDK_OP_MEDIA_SESSION_AUDIO_READ, req, &p,
	                      sizeof(p), 0U) == SDK_STATUS_QUEUED)
		return SDK_STATUS_QUEUED;
	return complete_status(req, comp, SDK_STATUS_BUSY);
}

static uint16_t handle_media_session_audio_bind(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp,
	uint16_t payload_len, uint16_t opcode)
{
	volatile struct SDKMediaSessionCommandPayload *payload;
	struct SDKMediaSessionAudioResult result;
	uint32_t session;
	uint32_t flags;
	uint16_t status;

	if (payload_len != sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	payload = (volatile struct SDKMediaSessionCommandPayload *)req->payload;
	session = get_be32(payload->session);
	flags = get_be32(payload->flags);
	if (session == 0U ||
	    !bytes_are_zero(payload->reserved, sizeof(payload->reserved)) ||
	    get_be32(payload->value_hi) != 0U ||
	    get_be32(payload->value_lo) != 0U)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	if (opcode == SDK_OP_MEDIA_SESSION_AUDIO_UNBIND) {
		if (flags != 0U)
			return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
		if (g_audio_playback.session == session &&
		    g_audio_playback.source_kind == AUDIO_PUMP_SOURCE_MEDIA)
			audio_playback_stop();
		status = sdk_media_session_audio_unbind(
			session, flags, &result);
		return complete_media_session_audio_result(
			req, comp, status, &result);
	}

	if ((flags & ~SDK_MEDIA_AUDIO_BIND_PAUSE) != 0U)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	if (g_audio_playback.session != 0U &&
	    (g_audio_playback.session != session ||
	     g_audio_playback.source_kind != AUDIO_PUMP_SOURCE_MEDIA))
		return complete_status(req, comp, SDK_STATUS_BUSY);

	if ((flags & SDK_MEDIA_AUDIO_BIND_PAUSE) != 0U) {
		if (g_audio_playback.session != session ||
		    g_audio_playback.source_kind != AUDIO_PUMP_SOURCE_MEDIA)
			return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
		g_audio_playback.paused = 1U;
		__asm__ __volatile__("" ::: "memory");
		status = sdk_media_session_audio_bind(
			session, flags, &result);
		if (status != SDK_STATUS_OK) {
			g_audio_playback.paused = 0U;
			return complete_status(req, comp, status);
		}
		audio_playback_clear_periods(
			g_audio_playback.period_source_bytes,
			AUDIO_NUM_PERIODS);
		audio_silence();
		return complete_media_session_audio_result(
			req, comp, SDK_STATUS_OK, &result);
	}

	if (g_audio_playback.session == session &&
	    g_audio_playback.source_kind == AUDIO_PUMP_SOURCE_MEDIA) {
		status = sdk_media_session_audio_bind(
			session, 0U, &result);
		if (status == SDK_STATUS_OK && g_audio_playback.paused)
			audio_playback_start(
				AUDIO_PUMP_SOURCE_MEDIA, session);
		return complete_media_session_audio_result(
			req, comp, status, &result);
	}
	if (!audio_codec_present())
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);
	if (audio_legacy_output_active())
		return complete_status(req, comp, SDK_STATUS_BUSY);
	status = sdk_media_session_audio_bind(session, 0U, &result);
	if (status == SDK_STATUS_OK)
		audio_playback_start(AUDIO_PUMP_SOURCE_MEDIA, session);
	return complete_media_session_audio_result(
		req, comp, status, &result);
}

static uint16_t handle_fill_surface(volatile struct SDKMailboxEntry *req,
                                    volatile struct SDKMailboxEntry *comp,
                                    uint16_t payload_len)
{
	volatile struct SDKSurfaceFillPayload *payload;
	struct SDKSurface surface;
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	uint32_t color;
	uint32_t flags;

	if (payload_len < sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	payload = (volatile struct SDKSurfaceFillPayload *)req->payload;
	if (!get_surface_info(get_be32(payload->surface), &surface))
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);

	x = get_be32(payload->x);
	y = get_be32(payload->y);
	width = get_be32(payload->width);
	height = get_be32(payload->height);
	color = get_be32(payload->color);
	flags = get_be32(payload->flags);

	if (flags != 0U)
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);
	if (!sdk_surface_fill_rect((uint8_t *)(uintptr_t)surface.address,
	                           surface.width, surface.height,
	                           surface.pitch, surface.format,
	                           x, y, width, height, color)) {
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	}

	flush_surface_rect(&surface, x, y, width, height);
	return complete_status(req, comp, SDK_STATUS_OK);
}

struct SDKQueryPalettePayload {
	uint8_t surface[4];
	uint8_t start[4];
	uint8_t count[4];
	uint8_t dst_handle[4];
	uint8_t dst_offset[4];
	uint8_t flags[4];
	uint8_t reserved[24];
};

/* The 256-entry CLUT cannot fit the fixed 48-byte reply, so - as the crypto
 * hash op does - the result goes to a caller shared buffer as `count`
 * consecutive 0x00RRGGBB words from index `start`. The palette is
 * display-global; `surface` is accepted for future per-surface tables but
 * does not select one today. */
static uint16_t handle_query_palette(volatile struct SDKMailboxEntry *req,
                                     volatile struct SDKMailboxEntry *comp,
                                     uint16_t payload_len)
{
	volatile struct SDKQueryPalettePayload *payload;
	struct SDKSharedBuffer *dst;
	uint32_t start;
	uint32_t count;
	uint32_t dst_offset;
	uint32_t flags;
	uint32_t written;

	if (payload_len < 24U)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	payload = (volatile struct SDKQueryPalettePayload *)req->payload;
	start = get_be32(payload->start);
	count = get_be32(payload->count);
	dst_offset = get_be32(payload->dst_offset);
	flags = get_be32(payload->flags);

	/* Only the primary CLUT is shadowed; the secondary query is reserved. */
	if ((flags & ~(uint32_t)SDK_PALETTE_QUERY_FLAG_SECONDARY) != 0U)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	if ((flags & SDK_PALETTE_QUERY_FLAG_SECONDARY) != 0U)
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);

	/* Bound the window to the 256-entry table. This also keeps count * 4,
	 * the byte count below, from overflowing. */
	if (count == 0U || count > 256U || start > 256U ||
	    (start + count) > 256U)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	dst = find_shared_buffer(get_be32(payload->dst_handle));
	if (!buffer_range_valid(dst, dst_offset, count * 4U))
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);

	written = sdk_palette_pack_be(
		(void *)(uintptr_t)(dst->address + dst_offset), start, count);
	if (written == 0U)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	Xil_DCacheFlushRange((INTPTR)(uintptr_t)(dst->address + dst_offset),
	                     written);

	return complete_status(req, comp, SDK_STATUS_OK);
}

static uint16_t handle_copy_surface(volatile struct SDKMailboxEntry *req,
                                    volatile struct SDKMailboxEntry *comp,
                                    uint16_t payload_len)
{
	volatile struct SDKSurfaceCopyPayload *payload;
	struct SDKSurface src;
	struct SDKSurface dst;
	uint32_t src_x;
	uint32_t src_y;
	uint32_t dst_x;
	uint32_t dst_y;
	uint32_t width;
	uint32_t height;
	uint32_t flags;

	if (payload_len < sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	payload = (volatile struct SDKSurfaceCopyPayload *)req->payload;
	if (!get_surface_info(get_be32(payload->src_surface), &src) ||
	    !get_surface_info(get_be32(payload->dst_surface), &dst)) {
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	}

	src_x = get_be32(payload->src_x);
	src_y = get_be32(payload->src_y);
	dst_x = get_be32(payload->dst_x);
	dst_y = get_be32(payload->dst_y);
	width = get_be32(payload->width);
	height = get_be32(payload->height);
	flags = get_be32(payload->flags);

	if (flags != 0U)
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);
	if (src.format != dst.format)
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);

	prepare_surface_for_arm_read(&src);
	if (!sdk_surface_copy_rect((uint8_t *)(uintptr_t)dst.address,
	                           dst.width, dst.height, dst.pitch,
	                           (const uint8_t *)(uintptr_t)src.address,
	                           src.width, src.height, src.pitch,
	                           src.format, src_x, src_y, dst_x,
	                           dst_y, width, height)) {
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	}

	flush_surface_rect(&dst, dst_x, dst_y, width, height);
	return complete_status(req, comp, SDK_STATUS_OK);
}

static uint16_t crypto_digest_length(uint32_t algorithm)
{
	switch (algorithm) {
	case SDK_CRYPTO_HASH_SHA1:
		return SDK_SHA1_DIGEST_SIZE;
	case SDK_CRYPTO_HASH_SHA256:
		return SDK_SHA256_DIGEST_SIZE;
	case SDK_CRYPTO_HASH_POLY1305:
		return SDK_POLY1305_TAG_SIZE;
	default:
		return 0;
	}
}

/*
 * Crypto compute cores (Phase 1 dual-core scheduler).
 *
 * The core-0 fronts (handle_crypto_*) resolve shared-buffer handles to card DDR
 * addresses and validate them, then pack the resolved addresses into one of the
 * param structs below and hand the task to a consumer via crypto_dispatch(). A
 * compute core runs on whichever core claims the task (core 1 normally, core 0
 * inline on fallback). It owns ALL data-buffer cache maintenance: invalidate
 * inputs before reading (pull the Amiga's DDR writes / drop stale lines) and
 * flush outputs after writing (push to DDR for the Amiga). The generic queue
 * region is SCU-coherent; these per-op data buffers are not, so the compute
 * core manages them explicitly exactly as the original single-core handlers did.
 *
 * These structs live in the task descriptor's opaque op_params[48]; both cores
 * are Cortex-A9 with identical ABI, so a plain struct overlay is safe.
 */
struct crypto_hash_params {
	uint32_t algorithm;
	uint32_t flags;
	uint32_t src_addr;
	uint32_t src_length;
	uint32_t key_addr;      /* 0 = no key (plain hash) */
	uint32_t key_length;
	uint32_t dst_addr;
	uint32_t digest_length;
};

struct crypto_stream_params {
	uint32_t algorithm;
	uint32_t src_addr;
	uint32_t src_length;
	uint32_t key_addr;
	uint32_t nonce_addr;
	uint32_t counter;
	uint32_t dst_addr;
};

struct crypto_aead_params {
	uint32_t algorithm;
	uint32_t flags;         /* carries SDK_CRYPTO_AEAD_FLAG_DECRYPT */
	uint32_t src_addr;
	uint32_t src_length;
	uint32_t src_total;
	uint32_t dst_addr;
	uint32_t key_addr;
	uint32_t key_size;
	uint32_t nonce_addr;
	uint32_t aad_addr;      /* 0 = no AAD */
	uint32_t aad_length;
};

struct crypto_kx_params {
	uint32_t algorithm;
	uint32_t flags;         /* SDK_CRYPTO_KX_FLAG_KEYGEN selects P-256 scalar*G */
	uint32_t scalar_addr;
	uint32_t point_addr;    /* unused for keygen */
	uint32_t dst_addr;
};

struct crypto_verify_params {
	uint32_t algorithm;
	uint32_t hash_addr;
	uint32_t key_addr;
	uint32_t key_length;
	uint32_t sig_addr;
	uint32_t sig_length;
};

typedef char crypto_params_fit_op_params[
    (sizeof(struct crypto_aead_params) <= TASKQ_OP_PARAM_BYTES) ? 1 : -1];

/* The core-1 worker (scheduler_arm.c) posts TASKQ_RESULT_PAYLOAD bytes for a
 * successful crypto completion; that must equal one full result payload. */
typedef char crypto_result_fits_payload[
    (sizeof(struct SDKCryptoResultPayload) == TASKQ_RESULT_PAYLOAD) ? 1 : -1];

/* Same guard for the batched-decompress completion posted by
 * SDK_OP_DECOMPRESS_BATCH's taskq_desc_t::result_payload. */
typedef char decompress_batch_result_fits_payload[
    (sizeof(struct SDKDecompressBatchResultPayload) == TASKQ_RESULT_PAYLOAD) ?
        1 : -1];

static void crypto_result(uint8_t *result_payload, uint32_t bytes_written,
                          uint32_t algorithm, uint32_t flags)
{
	volatile struct SDKCryptoResultPayload *r =
	    (volatile struct SDKCryptoResultPayload *)result_payload;
	put_be32(r->bytes_written, bytes_written);
	put_be32(r->algorithm, algorithm);
	put_be32(r->flags, flags);
}

static uint16_t crypto_hash_compute(const struct crypto_hash_params *p,
                                    uint8_t *result_payload)
{
	const uint8_t *src_data = (const uint8_t *)(uintptr_t)p->src_addr;
	uint8_t digest[SDK_SHA256_DIGEST_SIZE];

	Xil_DCacheInvalidateRange((INTPTR)src_data, p->src_length);
	if (p->key_addr != 0U) {
		const uint8_t *key_data = (const uint8_t *)(uintptr_t)p->key_addr;
		Xil_DCacheInvalidateRange((INTPTR)key_data, p->key_length);
		if (p->algorithm == SDK_CRYPTO_HASH_POLY1305)
			sdk_poly1305(key_data, src_data, p->src_length, digest);
		else if (p->algorithm == SDK_CRYPTO_HASH_SHA1)
			sdk_hmac_sha1(key_data, p->key_length, src_data,
			              p->src_length, digest);
		else
			sdk_hmac_sha256(key_data, p->key_length, src_data,
			                p->src_length, digest);
	} else {
		if (p->algorithm == SDK_CRYPTO_HASH_SHA1)
			sdk_sha1(src_data, p->src_length, digest);
		else
			sdk_sha256(src_data, p->src_length, digest);
	}

	memcpy((void *)(uintptr_t)p->dst_addr, digest, p->digest_length);
	Xil_DCacheFlushRange((INTPTR)(uintptr_t)p->dst_addr, p->digest_length);
	crypto_result(result_payload, p->digest_length, p->algorithm, p->flags);
	memset(digest, 0, sizeof(digest));
	return SDK_STATUS_OK;
}

static uint16_t crypto_stream_compute(const struct crypto_stream_params *p,
                                      uint8_t *result_payload)
{
	const uint8_t *src_data = (const uint8_t *)(uintptr_t)p->src_addr;
	const uint8_t *key_data = (const uint8_t *)(uintptr_t)p->key_addr;
	const uint8_t *nonce_data = (const uint8_t *)(uintptr_t)p->nonce_addr;
	uint8_t *dst_data = (uint8_t *)(uintptr_t)p->dst_addr;

	Xil_DCacheInvalidateRange((INTPTR)src_data, p->src_length);
	Xil_DCacheInvalidateRange((INTPTR)key_data, SDK_CHACHA20_KEY_SIZE);
	Xil_DCacheInvalidateRange((INTPTR)nonce_data, SDK_CHACHA20_NONCE_SIZE);
	sdk_chacha20_xor(key_data, nonce_data, p->counter, src_data, dst_data,
	                 p->src_length);
	Xil_DCacheFlushRange((INTPTR)dst_data, p->src_length);
	crypto_result(result_payload, p->src_length, p->algorithm, 0U);
	return SDK_STATUS_OK;
}

static uint16_t crypto_aead_compute(const struct crypto_aead_params *p,
                                    uint8_t *result_payload)
{
	const uint8_t *src_data = (const uint8_t *)(uintptr_t)p->src_addr;
	const uint8_t *aad_data = p->aad_length ?
	    (const uint8_t *)(uintptr_t)p->aad_addr : 0;
	const uint8_t *key_data = (const uint8_t *)(uintptr_t)p->key_addr;
	const uint8_t *nonce_data = (const uint8_t *)(uintptr_t)p->nonce_addr;
	uint8_t *dst_data = (uint8_t *)(uintptr_t)p->dst_addr;
	uint32_t decrypt = (p->flags & SDK_CRYPTO_AEAD_FLAG_DECRYPT) != 0;
	uint8_t tag[SDK_POLY1305_TAG_SIZE];

	Xil_DCacheInvalidateRange((INTPTR)src_data, p->src_total);
	if (p->aad_length != 0U)
		Xil_DCacheInvalidateRange((INTPTR)aad_data, p->aad_length);
	Xil_DCacheInvalidateRange((INTPTR)key_data, p->key_size);
	Xil_DCacheInvalidateRange((INTPTR)nonce_data, SDK_AES_GCM_NONCE_SIZE);

	if (decrypt) {
		int ok;
		if (p->algorithm == SDK_CRYPTO_AEAD_CHACHA20_POLY1305)
			ok = sdk_chacha20_poly1305_decrypt(
			    key_data, nonce_data, aad_data, p->aad_length,
			    src_data, p->src_length,
			    src_data + p->src_length, dst_data);
		else
			ok = sdk_aes_gcm_decrypt(
			    key_data, p->key_size, nonce_data, aad_data,
			    p->aad_length, src_data, p->src_length,
			    src_data + p->src_length, dst_data);
		if (!ok)
			return SDK_STATUS_IO_ERROR;
		Xil_DCacheFlushRange((INTPTR)dst_data, p->src_length);
	} else {
		if (p->algorithm == SDK_CRYPTO_AEAD_CHACHA20_POLY1305)
			sdk_chacha20_poly1305_encrypt(key_data, nonce_data,
			                              aad_data, p->aad_length,
			                              src_data, p->src_length,
			                              dst_data, tag);
		else
			sdk_aes_gcm_encrypt(key_data, p->key_size, nonce_data,
			                    aad_data, p->aad_length,
			                    src_data, p->src_length,
			                    dst_data, tag);
		memcpy(dst_data + p->src_length, tag, sizeof(tag));
		Xil_DCacheFlushRange((INTPTR)dst_data,
		                     p->src_length + SDK_AES_GCM_TAG_SIZE);
	}

	crypto_result(result_payload,
	              decrypt ? p->src_length :
	                        p->src_length + SDK_AES_GCM_TAG_SIZE,
	              p->algorithm, p->flags);
	memset(tag, 0, sizeof(tag));
	return SDK_STATUS_OK;
}

static uint16_t crypto_kx_compute(const struct crypto_kx_params *p,
                                  uint8_t *result_payload)
{
	const uint8_t *scalar_data = (const uint8_t *)(uintptr_t)p->scalar_addr;
	const uint8_t *point_data = (const uint8_t *)(uintptr_t)p->point_addr;
	uint8_t *dst_data = (uint8_t *)(uintptr_t)p->dst_addr;

	if (p->algorithm == SDK_CRYPTO_KX_X25519) {
		uint8_t xshared[SDK_X25519_SHARED_SIZE];
		Xil_DCacheInvalidateRange((INTPTR)scalar_data, SDK_X25519_KEY_SIZE);
		Xil_DCacheInvalidateRange((INTPTR)point_data, SDK_X25519_POINT_SIZE);
		if (!sdk_x25519(scalar_data, point_data, xshared)) {
			memset(xshared, 0, sizeof(xshared));
			return SDK_STATUS_BAD_REQUEST;
		}
		memcpy(dst_data, xshared, SDK_X25519_SHARED_SIZE);
		Xil_DCacheFlushRange((INTPTR)dst_data, SDK_X25519_SHARED_SIZE);
		memset(xshared, 0, sizeof(xshared));
		crypto_result(result_payload, SDK_X25519_SHARED_SIZE,
		              SDK_CRYPTO_KX_X25519, 0U);
	} else if (p->flags == SDK_CRYPTO_KX_FLAG_KEYGEN) {
		/* P-256 keygen: scalar*G -> full 65-byte uncompressed point. No peer
		 * point; the front validated scalar+dst and left point_addr unused. */
		uint8_t pub[SDK_P256_POINT_SIZE];
		Xil_DCacheInvalidateRange((INTPTR)scalar_data, SDK_P256_KEY_SIZE);
		if (!sdk_p256_keygen(scalar_data, pub)) {
			memset(pub, 0, sizeof(pub));
			return SDK_STATUS_BAD_REQUEST;
		}
		memcpy(dst_data, pub, SDK_P256_POINT_SIZE);
		Xil_DCacheFlushRange((INTPTR)dst_data, SDK_P256_POINT_SIZE);
		memset(pub, 0, sizeof(pub));  /* public, but scrub the stack copy */
		crypto_result(result_payload, SDK_P256_POINT_SIZE,
		              SDK_CRYPTO_KX_P256, SDK_CRYPTO_KX_FLAG_KEYGEN);
	} else {  /* SDK_CRYPTO_KX_P256 derive -- the front validated the algorithm */
		uint8_t shared[SDK_P256_SHARED_SIZE];
		Xil_DCacheInvalidateRange((INTPTR)scalar_data, SDK_P256_KEY_SIZE);
		Xil_DCacheInvalidateRange((INTPTR)point_data, SDK_P256_POINT_SIZE);
		if (!sdk_p256_ecdh(scalar_data, point_data, shared)) {
			memset(shared, 0, sizeof(shared));
			return SDK_STATUS_BAD_REQUEST;
		}
		memcpy(dst_data, shared, SDK_P256_SHARED_SIZE);
		Xil_DCacheFlushRange((INTPTR)dst_data, SDK_P256_SHARED_SIZE);
		memset(shared, 0, sizeof(shared));
		crypto_result(result_payload, SDK_P256_SHARED_SIZE,
		              SDK_CRYPTO_KX_P256, 0U);
	}
	return SDK_STATUS_OK;
}

static uint16_t crypto_verify_compute(const struct crypto_verify_params *p,
                                      uint8_t *result_payload)
{
	const uint8_t *hash_data = (const uint8_t *)(uintptr_t)p->hash_addr;
	const uint8_t *key_data = (const uint8_t *)(uintptr_t)p->key_addr;
	const uint8_t *sig_data = (const uint8_t *)(uintptr_t)p->sig_addr;
	uint8_t hash[SDK_SHA256_DIGEST_SIZE];
	int verified = 0;

	if (p->algorithm == SDK_CRYPTO_VERIFY_ECDSA_P256_SHA256) {
		uint8_t pubkey[SDK_P256_ECDSA_POINT_SIZE];
		uint8_t signature[SDK_P256_ECDSA_SIG_SIZE];
		Xil_DCacheInvalidateRange((INTPTR)hash_data, SDK_SHA256_DIGEST_SIZE);
		Xil_DCacheInvalidateRange((INTPTR)key_data, SDK_P256_ECDSA_POINT_SIZE);
		Xil_DCacheInvalidateRange((INTPTR)sig_data, SDK_P256_ECDSA_SIG_SIZE);
		memcpy(hash, hash_data, SDK_SHA256_DIGEST_SIZE);
		memcpy(pubkey, key_data, SDK_P256_ECDSA_POINT_SIZE);
		memcpy(signature, sig_data, SDK_P256_ECDSA_SIG_SIZE);
		verified = sdk_ecdsa_verify_p256(pubkey, signature, hash);
		memset(pubkey, 0, sizeof(pubkey));
		memset(signature, 0, sizeof(signature));
	} else {  /* SDK_CRYPTO_VERIFY_RSA_PKCS1_2048_SHA256 -- front validated */
		uint8_t modulus[SDK_RSA_MAX_KEY_BYTES];
		uint8_t exponent[4];
		uint8_t signature[SDK_RSA_MAX_KEY_BYTES];
		uint32_t mod_len = p->key_length - 4U;
		Xil_DCacheInvalidateRange((INTPTR)hash_data, SDK_SHA256_DIGEST_SIZE);
		Xil_DCacheInvalidateRange((INTPTR)key_data, p->key_length);
		Xil_DCacheInvalidateRange((INTPTR)sig_data, p->sig_length);
		memcpy(hash, hash_data, SDK_SHA256_DIGEST_SIZE);
		memcpy(modulus, key_data, mod_len);
		memcpy(exponent, key_data + mod_len, 4);
		memcpy(signature, sig_data, p->sig_length);
		verified = sdk_rsa_verify_pkcs1_sha256(modulus, mod_len, exponent,
		                                       4U, signature, p->sig_length,
		                                       hash);
		memset(modulus, 0, sizeof(modulus));
		memset(exponent, 0, sizeof(exponent));
		memset(signature, 0, sizeof(signature));
	}

	crypto_result(result_payload, verified ? 1U : 0U, p->algorithm, 0U);
	memset(hash, 0, sizeof(hash));
	return SDK_STATUS_OK;
}

/*
 * Run a crypto task's compute on the calling core. Shared by the core-1 worker
 * and the core-0 inline/fallback path. result_payload is a 48-byte
 * SDKCryptoResultPayload buffer; it is fully zeroed here so reserved bytes are
 * clean regardless of which op runs.
 */
uint16_t sdk_mailbox_run_crypto_task(uint16_t opcode, const void *op_params,
                                     uint8_t *result_payload)
{
	memset(result_payload, 0, sizeof(struct SDKCryptoResultPayload));
	switch (opcode) {
	case SDK_OP_CRYPTO_HASH:
		return crypto_hash_compute(
		    (const struct crypto_hash_params *)op_params, result_payload);
	case SDK_OP_CRYPTO_STREAM:
		return crypto_stream_compute(
		    (const struct crypto_stream_params *)op_params, result_payload);
	case SDK_OP_CRYPTO_AEAD:
		return crypto_aead_compute(
		    (const struct crypto_aead_params *)op_params, result_payload);
	case SDK_OP_CRYPTO_KX:
		return crypto_kx_compute(
		    (const struct crypto_kx_params *)op_params, result_payload);
	case SDK_OP_CRYPTO_VERIFY:
		return crypto_verify_compute(
		    (const struct crypto_verify_params *)op_params, result_payload);
	default:
		return SDK_STATUS_UNSUPPORTED;
	}
}

/*
 * Opcode-dispatched offload executor. This is the scheduler's single
 * compute-dispatch seam (scheduler_run_slot in scheduler_arm.c): the queue/
 * worker/harvest machinery is opcode-agnostic, and this function routes a
 * claimed taskq_desc_t to its service handler, filling result_payload and
 * *result_len for taskq_complete().
 *
 * The crypto opcode class reproduces the pre-extraction behaviour exactly:
 * on SDK_STATUS_OK the completion carries one full SDKCryptoResultPayload;
 * on any other status it carries zero payload bytes, matching sdk_mailbox's
 * own inline dispatch (crypto_dispatch, above) which likewise omits the
 * payload on failure via complete_status(). SDK_OP_DECOMPRESS mirrors the
 * same convention against handle_decompress's SDKDecompressResultPayload
 * (field order and cache invalidate/flush match exactly).
 *
 * SDK_OP_DECOMPRESS_BATCH runs the ENTIRE arena -- cache invalidate, header/
 * member validation, the per-member decode loop, and the final cache flush --
 * on whichever core claims the task. In production that is always core 1: the
 * front (handle_decompress_batch, below) never computes inline, because a
 * multi-member LZH batch can run long enough to starve the main loop, and the
 * main loop is what answers every 68k register-window bus cycle (the FPGA
 * withholds DTACK until the ARM responds -- no timeout) and kicks the ~12.9s
 * whole-PS watchdog (main.c:415). Running the batch here, off the main loop,
 * is the fix for a real Amiga hard-crash.
 */
uint16_t sdk_mailbox_run_offload_task(const taskq_desc_t *d,
                                      uint8_t *result_payload,
                                      uint32_t *result_len)
{
	switch (d->opcode) {
	case SDK_OP_CRYPTO_HASH:
	case SDK_OP_CRYPTO_STREAM:
	case SDK_OP_CRYPTO_AEAD:
	case SDK_OP_CRYPTO_KX:
	case SDK_OP_CRYPTO_VERIFY: {
		uint16_t s = sdk_mailbox_run_crypto_task((uint16_t)d->opcode,
		                                         d->op_params, result_payload);
		*result_len = (s == SDK_STATUS_OK) ?
		    (uint32_t)sizeof(struct SDKCryptoResultPayload) : 0U;
		return s;
	}
	case SDK_OP_SCALE_IMAGE:
	case SDK_OP_SCALE_IMAGE_CLIPPED: {
		const struct scale_op_params *p =
		    (const struct scale_op_params *)d->op_params;
		struct SDKSurface src;
		struct SDKSurface dst;
		int ok;

		/* Rebuild just enough of the two surfaces for the shared cache
		 * helpers; geometry was resolved and validated on core 0 at
		 * enqueue time (scale_defer_eligible/scale_pack_params).
		 * ARM-local surfaces never arrive here (excluded from deferral:
		 * their no-invalidate contract is single-core), so the rebuilt
		 * surfaces carry no ARM_LOCAL flag and the invalidate/flush
		 * below always run for real. */
		memset(&src, 0, sizeof(src));
		memset(&dst, 0, sizeof(dst));
		src.address = p->src_addr;
		src.width = p->src_w;
		src.height = p->src_h;
		src.pitch = p->src_pitch;
		src.format = p->format;
		src.length = (uint32_t)p->src_pitch * p->src_h;
		dst.address = p->dst_addr;
		dst.width = p->dst_w;
		dst.height = p->dst_h;
		dst.pitch = p->dst_pitch;
		dst.format = p->format;
		dst.length = (uint32_t)p->dst_pitch * p->dst_h;

		prepare_surface_for_arm_read(&src);
		if (d->opcode == SDK_OP_SCALE_IMAGE_CLIPPED) {
			ok = sdk_surface_scale_rect_clipped(
			    (uint8_t *)(uintptr_t)dst.address,
			    dst.width, dst.height, dst.pitch,
			    (const uint8_t *)(uintptr_t)src.address,
			    src.width, src.height, src.pitch, src.format,
			    p->src_x, p->src_y, p->src_rw, p->src_rh,
			    p->dst_x, p->dst_y, p->dst_rw, p->dst_rh,
			    p->clip_x, p->clip_y, p->clip_rw, p->clip_rh,
			    (uint32_t)(p->filter_flags & 0xffU));
		} else {
			ok = sdk_surface_scale_rect(
			    (uint8_t *)(uintptr_t)dst.address,
			    dst.width, dst.height, dst.pitch,
			    (const uint8_t *)(uintptr_t)src.address,
			    src.width, src.height, src.pitch, src.format,
			    p->src_x, p->src_y, p->src_rw, p->src_rh,
			    p->dst_x, p->dst_y, p->dst_rw, p->dst_rh,
			    (uint32_t)(p->filter_flags & 0xffU));
		}
		*result_len = 0;
		if (!ok)
			return SDK_STATUS_BAD_REQUEST;
		flush_surface_rect(&dst, p->dst_x, p->dst_y,
		                   p->dst_rw, p->dst_rh);
		return SDK_STATUS_OK;
	}
	case SDK_OP_DECODE_JPEG: {
		const struct jpeg_op_params *p =
		    (const struct jpeg_op_params *)d->op_params;
		volatile struct SDKImageDecodeResultPayload *reply;
		struct SDKSurface dst;
		uint32_t image_width = 0;
		uint32_t image_height = 0;
		uint32_t bytes_written = 0;

		Xil_DCacheInvalidateRange((INTPTR)p->src_addr, p->src_len);
		if (!sdk_jpeg_decode_to_surface(
		            (const uint8_t *)(uintptr_t)p->src_addr, p->src_len,
		            (uint8_t *)(uintptr_t)p->dst_addr,
		            p->dst_width, p->dst_height, p->dst_pitch,
		            p->output_format, p->dst_x, p->dst_y,
		            &image_width, &image_height, &bytes_written)) {
			*result_len = 0;
			return SDK_STATUS_IO_ERROR;
		}

		/* Just enough of the dst surface for flush_surface_rect;
		 * geometry was resolved and validated on core 0. ARM-local
		 * destinations never arrive here (excluded from deferral). */
		memset(&dst, 0, sizeof(dst));
		dst.address = p->dst_addr;
		dst.width = p->dst_surf_w;
		dst.height = p->dst_surf_h;
		dst.pitch = p->dst_pitch;
		dst.format = p->output_format;
		dst.length = (uint32_t)p->dst_pitch * p->dst_surf_h;
		flush_surface_rect(&dst, p->dst_x, p->dst_y,
		                   p->dst_width, p->dst_height);

		memset(result_payload, 0,
		       sizeof(struct SDKImageDecodeResultPayload));
		reply = (volatile struct SDKImageDecodeResultPayload *)
		    result_payload;
		put_be32(reply->width, image_width);
		put_be32(reply->height, image_height);
		put_be32(reply->output_format, p->output_format);
		put_be32(reply->flags, 0U);
		put_be32(reply->bytes_written, bytes_written);
		*result_len = sizeof(struct SDKImageDecodeResultPayload);
		return SDK_STATUS_OK;
	}
	case SDK_OP_DECODE_MP3: {
		const struct mp3_op_params *p =
		    (const struct mp3_op_params *)d->op_params;
		return mp3_decode_compute(&g_mp3_core1_ctx, p, result_payload,
		                          result_len);
	}
	case TASKQ_OP_VIDEO_COMPOSE: {
		const struct overlay_compose_params *p =
		    (const struct overlay_compose_params *)d->op_params;
		*result_len = 0U;
		return overlay_run_compose(p);
	}
	case SDK_OP_IMAGE_SESSION_FEED: {
		const struct imgsess_op_params *p =
		    (const struct imgsess_op_params *)d->op_params;
		volatile struct SDKImageSessionResultPayload *reply;
		struct SDKImageStreamFeed feed;
		struct SDKImageStreamResult sres;
		const uint8_t *src_data = 0;
		uint16_t s;

		memset(&feed, 0, sizeof(feed));
		feed.session = p->session;
		feed.src_handle = p->src_handle;
		feed.src_length = p->src_len;
		feed.flags = p->flags;
		if (p->src_len != 0U) {
			src_data = (const uint8_t *)(uintptr_t)p->src_addr;
			Xil_DCacheInvalidateRange((INTPTR)p->src_addr,
			                          p->src_len);
		}

		s = sdk_image_stream_feed(&feed, src_data, &sres);
		*result_len = 0;
		if (s != SDK_STATUS_OK)
			return s;
		flush_image_session_output(&sres);

		memset(result_payload, 0,
		       sizeof(struct SDKImageSessionResultPayload));
		reply = (volatile struct SDKImageSessionResultPayload *)
		    result_payload;
		put_be32(reply->session, sres.session);
		put_be32(reply->state, sres.state);
		put_be32(reply->image_width, sres.image_width);
		put_be32(reply->image_height, sres.image_height);
		put_be32(reply->output_format, sres.output_format);
		put_be32(reply->tile_x, sres.tile_x);
		put_be32(reply->tile_y, sres.tile_y);
		put_be32(reply->tile_width, sres.tile_width);
		put_be32(reply->tile_height, sres.tile_height);
		put_be32(reply->bytes_consumed, sres.bytes_consumed);
		put_be32(reply->bytes_written, sres.bytes_written);
		put_be32(reply->flags, sres.flags);
		*result_len = sizeof(struct SDKImageSessionResultPayload);
		return SDK_STATUS_OK;
	}
	case SDK_OP_IMAGE_SESSION_CLOSE: {
		const struct imgsess_op_params *p =
		    (const struct imgsess_op_params *)d->op_params;

		*result_len = 0;
		return sdk_image_stream_close(p->session);
	}
	case SDK_OP_VIDEO_SESSION_WRITE: {
		const struct video_write_op_params *p =
		    (const struct video_write_op_params *)d->op_params;
		struct SDKVideoStreamWrite write;
		struct SDKVideoStreamResult video_result;
		uint16_t s;

		if (smp_cpu_id() != 1)
			return SDK_STATUS_IO_ERROR;
		memset(&write, 0, sizeof(write));
		write.session = p->session;
		write.src = p->src_len != 0U
			? (const uint8_t *)(uintptr_t)p->src_addr : 0;
		write.src_length = p->src_len;
		write.flags = p->flags;
		if (p->src_len != 0U)
			Xil_DCacheInvalidateRange((INTPTR)p->src_addr, p->src_len);
		s = sdk_video_stream_write(&write, &video_result);
		*result_len = 0U;
		if (s != SDK_STATUS_OK)
			return s;
		memset(result_payload, 0,
		       sizeof(struct SDKVideoSessionResultPayload));
		encode_video_session_result(
			(volatile struct SDKVideoSessionResultPayload *)result_payload,
			&video_result);
		*result_len = sizeof(struct SDKVideoSessionResultPayload);
		return SDK_STATUS_OK;
	}
	case SDK_OP_VIDEO_SESSION_DECODE:
	case SDK_OP_VIDEO_SESSION_CLOSE: {
		const struct video_session_op_params *p =
		    (const struct video_session_op_params *)d->op_params;
		struct SDKVideoStreamResult video_result;
		uint16_t s;

		if (smp_cpu_id() != 1)
			return SDK_STATUS_IO_ERROR;
		if (d->opcode == SDK_OP_VIDEO_SESSION_DECODE) {
			struct SDKVideoStreamDecode decode;

			memset(&decode, 0, sizeof(decode));
			decode.session = p->session;
			s = sdk_video_stream_decode(&decode, &video_result);
		} else {
			s = sdk_video_stream_close(p->session, &video_result);
		}
		*result_len = 0U;
		if (s != SDK_STATUS_OK)
			return s;
		memset(result_payload, 0,
		       sizeof(struct SDKVideoSessionResultPayload));
		encode_video_session_result(
			(volatile struct SDKVideoSessionResultPayload *)result_payload,
			&video_result);
		*result_len = sizeof(struct SDKVideoSessionResultPayload);
		return SDK_STATUS_OK;
	}
	case SDK_OP_MEDIA_SESSION_WRITE: {
		const struct video_write_op_params *p =
		    (const struct video_write_op_params *)d->op_params;
		struct SDKMediaSessionWrite write;
		struct SDKMediaSessionMainResult media_result;
		uint16_t s;

		if (smp_cpu_id() != 1)
			return SDK_STATUS_IO_ERROR;
		memset(&write, 0, sizeof(write));
		write.session = p->session;
		write.src = p->src_len != 0U
			? (const uint8_t *)(uintptr_t)p->src_addr : 0;
		write.src_length = p->src_len;
		write.flags = p->flags;
		if (p->src_len != 0U)
			Xil_DCacheInvalidateRange((INTPTR)p->src_addr, p->src_len);
		s = sdk_media_session_write(&write, &media_result);
		*result_len = 0U;
		if (s != SDK_STATUS_OK)
			return s;
		memset(result_payload, 0,
		       sizeof(struct SDKMediaSessionMainResultPayload));
		encode_media_session_main_result(
			(volatile struct SDKMediaSessionMainResultPayload *)
			    result_payload,
			&media_result);
		*result_len =
			sizeof(struct SDKMediaSessionMainResultPayload);
		return SDK_STATUS_OK;
	}
	case SDK_OP_MEDIA_SESSION_DECODE:
	case SDK_OP_MEDIA_SESSION_CLOSE: {
		const struct video_session_op_params *p =
		    (const struct video_session_op_params *)d->op_params;
		struct SDKMediaSessionMainResult media_result;
		uint16_t s;

		if (smp_cpu_id() != 1)
			return SDK_STATUS_IO_ERROR;
		if (d->opcode == SDK_OP_MEDIA_SESSION_DECODE)
			s = sdk_media_session_decode(
				p->session, 0U, &media_result);
		else
			s = sdk_media_session_close(
				p->session, 0U, &media_result);
		*result_len = 0U;
		if (s != SDK_STATUS_OK)
			return s;
		memset(result_payload, 0,
		       sizeof(struct SDKMediaSessionMainResultPayload));
		encode_media_session_main_result(
			(volatile struct SDKMediaSessionMainResultPayload *)
			    result_payload,
			&media_result);
		*result_len =
			sizeof(struct SDKMediaSessionMainResultPayload);
		return SDK_STATUS_OK;
	}
	case SDK_OP_MEDIA_SESSION_AUDIO_READ: {
		const struct media_audio_op_params *p =
		    (const struct media_audio_op_params *)d->op_params;
		struct SDKMediaSessionAudioResult audio_result;
		uint64_t acknowledged =
			((uint64_t)p->acknowledged_hi << 32) |
			p->acknowledged_lo;
		uint16_t s;

		if (smp_cpu_id() != 1)
			return SDK_STATUS_IO_ERROR;
		s = sdk_media_session_audio_read(
			p->session, acknowledged, p->flags, &audio_result);
		*result_len = 0U;
		if (s != SDK_STATUS_OK)
			return s;
		memset(result_payload, 0,
		       sizeof(struct SDKMediaSessionAudioResultPayload));
		encode_media_session_audio_result(
			(volatile struct SDKMediaSessionAudioResultPayload *)
			    result_payload,
			&audio_result);
		*result_len =
			sizeof(struct SDKMediaSessionAudioResultPayload);
		return SDK_STATUS_OK;
	}
	case SDK_OP_AUDIO_STREAM_FEED: {
		const struct audio_feed_op_params *p =
		    (const struct audio_feed_op_params *)d->op_params;
		struct SDKAudioStream *stream = find_audio_stream(p->session);

		*result_len = 0;
		if (!stream)
			return SDK_STATUS_BAD_HANDLE;
		if (stream->faulted)
			return SDK_STATUS_IO_ERROR;
		audio_stream_feed_compute(stream, p->src_addr, p->src_len,
		                          p->flags);
		/* This worker runs on core 1, which owns the mp3 staging ring's
		 * cache (see core1_affine). Only the FEED path writes that ring
		 * (feed_compute's memcpy + audio_stream_compact_input); a READ or
		 * the internal refill just reads it. Clean+invalidate the ring
		 * here so core 1 leaves no dirty lines behind: CLOSE/free runs on
		 * core 0 and cannot reach core 1's L1, so without this a later
		 * eviction of stale compressed data could clobber whatever reuses
		 * the freed shared buffer. */
		if (stream->mp3_ring_addr != 0U && stream->mp3_capacity != 0U)
			Xil_DCacheFlushRange(
				(INTPTR)(uintptr_t)stream->mp3_ring_addr,
				stream->mp3_capacity);
		audio_stream_fill_result(result_payload, stream);
		*result_len = sizeof(struct SDKAudioStreamResultPayload);
		return SDK_STATUS_OK;
	}
	case SDK_OP_AUDIO_STREAM_READ: {
		const struct audio_read_op_params *p =
		    (const struct audio_read_op_params *)d->op_params;
		struct SDKAudioStream *stream = find_audio_stream(p->session);

		*result_len = 0;
		if (!stream)
			return SDK_STATUS_BAD_HANDLE;
		if (stream->faulted)
			return SDK_STATUS_IO_ERROR;
		/* Re-validate against the LIVE used count. p->pcm_read was
		 * checked on core 0 against audio_stream_pcm_used() at intake,
		 * but an earlier queued READ for this stream may have advanced
		 * pcm_consumed_total since -- the snapshot is stale. Consuming
		 * blindly would overshoot pcm_ready_total and underflow
		 * audio_stream_pcm_used(), letting decode/pump work overwrite
		 * still-unread PCM. Reject (consume nothing), matching the
		 * inline intake path's over-read behavior. */
		if (p->pcm_read > audio_stream_pcm_used(stream))
			return SDK_STATUS_BAD_REQUEST;
		audio_stream_read_compute(stream, p->pcm_read);
		audio_stream_fill_result(result_payload, stream);
		*result_len = sizeof(struct SDKAudioStreamResultPayload);
		return SDK_STATUS_OK;
	}
	case SDK_OP_DECOMPRESS: {
		const struct decompress_op_params *p =
		    (const struct decompress_op_params *)d->op_params;
		struct SDKDecompressResult result;
		volatile struct SDKDecompressResultPayload *reply;
		XTime decode_start;
		XTime decode_end;
		uint16_t s;

		Xil_DCacheInvalidateRange((INTPTR)d->in_addr, d->in_len);
		XTime_GetTime(&decode_start);
		s = sdk_decompress_buffer(p->algorithm, p->flags,
		        (const uint8_t *)(uintptr_t)d->in_addr, d->in_len,
		        (uint8_t *)(uintptr_t)d->out_addr, d->out_cap, &result);
		XTime_GetTime(&decode_end);
		timing_decode_requests = saturated_add_u32(timing_decode_requests, 1U);
		timing_decode_us = saturated_add_u32(timing_decode_us,
		    timing_delta_us(decode_start, decode_end));
		if (s != SDK_STATUS_OK) {
			*result_len = 0;
			return s;
		}
		Xil_DCacheFlushRange((INTPTR)d->out_addr, result.bytes_written);

		memset(result_payload, 0, sizeof(struct SDKDecompressResultPayload));
		reply = (volatile struct SDKDecompressResultPayload *)result_payload;
		put_be32(reply->bytes_consumed, result.bytes_consumed);
		put_be32(reply->bytes_written, result.bytes_written);
		put_be32(reply->checksum, result.checksum);
		put_be32(reply->algorithm, result.algorithm);
		put_be32(reply->flags, result.flags);
		*result_len = sizeof(struct SDKDecompressResultPayload);
		return SDK_STATUS_OK;
	}
	case SDK_OP_DECOMPRESS_BATCH: {
		volatile struct SDKDecompressBatchResultPayload *reply;
		volatile uint8_t *base;
		uint32_t arena_addr = d->in_addr;
		uint32_t arena_length = d->in_len;
		uint32_t mode;
		uint32_t member_count;
		uint32_t desc_offset;
		uint32_t blob_offset;
		uint32_t blob_length;
		uint32_t output_offset;
		uint32_t output_capacity;
		uint32_t result_offset;
		uint32_t members_ok = 0;
		uint32_t members_failed = 0;
		uint32_t i;
		uint32_t region_off[5];
		uint32_t region_len[5];
		uint32_t region_count;
		uint32_t oi, oj;
		XTime batch_start;
		XTime batch_end;

		base = (volatile uint8_t *)(uintptr_t)arena_addr;
		/* The 68k wrote the arena over Zorro (non-coherent), and core 0
		 * only resolved/range-checked the shared-buffer handle -- it never
		 * touched arena contents. The magic/version/mode/member fields
		 * below must be read only after this invalidate, on the core that
		 * is actually executing the batch. */
		Xil_DCacheInvalidateRange((INTPTR)arena_addr, arena_length);

		if (get_be32(base + SDK_BATCH_HDR_MAGIC) != SDK_BATCH_ARENA_MAGIC) {
			*result_len = 0;
			return SDK_STATUS_BAD_REQUEST;
		}
		if (get_be16(base + SDK_BATCH_HDR_VERSION) != SDK_BATCH_ARENA_VERSION) {
			*result_len = 0;
			return SDK_STATUS_UNSUPPORTED;
		}

		mode = get_be16(base + SDK_BATCH_HDR_MODE);
		member_count = get_be32(base + SDK_BATCH_HDR_MEMBER_COUNT);
		desc_offset = get_be32(base + SDK_BATCH_HDR_DESC_OFFSET);
		blob_offset = get_be32(base + SDK_BATCH_HDR_BLOB_OFFSET);
		blob_length = get_be32(base + SDK_BATCH_HDR_BLOB_LENGTH);
		output_offset = get_be32(base + SDK_BATCH_HDR_OUTPUT_OFFSET);
		output_capacity = get_be32(base + SDK_BATCH_HDR_OUTPUT_CAPACITY);
		result_offset = get_be32(base + SDK_BATCH_HDR_RESULT_OFFSET);

		if ((mode != SDK_BATCH_MODE_TEST && mode != SDK_BATCH_MODE_EXTRACT) ||
		    member_count == 0U || member_count > SDK_BATCH_MEMBER_LIMIT ||
		    !batch_range_ok(arena_length, desc_offset,
		                    member_count * SDK_BATCH_DESC_SIZE) ||
		    !batch_range_ok(arena_length, blob_offset, blob_length) ||
		    !batch_range_ok(arena_length, result_offset,
		                    member_count * SDK_BATCH_RESULT_SIZE) ||
		    (mode == SDK_BATCH_MODE_EXTRACT &&
		     !batch_range_ok(arena_length, output_offset, output_capacity))) {
			*result_len = 0;
			return SDK_STATUS_BAD_REQUEST;
		}

		/* The documented arena layout is strictly ordered and disjoint:
		 * header, then desc[], blob, output (EXTRACT only), result[].
		 * Overlapping regions would let per-member result writes (or
		 * the EXTRACT output region) clobber descriptors or blob bytes
		 * before they are read, or clobber the header itself mid-batch.
		 * The batch_range_ok calls above already proved every region
		 * individually fits within [0, arena_length), and arena_length
		 * is bounded by the shared heap (a few MB) -- nowhere near
		 * 2^31 -- so none of the offset+length sums below can wrap a
		 * uint32_t; ranges_overlap's existing uint32_t signature is
		 * used as-is, matching its other caller (decompress_dispatch).
		 * ranges_overlap treats any zero-length region as
		 * non-overlapping by construction, which covers blob_length
		 * == 0 (valid for batches made entirely of empty members).
		 */
		region_count = 0;
		region_off[region_count] = 0U;
		region_len[region_count] = SDK_BATCH_HEADER_SIZE;
		region_count++;
		region_off[region_count] = desc_offset;
		region_len[region_count] = member_count * SDK_BATCH_DESC_SIZE;
		region_count++;
		region_off[region_count] = blob_offset;
		region_len[region_count] = blob_length;
		region_count++;
		if (mode == SDK_BATCH_MODE_EXTRACT) {
			region_off[region_count] = output_offset;
			region_len[region_count] = output_capacity;
			region_count++;
		}
		region_off[region_count] = result_offset;
		region_len[region_count] = member_count * SDK_BATCH_RESULT_SIZE;
		region_count++;

		for (oi = 0; oi < region_count; oi++) {
			for (oj = oi + 1; oj < region_count; oj++) {
				if (ranges_overlap(region_off[oi], region_len[oi],
				                    region_off[oj], region_len[oj])) {
					*result_len = 0;
					return SDK_STATUS_BAD_REQUEST;
				}
			}
		}

		XTime_GetTime(&batch_start);
		for (i = 0; i < member_count; i++) {
			volatile uint8_t *desc =
			    base + desc_offset + i * SDK_BATCH_DESC_SIZE;
			volatile uint8_t *res =
			    base + result_offset + i * SDK_BATCH_RESULT_SIZE;
			uint32_t algorithm = get_be32(desc + SDK_BATCH_DESC_ALGORITHM);
			uint32_t src_offset = get_be32(desc + SDK_BATCH_DESC_SRC_OFFSET);
			uint32_t src_length = get_be32(desc + SDK_BATCH_DESC_SRC_LENGTH);
			uint32_t dst_offset = get_be32(desc + SDK_BATCH_DESC_DST_OFFSET);
			uint32_t expected_size =
			    get_be32(desc + SDK_BATCH_DESC_UNCOMP_SIZE);
			struct SDKDecompressResult result;
			uint16_t status;

			memset(&result, 0, sizeof(result));
			if ((algorithm != SDK_COMPRESSION_LH1 &&
			     algorithm != SDK_COMPRESSION_LH5 &&
			     algorithm != SDK_COMPRESSION_LH6 &&
			     algorithm != SDK_COMPRESSION_LH7) ||
			    ((src_length == 0U) != (expected_size == 0U)) ||
			    !batch_range_ok(blob_length, src_offset, src_length) ||
			    (mode == SDK_BATCH_MODE_EXTRACT &&
			     !batch_range_ok(output_capacity, dst_offset,
			                     expected_size)) ||
			    (mode == SDK_BATCH_MODE_TEST &&
			     expected_size > SDK_BATCH_TEST_MAX_EXPECTED)) {
				status = SDK_STATUS_BAD_REQUEST;
			} else {
				const uint8_t *src = (const uint8_t *)(uintptr_t)(
				    arena_addr + blob_offset + src_offset);
				uint8_t *dst = NULL;

				if (mode == SDK_BATCH_MODE_EXTRACT) {
					dst = (uint8_t *)(uintptr_t)(
					    arena_addr + output_offset + dst_offset);
				}
				status = sdk_decompress_lzh(algorithm, src, src_length,
				                            dst, expected_size, &result);
			}

			put_be32(res + SDK_BATCH_RESULT_STATUS, status);
			put_be32(res + SDK_BATCH_RESULT_BYTES_WRITTEN,
			         result.bytes_written);
			put_be32(res + SDK_BATCH_RESULT_CHECKSUM, result.checksum);
			put_be32(res + SDK_BATCH_RESULT_RESERVED, 0U);
			if (status == SDK_STATUS_OK)
				members_ok++;
			else
				members_failed++;
		}
		XTime_GetTime(&batch_end);
		timing_decode_requests =
		    saturated_add_u32(timing_decode_requests, member_count);
		timing_decode_us = saturated_add_u32(
		    timing_decode_us, timing_delta_us(batch_start, batch_end));

		/* Results (and extract output) must be visible to the 68k. */
		Xil_DCacheFlushRange((INTPTR)arena_addr, arena_length);

		memset(result_payload, 0, sizeof(struct SDKDecompressBatchResultPayload));
		reply = (volatile struct SDKDecompressBatchResultPayload *)result_payload;
		put_be32(reply->members_total, member_count);
		put_be32(reply->members_ok, members_ok);
		put_be32(reply->members_failed, members_failed);
		put_be32(reply->flags, 0U);
		*result_len = sizeof(struct SDKDecompressBatchResultPayload);
		return SDK_STATUS_OK;
	}
	default:
		*result_len = 0;
		return SDK_STATUS_UNSUPPORTED;
	}
}

/*
 * Front dispatch helper. When core 1 is available, enqueue the task and defer
 * the completion (return SDK_STATUS_QUEUED); sdk_mailbox_task then consumes the
 * request without posting a completion, and scheduler_core0_poll posts it when
 * the worker finishes. Otherwise -- single-core fallback, or a full queue --
 * compute inline on core 0 and post the completion now, byte-identically to the
 * pre-scheduler firmware.
 *
 * in_len is the input data length, used only to classify the task SHORT/LONG
 * (KX/VERIFY are always SHORT and ignore it). Crypto tasks carry their resolved
 * card-DDR addresses inside op_params, so the descriptor's in_addr/out_addr/
 * out_cap fields are unused for the crypto opcode class and passed as 0.
 */
static uint16_t crypto_dispatch(uint16_t opcode, uint32_t in_len,
                                uint32_t param_len,
                                volatile struct SDKMailboxEntry *req,
                                volatile struct SDKMailboxEntry *comp,
                                const void *params)
{
	uint8_t result_payload[sizeof(struct SDKCryptoResultPayload)];
	uint16_t status;

	/*
	 * Only defer to core 1 when this is the last request in the batch; a crypto
	 * op with requests behind it must complete its shared-buffer writes inline
	 * so a later batched op (MEM_COPY/FREE_SHARED of the same handle, etc.)
	 * never races core 1. See g_request_is_batch_tail.
	 */
	if (scheduler_core1_available() && g_request_is_batch_tail) {
		taskq_shared_t *sh = scheduler_shared();
		int slot = taskq_enqueue(&sh->queue, opcode,
		                         taskq_class_for_opcode(opcode, in_len),
		                         0u, in_len, 0u, 0u,
		                         get_be32(req->request_id),
		                         get_be32(req->user_cookie), params, param_len);
		if (slot >= 0) {
			/* Stamp the current mailbox generation so this task is dropped
			 * rather than posted if the mailbox is re-initialised before it
			 * finishes. Core-0-only field; safe to write after the QUEUED
			 * release-store since core 1 never reads it. */
			g_task_generation[slot] = sdk_mailbox_generation;
			/* taskq_enqueue release-stored the QUEUED state; make it globally
			 * visible and wake the worker if it is idling on WFE. */
			__asm__ __volatile__("dsb ish\n\tsev" ::: "memory");
			return SDK_STATUS_QUEUED;
		}
		/* queue full -> fall through to synchronous inline compute */
	}

	status = sdk_mailbox_run_crypto_task(opcode, params, result_payload);
	scheduler_shared()->tasks_on_core0++;   /* executed inline on core 0 */
	if (status != SDK_STATUS_OK)
		return complete_status(req, comp, status);
	write_completion(comp, req, SDK_STATUS_OK,
	                 sizeof(struct SDKCryptoResultPayload));
	memset((void *)comp->payload, 0, sizeof(comp->payload));
	memcpy((void *)comp->payload, result_payload,
	       sizeof(struct SDKCryptoResultPayload));
	return SDK_STATUS_OK;
}

/*
 * Front dispatch helper for op_params-carried service ops (surface scale,
 * JPEG decode), following crypto_dispatch: a defer-eligible request
 * enqueues to the scheduler and returns SDK_STATUS_QUEUED; any failure to
 * defer returns SDK_STATUS_BUSY and the CALLER computes inline (these ops
 * have no cross-call state, so the inline fallback is always safe --
 * unlike LZH). in_len feeds SHORT/LONG classification (for scale it is
 * the destination-rect byte count, so small blits may be drained by an
 * idle core 0 exactly like small crypto ops).
 */
static uint16_t service_try_defer(uint16_t opcode,
                                  volatile struct SDKMailboxEntry *req,
                                  const void *params, uint32_t param_len,
                                  uint32_t in_len)
{
	if (scheduler_core1_available() && g_request_is_batch_tail) {
		taskq_shared_t *sh = scheduler_shared();
		int slot = taskq_enqueue(&sh->queue, opcode,
		                         taskq_class_for_opcode(opcode, in_len),
		                         0u, in_len, 0u, 0u,
		                         get_be32(req->request_id),
		                         get_be32(req->user_cookie),
		                         params, param_len);
		if (slot >= 0) {
			/* Stamp the current mailbox generation so this task is dropped
			 * rather than posted if the mailbox is re-initialised before it
			 * finishes (see g_task_generation). */
			g_task_generation[slot] = sdk_mailbox_generation;
			/* taskq_enqueue release-stored the QUEUED state; make it globally
			 * visible and wake the worker if it is idling on WFE. */
			__asm__ __volatile__("dsb ish\n\tsev" ::: "memory");
			return SDK_STATUS_QUEUED;
		}
		/* queue full -> caller computes inline */
	}
	return SDK_STATUS_BUSY;
}

/*
 * Front dispatch helper for decompress, mirroring crypto_dispatch exactly
 * (same batch-tail deferral gate, generation stamp, and inline-fallback
 * completion idiom). handle_decompress computes in_addr/out_addr from its
 * resolved shared buffers and hands off here after validation.
 *
 * Only defer to core 1 when this is the last request in the batch; a
 * decompress op with requests behind it must complete its shared-buffer
 * writes inline so a later batched op touching the same handle never races
 * core 1. See g_request_is_batch_tail.
 */
static uint16_t decompress_dispatch(volatile struct SDKMailboxEntry *req,
                                    volatile struct SDKMailboxEntry *comp,
                                    uint32_t in_addr, uint32_t in_len,
                                    uint32_t out_addr, uint32_t out_cap,
                                    uint32_t algorithm, uint32_t flags)
{
	uint8_t result_payload[TASKQ_RESULT_PAYLOAD];
	uint32_t result_len = 0;
	taskq_desc_t local;
	uint16_t status;
	int is_lzh = (algorithm == SDK_COMPRESSION_LH1 ||
	              algorithm == SDK_COMPRESSION_LH5 ||
	              algorithm == SDK_COMPRESSION_LH6 ||
	              algorithm == SDK_COMPRESSION_LH7);

	/* LZH single-op decodes and the batch task (SDK_OP_DECOMPRESS_BATCH) both
	 * defer to core 1 -- neither runs inline on core 0 anymore. The vendored
	 * LZH decoder keeps GLOBAL state (slide.c dtext, membuf cursors, Huffman
	 * tables), so two LZH decodes running at once would corrupt each other.
	 * The invariant below is ENFORCED, not advisory: all LZH work -- singles
	 * and the batch task -- runs ONLY on the single core-1 worker. When core
	 * 1 cannot take a single-op LZH request (core 1 unavailable, this
	 * request is not the batch tail, or the queue is full), the firmware
	 * answers BUSY instead of computing inline on core 0; the 68k client
	 * falls back to software decode. Safety holds because ALL LZH work --
	 * single ops and the batch task -- serializes on the single core-1
	 * worker: scheduler_core1_worker's claim/run/complete loop
	 * (scheduler_arm.c) executes one task at a time, so core 1 itself can
	 * never run two LZH decodes concurrently. That's paired with the
	 * sole-synchronous-client invariant: zz9k-archive is the only LZH
	 * producer today and it blocks on QUEUED completions (zz9k_call), so it
	 * never has two LZH requests in flight at once either. Adding a second
	 * concurrent LZH consumer (ARM-hosted app, datatype path) still requires
	 * excluding LZH algorithms from core-1 routing first -- the single
	 * worker serializes core-1 *execution*, not concurrent *producers*. */
	if (scheduler_core1_available() && g_request_is_batch_tail) {
		taskq_shared_t *sh = scheduler_shared();
		struct decompress_op_params p = { algorithm, flags };
		int slot = taskq_enqueue(&sh->queue, SDK_OP_DECOMPRESS, TASK_LONG,
		                         in_addr, in_len, out_addr, out_cap,
		                         get_be32(req->request_id),
		                         get_be32(req->user_cookie), &p, sizeof(p));
		if (slot >= 0) {
			/* Stamp the current mailbox generation so this task is dropped
			 * rather than posted if the mailbox is re-initialised before it
			 * finishes. Core-0-only field; safe to write after the QUEUED
			 * release-store since core 1 never reads it. */
			g_task_generation[slot] = sdk_mailbox_generation;
			/* taskq_enqueue release-stored the QUEUED state; make it globally
			 * visible and wake the worker if it is idling on WFE. */
			__asm__ __volatile__("dsb ish\n\tsev" ::: "memory");
			return SDK_STATUS_QUEUED;
		}
		/* queue full -> fall through to synchronous inline compute */
	}

	/* LZH must not run inline on core 0 (main-loop liveness / global
	 * decoder state); the 68k client falls back to software on BUSY. */
	if (is_lzh)
		return complete_status(req, comp, SDK_STATUS_BUSY);

	memset(&local, 0, sizeof(local));
	decompress_fill_desc(&local, in_addr, in_len, out_addr, out_cap,
	                     algorithm, flags);
	status = sdk_mailbox_run_offload_task(&local, result_payload, &result_len);
	scheduler_shared()->tasks_on_core0++;   /* executed inline on core 0 */
	if (status != SDK_STATUS_OK)
		return complete_status(req, comp, status);
	write_completion(comp, req, SDK_STATUS_OK, (uint16_t)result_len);
	memset((void *)comp->payload, 0, sizeof(comp->payload));
	memcpy((void *)comp->payload, result_payload, result_len);
	return SDK_STATUS_OK;
}

static uint16_t handle_crypto_hash(volatile struct SDKMailboxEntry *req,
                                   volatile struct SDKMailboxEntry *comp,
                                   uint16_t payload_len)
{
	volatile struct SDKCryptoHashPayload *payload;
	struct SDKSharedBuffer *src;
	struct SDKSharedBuffer *dst;
	struct SDKSharedBuffer *key;
	struct crypto_hash_params hp;
	uint32_t src_offset;
	uint32_t src_length;
	uint32_t dst_offset;
	uint32_t key_offset;
	uint32_t key_length;
	uint32_t algorithm;
	uint32_t flags;
	uint32_t key_required;
	uint16_t digest_length;

	if (payload_len < 40U)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	payload = (volatile struct SDKCryptoHashPayload *)req->payload;
	algorithm = get_be32(payload->algorithm);
	flags = get_be32(payload->flags);
	if ((flags & ~SDK_CRYPTO_HASH_FLAG_HMAC) != 0)
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);
	if (algorithm == SDK_CRYPTO_HASH_POLY1305 &&
	    flags != 0)
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);
	if (algorithm != SDK_CRYPTO_HASH_SHA1 &&
	    algorithm != SDK_CRYPTO_HASH_SHA256 &&
	    (flags & SDK_CRYPTO_HASH_FLAG_HMAC) != 0) {
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);
	}

	digest_length = crypto_digest_length(algorithm);
	if (digest_length == 0)
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);

	src = find_shared_buffer(get_be32(payload->src_handle));
	dst = find_shared_buffer(get_be32(payload->dst_handle));
	src_offset = get_be32(payload->src_offset);
	src_length = get_be32(payload->src_length);
	dst_offset = get_be32(payload->dst_offset);
	if (!buffer_range_valid(src, src_offset, src_length) ||
	    src_length == 0 ||
	    !buffer_range_valid(dst, dst_offset, digest_length)) {
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	}

	key = 0;
	key_offset = get_be32(payload->key_offset);
	key_length = get_be32(payload->key_length);
	key_required = (flags & SDK_CRYPTO_HASH_FLAG_HMAC) != 0 ||
	               algorithm == SDK_CRYPTO_HASH_POLY1305;
	if (key_required) {
		key = find_shared_buffer(get_be32(payload->key_handle));
		if (!buffer_range_valid(key, key_offset, key_length) ||
		    key_length == 0) {
			return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
		}
		if (algorithm == SDK_CRYPTO_HASH_POLY1305 &&
		    key_length != SDK_POLY1305_KEY_SIZE) {
			return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
		}
	}

	hp.algorithm = algorithm;
	hp.flags = flags;
	hp.src_addr = src->address + src_offset;
	hp.src_length = src_length;
	hp.key_addr = key_required ? (key->address + key_offset) : 0U;
	hp.key_length = key_length;
	hp.dst_addr = dst->address + dst_offset;
	hp.digest_length = digest_length;
	return crypto_dispatch(SDK_OP_CRYPTO_HASH, src_length, sizeof(hp),
	                       req, comp, &hp);
}

static uint16_t handle_crypto_stream(volatile struct SDKMailboxEntry *req,
                                     volatile struct SDKMailboxEntry *comp,
                                     uint16_t payload_len)
{
	volatile struct SDKCryptoStreamPayload *payload;
	struct SDKSharedBuffer *src;
	struct SDKSharedBuffer *dst;
	struct SDKSharedBuffer *key;
	struct SDKSharedBuffer *nonce;
	struct crypto_stream_params sp;
	uint32_t src_offset;
	uint32_t src_length;
	uint32_t dst_offset;
	uint32_t key_offset;
	uint32_t nonce_offset;
	uint32_t counter;
	uint32_t algorithm;
	uint32_t flags;

	if (payload_len < sizeof(struct SDKCryptoStreamPayload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	payload = (volatile struct SDKCryptoStreamPayload *)req->payload;
	algorithm = get_be32(payload->algorithm);
	flags = get_be32(payload->flags);
	if (flags != 0)
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);
	if (algorithm != SDK_CRYPTO_STREAM_CHACHA20)
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);

	src = find_shared_buffer(get_be32(payload->src_handle));
	dst = find_shared_buffer(get_be32(payload->dst_handle));
	key = find_shared_buffer(get_be32(payload->key_handle));
	nonce = find_shared_buffer(get_be32(payload->nonce_handle));
	src_offset = get_be32(payload->src_offset);
	src_length = get_be32(payload->src_length);
	dst_offset = get_be32(payload->dst_offset);
	key_offset = get_be32(payload->key_offset);
	nonce_offset = get_be32(payload->nonce_offset);
	counter = get_be32(payload->counter);

	if (!buffer_range_valid(src, src_offset, src_length) ||
	    src_length == 0 ||
	    !buffer_range_valid(dst, dst_offset, src_length) ||
	    !buffer_range_valid(key, key_offset, SDK_CHACHA20_KEY_SIZE) ||
	    !buffer_range_valid(nonce, nonce_offset, SDK_CHACHA20_NONCE_SIZE)) {
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	}

	(void)flags;
	sp.algorithm = algorithm;
	sp.src_addr = src->address + src_offset;
	sp.src_length = src_length;
	sp.key_addr = key->address + key_offset;
	sp.nonce_addr = nonce->address + nonce_offset;
	sp.counter = counter;
	sp.dst_addr = dst->address + dst_offset;
	return crypto_dispatch(SDK_OP_CRYPTO_STREAM, src_length, sizeof(sp),
	                       req, comp, &sp);
}

static uint16_t handle_crypto_aead(volatile struct SDKMailboxEntry *req,
                                   volatile struct SDKMailboxEntry *comp,
                                   uint16_t payload_len)
{
	volatile struct SDKCryptoAeadPayload *payload;
	struct SDKSharedBuffer *src;
	struct SDKSharedBuffer *dst;
	struct SDKSharedBuffer *aad;
	struct SDKSharedBuffer *key;
	struct SDKSharedBuffer *nonce;
	struct crypto_aead_params ap;
	uint32_t src_offset;
	uint32_t src_length;
	uint32_t src_total;
	uint32_t dst_offset;
	uint32_t aad_offset;
	uint32_t aad_length;
	uint32_t key_offset;
	uint32_t flags;
	uint32_t algorithm;
	uint32_t key_size;

	if (payload_len < sizeof(struct SDKCryptoAeadPayload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	payload = (volatile struct SDKCryptoAeadPayload *)req->payload;
	flags = get_be32(payload->flags);
	if ((flags & ~(SDK_CRYPTO_AEAD_FLAG_DECRYPT | SDK_CRYPTO_AEAD_ALG_MASK)) != 0)
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);

	/* The algorithm rides in the flags; a zero nibble is the legacy default. */
	algorithm = SDK_CRYPTO_AEAD_FLAG_GET_ALG(flags);
	if (algorithm == SDK_CRYPTO_AEAD_NONE)
		algorithm = SDK_CRYPTO_AEAD_CHACHA20_POLY1305;
	switch (algorithm) {
	case SDK_CRYPTO_AEAD_CHACHA20_POLY1305:
		key_size = SDK_CHACHA20_KEY_SIZE;
		break;
	case SDK_CRYPTO_AEAD_AES128_GCM:
		key_size = SDK_AES128_KEY_SIZE;
		break;
	case SDK_CRYPTO_AEAD_AES256_GCM:
		key_size = SDK_AES256_KEY_SIZE;
		break;
	default:
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);
	}

	src = find_shared_buffer(get_be32(payload->src_handle));
	dst = find_shared_buffer(get_be32(payload->dst_handle));
	key = find_shared_buffer(get_be32(payload->key_handle));
	nonce = find_shared_buffer(get_be32(payload->nonce_handle));
	src_offset = get_be32(payload->src_offset);
	src_length = get_be32(payload->src_length);
	dst_offset = get_be32(payload->dst_offset);
	aad_offset = get_be32(payload->aad_offset);
	aad_length = get_be32(payload->aad_length);
	key_offset = get_be32(payload->key_offset);

	if (src_length == 0U || src_length > (0xffffffffU - SDK_POLY1305_TAG_SIZE))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	src_total = src_length;
	if ((flags & SDK_CRYPTO_AEAD_FLAG_DECRYPT) != 0)
		src_total += SDK_POLY1305_TAG_SIZE;

	if (!buffer_range_valid(src, src_offset, src_total) ||
	    !buffer_range_valid(key, key_offset, key_size) ||
	    !buffer_range_valid(nonce, 0, SDK_AES_GCM_NONCE_SIZE)) {
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	}
	if ((flags & SDK_CRYPTO_AEAD_FLAG_DECRYPT) != 0) {
		if (!buffer_range_valid(dst, dst_offset, src_length))
			return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	} else if (!buffer_range_valid(dst, dst_offset,
	                               src_length + SDK_POLY1305_TAG_SIZE)) {
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	}

	aad = 0;
	if (aad_length != 0U) {
		aad = find_shared_buffer(get_be32(payload->aad_handle));
		if (!buffer_range_valid(aad, aad_offset, aad_length))
			return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	}

	ap.algorithm = algorithm;
	ap.flags = flags;
	ap.src_addr = src->address + src_offset;
	ap.src_length = src_length;
	ap.src_total = src_total;
	ap.dst_addr = dst->address + dst_offset;
	ap.key_addr = key->address + key_offset;
	ap.key_size = key_size;
	ap.nonce_addr = nonce->address;
	ap.aad_addr = (aad_length != 0U) ? (aad->address + aad_offset) : 0U;
	ap.aad_length = aad_length;
	return crypto_dispatch(SDK_OP_CRYPTO_AEAD, src_length, sizeof(ap),
	                       req, comp, &ap);
}

static uint16_t handle_crypto_kx(volatile struct SDKMailboxEntry *req,
                                  volatile struct SDKMailboxEntry *comp,
                                  uint16_t payload_len)
{
	volatile struct SDKCryptoKXPayload *p;
	struct SDKSharedBuffer *scalar;
	struct SDKSharedBuffer *point;
	struct SDKSharedBuffer *dst;
	struct crypto_kx_params kp;
	uint32_t algorithm;
	uint32_t flags;
	uint32_t scalar_off;
	uint32_t point_off;
	uint32_t dst_off;

	if (payload_len < sizeof(struct SDKCryptoKXPayload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	p = (volatile struct SDKCryptoKXPayload *)req->payload;
	algorithm = get_be32(p->algorithm);
	flags = get_be32(p->flags);
	/* flags are validated per algorithm below: X25519 requires 0; P-256 also
	 * accepts SDK_CRYPTO_KX_FLAG_KEYGEN (scalar*G -> full public point). */

	scalar = find_shared_buffer(get_be32(p->scalar_handle));
	point  = find_shared_buffer(get_be32(p->point_handle));
	dst    = find_shared_buffer(get_be32(p->dst_handle));
	scalar_off = get_be32(p->scalar_offset);
	point_off  = get_be32(p->point_offset);
	dst_off    = get_be32(p->dst_offset);

	if (algorithm == SDK_CRYPTO_KX_X25519) {
		if (flags != 0U)   /* X25519 keygen reuses derive with the base point */
			return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);
		if (!buffer_range_valid(scalar, scalar_off, SDK_X25519_KEY_SIZE)   ||
		    !buffer_range_valid(point,  point_off,  SDK_X25519_POINT_SIZE) ||
		    !buffer_range_valid(dst,    dst_off,    SDK_X25519_SHARED_SIZE))
			return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	} else if (algorithm == SDK_CRYPTO_KX_P256 &&
	           flags == SDK_CRYPTO_KX_FLAG_KEYGEN) {
		/* Keygen: scalar*G. No peer point; dst holds the full 65-byte point. */
		if (!buffer_range_valid(scalar, scalar_off, SDK_P256_KEY_SIZE) ||
		    !buffer_range_valid(dst,    dst_off,    SDK_P256_POINT_SIZE))
			return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	} else if (algorithm == SDK_CRYPTO_KX_P256 && flags == 0U) {
		if (!buffer_range_valid(scalar, scalar_off, SDK_P256_KEY_SIZE)   ||
		    !buffer_range_valid(point,  point_off,  SDK_P256_POINT_SIZE) ||
		    !buffer_range_valid(dst,    dst_off,    SDK_P256_SHARED_SIZE))
			return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	} else {
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);
	}

	kp.algorithm = algorithm;
	kp.flags = flags;
	kp.scalar_addr = scalar->address + scalar_off;
	/* keygen leaves the peer point unvalidated (and often unset), so don't
	 * dereference it -- the compute ignores point_addr for keygen. */
	kp.point_addr = (algorithm == SDK_CRYPTO_KX_P256 &&
	                 flags == SDK_CRYPTO_KX_FLAG_KEYGEN)
	                ? 0u : (point->address + point_off);
	kp.dst_addr = dst->address + dst_off;
	return crypto_dispatch(SDK_OP_CRYPTO_KX, 0u, sizeof(kp), req, comp, &kp);
}

static uint16_t handle_crypto_verify(volatile struct SDKMailboxEntry *req,
                                     volatile struct SDKMailboxEntry *comp,
                                     uint16_t payload_len)
{
	volatile struct SDKCryptoVerifyPayload *p;
	struct SDKSharedBuffer *hash_buf;
	struct SDKSharedBuffer *sig_buf;
	struct SDKSharedBuffer *key_buf;
	struct crypto_verify_params vp;
	uint32_t algorithm;
	uint32_t hash_off;
	uint32_t sig_off;
	uint32_t key_off;

	if (payload_len < sizeof(struct SDKCryptoVerifyPayload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	p = (volatile struct SDKCryptoVerifyPayload *)req->payload;
	algorithm = get_be32(p->algorithm);
	hash_off  = get_be32(p->hash_offset);
	sig_off   = get_be32(p->sig_offset);
	key_off   = get_be32(p->key_offset);

	hash_buf = find_shared_buffer(get_be32(p->hash_handle));
	sig_buf  = find_shared_buffer(get_be32(p->sig_handle));
	key_buf  = find_shared_buffer(get_be32(p->key_handle));

	/* The caller supplies a precomputed 32-byte SHA-256 digest. */
	if (!buffer_range_valid(hash_buf, hash_off, SDK_SHA256_DIGEST_SIZE))
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);

	vp.algorithm = algorithm;
	vp.hash_addr = hash_buf->address + hash_off;

	if (algorithm == SDK_CRYPTO_VERIFY_ECDSA_P256_SHA256) {
		/* key buffer = 65-byte uncompressed point, signature = raw r||s. */
		if (!buffer_range_valid(key_buf, key_off, SDK_P256_ECDSA_POINT_SIZE) ||
		    !buffer_range_valid(sig_buf, sig_off, SDK_P256_ECDSA_SIG_SIZE))
			return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);

		vp.key_addr = key_buf->address + key_off;
		vp.key_length = SDK_P256_ECDSA_POINT_SIZE;
		vp.sig_addr = sig_buf->address + sig_off;
		vp.sig_length = SDK_P256_ECDSA_SIG_SIZE;

	} else if (algorithm == SDK_CRYPTO_VERIFY_RSA_PKCS1_2048_SHA256) {
		uint32_t key_len = get_be32(p->key_length);
		uint32_t sig_len = get_be32(p->sig_length);
		uint32_t mod_len;

		/* key buffer = modulus (variable) followed by a 4-byte BE exponent;
		 * the signature width equals the modulus width (RSA-2048/3072/4096). */
		if (key_len < 4U + 1U || key_len > SDK_RSA_MAX_KEY_BYTES + 4U)
			return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
		mod_len = key_len - 4U;
		if (sig_len != mod_len || sig_len > SDK_RSA_MAX_KEY_BYTES)
			return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

		if (!buffer_range_valid(key_buf, key_off, key_len) ||
		    !buffer_range_valid(sig_buf, sig_off, sig_len))
			return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);

		vp.key_addr = key_buf->address + key_off;
		vp.key_length = key_len;
		vp.sig_addr = sig_buf->address + sig_off;
		vp.sig_length = sig_len;

	} else {
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);
	}

	return crypto_dispatch(SDK_OP_CRYPTO_VERIFY, 0u, sizeof(vp), req, comp, &vp);
}

static uint16_t handle_decompress(volatile struct SDKMailboxEntry *req,
                                  volatile struct SDKMailboxEntry *comp,
                                  uint16_t payload_len)
{
	volatile struct SDKDecompressPayload *payload;
	struct SDKSharedBuffer *src;
	struct SDKSharedBuffer *dst;
	uint32_t src_offset;
	uint32_t src_length;
	uint32_t dst_offset;
	uint32_t dst_capacity;
	uint32_t algorithm;
	uint32_t flags;
	uint32_t in_addr;
	uint32_t out_addr;
	int is_lzh;

	if (payload_len < sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	payload = (volatile struct SDKDecompressPayload *)req->payload;
	src = find_shared_buffer(get_be32(payload->src_handle));
	dst = find_shared_buffer(get_be32(payload->dst_handle));
	src_offset = get_be32(payload->src_offset);
	src_length = get_be32(payload->src_length);
	dst_offset = get_be32(payload->dst_offset);
	dst_capacity = get_be32(payload->dst_capacity);
	algorithm = get_be32(payload->algorithm);
	flags = get_be32(payload->flags);
	is_lzh = (algorithm == SDK_COMPRESSION_LH1 ||
	          algorithm == SDK_COMPRESSION_LH5 ||
	          algorithm == SDK_COMPRESSION_LH6 ||
	          algorithm == SDK_COMPRESSION_LH7);

	if (!src || !dst)
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	if ((!is_lzh && (src_length == 0U || dst_capacity == 0U)) ||
	    (is_lzh && ((src_length == 0U) != (dst_capacity == 0U))) ||
	    !buffer_range_valid(src, src_offset, src_length) ||
	    !buffer_range_valid(dst, dst_offset, dst_capacity)) {
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	}
	if (src == dst &&
	    ranges_overlap(src_offset, src_length, dst_offset, dst_capacity)) {
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	}

	in_addr = src->address + src_offset;
	out_addr = dst->address + dst_offset;
	return decompress_dispatch(req, comp, in_addr, src_length,
	                           out_addr, dst_capacity, algorithm, flags);
}

/* Batched LZH decode (SDK_OP_DECOMPRESS_BATCH).
 *
 * Decodes N LZH members described by a self-describing arena in ONE
 * mailbox round-trip, collapsing the per-member alloc/decode/free
 * round-trips that dominate whole-archive test/extract runs. Per-member
 * failures are recorded in the arena's result table and NEVER abort the
 * batch; only a malformed arena rejects the whole request.
 *
 * This front runs on core 0 and does NO arena memory access: it only
 * resolves and range-checks the arena handle (find_shared_buffer,
 * buffer_range_valid), then hands the resolved arena address off to core 1
 * as a single TASK_LONG task, mirroring decompress_dispatch below exactly
 * (enqueue/generation-stamp/sev/QUEUED). The actual cache invalidate,
 * header/member validation, and per-member decode loop live in
 * sdk_mailbox_run_offload_task's SDK_OP_DECOMPRESS_BATCH case (above), which
 * runs on whichever core claims the task -- core 1 in production.
 *
 * This MUST NOT run inline on core 0. It used to: the whole arena -- every
 * member -- was decoded in one main-loop iteration. The main loop is what
 * answers every 68k register-window bus cycle (the FPGA withholds DTACK
 * until the ARM responds -- there is no timeout) and it is the only place
 * the ~12.9s whole-PS watchdog gets kicked (main.c:415). A long enough batch
 * decode therefore froze the Amiga mid bus-cycle -- motherboard BERR,
 * guru/reboot -- and could trip the watchdog on top of that. This is a real
 * hardware crash, not a theoretical one; deferring to core 1 keeps the main
 * loop live exactly like every other decompress/crypto offload already does.
 *
 * Unlike decompress_dispatch, this never falls back to inline core-0 compute:
 * when core 1 is unavailable, this request is not the tail of its mailbox
 * submission (g_request_is_batch_tail; defense-in-depth against a pipelined
 * successor racing the deferred batch), or the task queue is full, we return
 * SDK_STATUS_BUSY so the 68k tool falls back to its per-member decode path,
 * instead of re-introducing the main-loop stall this fix removes.
 *
 * Memory: touches ONLY the host-provided arena (inside the SDK shared
 * heap) plus the decoder's private <=64 KB window -- no new reserved DDR
 * region, so it cannot collide with the Z3 fast-RAM window or the video
 * codec scratch at 0x30000000 (see memorymap.h).
 */
static uint16_t handle_decompress_batch(volatile struct SDKMailboxEntry *req,
                                        volatile struct SDKMailboxEntry *comp,
                                        uint16_t payload_len)
{
	volatile struct SDKDecompressBatchPayload *payload;
	struct SDKSharedBuffer *arena;
	uint32_t arena_offset;
	uint32_t arena_length;
	taskq_shared_t *sh;
	int slot;

	if (payload_len < sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	payload = (volatile struct SDKDecompressBatchPayload *)req->payload;
	arena = find_shared_buffer(get_be32(payload->arena_handle));
	arena_offset = get_be32(payload->arena_offset);
	arena_length = get_be32(payload->arena_length);

	if (!arena)
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	if (arena_length < SDK_BATCH_HEADER_SIZE ||
	    !buffer_range_valid(arena, arena_offset, arena_length))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	if (!scheduler_core1_available())
		return complete_status(req, comp, SDK_STATUS_BUSY);

	/* Pipelined successor requests in the same mailbox submission could race
	 * the deferred batch on the same handle; the sole synchronous client
	 * always IS the tail, so this is defense-in-depth (see
	 * g_request_is_batch_tail). */
	if (!g_request_is_batch_tail)
		return complete_status(req, comp, SDK_STATUS_BUSY);

	sh = scheduler_shared();
	slot = taskq_enqueue(&sh->queue, SDK_OP_DECOMPRESS_BATCH, TASK_LONG,
	                     arena->address + arena_offset, arena_length,
	                     0u, 0u, get_be32(req->request_id),
	                     get_be32(req->user_cookie), NULL, 0u);
	if (slot < 0)
		return complete_status(req, comp, SDK_STATUS_BUSY);

	/* Stamp the current mailbox generation so this task is dropped rather
	 * than posted if the mailbox is re-initialised before it finishes.
	 * Core-0-only field; safe to write after the QUEUED release-store
	 * since core 1 never reads it. */
	g_task_generation[slot] = sdk_mailbox_generation;
	/* taskq_enqueue release-stored the QUEUED state; make it globally
	 * visible and wake the worker if it is idling on WFE. */
	__asm__ __volatile__("dsb ish\n\tsev" ::: "memory");
	return SDK_STATUS_QUEUED;
}

static uint16_t handle_decompress_test(volatile struct SDKMailboxEntry *req,
                                       volatile struct SDKMailboxEntry *comp,
                                       uint16_t payload_len)
{
	volatile struct SDKDecompressTestPayload *payload;
	volatile struct SDKDecompressResultPayload *reply;
	struct SDKSharedBuffer *src;
	struct SDKDecompressResult result;
	uint32_t src_offset;
	uint32_t src_length;
	uint32_t output_limit;
	uint32_t algorithm;
	uint32_t flags;
	uint16_t status;

	if (payload_len < sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	payload = (volatile struct SDKDecompressTestPayload *)req->payload;
	src = find_shared_buffer(get_be32(payload->src_handle));
	src_offset = get_be32(payload->src_offset);
	src_length = get_be32(payload->src_length);
	output_limit = get_be32(payload->output_limit);
	algorithm = get_be32(payload->algorithm);
	flags = get_be32(payload->flags);

	if (!src)
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	if (src_length == 0U || output_limit == 0U ||
	    !buffer_range_valid(src, src_offset, src_length)) {
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	}

	Xil_DCacheInvalidateRange((INTPTR)(src->address + src_offset),
	                          src_length);
	status = sdk_decompress_test_buffer(
		algorithm, flags,
		(const uint8_t *)(uintptr_t)(src->address + src_offset),
		src_length, output_limit, &result);
	if (status != SDK_STATUS_OK)
		return complete_status(req, comp, status);

	write_completion(comp, req, SDK_STATUS_OK, sizeof(*reply));
	memset((void *)comp->payload, 0, sizeof(comp->payload));
	reply = (volatile struct SDKDecompressResultPayload *)comp->payload;
	put_be32(reply->bytes_consumed, result.bytes_consumed);
	put_be32(reply->bytes_written, result.bytes_written);
	put_be32(reply->checksum, result.checksum);
	put_be32(reply->algorithm, result.algorithm);
	put_be32(reply->flags, result.flags);
	return SDK_STATUS_OK;
}

static void write_decompress_stream_result(
	volatile struct SDKDecompressStreamResultPayload *reply,
	const struct SDKDecompressStreamResult *result)
{
	put_be32(reply->session, result->session);
	put_be32(reply->bytes_consumed, result->bytes_consumed);
	put_be32(reply->bytes_written, result->bytes_written);
	put_be32(reply->checksum, result->checksum);
	put_be32(reply->algorithm, result->algorithm);
	put_be32(reply->flags, result->flags);
}

static uint16_t handle_decompress_stream_begin(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp,
	uint16_t payload_len)
{
	volatile struct SDKDecompressStreamBeginPayload *payload;
	volatile struct SDKDecompressStreamResultPayload *reply;
	struct SDKSharedBuffer *src;
	struct SDKDecompressStreamResult result;
	uint32_t src_offset;
	uint32_t src_length;
	uint32_t output_limit;
	uint32_t algorithm;
	uint32_t flags;
	uint16_t status;
	int feed_input;

	if (payload_len < sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	payload = (volatile struct SDKDecompressStreamBeginPayload *)req->payload;
	src_offset = get_be32(payload->src_offset);
	src_length = get_be32(payload->src_length);
	output_limit = get_be32(payload->output_limit);
	algorithm = get_be32(payload->algorithm);
	flags = get_be32(payload->flags);
	feed_input = (flags & SDK_DECOMPRESS_FLAG_FEED_INPUT) != 0U;

	if (output_limit == 0U)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	if (feed_input) {
		if (get_be32(payload->src_handle) != SDK_INVALID_HANDLE ||
		    src_offset != 0U || src_length != 0U) {
			return complete_status(req, comp,
			                       SDK_STATUS_BAD_REQUEST);
		}
		src = 0;
	} else {
		src = find_shared_buffer(get_be32(payload->src_handle));
		if (!src)
			return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
		if (src_length == 0U ||
		    !buffer_range_valid(src, src_offset, src_length)) {
			return complete_status(req, comp,
			                       SDK_STATUS_BAD_REQUEST);
		}
		Xil_DCacheInvalidateRange((INTPTR)(src->address + src_offset),
		                          src_length);
	}

	status = sdk_decompress_stream_begin(
		algorithm, flags,
		src ? (const uint8_t *)(uintptr_t)(src->address + src_offset) : 0,
		src_length, output_limit, &result);
	if (status != SDK_STATUS_OK)
		return complete_status(req, comp, status);

	write_completion(comp, req, SDK_STATUS_OK, sizeof(*reply));
	memset((void *)comp->payload, 0, sizeof(comp->payload));
	reply = (volatile struct SDKDecompressStreamResultPayload *)
		comp->payload;
	write_decompress_stream_result(reply, &result);
	return SDK_STATUS_OK;
}

static uint16_t handle_decompress_stream_read(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp,
	uint16_t payload_len)
{
	volatile struct SDKDecompressStreamReadPayload *payload;
	volatile struct SDKDecompressStreamResultPayload *reply;
	struct SDKSharedBuffer *dst;
	struct SDKDecompressStreamResult result;
	uint32_t session;
	uint32_t dst_offset;
	uint32_t dst_capacity;
	uint32_t flags;
	uint16_t status;

	if (payload_len < sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	payload = (volatile struct SDKDecompressStreamReadPayload *)req->payload;
	session = get_be32(payload->session);
	dst = find_shared_buffer(get_be32(payload->dst_handle));
	dst_offset = get_be32(payload->dst_offset);
	dst_capacity = get_be32(payload->dst_capacity);
	flags = get_be32(payload->flags);

	if (!dst)
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	if (session == 0U || dst_capacity == 0U || flags != 0U ||
	    !buffer_range_valid(dst, dst_offset, dst_capacity)) {
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	}

	status = sdk_decompress_stream_read(
		session,
		(uint8_t *)(uintptr_t)(dst->address + dst_offset),
		dst_capacity, &result);
	if (status != SDK_STATUS_OK)
		return complete_status(req, comp, status);
	if (result.bytes_written != 0U) {
		Xil_DCacheFlushRange((INTPTR)(dst->address + dst_offset),
		                     result.bytes_written);
	}

	write_completion(comp, req, SDK_STATUS_OK, sizeof(*reply));
	memset((void *)comp->payload, 0, sizeof(comp->payload));
	reply = (volatile struct SDKDecompressStreamResultPayload *)
		comp->payload;
	write_decompress_stream_result(reply, &result);
	return SDK_STATUS_OK;
}

static uint16_t handle_decompress_stream_feed(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp,
	uint16_t payload_len)
{
	volatile struct SDKDecompressStreamFeedPayload *payload;
	volatile struct SDKDecompressStreamResultPayload *reply;
	struct SDKSharedBuffer *src;
	struct SDKDecompressStreamResult result;
	uint32_t session;
	uint32_t src_offset;
	uint32_t src_length;
	uint32_t flags;
	uint16_t status;

	if (payload_len < sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	payload = (volatile struct SDKDecompressStreamFeedPayload *)req->payload;
	session = get_be32(payload->session);
	src = find_shared_buffer(get_be32(payload->src_handle));
	src_offset = get_be32(payload->src_offset);
	src_length = get_be32(payload->src_length);
	flags = get_be32(payload->flags);

	if (!src)
		return complete_status(req, comp, SDK_STATUS_BAD_HANDLE);
	if (session == 0U || src_length == 0U ||
	    (flags & ~SDK_DECOMPRESS_STREAM_FEED_EOF) != 0U ||
	    !buffer_range_valid(src, src_offset, src_length)) {
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);
	}

	Xil_DCacheInvalidateRange((INTPTR)(src->address + src_offset),
	                          src_length);
	status = sdk_decompress_stream_feed(
		session,
		(const uint8_t *)(uintptr_t)(src->address + src_offset),
		src_length, flags, &result);
	if (status != SDK_STATUS_OK)
		return complete_status(req, comp, status);

	write_completion(comp, req, SDK_STATUS_OK, sizeof(*reply));
	memset((void *)comp->payload, 0, sizeof(comp->payload));
	reply = (volatile struct SDKDecompressStreamResultPayload *)
		comp->payload;
	write_decompress_stream_result(reply, &result);
	return SDK_STATUS_OK;
}

static uint16_t handle_decompress_stream_close(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp,
	uint16_t payload_len)
{
	volatile struct SDKDecompressStreamClosePayload *payload;
	uint32_t session;
	uint32_t flags;
	uint16_t status;

	if (payload_len < sizeof(*payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	payload = (volatile struct SDKDecompressStreamClosePayload *)req->payload;
	session = get_be32(payload->session);
	flags = get_be32(payload->flags);
	if (session == 0U || flags != 0U)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	status = sdk_decompress_stream_close(session);
	return complete_status(req, comp, status);
}

static uint16_t handle_diag_read(volatile struct SDKMailboxEntry *req,
                                 volatile struct SDKMailboxEntry *comp,
                                 uint32_t pending_requests)
{
	volatile struct SDKDiagPayload *diag;
	uint32_t free_total;
	uint32_t largest_free;
	uint32_t invalid_slots;

	invalid_slots = sanitize_allocator_metadata();
	heap_free_stats(&free_total, &largest_free);
	write_completion(comp, req, SDK_STATUS_OK, sizeof(*diag));
	memset((void *)comp->payload, 0, sizeof(comp->payload));
	diag = (volatile struct SDKDiagPayload *)comp->payload;
	put_be32(diag->requests_completed, requests_completed);
	put_be32(diag->requests_failed, requests_failed);
	put_be32(diag->last_status, sdk_status);
	put_be32(diag->pending_requests, pending_requests);
	put_be32(diag->shared_buffers_used, count_used_shared_buffers());
	put_be32(diag->shared_heap_total, SDK_SHARED_HEAP_SIZE);
	put_be32(diag->shared_heap_free, free_total);
	put_be32(diag->shared_heap_largest_free, largest_free);
	put_be32(diag->mailbox_arm_addr, SDK_MAILBOX_ADDRESS);
	put_be32(diag->mailbox_ring_entries, SDK_MAILBOX_RING_ENTRIES);
	put_be32(diag->surfaces_used, count_used_surfaces());
	put_be32(diag->allocator_invalid_slots, invalid_slots);
	return SDK_STATUS_OK;
}

static uint16_t handle_diag_timing(volatile struct SDKMailboxEntry *req,
                                   volatile struct SDKMailboxEntry *comp)
{
	volatile struct SDKDiagTimingPayload *timing;

	write_completion(comp, req, SDK_STATUS_OK, sizeof(*timing));
	memset((void *)comp->payload, 0, sizeof(comp->payload));
	timing = (volatile struct SDKDiagTimingPayload *)comp->payload;
	put_be32(timing->version, 1U);
	put_be32(timing->timer_hz, COUNTS_PER_SECOND);
	put_be32(timing->requests_timed, timing_requests_timed);
	put_be32(timing->total_us, timing_total_us);
	put_be32(timing->surface_requests, timing_surface_requests);
	put_be32(timing->surface_us, timing_surface_us);
	put_be32(timing->audio_requests, timing_audio_requests);
	put_be32(timing->audio_us, timing_audio_us);
	put_be32(timing->last_opcode, timing_last_opcode);
	put_be32(timing->last_us, timing_last_us);
	put_be32(timing->max_opcode, timing_max_opcode);
	put_be32(timing->max_us, timing_max_us);
	return SDK_STATUS_OK;
}

static uint16_t handle_diag_sched(volatile struct SDKMailboxEntry *req,
                                  volatile struct SDKMailboxEntry *comp)
{
	volatile struct SDKDiagSchedPayload *sched;
	taskq_shared_t *sh = scheduler_shared();

	write_completion(comp, req, SDK_STATUS_OK, sizeof(*sched));
	memset((void *)comp->payload, 0, sizeof(comp->payload));
	sched = (volatile struct SDKDiagSchedPayload *)comp->payload;
	/* version 2: appends decode_requests/decode_us (Task 7). Readers that
	 * only know version 1 can keep reading the first 16 bytes unchanged. */
	put_be32(sched->version, 2U);
	put_be32(sched->core1_online, scheduler_core1_available() ? 1U : 0U);
	put_be32(sched->tasks_on_core1, sh->tasks_on_core1);
	put_be32(sched->tasks_on_core0, sh->tasks_on_core0);
	put_be32(sched->decode_requests, timing_decode_requests);
	put_be32(sched->decode_us, timing_decode_us);
	return SDK_STATUS_OK;
}

static uint16_t handle_query_aperture_layout(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp)
{
	const struct sdk_aperture_layout *layout;
	volatile struct SDKQueryApertureLayoutPayload *reply;

	if (!aperture_contract_present())
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);
	layout = sdk_aperture_runtime_layout();
	if (!sdk_aperture_layout_validate(layout))
		return complete_status(req, comp, SDK_STATUS_INTERNAL_ERROR);

	write_completion(comp, req, SDK_STATUS_OK, sizeof(*reply));
	memset((void *)comp->payload, 0, sizeof(comp->payload));
	reply = (volatile struct SDKQueryApertureLayoutPayload *)comp->payload;
	put_be32(reply->profile, layout->profile);
	put_be32(reply->aperture_size, layout->aperture_size);
	put_be32(reply->framebuffer_base, layout->framebuffer.base);
	put_be32(reply->framebuffer_size, layout->framebuffer.size);
	put_be32(reply->pip_base, layout->pip.base);
	put_be32(reply->pip_size, layout->pip.size);
	put_be32(reply->template_base, layout->template_scratch.base);
	put_be32(reply->template_size, layout->template_scratch.size);
	put_be32(reply->host_window_base, layout->host_window.base);
	put_be32(reply->host_window_size, layout->host_window.size);
	put_be32(reply->audio_base, layout->audio.base);
	put_be32(reply->audio_size, layout->audio.size);
	return SDK_STATUS_OK;
}

static uint16_t handle_diag_memory(volatile struct SDKMailboxEntry *req,
				   volatile struct SDKMailboxEntry *comp)
{
	const struct sdk_aperture_layout *layout =
		sdk_aperture_runtime_layout();
	volatile struct SDKDiagMemoryPayload *diag;
	uint32_t flags = sdk_aperture_runtime_flags();
	uint32_t host_arm = effective_host_window_address();
	uint32_t host_total = effective_host_window_size();
	uint32_t host_board = 0U;
	uint32_t free_total = 0U;
	uint32_t largest_free = 0U;
	uint32_t invalid_slots;

	invalid_slots = sanitize_allocator_metadata();
	if (host_arm != 0U && host_total != 0U) {
		heap_free_stats_in(host_arm, host_total,
				   &free_total, &largest_free);
		host_board = host_arm - SDK_APERTURE_ARM_ADDRESS_ADJUSTMENT;
	} else if ((flags & SDK_APERTURE_FLAG_VALID) != 0U) {
		host_board = layout->host_window.base;
	}

	write_completion(comp, req, SDK_STATUS_OK, sizeof(*diag));
	memset((void *)comp->payload, 0, sizeof(comp->payload));
	diag = (volatile struct SDKDiagMemoryPayload *)comp->payload;
	put_be32(diag->version, 1U);
	put_be32(diag->layout_state, sdk_aperture_runtime_diag_state());
	put_be32(diag->aperture_size,
		 sdk_aperture_runtime_reported_size());
	put_be32(diag->aperture_info,
		 (flags & SDK_APERTURE_FLAG_VALID) != 0U ?
		 sdk_aperture_layout_info_word(layout) : 0U);
	put_be32(diag->host_window_board_base, host_board);
	put_be32(diag->host_window_arm_base, host_arm);
	put_be32(diag->host_window_total, host_total);
	put_be32(diag->host_window_free, free_total);
	put_be32(diag->host_window_largest_free, largest_free);
	put_be32(diag->host_window_allocations,
		 count_host_window_allocations());
	put_be32(diag->allocator_invalid_slots, invalid_slots);
	return SDK_STATUS_OK;
}

static uint16_t handle_query_service(volatile struct SDKMailboxEntry *req,
                                     volatile struct SDKMailboxEntry *comp,
                                     uint16_t payload_len)
{
	volatile struct SDKQueryServicePayload *query;
	volatile struct SDKServiceInfoPayload *info;
	const struct SDKServiceDescriptor *service;
	uint32_t service_id;

	if (payload_len < 4U)
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	query = (volatile struct SDKQueryServicePayload *)req->payload;
	service_id = get_be32(query->service_id);
	service = find_service(service_id);
	if (!service)
		return complete_status(req, comp, SDK_STATUS_NOT_FOUND);

	write_completion(comp, req, SDK_STATUS_OK, sizeof(*info));
	memset((void *)comp->payload, 0, sizeof(comp->payload));
	info = (volatile struct SDKServiceInfoPayload *)comp->payload;
	put_be32(info->service_id, service->service_id);
	put_be32(info->version, service->version);
	{
		uint32_t capabilities = service->capability_bits;
		if (service->service_id == SDK_SERVICE_CORE)
			capabilities |= mailbox_capability_bits() &
				SDK_CAP_APERTURE_LAYOUT;
		if (service->service_id == SDK_SERVICE_MEMORY)
			capabilities |= mailbox_capability_bits() &
				SDK_CAP_HOST_WINDOW_HEAP;
		put_be32(info->capability_bits, capabilities);
	}
	put_be32(info->flags, service_flags(service));
	put_be32(info->opcode_base, service->opcode_base);
	put_be32(info->opcode_count, service->opcode_count);
	put_be32(info->max_inline_payload, sizeof(req->payload));
	copy_name(info->name, service->name);
	return SDK_STATUS_OK;
}

/*
 * request_id 0 is the firmware-internal marker for deferred scheduler
 * tasks that have no client completion to post: the AX playback refill
 * enqueues with id 0, and sdk_mailbox_post_deferred swallows that
 * completion (and retires the in-flight marker) instead of writing it to
 * the client completion ring. A CLIENT request that carries id 0 and
 * then takes a deferred path collides with that marker -- its completion
 * would be swallowed (hanging the caller) and it would spuriously retire
 * the refill marker -- so only opcodes that can defer to core 1 reserve
 * id 0. Every opcode below routes through post_deferred via one of the
 * deferral helpers (service_try_defer / crypto_dispatch /
 * decompress_dispatch / handle_decompress_batch); keep this list in sync
 * with the handlers that can return SDK_STATUS_QUEUED.
 *
 * Inline-only opcodes (NOP, PING, QUERY_*, ALLOC_*, MEM_*, the surface
 * and audio/decompress control ops, ...) complete synchronously and just
 * echo the client's request_id, so a client whose request counter starts
 * at 0 keeps working -- request_id is an echoed token the ABI never
 * reserved. (zz9k.library's own generator never emits 0 anyway,
 * including on wrap.)
 */
static int opcode_reserves_request_id_zero(uint16_t opcode)
{
	switch (opcode) {
	case SDK_OP_SCALE_IMAGE:
	case SDK_OP_SCALE_IMAGE_CLIPPED:
	case SDK_OP_DECODE_JPEG:
	case SDK_OP_DECODE_MP3:
	case SDK_OP_AUDIO_STREAM_FEED:
	case SDK_OP_AUDIO_STREAM_READ:
	case SDK_OP_IMAGE_SESSION_FEED:
	case SDK_OP_IMAGE_SESSION_CLOSE:
	case SDK_OP_VIDEO_SESSION_WRITE:
	case SDK_OP_VIDEO_SESSION_DECODE:
	case SDK_OP_VIDEO_SESSION_CLOSE:
	case SDK_OP_MEDIA_SESSION_WRITE:
	case SDK_OP_MEDIA_SESSION_DECODE:
	case SDK_OP_MEDIA_SESSION_AUDIO_READ:
	case SDK_OP_MEDIA_SESSION_CLOSE:
	case SDK_OP_DECOMPRESS:
	case SDK_OP_DECOMPRESS_BATCH:
	case SDK_OP_CRYPTO_HASH:
	case SDK_OP_CRYPTO_STREAM:
	case SDK_OP_CRYPTO_AEAD:
	case SDK_OP_CRYPTO_KX:
	case SDK_OP_CRYPTO_VERIFY:
		return 1;
	default:
		return 0;
	}
}

static uint16_t handle_request(volatile struct SDKMailboxEntry *req,
                               volatile struct SDKMailboxEntry *comp,
                               uint32_t pending_requests)
{
	uint16_t opcode = get_be16(req->opcode);
	uint16_t payload_len = get_be16(req->payload_len);

	if (payload_len > sizeof(req->payload))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	/* Reserve request_id 0 only for opcodes that can defer to the core-1
	 * scheduler; inline-only ops accept it as an echoed token (see
	 * opcode_reserves_request_id_zero). */
	if (get_be32(req->request_id) == 0U &&
	    opcode_reserves_request_id_zero(opcode))
		return complete_status(req, comp, SDK_STATUS_BAD_REQUEST);

	switch (opcode) {
	case SDK_OP_NOP:
		write_completion(comp, req, SDK_STATUS_OK, 0);
		return SDK_STATUS_OK;
	case SDK_OP_PING:
		write_completion(comp, req, SDK_STATUS_OK, payload_len);
		copy_payload(comp->payload, req->payload, payload_len);
		return SDK_STATUS_OK;
	case SDK_OP_QUERY_CAPS: {
		volatile struct SDKCapsPayload *caps =
			(volatile struct SDKCapsPayload *)comp->payload;
		write_completion(comp, req, SDK_STATUS_OK, sizeof(*caps));
		memset((void *)comp->payload, 0, sizeof(comp->payload));
		put_be32(caps->magic, SDK_MAILBOX_MAGIC);
		put_be16(caps->abi_major, SDK_MAILBOX_ABI_MAJOR);
		put_be16(caps->abi_minor, SDK_MAILBOX_ABI_MINOR);
		put_be32(caps->capability_bits, mailbox_capability_bits());
		put_be32(caps->max_inline_payload, sizeof(req->payload));
		put_be32(caps->max_shared_buffers, SDK_MAX_SHARED_BUFFERS);
		put_be32(caps->max_surfaces, SDK_MAX_SURFACES + 1U);
		put_be32(caps->firmware_version, 0);
		put_be32(caps->request_ring_entries, SDK_MAILBOX_RING_ENTRIES);
		put_be32(caps->completion_ring_entries, SDK_MAILBOX_RING_ENTRIES);
		put_be32(caps->host_window_heap_size,
		         effective_host_window_size());
		return SDK_STATUS_OK;
	}
	case SDK_OP_QUERY_APERTURE_LAYOUT:
		return handle_query_aperture_layout(req, comp);
	case SDK_OP_QUERY_SERVICE:
		return handle_query_service(req, comp, payload_len);
	case SDK_OP_ALLOC_SHARED:
		return handle_alloc_shared(req, comp, payload_len);
	case SDK_OP_FREE_SHARED:
		return handle_free_shared(req, comp, payload_len);
	case SDK_OP_MEM_FILL:
		return handle_mem_fill(req, comp, payload_len);
	case SDK_OP_MEM_COPY:
		return handle_mem_copy(req, comp, payload_len);
	case SDK_OP_ALLOC_SURFACE:
		return handle_alloc_surface(req, comp, payload_len);
	case SDK_OP_FREE_SURFACE:
		return handle_free_surface(req, comp, payload_len);
	case SDK_OP_MAP_FRAMEBUFFER_SURFACE:
		return handle_map_framebuffer_surface(req, comp);
	case SDK_OP_FILL_SURFACE:
		return handle_fill_surface(req, comp, payload_len);
	case SDK_OP_QUERY_PALETTE:
		return handle_query_palette(req, comp, payload_len);
	case SDK_OP_COPY_SURFACE:
		return handle_copy_surface(req, comp, payload_len);
	case SDK_OP_SCALE_IMAGE:
		return handle_scale_image(req, comp, payload_len);
	case SDK_OP_SCALE_IMAGE_CLIPPED:
		return handle_scale_image_clipped(req, comp, payload_len);
	case SDK_OP_DECODE_JPEG:
		return handle_decode_jpeg(req, comp, payload_len);
	case SDK_OP_DECODE_PNG:
	case SDK_OP_DECODE_GIF:
		return complete_status(req, comp, SDK_STATUS_UNSUPPORTED);
	case SDK_OP_DECODE_MP3:
		return handle_decode_mp3(req, comp, payload_len);
	case SDK_OP_AUDIO_STREAM_BEGIN:
		return handle_audio_stream_begin(req, comp, payload_len);
	case SDK_OP_AUDIO_STREAM_FEED:
		return handle_audio_stream_feed(req, comp, payload_len);
	case SDK_OP_AUDIO_STREAM_READ:
		return handle_audio_stream_read(req, comp, payload_len);
	case SDK_OP_AUDIO_STREAM_CLOSE:
		return handle_audio_stream_close(req, comp, payload_len);
	case SDK_OP_AUDIO_STREAM_PLAY:
		return handle_audio_stream_play(req, comp, payload_len);
	case SDK_OP_AUDIO_STREAM_STOP:
		return handle_audio_stream_stop(req, comp, payload_len);
	case SDK_OP_IMAGE_SESSION_BEGIN:
		return handle_image_session_begin(req, comp, payload_len);
	case SDK_OP_IMAGE_SESSION_FEED:
		return handle_image_session_feed(req, comp, payload_len);
	case SDK_OP_IMAGE_SESSION_CLOSE:
		return handle_image_session_close(req, comp, payload_len);
	case SDK_OP_VIDEO_SESSION_BEGIN:
		return handle_video_session_begin(req, comp, payload_len);
	case SDK_OP_VIDEO_SESSION_WRITE:
		return handle_video_session_write(req, comp, payload_len);
	case SDK_OP_VIDEO_SESSION_DECODE:
	case SDK_OP_VIDEO_SESSION_CLOSE:
		return handle_video_session_simple(req, comp, payload_len, opcode);
	case SDK_OP_MEDIA_SESSION_BEGIN:
		return handle_media_session_begin(req, comp, payload_len);
	case SDK_OP_MEDIA_SESSION_WRITE:
		return handle_media_session_write(req, comp, payload_len);
	case SDK_OP_MEDIA_SESSION_DECODE:
	case SDK_OP_MEDIA_SESSION_CLOSE:
		return handle_media_session_deferred_simple(
			req, comp, payload_len, opcode);
	case SDK_OP_MEDIA_SESSION_PRESENT:
	case SDK_OP_MEDIA_SESSION_DISCARD:
		return handle_media_session_present_or_discard(
			req, comp, payload_len, opcode);
	case SDK_OP_MEDIA_SESSION_STATUS:
		return handle_media_session_status(req, comp, payload_len);
	case SDK_OP_MEDIA_SESSION_AUDIO_READ:
		return handle_media_session_audio_read(
			req, comp, payload_len);
	case SDK_OP_MEDIA_SESSION_AUDIO_BIND:
	case SDK_OP_MEDIA_SESSION_AUDIO_UNBIND:
		return handle_media_session_audio_bind(
			req, comp, payload_len, opcode);
	case SDK_OP_DECOMPRESS:
		return handle_decompress(req, comp, payload_len);
	case SDK_OP_DECOMPRESS_TEST:
		return handle_decompress_test(req, comp, payload_len);
	case SDK_OP_DECOMPRESS_STREAM_BEGIN:
		return handle_decompress_stream_begin(req, comp, payload_len);
	case SDK_OP_DECOMPRESS_STREAM_READ:
		return handle_decompress_stream_read(req, comp, payload_len);
	case SDK_OP_DECOMPRESS_STREAM_FEED:
		return handle_decompress_stream_feed(req, comp, payload_len);
	case SDK_OP_DECOMPRESS_STREAM_CLOSE:
		return handle_decompress_stream_close(req, comp, payload_len);
	case SDK_OP_DECOMPRESS_BATCH:
		return handle_decompress_batch(req, comp, payload_len);
	case SDK_OP_CRYPTO_HASH:
		return handle_crypto_hash(req, comp, payload_len);
	case SDK_OP_CRYPTO_STREAM:
		return handle_crypto_stream(req, comp, payload_len);
	case SDK_OP_CRYPTO_AEAD:
		return handle_crypto_aead(req, comp, payload_len);
	case SDK_OP_CRYPTO_KX:
		return handle_crypto_kx(req, comp, payload_len);
	case SDK_OP_CRYPTO_VERIFY:
		return handle_crypto_verify(req, comp, payload_len);
	case SDK_OP_DIAG_READ:
		return handle_diag_read(req, comp, pending_requests);
	case SDK_OP_DIAG_TIMING:
		return handle_diag_timing(req, comp);
	case SDK_OP_DIAG_SCHED:
		return handle_diag_sched(req, comp);
	case SDK_OP_DIAG_MEMORY:
		return handle_diag_memory(req, comp);
	default:
		write_completion(comp, req, SDK_STATUS_UNSUPPORTED, 0);
		return SDK_STATUS_UNSUPPORTED;
	}
}

void sdk_mailbox_init(void)
{
	volatile struct SDKMailboxDescriptor *desc = descriptor();

	/* Drain any in-flight core-1 task before we tear the mailbox down. A task
	 * still executing on core 1 is mid-write into its resolved data buffers;
	 * the shared-buffer allocator reset below (next_shared_handle = 1 +
	 * memset(shared_buffers)) re-hands those DDR ranges to the next lifetime's
	 * requests, so a late write would corrupt a freshly allocated buffer. The
	 * generation tag stops the stale completion from posting, but only the
	 * quiesce stops the write itself. No-op at cold boot (core 1 not yet up). */
	scheduler_quiesce_for_reset();
	/* internal tasks (video compose) were drained/dropped with the
	 * queue and will never post their deferred completion */
	overlay_scheduler_reset();

	memset((void *)SDK_MAILBOX_ADDRESS, 0, SDK_MAILBOX_TOTAL_SIZE);
	put_be32(desc->magic, SDK_MAILBOX_MAGIC);
	put_be16(desc->abi_major, SDK_MAILBOX_ABI_MAJOR);
	put_be16(desc->abi_minor, SDK_MAILBOX_ABI_MINOR);
	put_be32(desc->descriptor_size, sizeof(*desc));
	put_be32(desc->request_ring_offset, SDK_MAILBOX_REQUEST_OFFSET);
	put_be32(desc->request_ring_entries, SDK_MAILBOX_RING_ENTRIES);
	put_be32(desc->completion_ring_offset, SDK_MAILBOX_COMPLETION_OFFSET);
	put_be32(desc->completion_ring_entries, SDK_MAILBOX_RING_ENTRIES);
	put_be32(desc->capability_bits, mailbox_capability_bits());

	sdk_status = SDK_STATUS_OK;
	sdk_mailbox_active = 0;
	sdk_mailbox_pending = 0;
	/* New mailbox lifetime: any task still queued from the previous one is now
	 * stale and must not post into this mailbox's completion ring. */
	sdk_mailbox_generation++;
	sdk_completion_irq_enabled = 0;
	next_shared_handle = 1;
	next_surface_handle = 1;
	next_audio_stream_id = 1;
	requests_completed = 0;
	requests_failed = 0;
	timing_requests_timed = 0;
	timing_total_us = 0;
	timing_surface_requests = 0;
	timing_surface_us = 0;
	timing_audio_requests = 0;
	timing_audio_us = 0;
	timing_last_opcode = 0;
	timing_last_us = 0;
	timing_max_opcode = 0;
	timing_max_us = 0;
	timing_decode_requests = 0;
	timing_decode_us = 0;
	media_pcm_ring = 0;
	media_pcm_ring_session = 0U;
	memset(shared_buffers, 0, sizeof(shared_buffers));
	memset(surfaces, 0, sizeof(surfaces));
	/* Unbind AX playback before the table goes away. */
	g_audio_playback.session = 0U;
	g_audio_playback.source_kind = AUDIO_PUMP_SOURCE_NONE;
	g_audio_playback.paused = 0U;
	audio_playback_clear_periods(
		g_audio_playback.period_source_bytes, AUDIO_NUM_PERIODS);
	g_audio_playback.refill_pending = 0U;
	/* Pointer into the coherent region, not an array: size explicitly.
	 * Flush so the zeroed table is in DRAM whatever MMU attributes the
	 * lines were filled under (this can run before the scheduler stamps
	 * the coherent attributes on the section). */
	memset(audio_streams, 0,
	       SDK_MAX_AUDIO_STREAMS * sizeof(struct SDKAudioStream));
	Xil_DCacheFlushRange((INTPTR)(uintptr_t)audio_streams,
	                     SDK_MAX_AUDIO_STREAMS *
	                     sizeof(struct SDKAudioStream));
	sdk_decompress_stream_reset_all();
	/* A vanished client (Amiga reboot/crash) can leave core-1-affine image
	 * sessions open with no task in flight; the quiesce above does not
	 * restart core 1 then, and zeroing the session table would strand the
	 * sessions' core-1 heap blocks AND their decode-tracker slots (the
	 * tracker only empties on free or fault reclaim, so repeated resets
	 * would exhaust it and starve future fault recovery). Cold-restart
	 * core 1 first: its reclaim pass frees every tracked block while the
	 * worker is held in reset. */
	if ((sdk_image_stream_has_core1_sessions() ||
	     sdk_video_stream_has_core1_sessions()) &&
	    scheduler_core1_available())
		core1_cold_restart();
	sdk_image_stream_init();
	sdk_video_stream_init();
	sdk_media_session_init();
	amiga_interrupt_clear(AMIGA_INTERRUPT_SDK);

	Xil_DCacheFlushRange(SDK_MAILBOX_ADDRESS, SDK_MAILBOX_TOTAL_SIZE);
	__asm__ __volatile__("dsb" ::: "memory");
}

void sdk_mailbox_activate(void)
{
	sdk_mailbox_active = 1;
}

void sdk_mailbox_doorbell(void)
{
	sdk_mailbox_active = 1;
	sdk_mailbox_pending = 1;
}

void sdk_mailbox_ack_irq(void)
{
	amiga_interrupt_clear(AMIGA_INTERRUPT_SDK);
}

void sdk_mailbox_irq_enable(void)
{
	sdk_completion_irq_enabled = 1;
	amiga_interrupt_clear(AMIGA_INTERRUPT_SDK);
}

void sdk_mailbox_irq_disable(void)
{
	sdk_completion_irq_enabled = 0;
	amiga_interrupt_clear(AMIGA_INTERRUPT_SDK);
}

void sdk_mailbox_task(void)
{
	volatile struct SDKMailboxDescriptor *desc = descriptor();
	volatile struct SDKMailboxEntry *req_ring = request_ring();
	volatile struct SDKMailboxEntry *comp_ring = completion_ring();
	uint32_t req_head;
	uint32_t req_tail;
	uint32_t comp_head;
	uint32_t comp_tail;
	uint32_t next_comp_tail;
	int completed_any = 0;

	if (!sdk_mailbox_active && !sdk_mailbox_pending)
		return;

	req_head = get_be32(desc->request_head);
	req_tail = get_be32(desc->request_tail);
	if (!sdk_mailbox_pending && req_head == req_tail)
		return;

	if (!descriptor_valid(desc)) {
		sdk_mailbox_pending = 0;
		sdk_status = SDK_STATUS_UNSUPPORTED;
		return;
	}

	/*
	 * The Amiga side owns request_tail, completion_head, and request
	 * entries. Do not flush this whole window here: a stale ARM cache line
	 * can overwrite a freshly acknowledged completion_head and make the
	 * completion ring appear full. ARM-owned descriptor/completion writes
	 * are flushed at their write sites.
	 */
	Xil_DCacheInvalidateRange(SDK_MAILBOX_ADDRESS, SDK_MAILBOX_TOTAL_SIZE);
	__asm__ __volatile__("dsb" ::: "memory");

	if (!descriptor_valid(desc)) {
		sdk_mailbox_pending = 0;
		sdk_status = SDK_STATUS_UNSUPPORTED;
		return;
	}

	req_head = get_be32(desc->request_head);
	req_tail = get_be32(desc->request_tail);
	comp_head = get_be32(desc->completion_head);
	comp_tail = get_be32(desc->completion_tail);

	while (req_head != req_tail) {
		uint32_t pending_requests;
		uint32_t opcode;
		XTime timing_start;
		XTime timing_end;

		next_comp_tail = next_index(comp_tail);
		if (next_comp_tail == comp_head) {
			sdk_status = SDK_STATUS_BUSY;
			break;
		}

		if (req_tail >= req_head)
			pending_requests = req_tail - req_head;
		else
			pending_requests = SDK_MAILBOX_RING_ENTRIES - req_head + req_tail;

		opcode = get_be16(req_ring[req_head].opcode);
		/* Gate crypto offload: defer to core 1 only when nothing in this batch
		 * is queued behind the current request (see g_request_is_batch_tail). */
		g_request_is_batch_tail = (pending_requests == 1u) ? 1 : 0;
		XTime_GetTime(&timing_start);
		sdk_status = handle_request(&req_ring[req_head],
		                            &comp_ring[comp_tail],
		                            pending_requests);
		XTime_GetTime(&timing_end);
		record_request_timing(opcode,
		                      timing_delta_us(timing_start,
		                                      timing_end));

		if (sdk_status == SDK_STATUS_QUEUED) {
			/*
			 * Deferred to the core-1 task scheduler. Consume the request but
			 * post no completion now and do NOT advance completion_tail --
			 * scheduler_core0_poll -> sdk_mailbox_post_deferred posts it (and
			 * counts it) when the worker finishes. The reserved comp slot at
			 * comp_tail stays free for that later post; the completion IRQ is
			 * raised then, not now.
			 */
			req_head = next_index(req_head);
			put_be32(desc->request_head, req_head);
			Xil_DCacheFlushRange((INTPTR)desc, sizeof(*desc));
			__asm__ __volatile__("dsb" ::: "memory");
			continue;
		}

		requests_completed++;
		completed_any = 1;
		if (sdk_status != SDK_STATUS_OK)
			requests_failed++;
		Xil_DCacheFlushRange((INTPTR)&comp_ring[comp_tail],
		                     sizeof(comp_ring[comp_tail]));

		req_head = next_index(req_head);
		comp_tail = next_comp_tail;
		put_be32(desc->request_head, req_head);
		put_be32(desc->completion_tail, comp_tail);
		Xil_DCacheFlushRange((INTPTR)desc, sizeof(*desc));
		__asm__ __volatile__("dsb" ::: "memory");
	}

	if (req_head == req_tail)
		sdk_mailbox_pending = 0;

	if (completed_any && sdk_completion_irq_enabled)
		amiga_interrupt_set(AMIGA_INTERRUPT_SDK);
}

/*
 * True while a task whose slot was stamped at enqueue still belongs to the
 * current mailbox lifetime. scheduler_core0_poll calls this before posting a
 * harvested task's completion and drops it if this returns 0 -- so a task that
 * outlived its mailbox never posts a stale request_id into the new one.
 */
int sdk_mailbox_task_gen_ok(int slot)
{
	if (slot < 0 || slot >= (int)TASKQ_CAPACITY)
		return 0;
	return g_task_generation[slot] == sdk_mailbox_generation;
}

/*
 * Post a deferred completion for a task the core-1 scheduler finished. Reuses
 * the exact completion machinery of sdk_mailbox_task -- grab the next slot,
 * fill request_id/opcode/status/user_cookie/payload, flush the entry, advance
 * completion_tail, flush the descriptor, and raise the completion IRQ. Both
 * this and sdk_mailbox_task run only on core 0's main loop (never concurrently),
 * so they share completion_tail safely. Returns 1 if posted, 0 if the
 * completion ring is full (caller leaves the task harvested and retries) or the
 * mailbox is inactive/invalid. Deferred completions carry entry flags = 0;
 * crypto requests never set the entry flags word, so this matches the
 * synchronous path (which echoes the request's zero flags) byte-for-byte.
 */
int sdk_mailbox_post_deferred(uint32_t request_id, uint32_t user_cookie,
                              uint16_t opcode, uint16_t status,
                              const uint8_t *payload, uint16_t payload_len)
{
	volatile struct SDKMailboxDescriptor *desc = descriptor();
	volatile struct SDKMailboxEntry *comp_ring = completion_ring();
	volatile struct SDKMailboxEntry *comp;
	uint32_t comp_head;
	uint32_t comp_tail;
	uint32_t next_comp_tail;

	if (request_id == 0U) {
		/* Internal task: no client completion to post; retire the
		 * producer's in-flight marker, dispatched by opcode. */
		if (opcode == (uint16_t)TASKQ_OP_VIDEO_COMPOSE)
			overlay_compose_retired(status == SDK_STATUS_OK);
		else
			g_audio_playback.refill_pending = 0U; /* AX refill */
		return 1;
	}

	if (!sdk_mailbox_active)
		return 0;

	/* Refresh the Amiga-owned completion_head before checking for room. */
	Xil_DCacheInvalidateRange((INTPTR)desc, sizeof(*desc));
	__asm__ __volatile__("dsb" ::: "memory");

	if (!descriptor_valid(desc))
		return 0;

	comp_head = get_be32(desc->completion_head);
	comp_tail = get_be32(desc->completion_tail);
	next_comp_tail = next_index(comp_tail);
	if (next_comp_tail == comp_head)
		return 0;                    /* ring full -> caller retries next poll */

	comp = &comp_ring[comp_tail];
	put_be32(comp->request_id, request_id);
	put_be16(comp->opcode, opcode);
	put_be16(comp->status, status);
	put_be16(comp->flags, 0);
	put_be16(comp->payload_len, payload_len);
	put_be32(comp->user_cookie, user_cookie);
	if (payload_len > 0U && payload != 0) {
		uint16_t n = payload_len > sizeof(comp->payload)
		                 ? (uint16_t)sizeof(comp->payload) : payload_len;
		memset((void *)comp->payload, 0, sizeof(comp->payload));
		memcpy((void *)comp->payload, payload, n);
	}
	Xil_DCacheFlushRange((INTPTR)comp, sizeof(*comp));

	comp_tail = next_comp_tail;
	put_be32(desc->completion_tail, comp_tail);
	Xil_DCacheFlushRange((INTPTR)desc, sizeof(*desc));
	__asm__ __volatile__("dsb" ::: "memory");

	/* Video decode and overlay composition share core 1. Retire the decoded
	 * frame into the overlay scheduler before raising the client IRQ: the
	 * following overlay_main_poll then places the compose ahead of any next
	 * decode that could overwrite the same P96 bitmap. */
	if (status == SDK_STATUS_OK && payload != 0 &&
	    payload_len >= sizeof(struct SDKVideoSessionResultPayload)) {
		const struct SDKVideoSessionResultPayload *video =
			(const struct SDKVideoSessionResultPayload *)payload;

		if (opcode == SDK_OP_VIDEO_SESSION_DECODE &&
		    (get_be32(video->flags) &
		     SDK_VIDEO_SESSION_RESULT_FRAME_READY) != 0U)
			overlay_video_frame_ready(get_be32(video->session));
		else if (opcode == SDK_OP_VIDEO_SESSION_CLOSE)
			overlay_video_session_closed(get_be32(video->session));
		else if (opcode == SDK_OP_MEDIA_SESSION_CLOSE)
		{
			uint32_t session = get_be32(video->session);

			sdk_media_session_close_retired(session);
			release_media_pcm_ring(session);
		}
	}

	requests_completed++;
	if (status != SDK_STATUS_OK)
		requests_failed++;

	/* Mirror the synchronous path: reflect this completion's result in the
	 * global mailbox status. The offload left sdk_status at the QUEUED sentinel;
	 * without this, REG_ZZ_SDK_DOORBELL / diag last_status would report QUEUED
	 * forever after an offloaded request instead of its real success/error. */
	sdk_status = status;

	if (sdk_completion_irq_enabled)
		amiga_interrupt_set(AMIGA_INTERRUPT_SDK);

	return 1;
}

uint16_t sdk_mailbox_status(void)
{
	return sdk_status;
}

uint32_t sdk_mailbox_address(void)
{
	return SDK_MAILBOX_ADDRESS;
}
