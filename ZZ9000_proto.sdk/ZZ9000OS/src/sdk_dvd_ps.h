/* Bounded MPEG-2 Program Stream/VOB demultiplexer for the U11 experiment.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SDK_DVD_PS_H
#define SDK_DVD_PS_H

#include <stddef.h>
#include <stdint.h>

#define SDK_DVD_PS_NO_TS UINT64_C(0xffffffffffffffff)
#define SDK_DVD_PS_PES_MAX 65535U
#define SDK_DVD_PS_STAGING_CAPACITY (SDK_DVD_PS_PES_MAX + 64U)
#define SDK_DVD_PS_QUEUE_DEPTH 4U

enum sdk_dvd_ps_kind {
	SDK_DVD_PS_VIDEO = 1,
	SDK_DVD_PS_MP2 = 2,
	SDK_DVD_PS_AC3 = 3,
	SDK_DVD_PS_LPCM = 4
};

struct SDKDVDPSPacket {
	uint64_t pts;
	uint64_t dts;
	uint32_t length;
	uint8_t stream_id;
	uint8_t substream_id;
	uint8_t kind;
	uint8_t header_bytes;
	uint8_t data[SDK_DVD_PS_PES_MAX];
};

struct SDKDVDPSQueue {
	struct SDKDVDPSPacket packet[SDK_DVD_PS_QUEUE_DEPTH];
	uint32_t read_index;
	uint32_t write_index;
	uint32_t count;
};

struct SDKDVDPSDemux {
	uint8_t staging[SDK_DVD_PS_STAGING_CAPACITY];
	uint32_t staged;
	struct SDKDVDPSQueue video;
	struct SDKDVDPSQueue audio;
	uint64_t packets_seen;
	uint64_t bytes_ignored;
	uint32_t malformed_packets;
	uint32_t backpressure_events;
	uint8_t eof;
};

void sdk_dvd_ps_init(struct SDKDVDPSDemux *demux);
/* Consumes as much input as bounded staging and output queues permit. */
int sdk_dvd_ps_write(struct SDKDVDPSDemux *demux, const uint8_t *src,
                     uint32_t length, uint32_t *accepted, int eof);
const struct SDKDVDPSPacket *sdk_dvd_ps_peek_video(
	const struct SDKDVDPSDemux *demux);
const struct SDKDVDPSPacket *sdk_dvd_ps_peek_audio(
	const struct SDKDVDPSDemux *demux);
int sdk_dvd_ps_pop_video(struct SDKDVDPSDemux *demux,
                         struct SDKDVDPSPacket *packet);
int sdk_dvd_ps_pop_audio(struct SDKDVDPSDemux *demux,
                         struct SDKDVDPSPacket *packet);
int sdk_dvd_ps_done(const struct SDKDVDPSDemux *demux);

#endif
