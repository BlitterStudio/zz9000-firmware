/* Bounded MPEG-2 Program Stream/VOB demultiplexer for the U11 experiment.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdk_dvd_ps.h"
#include "sdk_dvd_lpcm.h"

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
	d->head += bytes;
	if (d->head >= d->staged)
		d->head = d->staged = 0U;
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

/* A unit that needs more bytes: wait for them, or once input has ended
 * (all of it is staged by then) drop the tail, which can never complete,
 * so the demux still drains. */
static int need_more(struct SDKDVDPSDemux *d)
{
	if (!d->eof || d->staged == 0U)
		return 2;
	d->bytes_ignored += d->staged - d->head;
	consume(d, d->staged - d->head);
	return 1;
}

/* 0: output queue blocked, 1: consumed a unit, 2: need more input. */
static int parse_one(struct SDKDVDPSDemux *d)
{
	uint8_t *p = d->staging + d->head;
	uint32_t avail = d->staged - d->head;
	uint8_t sid;
	uint32_t total;
	uint32_t payload_at;
	uint32_t payload_length;
	uint64_t pts = SDK_DVD_PS_NO_TS;
	uint64_t dts = SDK_DVD_PS_NO_TS;
	uint32_t queued_audio_before = d->audio.count;
	int queued = 1;

	if (avail < 4U)
		return need_more(d);
	if (p[0] != 0U || p[1] != 0U || p[2] != 1U) {
		uint32_t at = find_start(p, 1U, avail);
		uint32_t drop = at == avail ? avail - 2U : at;
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
		if (avail < 12U)
			return need_more(d);
		if ((p[4] & 0xc0U) == 0x40U) {
			if (avail < 14U)
				return need_more(d);
			total = 14U + (p[13] & 7U);
		} else if ((p[4] & 0xf0U) == 0x20U) {
			total = 12U;
		} else {
			d->malformed_packets++;
			consume(d, 4U);
			return 1;
		}
		if (avail < total)
			return need_more(d);
		consume(d, total);
		return 1;
	}
	if (avail < 6U)
		return need_more(d);
	total = 6U + be16(p + 4U);
	if (total == 6U) {
		uint32_t next = find_start(p, 6U, avail);
		if (next == avail) {
			if (!d->eof)
				return 2;
			total = avail;
		} else {
			total = next;
		}
	}
	if (total > SDK_DVD_PS_STAGING_CAPACITY) {
		d->malformed_packets++;
		consume(d, 4U);
		return 1;
	}
	if (avail < total)
		return need_more(d);
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
		if (d->video_stream == 0U)
			d->video_stream = sid;
		if (sid != d->video_stream)
			d->bytes_ignored += total;
		else
			queued = queue_packet(d, &d->video, SDK_DVD_PS_VIDEO, sid, 0U,
			                      0U, p + payload_at, payload_length, pts, dts);
	} else if (sid >= 0xc0U && sid <= 0xdfU) {
		if (d->mp2_stream == 0U)
			d->mp2_stream = sid;
		if (sid != d->mp2_stream)
			d->bytes_ignored += total;
		else
			queued = queue_packet(d, &d->audio, SDK_DVD_PS_MP2, sid,
			                      0U, 0U, p + payload_at,
			                      payload_length, pts, dts);
	} else if (sid == 0xbdU && payload_length != 0U) {
		uint8_t sub = p[payload_at];
		if (sub >= 0x80U && sub <= 0x87U) {
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
			if (payload_length < 7U) {
				queued = -1;
			} else if (!sdk_dvd_lpcm_format_supported(p[payload_at + 5U])) {
				d->bytes_ignored += total;
			} else if (d->lpcm_substream == 0U) {
				d->lpcm_substream = sub;
				queued = queue_packet(d, &d->audio, SDK_DVD_PS_LPCM,
				                      sid, sub, 7U, p + payload_at,
				                      payload_length, pts, dts);
			} else if (sub != d->lpcm_substream) {
				d->bytes_ignored += total;
			} else {
				queued = queue_packet(d, &d->audio, SDK_DVD_PS_LPCM,
				                      sid, sub, 7U, p + payload_at,
				                      payload_length, pts, dts);
			}
		} else {
			/* 0x20..0x3f subpictures and other private streams are not
			 * part of the experiment; bounded skip is intentional. */
			d->bytes_ignored += total;
		}
	} else {
		d->bytes_ignored += total;
	}
	if (queued == 0) {
		int audio_packet = (sid >= 0xc0U && sid <= 0xdfU) ||
		                   sid == 0xbdU;

		if (d->drop_audio_when_full && audio_packet) {
			/* The audio queue is full behind a PCM ring the client
			 * is not draining. Drop this packet so the parse (and
			 * the video packets behind it) keeps moving; the owner
			 * only sets the policy while video would otherwise
			 * starve. */
			if (!d->audio_drop_run_active) {
				d->audio_drop_run_active = 1U;
				d->first_dropped_audio_pts = pts;
				d->first_dropped_audio_pts_valid =
					pts != SDK_DVD_PS_NO_TS;
				d->last_dropped_audio_pts_valid = 0U;
			} else if (!d->first_dropped_audio_pts_valid &&
			           pts != SDK_DVD_PS_NO_TS) {
				d->first_dropped_audio_pts = pts;
				d->first_dropped_audio_pts_valid = 1U;
			}
			if (pts != SDK_DVD_PS_NO_TS) {
				d->last_dropped_audio_pts = pts;
				d->last_dropped_audio_pts_valid = 1U;
			}
			d->audio_packets_dropped++;
			d->audio_bytes_dropped += total;
			d->bytes_ignored += total;
			consume(d, total);
			d->packets_seen++;
			return 1;
		}
		return 0;
	}
	if (queued < 0)
		d->malformed_packets++;
	if (d->audio.count != queued_audio_before)
		d->audio_drop_run_active = 0U;
	consume(d, total);
	d->packets_seen++;
	return 1;
}

void sdk_dvd_ps_init(struct SDKDVDPSDemux *d)
{
	if (d)
		memset(d, 0, sizeof(*d));
}

void sdk_dvd_ps_write(struct SDKDVDPSDemux *d, const uint8_t *src,
                      uint32_t length, uint32_t *accepted, int eof)
{
	uint32_t used = 0U;
	int state = 1;

	if (accepted)
		*accepted = 0U;
	if (!d || (length != 0U && !src) || d->eof)
		return;
	while (used < length) {
		uint32_t room;
		uint32_t copy;

		while ((state = parse_one(d)) == 1) {}
		if (state == 0)
			break;
		room = SDK_DVD_PS_STAGING_CAPACITY - d->staged;
		if (room < length - used && d->head != 0U) {
			/* Parsing stopped inside a unit: move just that
			 * unit to the front, once per refill. */
			memmove(d->staging, d->staging + d->head,
			        d->staged - d->head);
			d->staged -= d->head;
			d->head = 0U;
			room = SDK_DVD_PS_STAGING_CAPACITY - d->staged;
		}
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
