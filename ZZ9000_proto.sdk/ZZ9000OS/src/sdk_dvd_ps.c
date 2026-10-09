/* Bounded MPEG-2 Program Stream/VOB demultiplexer for the U11 experiment.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdk_dvd_ps.h"

#include <string.h>

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint64_t pes_timestamp(const uint8_t *p)
{
	return (((uint64_t)(p[0] & 0x0eU)) << 29) |
	       ((uint64_t)p[1] << 22) |
	       ((uint64_t)(p[2] & 0xfeU) << 14) |
	       ((uint64_t)p[3] << 7) |
	       ((uint64_t)(p[4] & 0xfeU) >> 1);
}

static int valid_timestamp(const uint8_t *p, uint8_t prefix)
{
	return (p[0] >> 4) == prefix && (p[0] & 1U) != 0U &&
	       (p[2] & 1U) != 0U && (p[4] & 1U) != 0U;
}

static void consume(struct SDKDVDPSDemux *d, uint32_t bytes)
{
	if (bytes < d->staged)
		memmove(d->staging, d->staging + bytes, d->staged - bytes);
	d->staged -= bytes;
}

static int queue_packet(struct SDKDVDPSDemux *d, struct SDKDVDPSQueue *q,
                        uint8_t kind, uint8_t stream_id,
                        uint8_t substream_id, uint8_t header_bytes,
                        const uint8_t *payload, uint32_t length,
                        uint64_t pts, uint64_t dts)
{
	struct SDKDVDPSPacket *out;

	if (length > SDK_DVD_PS_PES_MAX) {
		d->malformed_packets++;
		return -1;
	}
	if (q->count == SDK_DVD_PS_QUEUE_DEPTH) {
		d->backpressure_events++;
		return 0;
	}
	out = &q->packet[q->write_index];
	out->pts = pts;
	out->dts = dts;
	out->length = length;
	out->stream_id = stream_id;
	out->substream_id = substream_id;
	out->kind = kind;
	out->header_bytes = header_bytes;
	if (length != 0U)
		memcpy(out->data, payload, length);
	q->write_index = (q->write_index + 1U) % SDK_DVD_PS_QUEUE_DEPTH;
	q->count++;
	return 1;
}

static uint32_t find_start(const uint8_t *p, uint32_t from, uint32_t length)
{
	uint32_t i;

	for (i = from; i + 3U <= length; i++)
		if (p[i] == 0U && p[i + 1U] == 0U && p[i + 2U] == 1U)
			return i;
	return length;
}

/* 0: output queue blocked, 1: consumed a unit, 2: need more input. */
static int parse_one(struct SDKDVDPSDemux *d)
{
	uint8_t *p = d->staging;
	uint8_t sid;
	uint32_t total;
	uint32_t payload_at;
	uint32_t payload_length;
	uint64_t pts = SDK_DVD_PS_NO_TS;
	uint64_t dts = SDK_DVD_PS_NO_TS;
	int queued = 1;

	if (d->staged < 4U)
		return 2;
	if (p[0] != 0U || p[1] != 0U || p[2] != 1U) {
		uint32_t at = find_start(p, 1U, d->staged);
		uint32_t drop = at == d->staged ? d->staged - 2U : at;
		d->bytes_ignored += drop;
		consume(d, drop);
		return 1;
	}
	sid = p[3];
	if (sid == 0xb9U) {
		consume(d, 4U);
		return 1;
	}
	if (sid == 0xbaU) {
		if (d->staged < 12U)
			return 2;
		if ((p[4] & 0xc0U) == 0x40U) {
			if (d->staged < 14U)
				return 2;
			total = 14U + (p[13] & 7U);
		} else if ((p[4] & 0xf0U) == 0x20U) {
			total = 12U;
		} else {
			d->malformed_packets++;
			consume(d, 4U);
			return 1;
		}
		if (d->staged < total)
			return 2;
		consume(d, total);
		return 1;
	}
	if (d->staged < 6U)
		return 2;
	total = 6U + be16(p + 4U);
	if (total == 6U) {
		uint32_t next = find_start(p, 6U, d->staged);
		if (next == d->staged) {
			if (!d->eof)
				return 2;
			total = d->staged;
		} else {
			total = next;
		}
	}
	if (total > SDK_DVD_PS_STAGING_CAPACITY) {
		d->malformed_packets++;
		consume(d, 4U);
		return 1;
	}
	if (d->staged < total)
		return 2;
	if (sid == 0xbbU || sid == 0xbcU || sid == 0xbeU || sid == 0xbfU ||
	    sid == 0xf0U || sid == 0xf1U || sid == 0xffU) {
		d->bytes_ignored += total;
		consume(d, total);
		d->packets_seen++;
		return 1;
	}

	payload_at = 6U;
	if (total >= 9U && (p[6] & 0xc0U) == 0x80U) {
		uint8_t flags = p[7];
		uint32_t optional = p[8];
		payload_at = 9U + optional;
		if (payload_at > total) {
			d->malformed_packets++;
			consume(d, total);
			return 1;
		}
		if ((flags & 0x80U) != 0U) {
			if (optional < 5U || !valid_timestamp(p + 9U,
			    (flags & 0x40U) != 0U ? 3U : 2U)) {
				d->malformed_packets++;
				consume(d, total);
				return 1;
			}
			pts = pes_timestamp(p + 9U);
		}
		if ((flags & 0x40U) != 0U) {
			if (optional < 10U || !valid_timestamp(p + 14U, 1U)) {
				d->malformed_packets++;
				consume(d, total);
				return 1;
			}
			dts = pes_timestamp(p + 14U);
		}
	} else {
		while (payload_at < total && p[payload_at] == 0xffU)
			payload_at++;
		if (payload_at + 1U < total &&
		    (p[payload_at] & 0xc0U) == 0x40U)
			payload_at += 2U;
		if (payload_at < total && (p[payload_at] & 0xf0U) == 0x20U) {
			if (payload_at + 5U > total ||
			    !valid_timestamp(p + payload_at, 2U)) {
				d->malformed_packets++;
				consume(d, total);
				return 1;
			}
			pts = pes_timestamp(p + payload_at);
			payload_at += 5U;
		} else if (payload_at < total &&
		           (p[payload_at] & 0xf0U) == 0x30U) {
			if (payload_at + 10U > total ||
			    !valid_timestamp(p + payload_at, 3U) ||
			    !valid_timestamp(p + payload_at + 5U, 1U)) {
				d->malformed_packets++;
				consume(d, total);
				return 1;
			}
			pts = pes_timestamp(p + payload_at);
			dts = pes_timestamp(p + payload_at + 5U);
			payload_at += 10U;
		} else if (payload_at < total && p[payload_at] == 0x0fU) {
			payload_at++;
		}
	}
	payload_length = total - payload_at;
	if (sid >= 0xe0U && sid <= 0xefU) {
		queued = queue_packet(d, &d->video, SDK_DVD_PS_VIDEO, sid, 0U,
		                      0U, p + payload_at, payload_length, pts, dts);
	} else if (sid >= 0xc0U && sid <= 0xdfU) {
		queued = queue_packet(d, &d->audio, SDK_DVD_PS_MP2, sid, 0U,
		                      0U, p + payload_at, payload_length, pts, dts);
	} else if (sid == 0xbdU && payload_length != 0U) {
		uint8_t sub = p[payload_at];
		if (sub >= 0x80U && sub <= 0x87U) {
			/* One AC-3 track: the first substream seen. Packets of
			 * the others would interleave foreign frames into the
			 * one decoder, so they are skipped. */
			if (d->ac3_substream == 0U)
				d->ac3_substream = sub;
			if (sub != d->ac3_substream) {
				d->bytes_ignored += total;
			} else if (payload_length < 4U) {
				queued = -1;
			} else {
				queued = queue_packet(d, &d->audio, SDK_DVD_PS_AC3,
					sid, sub, 4U, p + payload_at, payload_length,
					pts, dts);
			}
		} else if (sub >= 0xa0U && sub <= 0xa7U) {
			if (payload_length < 7U) queued = -1;
			else queued = queue_packet(d, &d->audio, SDK_DVD_PS_LPCM,
				sid, sub, 7U, p + payload_at, payload_length, pts, dts);
		} else {
			/* 0x20..0x3f subpictures and other private streams are not
			 * part of the experiment; bounded skip is intentional. */
			d->bytes_ignored += total;
		}
	} else {
		d->bytes_ignored += total;
	}
	if (queued == 0)
		return 0;
	if (queued < 0)
		d->malformed_packets++;
	consume(d, total);
	d->packets_seen++;
	return 1;
}

void sdk_dvd_ps_init(struct SDKDVDPSDemux *d)
{
	if (d)
		memset(d, 0, sizeof(*d));
}

int sdk_dvd_ps_write(struct SDKDVDPSDemux *d, const uint8_t *src,
                     uint32_t length, uint32_t *accepted, int eof)
{
	uint32_t used = 0U;
	int state = 1;

	if (accepted)
		*accepted = 0U;
	if (!d || (length != 0U && !src) || d->eof)
		return 0;
	while (used < length) {
		uint32_t room;
		uint32_t copy;

		while ((state = parse_one(d)) == 1) {}
		if (state == 0)
			break;
		room = SDK_DVD_PS_STAGING_CAPACITY - d->staged;
		if (room == 0U)
			break;
		copy = length - used;
		if (copy > room)
			copy = room;
		memcpy(d->staging + d->staged, src + used, copy);
		d->staged += copy;
		used += copy;
	}
	if (used == length && eof)
		d->eof = 1U;
	while ((state = parse_one(d)) == 1) {}
	if (accepted)
		*accepted = used;
	return state != 0 || used != 0U;
}

static void pump(struct SDKDVDPSDemux *d)
{
	while (parse_one(d) == 1) {}
}

static int pop(struct SDKDVDPSDemux *d, struct SDKDVDPSQueue *q,
               struct SDKDVDPSPacket *packet)
{
	if (!d || !q || q->count == 0U)
		return 0;
	if (packet)
		*packet = q->packet[q->read_index];
	q->read_index = (q->read_index + 1U) % SDK_DVD_PS_QUEUE_DEPTH;
	q->count--;
	/* Freeing a queue slot may unblock parsing (bounded queues gate the
	 * staging drain), so re-pump immediately: a pure consumer that never
	 * writes again still drains the stream to completion. */
	pump(d);
	return 1;
}

const struct SDKDVDPSPacket *sdk_dvd_ps_peek_video(
	const struct SDKDVDPSDemux *d)
{
	if (!d || d->video.count == 0U)
		return 0;
	return &d->video.packet[d->video.read_index];
}

const struct SDKDVDPSPacket *sdk_dvd_ps_peek_audio(
	const struct SDKDVDPSDemux *d)
{
	if (!d || d->audio.count == 0U)
		return 0;
	return &d->audio.packet[d->audio.read_index];
}

int sdk_dvd_ps_pop_video(struct SDKDVDPSDemux *d,
                         struct SDKDVDPSPacket *packet)
{
	return pop(d, &d->video, packet);
}

int sdk_dvd_ps_pop_audio(struct SDKDVDPSDemux *d,
                         struct SDKDVDPSPacket *packet)
{
	return pop(d, &d->audio, packet);
}
int sdk_dvd_ps_done(const struct SDKDVDPSDemux *d)
{
	return d && d->eof && d->staged == 0U && d->video.count == 0U &&
	       d->audio.count == 0U;
}
