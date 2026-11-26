/* Bounded MPEG-2 Transport Stream demultiplexer front end for the MPEG-1/2
 * video module. It fills the queues of the Program Stream demux
 * (sdk_dvd_ps.h) with the same SDKDVDPSPacket entries, so the decoder reads
 * a Transport Stream through the Program Stream peek/pop calls.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SDK_DVD_TS_H
#define SDK_DVD_TS_H

#include "sdk_dvd_ps.h"

#include <stdint.h>

#define SDK_DVD_TS_PACKET_BYTES 188U
/* M2TS/BDAV: a 4-byte timestamp prefix before each 188-byte packet. */
#define SDK_DVD_TS_M2TS_PACKET_BYTES 192U
/* One reassembled PES packet, or one chunk of an unbounded one: the queue
 * carries at most SDK_DVD_PS_PES_MAX payload bytes after a 9-byte PES
 * header without optional fields. */
#define SDK_DVD_TS_PES_CAPACITY (SDK_DVD_PS_PES_MAX + 9U)
#define SDK_DVD_TS_SECTION_MAX 1024U
#define SDK_DVD_TS_NO_PID 0xffffU

/* PES reassembly for the one elementary stream followed per kind. */
struct SDKDVDTSStream {
	uint16_t pid;
	uint8_t active;
	uint8_t cc;
	uint8_t cc_valid;
	/* A PES is being collected in data[0..fill). total is its length
	 * from the PES header (start code included), 0 when unbounded. */
	uint8_t collecting;
	/* The buffer holds a finished PES (or a full chunk of an unbounded
	 * one, chunk set) that the queue has not taken yet. */
	uint8_t complete;
	uint8_t chunk;
	/* The buffer starts with a synthetic header: an earlier chunk of
	 * this PES already went out with the timestamps. */
	uint8_t continued;
	uint32_t fill;
	uint32_t total;
	uint8_t data[SDK_DVD_TS_PES_CAPACITY];
};

struct SDKDVDTSDemux {
	struct SDKDVDTSStream video;
	struct SDKDVDTSStream audio;
	/* 188, or 192 for M2TS; 0 until the sync byte pattern is found
	 * (again). sync_offset is where the 0x47 sits inside a packet. */
	uint32_t packet_bytes;
	uint32_t sync_offset;
	/* The packet at the head of the staged input was already handled and
	 * only waits for the queue to take what it completed. */
	uint8_t processed;
	/* Its continuity counter was already checked. */
	uint8_t cc_done;
	uint8_t have_pmt;
	uint16_t pmt_pid;
	uint16_t program;
	uint8_t section_open;
	uint32_t section_fill;
	uint8_t section[SDK_DVD_TS_SECTION_MAX];
	uint32_t sync_losses;
	uint32_t continuity_errors;
	uint32_t tei_packets;
	uint32_t duplicate_packets;
	uint32_t truncated_pes;
};

/* Makes demux parse a Transport Stream: the demux keeps the staging,
 * queues, counters and the audio drop policy, ts the stream state. Both
 * must stay in place while in use. */
void sdk_dvd_ts_init(struct SDKDVDTSDemux *ts, struct SDKDVDPSDemux *demux);

#endif
