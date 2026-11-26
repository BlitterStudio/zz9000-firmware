/* Bounded MPEG-2 Transport Stream demultiplexer front end; see sdk_dvd_ts.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The input is the Program Stream demux's staging area, parsed one 188-byte
 * packet (192 for M2TS) at a time. PAT and PMT select the first program, its
 * first MPEG video PID (stream_type 0x01, 0x02) and first MPEG audio PID
 * (0x03, 0x04). The PES packets of those two PIDs are reassembled and handed
 * to sdk_dvd_ps_handle_pes(), which queues them exactly as a Program Stream
 * PES would be.
 */
#include "sdk_dvd_ts.h"

#include <string.h>

#define TS_SYNC 0x47U
#define TS_PID_PAT 0U
#define TS_PID_NULL 0x1fffU
#define TS_LOCK_CONFIRM 3U
#define TS_PES_HEADER_BYTES 9U

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* CRC-32/MPEG-2 over a PSI section including its CRC field is 0. */
static uint32_t crc32_mpeg(const uint8_t *p, uint32_t length)
{
	uint32_t crc = 0xffffffffU;
	uint32_t i;
	uint32_t bit;

	for (i = 0U; i < length; i++) {
		crc ^= (uint32_t)p[i] << 24;
		for (bit = 0U; bit < 8U; bit++)
			crc = (crc & 0x80000000U) != 0U
				? (crc << 1) ^ 0x04c11db7U : crc << 1;
	}
	return crc;
}

/* ------------------------------------------------------------ PES stream */

static int stream_id_ok(int video, uint8_t sid)
{
	if (video)
		return sid >= 0xe0U && sid <= 0xefU;
	return sid >= 0xc0U && sid <= 0xdfU;
}

static void stream_reset(struct SDKDVDTSStream *s)
{
	s->collecting = 0U;
	s->complete = 0U;
	s->chunk = 0U;
	s->continued = 0U;
	s->fill = 0U;
	s->total = 0U;
}

/* Hands the buffered PES (or chunk) to the queue; 0 when it is full. A
 * chunk leaves a synthetic header without timestamps in the buffer, so the
 * rest of an unbounded PES queues as continuation payload. */
static int stream_flush(struct SDKDVDPSDemux *d, struct SDKDVDTSStream *s)
{
	if (!sdk_dvd_ps_handle_pes(d, s->data, s->fill))
		return 0;
	if (s->chunk) {
		uint8_t sid = s->data[3];

		s->data[0] = 0U;
		s->data[1] = 0U;
		s->data[2] = 1U;
		s->data[3] = sid;
		s->data[4] = 0U;
		s->data[5] = 0U;
		s->data[6] = 0x80U;
		s->data[7] = 0U;
		s->data[8] = 0U;
		s->fill = TS_PES_HEADER_BYTES;
		s->complete = 0U;
		s->chunk = 0U;
		s->continued = 1U;
		return 1;
	}
	stream_reset(s);
	return 1;
}

/* One TS payload of a followed PID. Returns 0 when the queue is full and
 * nothing of this packet was taken yet. */
static int stream_packet(struct SDKDVDPSDemux *d, struct SDKDVDTSDemux *ts,
                         struct SDKDVDTSStream *s, int pusi,
                         const uint8_t *payload, uint32_t length)
{
	if (s->complete && !stream_flush(d, s))
		return 0;
	if (pusi) {
		if (s->collecting) {
			if (s->total == 0U &&
			    !(s->continued && s->fill == TS_PES_HEADER_BYTES)) {
				/* An unbounded PES ends where the next one
				 * starts. */
				s->complete = 1U;
				s->chunk = 0U;
				if (!stream_flush(d, s))
					return 0;
			} else {
				if (s->total != 0U) {
					ts->truncated_pes++;
					d->malformed_packets++;
				}
				stream_reset(s);
			}
		}
		if (length < 6U || payload[0] != 0U || payload[1] != 0U ||
		    payload[2] != 1U) {
			d->malformed_packets++;
			d->bytes_ignored += length;
			return 1;
		}
		if (!stream_id_ok(s == &ts->video, payload[3])) {
			d->bytes_ignored += length;
			return 1;
		}
		s->fill = 0U;
		s->continued = 0U;
		s->total = be16(payload + 4U) != 0U
			? 6U + be16(payload + 4U) : 0U;
		s->collecting = 1U;
	} else if (!s->collecting) {
		d->bytes_ignored += length;
		return 1;
	}
	if (s->total != 0U) {
		if (length > s->total - s->fill)
			length = s->total - s->fill;
	} else if (s->fill + length > SDK_DVD_TS_PES_CAPACITY) {
		s->complete = 1U;
		s->chunk = 1U;
		if (!stream_flush(d, s))
			return 0;
	}
	memcpy(s->data + s->fill, payload, length);
	s->fill += length;
	if (s->total != 0U && s->fill >= s->total)
		s->complete = 1U;
	return 1;
}

/* The input ended: an unbounded PES is complete, a bounded one that did
 * not reach its length can never be. 0 when the queue is full. */
static int stream_end(struct SDKDVDPSDemux *d, struct SDKDVDTSDemux *ts,
                      struct SDKDVDTSStream *s)
{
	if (s->complete && !stream_flush(d, s))
		return 0;
	if (!s->collecting)
		return 1;
	if (s->total != 0U) {
		ts->truncated_pes++;
		d->malformed_packets++;
		stream_reset(s);
		return 1;
	}
	if (s->continued && s->fill == TS_PES_HEADER_BYTES) {
		stream_reset(s);
		return 1;
	}
	s->complete = 1U;
	s->chunk = 0U;
	return stream_flush(d, s);
}

/* ------------------------------------------------------------------- PSI */

static void section_done(struct SDKDVDTSDemux *ts, uint32_t total)
{
	const uint8_t *sec = ts->section;
	uint32_t at;
	uint32_t end;

	if ((sec[1] & 0x80U) == 0U || total < 12U || (sec[5] & 1U) == 0U ||
	    crc32_mpeg(sec, total) != 0U)
		return;
	end = total - 4U;
	if (sec[0] == 0x00U && ts->pmt_pid == SDK_DVD_TS_NO_PID) {
		for (at = 8U; at + 4U <= end; at += 4U) {
			uint16_t pid = be16(sec + at + 2U) & 0x1fffU;

			if (be16(sec + at) != 0U) {
				ts->program = be16(sec + at);
				ts->pmt_pid = pid;
				/* The PAT is no longer watched. */
				ts->section_open = 0U;
				return;
			}
		}
	} else if (sec[0] == 0x02U && ts->pmt_pid != SDK_DVD_TS_NO_PID &&
	           be16(sec + 3U) == ts->program) {
		uint32_t info = be16(sec + 10U) & 0x0fffU;

		for (at = 12U + info; at + 5U <= end;) {
			uint8_t type = sec[at];
			uint16_t pid = be16(sec + at + 1U) & 0x1fffU;
			uint32_t es_info = be16(sec + at + 3U) & 0x0fffU;

			if (at + 5U + es_info > end)
				break;
			at += 5U + es_info;
			if (pid == TS_PID_PAT || pid == TS_PID_NULL ||
			    pid == ts->pmt_pid)
				continue;
			if (!ts->video.active && (type == 0x01U || type == 0x02U)) {
				ts->video.pid = pid;
				ts->video.active = 1U;
			} else if (!ts->audio.active &&
			           (type == 0x03U || type == 0x04U)) {
				ts->audio.pid = pid;
				ts->audio.active = 1U;
			}
		}
		ts->have_pmt = ts->video.active || ts->audio.active;
	}
}

/* Section bytes of the PID being watched (the PAT until a program is
 * chosen, then its PMT). Sections may span packets and share a packet. */
static void section_feed(struct SDKDVDTSDemux *ts, const uint8_t *data,
                         uint32_t length)
{
	while (length != 0U) {
		uint32_t total;
		uint32_t take;

		if (!ts->section_open) {
			if (data[0] == 0xffU)
				return;
			ts->section_open = 1U;
			ts->section_fill = 0U;
		}
		total = ts->section_fill >= 3U
			? 3U + (be16(ts->section + 1U) & 0x0fffU) : 3U;
		take = total - ts->section_fill;
		if (take > length)
			take = length;
		if (ts->section_fill + take > SDK_DVD_TS_SECTION_MAX) {
			ts->section_open = 0U;
			return;
		}
		memcpy(ts->section + ts->section_fill, data, take);
		ts->section_fill += take;
		data += take;
		length -= take;
		if (ts->section_fill < 3U)
			continue;
		total = 3U + (be16(ts->section + 1U) & 0x0fffU);
		if (total > SDK_DVD_TS_SECTION_MAX) {
			ts->section_open = 0U;
			return;
		}
		if (ts->section_fill == total) {
			ts->section_open = 0U;
			section_done(ts, total);
			if (ts->have_pmt)
				return;
		}
	}
}

static void psi_packet(struct SDKDVDTSDemux *ts, int pusi,
                       const uint8_t *payload, uint32_t length)
{
	if (pusi) {
		uint32_t pointer = payload[0];

		if (1U + pointer > length) {
			ts->section_open = 0U;
			return;
		}
		/* The bytes before the pointer finish the previous section. */
		if (ts->section_open)
			section_feed(ts, payload + 1U, pointer);
		ts->section_open = 0U;
		section_feed(ts, payload + 1U + pointer, length - 1U - pointer);
	} else if (ts->section_open) {
		section_feed(ts, payload, length);
	}
}

/* ----------------------------------------------------------- sync search */

/* Do TS_LOCK_CONFIRM sync bytes one packet apart start at i? 1 yes, 0 no,
 * -1 the staged input is too short to tell. At the end of input a run cut
 * short still counts when one whole packet is there. */
static int sync_run(const uint8_t *p, uint32_t avail, uint32_t i,
                    uint32_t size, uint32_t offset, int eof)
{
	uint32_t k;

	for (k = 0U; k < TS_LOCK_CONFIRM; k++) {
		uint32_t at = i + k * size + offset;

		if (at >= avail) {
			if (!eof)
				return -1;
			return k != 0U && i + size <= avail;
		}
		if (p[at] != TS_SYNC)
			return 0;
	}
	return 1;
}

/* Finds the first packet start at or after from. Drops what precedes it.
 * Returns 1 when locked, 2 when more input is needed, 0 when the input
 * holds no packet start (everything was dropped). */
static int lock(struct SDKDVDPSDemux *d, struct SDKDVDTSDemux *ts,
                uint32_t from)
{
	const uint8_t *p = d->staging + d->head;
	uint32_t avail = d->staged - d->head;
	uint32_t i;

	for (i = from; i < avail; i++) {
		int r188 = sync_run(p, avail, i, SDK_DVD_TS_PACKET_BYTES, 0U,
		                    d->eof);
		int r192;

		if (r188 == 1) {
			ts->packet_bytes = SDK_DVD_TS_PACKET_BYTES;
			ts->sync_offset = 0U;
			break;
		}
		r192 = sync_run(p, avail, i, SDK_DVD_TS_M2TS_PACKET_BYTES, 4U,
		                d->eof);
		if (r192 == 1) {
			ts->packet_bytes = SDK_DVD_TS_M2TS_PACKET_BYTES;
			ts->sync_offset = 4U;
			break;
		}
		if (r188 < 0 || r192 < 0) {
			d->bytes_ignored += i;
			sdk_dvd_ps_consume(d, i);
			return 2;
		}
	}
	if (i >= avail) {
		d->bytes_ignored += avail;
		sdk_dvd_ps_consume(d, avail);
		return 0;
	}
	d->bytes_ignored += i;
	sdk_dvd_ps_consume(d, i);
	return 1;
}

/* 1: a packet is at the head of the staged input, 2: more input is needed,
 * 0: the input ended and no further whole packet is staged. */
static int locate(struct SDKDVDPSDemux *d, struct SDKDVDTSDemux *ts)
{
	uint32_t from = 0U;

	for (;;) {
		uint32_t avail = d->staged - d->head;
		int r;

		if (ts->packet_bytes != 0U) {
			if (avail < ts->packet_bytes)
				return d->eof ? 0 : 2;
			if (d->staging[d->head + ts->sync_offset] == TS_SYNC)
				return 1;
			ts->packet_bytes = 0U;
			ts->sync_losses++;
			d->malformed_packets++;
			from = 1U;
		}
		if (avail == 0U)
			return d->eof ? 0 : 2;
		r = lock(d, ts, from);
		if (r != 1)
			return r;
		from = 0U;
	}
}

/* --------------------------------------------------------------- packets */

static struct SDKDVDTSStream *stream_for_pid(struct SDKDVDTSDemux *ts,
                                             uint16_t pid)
{
	if (ts->video.active && ts->video.pid == pid)
		return &ts->video;
	if (ts->audio.active && ts->audio.pid == pid)
		return &ts->audio;
	return 0;
}

/* Handles the packet at the head; 0 when the queue was full before any of
 * it was taken (call again later), 1 once handled. */
static int packet_one(struct SDKDVDPSDemux *d, struct SDKDVDTSDemux *ts)
{
	const uint8_t *t = d->staging + d->head + ts->sync_offset;
	uint16_t pid = (uint16_t)(be16(t + 1U) & 0x1fffU);
	int pusi = (t[1] & 0x40U) != 0U;
	uint32_t control = (t[3] >> 4) & 3U;
	uint32_t at = 4U;
	int discontinuity = 0;
	struct SDKDVDTSStream *s;

	if ((t[1] & 0x80U) != 0U) {
		/* Transport error: nothing in it can be trusted, and the
		 * counter jump after it drops a PES it cut. */
		ts->tei_packets++;
		d->bytes_ignored += SDK_DVD_TS_PACKET_BYTES;
		return 1;
	}
	if ((control & 2U) != 0U) {
		uint32_t field = t[4];

		if (field > 183U) {
			d->malformed_packets++;
			d->bytes_ignored += SDK_DVD_TS_PACKET_BYTES;
			return 1;
		}
		discontinuity = field != 0U && (t[5] & 0x80U) != 0U;
		at = 5U + field;
	}
	s = ts->have_pmt ? stream_for_pid(ts, pid) : 0;
	if ((control & 1U) == 0U) {
		if (discontinuity && s)
			s->cc_valid = 0U;
		d->bytes_ignored += SDK_DVD_TS_PACKET_BYTES;
		return 1;
	}
	if ((t[3] & 0xc0U) != 0U || at >= SDK_DVD_TS_PACKET_BYTES) {
		d->bytes_ignored += SDK_DVD_TS_PACKET_BYTES;
		return 1;
	}
	if (!ts->have_pmt) {
		uint16_t watched = ts->pmt_pid == SDK_DVD_TS_NO_PID
			? TS_PID_PAT : ts->pmt_pid;

		if (pid == watched)
			psi_packet(ts, pusi, t + at, SDK_DVD_TS_PACKET_BYTES - at);
		d->bytes_ignored += SDK_DVD_TS_PACKET_BYTES;
		return 1;
	}
	if (!s) {
		d->bytes_ignored += SDK_DVD_TS_PACKET_BYTES;
		return 1;
	}
	if (!ts->cc_done) {
		uint8_t cc = t[3] & 0x0fU;

		if (discontinuity)
			s->cc_valid = 0U;
		if (s->cc_valid) {
			if (cc == s->cc) {
				ts->duplicate_packets++;
				d->bytes_ignored += SDK_DVD_TS_PACKET_BYTES;
				return 1;
			}
			if (cc != ((s->cc + 1U) & 0x0fU)) {
				/* A packet of this PID was lost: the PES in
				 * progress is cut and cannot be used. */
				ts->continuity_errors++;
				d->malformed_packets++;
				stream_reset(s);
			}
		}
		s->cc = cc;
		s->cc_valid = 1U;
		ts->cc_done = 1U;
	}
	return stream_packet(d, ts, s, pusi, t + at,
	                     SDK_DVD_TS_PACKET_BYTES - at);
}

/* Takes what the packet at the head completed into the queues and drops
 * the packet. At the end of input the last packet also completes the
 * unbounded PES still open. 0 when a queue is full. */
static int packet_finish(struct SDKDVDPSDemux *d, struct SDKDVDTSDemux *ts)
{
	if (ts->video.complete && !stream_flush(d, &ts->video))
		return 0;
	if (ts->audio.complete && !stream_flush(d, &ts->audio))
		return 0;
	if (d->eof && d->staged - d->head < 2U * ts->packet_bytes &&
	    (!stream_end(d, ts, &ts->video) || !stream_end(d, ts, &ts->audio)))
		return 0;
	sdk_dvd_ps_consume(d, ts->packet_bytes);
	ts->processed = 0U;
	ts->cc_done = 0U;
	return 1;
}

static int ts_parse(struct SDKDVDPSDemux *d)
{
	struct SDKDVDTSDemux *ts = (struct SDKDVDTSDemux *)d->parser_state;
	int r;

	if (ts->processed)
		return packet_finish(d, ts);
	r = locate(d, ts);
	if (r == 2)
		return 2;
	if (r == 0) {
		uint32_t tail = d->staged - d->head;

		if (!stream_end(d, ts, &ts->video) ||
		    !stream_end(d, ts, &ts->audio))
			return 0;
		if (tail == 0U)
			return 2;
		/* Bytes short of a packet at the end of input. */
		d->bytes_ignored += tail;
		sdk_dvd_ps_consume(d, tail);
		return 1;
	}
	if (!packet_one(d, ts))
		return 0;
	ts->processed = 1U;
	return packet_finish(d, ts);
}

void sdk_dvd_ts_init(struct SDKDVDTSDemux *ts, struct SDKDVDPSDemux *demux)
{
	memset(ts, 0, sizeof(*ts));
	ts->pmt_pid = SDK_DVD_TS_NO_PID;
	demux->parse = ts_parse;
	demux->parser_state = ts;
}
