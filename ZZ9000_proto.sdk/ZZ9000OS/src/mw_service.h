/*
 * ZZ9000 ARM Firmware Wire-v9 Transport Service
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MW_SERVICE_H
#define MW_SERVICE_H

#include <stdint.h>
#include <stddef.h>
#include "mw_types.h"
#include "mw_ctx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MW_SERVICE_MAGIC            0x4D474C39UL /* 'MGL9' */
#define MW_SERVICE_TRANSPORT_VER    1U
#define MW_SERVICE_WIRE_VER         9U
#define MW_CAP_WIRE_V9              (1U << 18)
#define MW_CAP_LATEST_READBACK_ROWS (1U << 19)


#define MW_DEFAULT_SLOT_CAPACITY_Z3 (256U * 1024U)
#define MW_DEFAULT_SLOT_COUNT_Z3    2U
#define MW_DEFAULT_REPLY_CAPACITY   (64U * 1024U)
#define MW_DEFAULT_SLOT_CAPACITY_Z2 (64U * 1024U)
#define MW_DEFAULT_SLOT_COUNT_Z2    2U
#define MW_DEFAULT_REPLY_CAPACITY_Z2 (16U * 1024U)

/*
 * Transport window geometry (offsets inside the single host-visible heap
 * allocation the firmware binds at runtime; see memorymap.h for placement
 * and docs/minigl-v29-zz9000-transport.md for the contract). The library
 * learns these offsets from the published transport descriptor, never from
 * fixed addresses.
 */
#define MW_WINDOW_CTRL_OFFSET       0x00000000U
#define MW_WINDOW_STATUS_OFFSET     0x00000080U
#define MW_WINDOW_CTRL_PAGE_SIZE    0x00001000U
#define MW_WINDOW_SLOT0_OFFSET      0x00001000U
#define MW_WINDOW_SLOT_STRIDE_Z3    MW_DEFAULT_SLOT_CAPACITY_Z3
#define MW_WINDOW_SLOT_STRIDE_Z2    MW_DEFAULT_SLOT_CAPACITY_Z2
#define MW_WINDOW_REPLY_OFFSET_Z3   \
    (MW_WINDOW_SLOT0_OFFSET + 2U * MW_WINDOW_SLOT_STRIDE_Z3)
#define MW_WINDOW_REPLY_OFFSET_Z2_2SLOT \
    (MW_WINDOW_SLOT0_OFFSET + 2U * MW_WINDOW_SLOT_STRIDE_Z2)
#define MW_WINDOW_REPLY_OFFSET_Z2_1SLOT \
    (MW_WINDOW_SLOT0_OFFSET + MW_WINDOW_SLOT_STRIDE_Z2)
#define MW_WINDOW_SIZE_Z3 \
    (MW_WINDOW_REPLY_OFFSET_Z3 + MW_DEFAULT_REPLY_CAPACITY)
#define MW_WINDOW_SIZE_Z2_2SLOT \
    (MW_WINDOW_REPLY_OFFSET_Z2_2SLOT + MW_DEFAULT_REPLY_CAPACITY_Z2)
#define MW_WINDOW_SIZE_Z2_1SLOT \
    (MW_WINDOW_REPLY_OFFSET_Z2_1SLOT + MW_DEFAULT_REPLY_CAPACITY_Z2)

/*
 * Worker drain guard rails. The window is raw host-visible heap memory: a
 * compliant host can never be more than slot_count ring packets ahead of
 * the firmware, so a gap far beyond that means uninitialized or destroyed
 * control memory. The worker resynchronizes instead of draining a phantom
 * backlog, and one mw_service_poll() call never drains more than
 * MW_POLL_MAX_RING_BATCH ring packets, so a bad transport can never wedge
 * the worker (or starve core 0 / flood the UART) inside a single poll.
 */
#define MW_RING_MAX_OUTSTANDING 1024U
#define MW_POLL_MAX_RING_BATCH    64U
/* Reject diagnostics: log the first reject, then one line per this many. */
#define MW_REJECT_LOG_INTERVAL  1024U

/* Reset diagnostics retain only a bounded packet history: printing remains a
 * reset-only operation because core 0 and the worker share an unarbitrated
 * UART. */
#define MW_PACKET_DIAG_RING_SIZE 8U
typedef struct MWPacketDiagnostic {
    uint32_t sequence;
    uint32_t first_opcode;
    uint32_t command_count;
    uint32_t bytes;
    uint32_t status;
    uint32_t timestamp_ms;
} MWPacketDiagnostic;

/* Control Block: Exactly 128 bytes, 64-byte ownership partition. All
 * multi-byte fields are big-endian wire values (the 68k library reads
 * them directly), so use the mw_wire_* helpers below. */
typedef struct ZZ9KMiniGLControlBlockV9 {
    /* Bytes 0..63: Firmware Ownership */
    uint32_t magic;              /* 0x4D474C39 ('MGL9') */
    uint16_t transport_version;  /* 1 */
    uint16_t wire_version;       /* 9 */
    uint32_t consumer_seq;
    uint32_t completion_fence;
    uint32_t slot_capacity;      /* 256K on Z3, 64K on Z2 */
    uint32_t slot_count;         /* 2 on Z3, 1 on Z2 */
    uint32_t firmware_heartbeat;
    uint32_t op_status;          /* bridge return status (0 ok, etc.) */
    uint32_t op_reply_bytes;     /* bytes written to reply */
    uint32_t op_done_seq;        /* echoed op_seq on completion */
    uint32_t op_reply_handle;    /* context handle returned on create */
    uint8_t  firmware_reserved[20];

    /* Bytes 64..127: Host 68k Ownership (Host writes, firmware reads only) */
    const uint32_t producer_seq;
    const uint32_t active_slot;
    const uint32_t host_generation;
    const uint32_t op_code;      /* 0=submit/none, 1=create, 3=latest readback, 4=destroy, 5=request, 6=blit */
    const uint32_t op_handle;    /* context handle */
    const uint32_t op_arg1;      /* create: width; latest readback: first row; req/blit: req bytes */
    const uint32_t op_arg2;      /* create: height; latest readback: row count; req/blit: desc[0] */
    const uint32_t op_arg3;      /* req/blit: desc[1] (reply cap / span bytes) */
    const uint32_t op_seq;       /* incremented by 68k for each synchronous op */
    const uint8_t  host_reserved[28];
} __attribute__((packed, aligned(64))) ZZ9KMiniGLControlBlockV9;

/* Status Block: Exactly 128 bytes, 64-byte ownership partition. All
 * multi-byte fields are big-endian wire values (the 68k library reads
 * them directly), so use the mw_wire_* helpers below. */
typedef struct ZZ9KMiniGLStatusV9 {
    /* Bytes 0..63: Firmware Ownership */
    uint32_t generation;
    uint32_t error;
    uint32_t error_sequence_hi;
    uint32_t error_sequence_lo;
    uint32_t firmware_epoch;
    uint32_t heartbeat;
    uint32_t capabilities;
    uint32_t trace_frame;
    uint32_t trace_transform_clip_ticks;
    uint32_t trace_raster_ticks;
    uint32_t trace_present_ticks;
    uint32_t trace_idle_ticks;
    uint32_t trace_clock_hz;
    uint32_t trace_draws_hardware;
    uint32_t trace_draws_software;
    uint32_t trace_fallback_reason;

    /* Bytes 64..127: Host Ownership */
    uint32_t host_generation;
    uint32_t producer_epoch;
    uint32_t trace_enabled;
    uint8_t  host_reserved[52];
} __attribute__((packed, aligned(64))) ZZ9KMiniGLStatusV9;

/* Shared control words are aligned. Read or publish each sequence as one
 * volatile word; assembling four independently observable bytes can tear a
 * m68k producer update at a byte boundary. */
static inline uint32_t mw_wire_ld32(const volatile uint32_t *field)
{
	uint32_t value = *field;

	__sync_synchronize();
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return __builtin_bswap32(value);
#else
	return value;
#endif
}

static inline void mw_wire_st32(volatile uint32_t *field, uint32_t value)
{
	__sync_synchronize();
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	*field = __builtin_bswap32(value);
#else
	*field = value;
#endif
}

static inline uint16_t mw_wire_ld16(const volatile uint16_t *field)
{
	const volatile uint8_t *b = (const volatile uint8_t *)field;
	return (uint16_t)(((uint16_t)b[0] << 8) | (uint16_t)b[1]);
}

static inline void mw_wire_st16(volatile uint16_t *field, uint16_t value)
{
	volatile uint8_t *b = (volatile uint8_t *)field;
	b[0] = (uint8_t)(value >> 8);
	b[1] = (uint8_t)value;
}

/*
 * Versioned transport allocation descriptor, published big-endian at a
 * fixed offset inside the SDK mailbox reservation (memorymap.h
 * SDK_MAILBOX_MINIGL_DESC_OFFSET). The MiniGL library discovers the
 * mailbox through REG_ZZ_SDK_MAILBOX_HI/LO, validates this descriptor,
 * and uses the returned addresses verbatim; an all-zero or
 * version-mismatched descriptor means the service is unavailable and the
 * library falls back exactly as on firmware built without the service.
 * Fields are byte arrays with the be32/be16 accessors below, matching the
 * SDK mailbox descriptor conventions for direct 68k reads.
 */
#define MW_TRANSPORT_DESCRIPTOR_MAGIC    0x4D475444UL /* 'MGTD' */
#define MW_TRANSPORT_DESCRIPTOR_VERSION  1U
/* cache_rules bits */
#define MW_TRANSPORT_CACHE_ARM_NONCACHEABLE 0x1U /* firmware maps the window
                                                    * Normal non-cacheable; the
                                                    * 68k still flushes its own
                                                    * cache before doorbells */
/* Native-endian snapshot used inside the firmware and by host tests. */
typedef struct ZZ9KMiniGLTransportDescriptor {
    uint32_t magic;
    uint16_t descriptor_version;
    uint16_t transport_version;
    uint32_t generation;
    uint32_t window_address;
    uint32_t window_bytes;
    uint32_t address_adjust;
    uint32_t ctrl_offset;
    uint32_t status_offset;
    uint32_t slot0_offset;
    uint32_t slot1_offset;
    uint32_t slot_capacity;
    uint32_t slot_count;
    uint32_t reply_offset;
    uint32_t reply_capacity;
    uint32_t required_alignment;
    uint32_t cache_rules;
    uint32_t vram_limit_arm;
} ZZ9KMiniGLTransportDescriptor;

/* Big-endian wire image: 19 fields, 80 bytes total. */
#define MW_TRANSPORT_DESCRIPTOR_WIRE_BYTES 80U
typedef struct ZZ9KMiniGLTransportDescriptorWire {
    uint8_t magic[4];
    uint8_t descriptor_version[2];
    uint8_t transport_version[2];
    uint8_t generation[4];
    uint8_t window_address[4];
    uint8_t window_bytes[4];
    uint8_t address_adjust[4];
    uint8_t ctrl_offset[4];
    uint8_t status_offset[4];
    uint8_t slot0_offset[4];
    uint8_t slot1_offset[4];
    uint8_t slot_capacity[4];
    uint8_t slot_count[4];
    uint8_t reply_offset[4];
    uint8_t reply_capacity[4];
    uint8_t required_alignment[4];
    uint8_t cache_rules[4];
    uint8_t vram_limit_arm[4];
    uint8_t reserved[16];
} ZZ9KMiniGLTransportDescriptorWire;


static inline void mw_put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}


static inline uint16_t mw_get_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* Serialize/deserialize between the native snapshot and the wire image. */
void mw_service_serialize_transport_descriptor(
    const ZZ9KMiniGLTransportDescriptor *desc,
    ZZ9KMiniGLTransportDescriptorWire *out);
void mw_service_deserialize_transport_descriptor(
    const ZZ9KMiniGLTransportDescriptorWire *wire,
    ZZ9KMiniGLTransportDescriptor *out);
int  mw_service_bind_region(volatile void *region, uint32_t size);
void mw_service_init(void);
void mw_service_doorbell(void);
int  mw_service_poll(void);
int  mw_service_has_work(void);
void mw_service_reset(void);
/* Print the bounded packet history before reset tears down a live context. */
void mw_service_report_amiga_reset(void);
void mw_service_reset_local(void);
void mw_service_on_core1_fault(void);
int  mw_service_is_bound(void);
/* Snapshot the current binding into a host-endian descriptor record.
 * Returns 0 and fills `out` when bound; nonzero when unavailable. */
int  mw_service_get_transport_descriptor(ZZ9KMiniGLTransportDescriptor *out);
/* ARM DDR limit for BLIT/CAPTURE spans. Firmware-side mirror of the
 * descriptor's vram_limit_arm. */
void mw_service_set_vram_limit_arm(uint32_t limit_arm);
uint32_t mw_service_transport_window_base(void);
uint32_t mw_service_transport_window_size(void);
#if defined(MINIGL_HOST_TEST)
void mw_service_set_host_vram(void *base, uint32_t limit_arm);
#endif
/* Number of reject diagnostics the service has emitted (throttled by
 * MW_REJECT_LOG_INTERVAL); observable so host tests can prove a reject
 * storm cannot flood the UART. */
uint32_t mw_service_reject_log_events(void);
/* Oldest-to-newest packet summaries, for the host mapper/service tests. */
size_t mw_service_packet_diagnostics(MWPacketDiagnostic *out, size_t capacity,
                                     uint32_t *packets_processed);

#ifdef __cplusplus
}
#endif

#endif /* MW_SERVICE_H */
