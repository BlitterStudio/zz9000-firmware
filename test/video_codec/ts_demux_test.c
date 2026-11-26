/*
 * MPEG Transport Stream demux host tests (sdk_dvd_ts.c over sdk_dvd_ps.c).
 *
 * The real streams are ffmpeg output: the same encode muxed as a Program
 * Stream and as a Transport Stream (also M2TS) must give the same video and
 * audio elementary streams and the same video PTS. The synthetic streams
 * built here cover what ffmpeg does not write: PES larger than 64 KiB, a
 * missing PMT, damaged sync bytes, continuity gaps, transport errors,
 * duplicates, backpressure and truncation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_dvd_ps.h"
#include "sdk_dvd_ts.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ts_mpeg2_ps_fixture.inc"
#include "ts_mpeg2_ts_fixture.inc"
#include "ts_mpeg2_m2ts_fixture.inc"
#include "ts_mpeg1_ps_fixture.inc"
#include "ts_mpeg1_ts_fixture.inc"
#include "ts_mpeg2_2a_ps_fixture.inc"
#include "ts_mpeg2_2a_ts_fixture.inc"

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, \
			        __LINE__, #cond); \
			return 1; \
		} \
	} while (0)

#define NO_TS SDK_DVD_PS_NO_TS
#define MAX_PTS 1024U
#define PMT_PID 0x100U
#define V_PID 0x101U
#define A_PID 0x102U
#define A2_PID 0x103U
#define ALL_POPS 0xffffffffU

/* ----------------------------------------------------------- collection */

struct bytes {
	uint8_t *data;
	uint32_t len;
	uint32_t cap;
};

struct out {
	struct bytes video;
	struct bytes audio;
	uint64_t vpts[MAX_PTS];
	uint64_t apts[MAX_PTS];
	uint32_t nv;
	uint32_t na;
	uint32_t max_packet;
	uint8_t video_stream;
	uint8_t audio_stream;
	uint64_t bytes_ignored;
	uint32_t malformed;
	uint32_t audio_dropped;
	uint32_t backpressure;
	uint32_t sync_losses;
	uint32_t continuity_errors;
	uint32_t tei_packets;
	uint32_t duplicate_packets;
	uint32_t truncated_pes;
	uint32_t packet_bytes;
	int done;
	int stalled;
};

static void bytes_add(struct bytes *b, const uint8_t *p, uint32_t n)
{
	if (b->len + n > b->cap) {
		b->cap = (b->len + n) * 2U + 4096U;
		b->data = (uint8_t *)realloc(b->data, b->cap);
	}
	memcpy(b->data + b->len, p, n);
	b->len += n;
}

static void out_free(struct out *o)
{
	free(o->video.data);
	free(o->audio.data);
	memset(o, 0, sizeof(*o));
}

static uint64_t fnv(const uint8_t *p, uint32_t n)
{
	uint64_t h = UINT64_C(14695981039346656037);
	uint32_t i;

	for (i = 0U; i < n; i++) {
		h ^= p[i];
		h *= UINT64_C(1099511628211);
	}
	return h;
}

/* Takes up to pops packets out of the queues. */
static uint32_t drain(struct SDKDVDPSDemux *d, struct out *o, uint32_t pops)
{
	uint32_t n = 0U;

	while (n < pops) {
		const struct SDKDVDPSPacket *p = sdk_dvd_ps_peek_video(d);

		if (p) {
			bytes_add(&o->video, p->data, p->length);
			if (o->nv < MAX_PTS)
				o->vpts[o->nv++] = p->pts;
			if (p->length > o->max_packet)
				o->max_packet = p->length;
			o->video_stream = p->stream_id;
			sdk_dvd_ps_pop_video(d, 0);
			n++;
			continue;
		}
		p = sdk_dvd_ps_peek_audio(d);
		if (!p)
			break;
		bytes_add(&o->audio, p->data, p->length);
		if (o->na < MAX_PTS)
			o->apts[o->na++] = p->pts;
		if (p->length > o->max_packet)
			o->max_packet = p->length;
		o->audio_stream = p->stream_id;
		sdk_dvd_ps_pop_audio(d, 0);
		n++;
	}
	return n;
}

/* Feeds data in chunk-sized writes, taking at most pops packets after each,
 * then signals the end of input and drains to completion. */
static void run(const uint8_t *data, uint32_t len, uint32_t chunk,
                uint32_t pops, int ts, int drop_audio, struct out *o)
{
	struct SDKDVDPSDemux *d = (struct SDKDVDPSDemux *)calloc(1, sizeof(*d));
	struct SDKDVDTSDemux *t = (struct SDKDVDTSDemux *)calloc(1, sizeof(*t));
	uint32_t offset = 0U;
	uint32_t accepted;
	uint32_t guard = 0U;

	memset(o, 0, sizeof(*o));
	sdk_dvd_ps_init(d);
	if (ts)
		sdk_dvd_ts_init(t, d);
	d->drop_audio_when_full = (uint8_t)drop_audio;
	while (offset < len && guard++ < 10000000U) {
		uint32_t c = chunk < len - offset ? chunk : len - offset;

		sdk_dvd_ps_write(d, data + offset, c, &accepted, 0);
		offset += accepted;
		if (drain(d, o, pops) == 0U && accepted == 0U) {
			o->stalled = 1;
			break;
		}
	}
	if (!o->stalled) {
		sdk_dvd_ps_write(d, 0, 0U, &accepted, 1);
		while (!sdk_dvd_ps_done(d) && guard++ < 10000000U)
			if (drain(d, o, pops) == 0U)
				break;
	}
	o->done = sdk_dvd_ps_done(d);
	o->bytes_ignored = d->bytes_ignored;
	o->malformed = d->malformed_packets;
	o->audio_dropped = d->audio_packets_dropped;
	o->backpressure = d->backpressure_events;
	if (ts) {
		o->sync_losses = t->sync_losses;
		o->continuity_errors = t->continuity_errors;
		o->tei_packets = t->tei_packets;
		o->duplicate_packets = t->duplicate_packets;
		o->truncated_pes = t->truncated_pes;
		o->packet_bytes = t->packet_bytes;
	}
	free(d);
	free(t);
}

static const uint32_t chunk_sizes[] = {1U, 7U, 188U, 192U, 1000U, 4093U,
                                       70000U};
static const uint32_t pop_modes[] = {ALL_POPS, 1U};

/* Runs a stream through every write size and pop cadence; check() judges
 * each result. */
typedef int (*check_fn)(const struct out *o, void *ctx);

static int each_config(const uint8_t *data, uint32_t len, check_fn check,
                       void *ctx)
{
	uint32_t i;
	uint32_t j;

	for (i = 0U; i < sizeof(chunk_sizes) / sizeof(chunk_sizes[0]); i++)
		for (j = 0U; j < sizeof(pop_modes) / sizeof(pop_modes[0]); j++) {
			struct out o;
			int r;

			run(data, len, chunk_sizes[i], pop_modes[j], 1, 0, &o);
			r = check(&o, ctx);
			if (r != 0)
				fprintf(stderr, "  chunk %u pops %u\n", chunk_sizes[i],
				        pop_modes[j]);
			out_free(&o);
			if (r != 0)
				return r;
		}
	return 0;
}

/* -------------------------------------------------------- real streams */

static int check_equivalent(const struct out *ps, const struct out *ts,
                            const char *what)
{
	uint32_t i;
	uint32_t pv = 0U;
	uint32_t tv = 0U;
	uint64_t ps_first = NO_TS;
	uint64_t ts_first = NO_TS;

	if (ps->video.len != ts->video.len ||
	    fnv(ps->video.data, ps->video.len) !=
	        fnv(ts->video.data, ts->video.len)) {
		fprintf(stderr, "%s: video ES differs (PS %u bytes, TS %u)\n", what,
		        ps->video.len, ts->video.len);
		return 1;
	}
	if (ps->audio.len != ts->audio.len ||
	    fnv(ps->audio.data, ps->audio.len) !=
	        fnv(ts->audio.data, ts->audio.len)) {
		fprintf(stderr, "%s: audio ES differs (PS %u bytes, TS %u)\n", what,
		        ps->audio.len, ts->audio.len);
		return 1;
	}
	/* Video PTS: the muxers start at different offsets, the spacing and
	 * the audio start relative to the first picture must agree. */
	for (i = 0U; i < ps->nv; i++)
		if (ps->vpts[i] != NO_TS) {
			if (ps_first == NO_TS)
				ps_first = ps->vpts[i];
			pv++;
		}
	for (i = 0U; i < ts->nv; i++)
		if (ts->vpts[i] != NO_TS) {
			if (ts_first == NO_TS)
				ts_first = ts->vpts[i];
			tv++;
		}
	/* A Program Stream packet holds several small pictures and stamps only
	 * the first; a Transport Stream PES holds one and stamps each. Every
	 * Program Stream stamp must be one of the Transport Stream's, and the
	 * Transport Stream stamps must run in order. */
	if (pv == 0U || tv < pv) {
		fprintf(stderr, "%s: video PTS count PS %u TS %u\n", what, pv, tv);
		return 1;
	}
	for (i = 0U; i < ps->nv; i++) {
		uint32_t j;
		int found = 0;

		if (ps->vpts[i] == NO_TS)
			continue;
		for (j = 0U; j < ts->nv; j++)
			if (ts->vpts[j] != NO_TS &&
			    ts->vpts[j] - ts_first == ps->vpts[i] - ps_first)
				found = 1;
		if (!found) {
			fprintf(stderr, "%s: video PTS %u missing in TS\n", what, i);
			return 1;
		}
	}
	for (i = 1U; i < ts->nv; i++)
		if (ts->vpts[i] <= ts->vpts[i - 1U]) {
			fprintf(stderr, "%s: video PTS not increasing\n", what);
			return 1;
		}
	if (ps->na == 0U || ts->na == 0U || ps->apts[0] == NO_TS ||
	    ts->apts[0] == NO_TS ||
	    ps->apts[0] - ps_first != ts->apts[0] - ts_first) {
		fprintf(stderr, "%s: first audio PTS relative to video differs\n",
		        what);
		return 1;
	}
	if (ts->malformed != 0U || ts->sync_losses != 0U ||
	    ts->continuity_errors != 0U || ts->tei_packets != 0U ||
	    ts->truncated_pes != 0U) {
		fprintf(stderr, "%s: clean stream reported damage\n", what);
		return 1;
	}
	return 0;
}

static int test_fixture_pair(const uint8_t *ps, uint32_t ps_len,
                             const uint8_t *ts, uint32_t ts_len,
                             uint32_t packet_bytes, const char *what)
{
	uint32_t i;
	uint32_t j;

	for (i = 0U; i < sizeof(chunk_sizes) / sizeof(chunk_sizes[0]); i++)
		for (j = 0U; j < sizeof(pop_modes) / sizeof(pop_modes[0]); j++) {
			struct out po;
			struct out to;
			int r;

			run(ps, ps_len, chunk_sizes[i], pop_modes[j], 0, 0, &po);
			run(ts, ts_len, chunk_sizes[i], pop_modes[j], 1, 0, &to);
			r = !po.done || !to.done ||
			    to.packet_bytes != packet_bytes ||
			    po.video_stream != to.video_stream ||
			    po.audio_stream != to.audio_stream ||
			    check_equivalent(&po, &to, what);
			if (r)
				fprintf(stderr,
				        "  %s chunk %u pops %u (done %d/%d, packet %u, "
				        "streams %02x/%02x %02x/%02x)\n", what,
				        chunk_sizes[i], pop_modes[j], po.done, to.done,
				        to.packet_bytes, po.video_stream, to.video_stream,
				        po.audio_stream, to.audio_stream);
			out_free(&po);
			out_free(&to);
			if (r)
				return 1;
		}
	return 0;
}

static int test_real_streams(void)
{
	CHECK(test_fixture_pair(zz9k_ts_mpeg2_ps_fixture,
	                        zz9k_ts_mpeg2_ps_fixture_len,
	                        zz9k_ts_mpeg2_ts_fixture,
	                        zz9k_ts_mpeg2_ts_fixture_len, 188U,
	                        "mpeg2 ts") == 0);
	CHECK(test_fixture_pair(zz9k_ts_mpeg2_ps_fixture,
	                        zz9k_ts_mpeg2_ps_fixture_len,
	                        zz9k_ts_mpeg2_m2ts_fixture,
	                        zz9k_ts_mpeg2_m2ts_fixture_len, 192U,
	                        "mpeg2 m2ts") == 0);
	CHECK(test_fixture_pair(zz9k_ts_mpeg1_ps_fixture,
	                        zz9k_ts_mpeg1_ps_fixture_len,
	                        zz9k_ts_mpeg1_ts_fixture,
	                        zz9k_ts_mpeg1_ts_fixture_len, 188U,
	                        "mpeg1 ts") == 0);
	/* Two audio tracks: both demuxers follow the first. */
	CHECK(test_fixture_pair(zz9k_ts_mpeg2_2a_ps_fixture,
	                        zz9k_ts_mpeg2_2a_ps_fixture_len,
	                        zz9k_ts_mpeg2_2a_ts_fixture,
	                        zz9k_ts_mpeg2_2a_ts_fixture_len, 188U,
	                        "two audio") == 0);
	/* The first track of the two-track stream is the one-track stream's. */
	{
		struct out one;
		struct out two;

		run(zz9k_ts_mpeg2_ts_fixture, zz9k_ts_mpeg2_ts_fixture_len, 188U,
		    ALL_POPS, 1, 0, &one);
		run(zz9k_ts_mpeg2_2a_ts_fixture, zz9k_ts_mpeg2_2a_ts_fixture_len,
		    188U, ALL_POPS, 1, 0, &two);
		CHECK(one.audio.len != 0U && one.audio.len == two.audio.len);
		CHECK(memcmp(one.audio.data, two.audio.data, one.audio.len) == 0);
		CHECK(two.audio_stream == 0xc0U);
		out_free(&one);
		out_free(&two);
	}
	return 0;
}

/* ----------------------------------------------------- stream builder */

struct tsb {
	uint8_t *data;
	uint32_t len;
	uint32_t cap;
	uint32_t stride;
	uint8_t cc[0x2000];
};

static void tsb_init(struct tsb *b, uint32_t stride)
{
	memset(b, 0, sizeof(*b));
	b->stride = stride;
}

static void tsb_free(struct tsb *b)
{
	free(b->data);
}

static uint32_t tsb_count(const struct tsb *b)
{
	return b->len / b->stride;
}

static uint8_t *tsb_at(const struct tsb *b, uint32_t index)
{
	return b->data + (size_t)index * b->stride + (b->stride - 188U);
}

/* One packet; payload n <= 184, the rest stuffed through the adaptation
 * field. cc_step 0 repeats the counter (a duplicate-style packet). */
static void tsb_packet(struct tsb *b, uint32_t pid, int pusi,
                       const uint8_t *payload, uint32_t n)
{
	uint8_t *p;
	uint32_t at = 4U;

	if (b->len + b->stride > b->cap) {
		b->cap = (b->len + b->stride) * 2U;
		b->data = (uint8_t *)realloc(b->data, b->cap);
	}
	p = b->data + b->len;
	memset(p, 0, b->stride);
	if (b->stride == 192U) {
		p[0] = 0x00U;
		p[1] = 0x01U;
		p[2] = 0x02U;
		p[3] = 0x03U;
		p += 4;
	}
	p[0] = 0x47U;
	p[1] = (uint8_t)((pusi ? 0x40U : 0U) | (pid >> 8));
	p[2] = (uint8_t)pid;
	if (n < 184U) {
		uint32_t field = 183U - n;

		p[3] = (uint8_t)(0x30U | (b->cc[pid] & 15U));
		p[4] = (uint8_t)field;
		if (field != 0U) {
			p[5] = 0U;
			memset(p + 6, 0xff, field - 1U);
		}
		at = 5U + field;
	} else {
		p[3] = (uint8_t)(0x10U | (b->cc[pid] & 15U));
	}
	b->cc[pid]++;
	memcpy(p + at, payload, n);
	b->len += b->stride;
}

/* An adaptation-only packet (PCR), which carries no payload. */
static void tsb_adaptation_only(struct tsb *b, uint32_t pid, int discontinuity)
{
	uint8_t payload[1] = {0};
	uint32_t before = b->len;
	uint8_t *p;

	tsb_packet(b, pid, 0, payload, 0U);
	b->cc[pid]--;
	p = b->data + before + (b->stride - 188U);
	p[3] = (uint8_t)(0x20U | (b->cc[pid] & 15U));
	p[4] = 183U;
	p[5] = discontinuity ? 0x90U : 0x10U;
}

static uint32_t crc32_mpeg(const uint8_t *p, uint32_t n)
{
	uint32_t crc = 0xffffffffU;
	uint32_t i;
	int bit;

	for (i = 0U; i < n; i++) {
		crc ^= (uint32_t)p[i] << 24;
		for (bit = 0; bit < 8; bit++)
			crc = (crc & 0x80000000U) ? (crc << 1) ^ 0x04c11db7U
			                          : crc << 1;
	}
	return crc;
}

static void section_finish(uint8_t *s, uint32_t body_end)
{
	uint32_t crc;

	s[1] = (uint8_t)(0xb0U | (((body_end + 4U - 3U) >> 8) & 15U));
	s[2] = (uint8_t)(body_end + 4U - 3U);
	crc = crc32_mpeg(s, body_end);
	s[body_end] = (uint8_t)(crc >> 24);
	s[body_end + 1U] = (uint8_t)(crc >> 16);
	s[body_end + 2U] = (uint8_t)(crc >> 8);
	s[body_end + 3U] = (uint8_t)crc;
}

static uint32_t make_pat(uint8_t *s, const uint16_t *program,
                         const uint16_t *pid, uint32_t n)
{
	uint32_t at = 8U;
	uint32_t i;

	s[0] = 0x00U;
	s[3] = 0x00U;
	s[4] = 0x01U;
	s[5] = 0xc1U;
	s[6] = 0U;
	s[7] = 0U;
	for (i = 0U; i < n; i++) {
		s[at++] = (uint8_t)(program[i] >> 8);
		s[at++] = (uint8_t)program[i];
		s[at++] = (uint8_t)(0xe0U | (pid[i] >> 8));
		s[at++] = (uint8_t)pid[i];
	}
	section_finish(s, at);
	return at + 4U;
}

struct es {
	uint8_t type;
	uint16_t pid;
	uint32_t info; /* ES_info descriptor bytes */
};

static uint32_t make_pmt(uint8_t *s, uint16_t program, const struct es *es,
                         uint32_t n, uint32_t program_info)
{
	uint32_t at = 12U;
	uint32_t i;
	uint32_t k;

	s[0] = 0x02U;
	s[3] = (uint8_t)(program >> 8);
	s[4] = (uint8_t)program;
	s[5] = 0xc1U;
	s[6] = 0U;
	s[7] = 0U;
	s[8] = 0xe1U;
	s[9] = 0x01U;
	s[10] = (uint8_t)(0xf0U | (program_info >> 8));
	s[11] = (uint8_t)program_info;
	for (k = 0U; k < program_info; k++)
		s[at++] = (uint8_t)(0x80U + (k & 0x3fU));
	for (i = 0U; i < n; i++) {
		s[at++] = es[i].type;
		s[at++] = (uint8_t)(0xe0U | (es[i].pid >> 8));
		s[at++] = (uint8_t)es[i].pid;
		s[at++] = (uint8_t)(0xf0U | (es[i].info >> 8));
		s[at++] = (uint8_t)es[i].info;
		for (k = 0U; k < es[i].info; k++)
			s[at++] = (uint8_t)(0x80U + (k & 0x3fU));
	}
	section_finish(s, at);
	return at + 4U;
}

/* A PSI section over as many packets as it needs. */
static void tsb_section(struct tsb *b, uint32_t pid, const uint8_t *sec,
                        uint32_t n)
{
	uint8_t buf[184];
	uint32_t first = n < 183U ? n : 183U;
	uint32_t sent = first;

	buf[0] = 0U;
	memcpy(buf + 1, sec, first);
	tsb_packet(b, pid, 1, buf, first + 1U);
	while (sent < n) {
		uint32_t part = n - sent < 184U ? n - sent : 184U;

		tsb_packet(b, pid, 0, sec + sent, part);
		sent += part;
	}
}

static void tsb_psi(struct tsb *b)
{
	uint8_t sec[SDK_DVD_TS_SECTION_MAX];
	uint16_t program = 1U;
	uint16_t pid = PMT_PID;
	struct es es[2] = {{0x02U, V_PID, 0U}, {0x03U, A_PID, 0U}};

	tsb_section(b, 0U, sec, make_pat(sec, &program, &pid, 1U));
	tsb_section(b, PMT_PID, sec, make_pmt(sec, 1U, es, 2U, 0U));
}

static void pts5(uint8_t *p, uint8_t prefix, uint64_t pts)
{
	p[0] = (uint8_t)((prefix << 4) | (((pts >> 30) & 7U) << 1) | 1U);
	p[1] = (uint8_t)(pts >> 22);
	p[2] = (uint8_t)((((pts >> 15) & 0x7fU) << 1) | 1U);
	p[3] = (uint8_t)(pts >> 7);
	p[4] = (uint8_t)(((pts & 0x7fU) << 1) | 1U);
}

static void fill_es(uint8_t *p, uint32_t n, uint32_t seed)
{
	uint32_t i;

	for (i = 0U; i < n; i++)
		p[i] = (uint8_t)(seed + i * 7U + 3U);
}

/* A PES of one stream, as TS packets. bounded sets PES_packet_length. The
 * first packet carries first_len bytes of the PES (at most 184), which cuts
 * the header when it is short. */
static void tsb_pes_split(struct tsb *b, uint32_t pid, uint8_t sid,
                          uint64_t pts, const uint8_t *es, uint32_t es_len,
                          int bounded, uint32_t first_len)
{
	uint32_t header = pts != NO_TS ? 14U : 9U;
	uint32_t total = header + es_len;
	uint8_t *pes = (uint8_t *)malloc(total);
	uint32_t sent = 0U;
	int first = 1;

	pes[0] = 0U;
	pes[1] = 0U;
	pes[2] = 1U;
	pes[3] = sid;
	if (bounded) {
		pes[4] = (uint8_t)((total - 6U) >> 8);
		pes[5] = (uint8_t)(total - 6U);
	} else {
		pes[4] = 0U;
		pes[5] = 0U;
	}
	pes[6] = 0x80U;
	pes[7] = pts != NO_TS ? 0x80U : 0U;
	pes[8] = pts != NO_TS ? 5U : 0U;
	if (pts != NO_TS)
		pts5(pes + 9, 2U, pts);
	memcpy(pes + header, es, es_len);
	while (sent < total) {
		uint32_t limit = first ? first_len : 184U;
		uint32_t n = total - sent < limit ? total - sent : limit;

		tsb_packet(b, pid, first, pes + sent, n);
		first = 0;
		sent += n;
	}
	free(pes);
}

static void tsb_pes(struct tsb *b, uint32_t pid, uint8_t sid, uint64_t pts,
                    const uint8_t *es, uint32_t es_len, int bounded)
{
	tsb_pes_split(b, pid, sid, pts, es, es_len, bounded, 184U);
}

static void tsb_remove(struct tsb *b, uint32_t index)
{
	memmove(tsb_at(b, index) - (b->stride - 188U),
	        tsb_at(b, index + 1U) - (b->stride - 188U),
	        b->len - (index + 1U) * b->stride);
	b->len -= b->stride;
}

static void tsb_duplicate(struct tsb *b, uint32_t index)
{
	uint32_t start = index * b->stride;

	if (b->len + b->stride > b->cap) {
		b->cap = (b->len + b->stride) * 2U;
		b->data = (uint8_t *)realloc(b->data, b->cap);
	}
	memmove(b->data + start + b->stride, b->data + start,
	        b->len - start);
	b->len += b->stride;
}

/* --------------------------------------------------------------- tests */

struct expect {
	const uint8_t *video;
	uint32_t video_len;
	const uint8_t *audio;
	uint32_t audio_len;
};

static int check_es(const struct out *o, void *ctx)
{
	const struct expect *e = (const struct expect *)ctx;

	CHECK(o->done);
	CHECK(!o->stalled);
	CHECK(o->video.len == e->video_len);
	CHECK(e->video_len == 0U ||
	      memcmp(o->video.data, e->video, e->video_len) == 0);
	CHECK(o->audio.len == e->audio_len);
	CHECK(e->audio_len == 0U ||
	      memcmp(o->audio.data, e->audio, e->audio_len) == 0);
	CHECK(o->max_packet <= SDK_DVD_PS_PES_MAX);
	return 0;
}

static uint8_t *cat(const uint8_t *a, uint32_t an, const uint8_t *b,
                    uint32_t bn)
{
	uint8_t *r = (uint8_t *)malloc(an + bn + 1U);

	memcpy(r, a, an);
	memcpy(r + an, b, bn);
	return r;
}

/* PES larger than the 64 KiB queue entries: unbounded video PES are cut
 * into chunks; the PTS rides the first, the bytes stay exact. */
struct big_ctx {
	struct expect e;
};

static int check_big(const struct out *o, void *ctx)
{
	uint32_t i;
	uint32_t with_pts = 0U;

	if (check_es(o, ctx) != 0)
		return 1;
	CHECK(o->nv >= 5U);
	CHECK(o->vpts[0] == 900000U);
	for (i = 1U; i < o->nv; i++)
		if (o->vpts[i] != NO_TS)
			with_pts++;
	CHECK(with_pts == 1U);
	CHECK(o->vpts[o->nv - 1U] == 903600U);
	CHECK(o->malformed == 0U);
	return 0;
}

static int test_pes_over_64k(void)
{
	struct tsb b;
	uint8_t *big = (uint8_t *)malloc(200000U);
	uint8_t small[1500];
	struct big_ctx ctx;
	uint8_t *all;
	uint8_t *wide;

	fill_es(big, 200000U, 1U);
	fill_es(small, sizeof(small), 9U);
	tsb_init(&b, 188U);
	tsb_psi(&b);
	tsb_pes(&b, V_PID, 0xe0U, 900000U, big, 200000U, 0);
	tsb_pes(&b, V_PID, 0xe0U, 903600U, small, sizeof(small), 0);
	all = cat(big, 200000U, small, sizeof(small));
	ctx.e.video = all;
	ctx.e.video_len = 200000U + (uint32_t)sizeof(small);
	ctx.e.audio = 0;
	ctx.e.audio_len = 0U;
	CHECK(each_config(b.data, b.len, check_big, &ctx) == 0);
	tsb_free(&b);

	/* A bounded PES of the largest size the length field allows. */
	wide = (uint8_t *)malloc(65535U);
	fill_es(wide, 65535U - 8U, 4U);
	tsb_init(&b, 188U);
	tsb_psi(&b);
	tsb_pes(&b, V_PID, 0xe0U, 90000U, wide, 65535U - 8U, 1);
	ctx.e.video = wide;
	ctx.e.video_len = 65535U - 8U;
	{
		struct out o;

		run(b.data, b.len, 188U, ALL_POPS, 1, 0, &o);
		CHECK(check_es(&o, &ctx) == 0);
		CHECK(o.nv == 1U && o.vpts[0] == 90000U);
		CHECK(o.max_packet == 65535U - 8U);
		out_free(&o);
	}
	tsb_free(&b);
	free(wide);
	free(all);
	free(big);
	return 0;
}

/* Interleaved audio and video, unbounded video PES ended by the next
 * PUSI, the last one by the end of input. */
static int check_interleave(const struct out *o, void *ctx)
{
	if (check_es(o, ctx) != 0)
		return 1;
	CHECK(o->nv == 3U && o->na == 4U);
	CHECK(o->vpts[0] == 1000U && o->vpts[1] == 4600U &&
	      o->vpts[2] == 8200U);
	CHECK(o->apts[0] == 500U && o->apts[3] == 3500U);
	CHECK(o->video_stream == 0xe0U && o->audio_stream == 0xc0U);
	return 0;
}

static int test_interleave(void)
{
	struct tsb b;
	uint8_t v[3][2000];
	uint8_t a[4][700];
	uint8_t *vall = (uint8_t *)malloc(sizeof(v));
	uint8_t *aall = (uint8_t *)malloc(sizeof(a));
	struct expect e;
	uint32_t i;

	tsb_init(&b, 188U);
	tsb_psi(&b);
	for (i = 0U; i < 3U; i++) {
		fill_es(v[i], sizeof(v[i]), i);
		memcpy(vall + i * sizeof(v[i]), v[i], sizeof(v[i]));
	}
	for (i = 0U; i < 4U; i++) {
		fill_es(a[i], sizeof(a[i]), 50U + i);
		memcpy(aall + i * sizeof(a[i]), a[i], sizeof(a[i]));
	}
	/* A PCR-only packet and a null packet between the payloads. */
	tsb_pes(&b, V_PID, 0xe0U, 1000U, v[0], sizeof(v[0]), 0);
	tsb_adaptation_only(&b, V_PID, 0);
	tsb_pes(&b, A_PID, 0xc0U, 500U, a[0], sizeof(a[0]), 1);
	tsb_pes(&b, V_PID, 0xe0U, 4600U, v[1], sizeof(v[1]), 0);
	tsb_pes(&b, A_PID, 0xc0U, 1500U, a[1], sizeof(a[1]), 1);
	tsb_pes(&b, A_PID, 0xc0U, 2500U, a[2], sizeof(a[2]), 1);
	tsb_pes(&b, 0x1fffU, 0xe0U, NO_TS, v[0], 100U, 0);
	tsb_pes(&b, V_PID, 0xe0U, 8200U, v[2], sizeof(v[2]), 0);
	tsb_pes(&b, A_PID, 0xc0U, 3500U, a[3], sizeof(a[3]), 1);
	e.video = vall;
	e.video_len = (uint32_t)sizeof(v);
	e.audio = aall;
	e.audio_len = (uint32_t)sizeof(a);
	CHECK(each_config(b.data, b.len, check_interleave, &e) == 0);
	tsb_free(&b);
	free(vall);
	free(aall);
	return 0;
}

/* An unbounded PES is complete at the end of input, a bounded one cut
 * short is dropped. */
static int test_end_of_input(void)
{
	struct tsb b;
	uint8_t v[1000];
	uint8_t a[1000];
	struct expect e;
	struct out o;

	fill_es(v, sizeof(v), 3U);
	fill_es(a, sizeof(a), 5U);
	tsb_init(&b, 188U);
	tsb_psi(&b);
	tsb_pes(&b, A_PID, 0xc0U, 10U, a, sizeof(a), 1);
	tsb_pes(&b, V_PID, 0xe0U, 20U, v, sizeof(v), 0);
	e.video = v;
	e.video_len = sizeof(v);
	e.audio = a;
	e.audio_len = sizeof(a);
	CHECK(each_config(b.data, b.len, check_es, &e) == 0);

	/* Audio last, so video is still open when the input ends. */
	tsb_free(&b);
	tsb_init(&b, 188U);
	tsb_psi(&b);
	tsb_pes(&b, V_PID, 0xe0U, 20U, v, sizeof(v), 0);
	tsb_pes(&b, A_PID, 0xc0U, 10U, a, sizeof(a), 1);
	CHECK(each_config(b.data, b.len, check_es, &e) == 0);

	/* Bounded video PES missing its last packet. */
	tsb_free(&b);
	tsb_init(&b, 188U);
	tsb_psi(&b);
	tsb_pes(&b, A_PID, 0xc0U, 10U, a, sizeof(a), 1);
	tsb_pes(&b, V_PID, 0xe0U, 20U, v, sizeof(v), 1);
	b.len -= b.stride;
	e.video_len = 0U;
	run(b.data, b.len, 188U, ALL_POPS, 1, 0, &o);
	CHECK(check_es(&o, &e) == 0);
	CHECK(o.truncated_pes == 1U);
	out_free(&o);

	/* The input ends inside a packet. */
	tsb_free(&b);
	tsb_init(&b, 188U);
	tsb_psi(&b);
	tsb_pes(&b, A_PID, 0xc0U, 10U, a, sizeof(a), 1);
	tsb_pes(&b, V_PID, 0xe0U, 20U, v, sizeof(v), 0);
	b.len -= 100U;
	run(b.data, b.len, 188U, ALL_POPS, 1, 0, &o);
	CHECK(o.done);
	CHECK(o.audio.len == sizeof(a));
	CHECK(o.video.len == 0U || o.video.len < sizeof(v));
	out_free(&o);
	tsb_free(&b);
	return 0;
}

static int test_pmt_selection(void)
{
	struct tsb b;
	uint8_t sec[SDK_DVD_TS_SECTION_MAX];
	uint8_t v1[600];
	uint8_t v2[600];
	uint8_t a1[600];
	uint8_t a2[600];
	struct expect e;
	/* Program 0 is the network PID; program 1 is the first real one. */
	uint16_t programs[3] = {0U, 1U, 2U};
	uint16_t pids[3] = {0x20U, PMT_PID, 0x200U};
	struct es other[1] = {{0x02U, 0x210U, 0U}};
	/* Types the module cannot use come first; the first usable video and
	 * audio PIDs win, with descriptors and a long program info. */
	struct es es[6] = {{0x1bU, 0x150U, 6U}, {0x0fU, 0x151U, 0U},
	                   {0x01U, V_PID, 40U}, {0x04U, A_PID, 3U},
	                   {0x02U, 0x104U, 0U}, {0x03U, A2_PID, 0U}};
	struct out o;

	fill_es(v1, sizeof(v1), 1U);
	fill_es(v2, sizeof(v2), 2U);
	fill_es(a1, sizeof(a1), 3U);
	fill_es(a2, sizeof(a2), 4U);
	tsb_init(&b, 188U);
	tsb_section(&b, 0U, sec, make_pat(sec, programs, pids, 3U));
	tsb_section(&b, 0x200U, sec, make_pmt(sec, 2U, other, 1U, 0U));
	/* PMT longer than one packet. */
	tsb_section(&b, PMT_PID, sec, make_pmt(sec, 1U, es, 6U, 120U));
	CHECK(tsb_count(&b) >= 4U);
	tsb_pes(&b, 0x210U, 0xe0U, 1U, v2, sizeof(v2), 0);
	tsb_pes(&b, 0x104U, 0xe0U, 2U, v2, sizeof(v2), 0);
	tsb_pes(&b, A2_PID, 0xc1U, 3U, a2, sizeof(a2), 1);
	tsb_pes(&b, V_PID, 0xe0U, 100U, v1, sizeof(v1), 0);
	tsb_pes(&b, A_PID, 0xc0U, 100U, a1, sizeof(a1), 1);
	e.video = v1;
	e.video_len = sizeof(v1);
	e.audio = a1;
	e.audio_len = sizeof(a1);
	CHECK(each_config(b.data, b.len, check_es, &e) == 0);
	run(b.data, b.len, 188U, ALL_POPS, 1, 0, &o);
	CHECK(o.bytes_ignored != 0U);
	out_free(&o);
	tsb_free(&b);
	return 0;
}

static int test_missing_pmt(void)
{
	struct tsb b;
	uint8_t sec[SDK_DVD_TS_SECTION_MAX];
	uint8_t v[900];
	uint8_t a[900];
	uint16_t program = 1U;
	uint16_t pid = PMT_PID;
	struct es es[2] = {{0x02U, V_PID, 0U}, {0x03U, A_PID, 0U}};
	struct expect e;
	struct out o;
	uint32_t before;
	uint32_t i;

	fill_es(v, sizeof(v), 1U);
	fill_es(a, sizeof(a), 2U);

	/* PAT but no PMT: nothing is queued, the input still drains. */
	tsb_init(&b, 188U);
	tsb_section(&b, 0U, sec, make_pat(sec, &program, &pid, 1U));
	for (i = 0U; i < 3U; i++) {
		tsb_pes(&b, V_PID, 0xe0U, 100U + i, v, sizeof(v), 0);
		tsb_pes(&b, A_PID, 0xc0U, 100U + i, a, sizeof(a), 1);
	}
	run(b.data, b.len, 188U, ALL_POPS, 1, 0, &o);
	CHECK(o.done && !o.stalled);
	CHECK(o.nv == 0U && o.na == 0U);
	CHECK(o.bytes_ignored >= b.len);
	out_free(&o);
	tsb_free(&b);

	/* No PAT either. */
	tsb_init(&b, 188U);
	tsb_pes(&b, V_PID, 0xe0U, 100U, v, sizeof(v), 0);
	run(b.data, b.len, 188U, ALL_POPS, 1, 0, &o);
	CHECK(o.done && o.nv == 0U && o.na == 0U);
	out_free(&o);
	tsb_free(&b);

	/* PES before the PMT are skipped, those after it are kept. */
	tsb_init(&b, 188U);
	tsb_section(&b, 0U, sec, make_pat(sec, &program, &pid, 1U));
	tsb_pes(&b, V_PID, 0xe0U, 1U, v, sizeof(v), 0);
	tsb_pes(&b, A_PID, 0xc0U, 1U, a, sizeof(a), 1);
	tsb_section(&b, PMT_PID, sec, make_pmt(sec, 1U, es, 2U, 0U));
	tsb_pes(&b, V_PID, 0xe0U, 2U, v, sizeof(v), 0);
	tsb_pes(&b, A_PID, 0xc0U, 2U, a, sizeof(a), 1);
	tsb_pes(&b, V_PID, 0xe0U, 3U, v, sizeof(v), 0);
	run(b.data, b.len, 188U, ALL_POPS, 1, 0, &o);
	CHECK(o.done);
	CHECK(o.nv == 2U && o.vpts[0] == 2U && o.vpts[1] == 3U);
	CHECK(o.na == 1U && o.apts[0] == 2U);
	out_free(&o);
	tsb_free(&b);

	/* A PMT with a bad CRC is ignored; the next good one is used. */
	tsb_init(&b, 188U);
	tsb_section(&b, 0U, sec, make_pat(sec, &program, &pid, 1U));
	before = tsb_count(&b);
	tsb_section(&b, PMT_PID, sec, make_pmt(sec, 1U, es, 2U, 0U));
	tsb_at(&b, before)[187] ^= 0x01U;
	tsb_pes(&b, V_PID, 0xe0U, 1U, v, sizeof(v), 0);
	tsb_section(&b, PMT_PID, sec, make_pmt(sec, 1U, es, 2U, 0U));
	tsb_pes(&b, V_PID, 0xe0U, 2U, v, sizeof(v), 0);
	e.video = v;
	e.video_len = sizeof(v);
	e.audio = 0;
	e.audio_len = 0U;
	run(b.data, b.len, 188U, ALL_POPS, 1, 0, &o);
	CHECK(check_es(&o, &e) == 0);
	CHECK(o.nv == 1U && o.vpts[0] == 2U);
	out_free(&o);
	tsb_free(&b);
	return 0;
}

/* One damaged stream: three video PES of 5 packets each (index of their
 * first packet is returned through first[]). */
static void damaged_base(struct tsb *b, uint8_t es[3][800],
                         uint32_t first[3])
{
	uint32_t i;

	tsb_init(b, 188U);
	tsb_psi(b);
	for (i = 0U; i < 3U; i++) {
		fill_es(es[i], 800U, 10U + i);
		first[i] = tsb_count(b);
		tsb_pes(b, V_PID, 0xe0U, 100U + i, es[i], 800U, 0);
	}
}

struct damage_ctx {
	uint8_t (*es)[800];
	uint32_t keep_mask; /* bit i: PES i must arrive intact */
	uint32_t continuity;
	uint32_t tei;
	uint32_t duplicates;
	uint32_t sync;
};

static int check_damage(const struct out *o, void *vctx)
{
	const struct damage_ctx *c = (const struct damage_ctx *)vctx;
	uint32_t want = 0U;
	uint32_t at = 0U;
	uint32_t i;

	CHECK(o->done && !o->stalled);
	for (i = 0U; i < 3U; i++) {
		if ((c->keep_mask & (1U << i)) == 0U)
			continue;
		CHECK(o->video.len >= at + 800U);
		CHECK(memcmp(o->video.data + at, c->es[i], 800U) == 0);
		at += 800U;
		want++;
	}
	CHECK(o->video.len == at);
	CHECK(o->nv == want);
	CHECK(o->continuity_errors == c->continuity);
	CHECK(o->tei_packets == c->tei);
	CHECK(o->duplicate_packets == c->duplicates);
	if (c->sync != 0U)
		CHECK(o->sync_losses >= c->sync);
	else
		CHECK(o->sync_losses == 0U);
	return 0;
}

static int test_damage(void)
{
	struct tsb b;
	uint8_t es[3][800];
	uint32_t first[3];
	struct damage_ctx c;
	uint32_t i;
	uint32_t split;

	c.es = es;

	/* Clean. */
	damaged_base(&b, es, first);
	c.keep_mask = 7U;
	c.continuity = c.tei = c.duplicates = c.sync = 0U;
	CHECK(each_config(b.data, b.len, check_damage, &c) == 0);
	tsb_free(&b);

	/* A packet lost from the middle of the second PES: it is dropped,
	 * the first and the third survive. */
	damaged_base(&b, es, first);
	tsb_remove(&b, first[1] + 2U);
	c.keep_mask = 5U;
	c.continuity = 1U;
	CHECK(each_config(b.data, b.len, check_damage, &c) == 0);
	tsb_free(&b);

	/* The first packet of a PES lost: the counter gap at its second packet
	 * cuts the PES before it, which an unbounded PES leaves open until the
	 * next one starts, so both are dropped. */
	damaged_base(&b, es, first);
	tsb_remove(&b, first[1]);
	c.keep_mask = 4U;
	c.continuity = 1U;
	CHECK(each_config(b.data, b.len, check_damage, &c) == 0);
	tsb_free(&b);

	/* The last packet of the second PES lost: the gap shows at the first
	 * packet of the third, which still starts cleanly. The second PES
	 * ends short, so it is dropped too. */
	damaged_base(&b, es, first);
	tsb_remove(&b, first[2] - 1U);
	c.keep_mask = 5U;
	c.continuity = 1U;
	CHECK(each_config(b.data, b.len, check_damage, &c) == 0);
	tsb_free(&b);

	/* Transport error indicator inside the second PES. */
	damaged_base(&b, es, first);
	tsb_at(&b, first[1] + 1U)[1] |= 0x80U;
	c.keep_mask = 5U;
	c.continuity = 1U;
	c.tei = 1U;
	CHECK(each_config(b.data, b.len, check_damage, &c) == 0);
	tsb_free(&b);

	/* A duplicated packet is ignored. */
	damaged_base(&b, es, first);
	tsb_duplicate(&b, first[1] + 2U);
	c.keep_mask = 7U;
	c.continuity = 0U;
	c.tei = 0U;
	c.duplicates = 1U;
	CHECK(each_config(b.data, b.len, check_damage, &c) == 0);
	tsb_free(&b);

	/* Junk between packets: sync is found again, nothing is lost. */
	damaged_base(&b, es, first);
	split = first[1] * 188U;
	b.data = (uint8_t *)realloc(b.data, b.len + 37U);
	memmove(b.data + split + 37U, b.data + split, b.len - split);
	memset(b.data + split, 0x11, 37U);
	b.len += 37U;
	b.cap = b.len;
	c.keep_mask = 7U;
	c.duplicates = 0U;
	c.sync = 1U;
	CHECK(each_config(b.data, b.len, check_damage, &c) == 0);
	tsb_free(&b);

	/* Junk before the first packet, with a stray 0x47 in it. */
	damaged_base(&b, es, first);
	b.data = (uint8_t *)realloc(b.data, b.len + 301U);
	memmove(b.data + 301U, b.data, b.len);
	memset(b.data, 0x22, 301U);
	b.data[10] = 0x47U;
	b.data[198] = 0x47U;
	b.len += 301U;
	b.cap = b.len;
	c.sync = 0U;
	CHECK(each_config(b.data, b.len, check_damage, &c) == 0);
	tsb_free(&b);

	/* A corrupted sync byte loses that packet: its PES is dropped. */
	damaged_base(&b, es, first);
	tsb_at(&b, first[1] + 2U)[0] = 0x46U;
	c.keep_mask = 5U;
	c.continuity = 1U;
	c.sync = 1U;
	CHECK(each_config(b.data, b.len, check_damage, &c) == 0);
	tsb_free(&b);

	/* Sync bytes damaged on several packets over the stream. */
	damaged_base(&b, es, first);
	for (i = 0U; i < 3U; i++)
		tsb_at(&b, first[i] + 3U)[0] = 0x00U;
	{
		struct out o;

		run(b.data, b.len, 188U, ALL_POPS, 1, 0, &o);
		CHECK(o.done && !o.stalled);
		CHECK(o.sync_losses >= 1U);
		CHECK(o.nv <= 3U);
		out_free(&o);
	}
	tsb_free(&b);
	return 0;
}

/* Discontinuity indicator: a counter jump it announces is not a loss. */
static int test_discontinuity_indicator(void)
{
	struct tsb b;
	uint8_t v[800];
	struct expect e;
	struct out o;

	fill_es(v, sizeof(v), 1U);
	tsb_init(&b, 188U);
	tsb_psi(&b);
	tsb_pes(&b, V_PID, 0xe0U, 1U, v, sizeof(v), 0);
	tsb_adaptation_only(&b, V_PID, 1);
	b.cc[V_PID] += 5U;
	tsb_pes(&b, V_PID, 0xe0U, 2U, v, sizeof(v), 0);
	e.video = cat(v, sizeof(v), v, sizeof(v));
	e.video_len = 2U * sizeof(v);
	e.audio = 0;
	e.audio_len = 0U;
	run(b.data, b.len, 188U, ALL_POPS, 1, 0, &o);
	CHECK(check_es(&o, &e) == 0);
	CHECK(o.continuity_errors == 0U);
	out_free(&o);
	free((void *)e.video);
	tsb_free(&b);
	return 0;
}

/* Output queues full: parsing waits for the consumer and loses nothing. */
static int test_backpressure(void)
{
	struct tsb b;
	uint8_t a[60][700];
	uint8_t v[12][3000];
	uint8_t *aall = (uint8_t *)malloc(sizeof(a));
	uint8_t *vall = (uint8_t *)malloc(sizeof(v));
	struct expect e;
	struct SDKDVDPSDemux *d = (struct SDKDVDPSDemux *)calloc(1, sizeof(*d));
	struct SDKDVDTSDemux *t = (struct SDKDVDTSDemux *)calloc(1, sizeof(*t));
	struct out o;
	uint32_t accepted;
	uint32_t i;
	uint32_t offset = 0U;

	tsb_init(&b, 188U);
	tsb_psi(&b);
	for (i = 0U; i < 60U; i++) {
		fill_es(a[i], sizeof(a[i]), i);
		memcpy(aall + i * sizeof(a[i]), a[i], sizeof(a[i]));
		tsb_pes(&b, A_PID, 0xc0U, 100U + i, a[i], sizeof(a[i]), 1);
		if (i < 12U) {
			fill_es(v[i], sizeof(v[i]), 100U + i);
			memcpy(vall + i * sizeof(v[i]), v[i], sizeof(v[i]));
			tsb_pes(&b, V_PID, 0xe0U, 100U + i, v[i], sizeof(v[i]), 0);
		}
	}
	e.video = vall;
	e.video_len = (uint32_t)sizeof(v);
	e.audio = aall;
	e.audio_len = (uint32_t)sizeof(a);

	/* Without popping, the writes stop at the full queues. */
	memset(&o, 0, sizeof(o));
	sdk_dvd_ps_init(d);
	sdk_dvd_ts_init(t, d);
	while (offset < b.len) {
		sdk_dvd_ps_write(d, b.data + offset, b.len - offset, &accepted, 0);
		if (accepted == 0U)
			break;
		offset += accepted;
	}
	CHECK(offset < b.len);
	CHECK(d->audio.count == SDK_DVD_PS_QUEUE_DEPTH ||
	      d->video.count == SDK_DVD_PS_QUEUE_DEPTH);
	CHECK(d->backpressure_events != 0U);
	/* A consumer that never writes again still drains what is staged. */
	while (drain(d, &o, 1U) != 0U) {}
	while (offset < b.len) {
		sdk_dvd_ps_write(d, b.data + offset, b.len - offset, &accepted, 0);
		offset += accepted;
		while (drain(d, &o, 1U) != 0U) {}
	}
	sdk_dvd_ps_write(d, 0, 0U, &accepted, 1);
	while (drain(d, &o, 1U) != 0U) {}
	o.done = sdk_dvd_ps_done(d);
	CHECK(o.done);
	CHECK(check_es(&o, &e) == 0);
	out_free(&o);
	free(d);
	free(t);
	CHECK(each_config(b.data, b.len, check_es, &e) == 0);
	tsb_free(&b);
	free(aall);
	free(vall);
	return 0;
}

/* The decoder's audio overflow policy applies to TS audio as to PS. */
static int test_audio_drop_policy(void)
{
	struct tsb b;
	uint8_t a[40][500];
	uint8_t v[600];
	struct SDKDVDPSDemux *d = (struct SDKDVDPSDemux *)calloc(1, sizeof(*d));
	struct SDKDVDTSDemux *t = (struct SDKDVDTSDemux *)calloc(1, sizeof(*t));
	struct out o;
	uint32_t accepted;
	uint32_t i;

	fill_es(v, sizeof(v), 1U);
	tsb_init(&b, 188U);
	tsb_psi(&b);
	for (i = 0U; i < 40U; i++) {
		fill_es(a[i], sizeof(a[i]), i);
		tsb_pes(&b, A_PID, 0xc0U, 100U + i, a[i], sizeof(a[i]), 1);
	}
	tsb_pes(&b, V_PID, 0xe0U, 100U, v, sizeof(v), 0);
	memset(&o, 0, sizeof(o));
	sdk_dvd_ps_init(d);
	sdk_dvd_ts_init(t, d);
	d->drop_audio_when_full = 1U;
	sdk_dvd_ps_write(d, b.data, b.len, &accepted, 1);
	CHECK(accepted == b.len);
	/* Nobody drained audio, yet the video behind it was parsed. */
	CHECK(d->audio.count == SDK_DVD_PS_QUEUE_DEPTH);
	CHECK(d->audio_packets_dropped == 36U);
	CHECK(d->video.count == 1U);
	CHECK(d->first_dropped_audio_pts_valid &&
	      d->first_dropped_audio_pts == 104U);
	CHECK(d->last_dropped_audio_pts == 139U);
	while (drain(d, &o, 1U) != 0U) {}
	CHECK(sdk_dvd_ps_done(d));
	CHECK(o.nv == 1U && o.na == 4U);
	out_free(&o);
	tsb_free(&b);
	free(d);
	free(t);
	return 0;
}

/* M2TS: the 4-byte prefix, found from the start and after junk. */
static int test_m2ts(void)
{
	struct tsb b;
	uint8_t v[900];
	uint8_t a[900];
	struct expect e;
	struct out o;

	fill_es(v, sizeof(v), 1U);
	fill_es(a, sizeof(a), 2U);
	tsb_init(&b, 192U);
	tsb_psi(&b);
	tsb_pes(&b, V_PID, 0xe0U, 1U, v, sizeof(v), 0);
	tsb_pes(&b, A_PID, 0xc0U, 1U, a, sizeof(a), 1);
	tsb_pes(&b, V_PID, 0xe0U, 2U, v, sizeof(v), 0);
	e.video = cat(v, sizeof(v), v, sizeof(v));
	e.video_len = 2U * sizeof(v);
	e.audio = a;
	e.audio_len = sizeof(a);
	CHECK(each_config(b.data, b.len, check_es, &e) == 0);
	run(b.data, b.len, 192U, ALL_POPS, 1, 0, &o);
	CHECK(o.packet_bytes == 192U);
	out_free(&o);
	free((void *)e.video);
	tsb_free(&b);
	return 0;
}

/* Packets of the selected PID that are scrambled, or carry another stream
 * id, are not queued. */
static int test_foreign_payloads(void)
{
	struct tsb b;
	uint8_t v[900];
	struct out o;

	fill_es(v, sizeof(v), 1U);
	tsb_init(&b, 188U);
	tsb_psi(&b);
	tsb_pes(&b, V_PID, 0xc0U, 1U, v, sizeof(v), 1);
	tsb_pes(&b, V_PID, 0xbdU, 2U, v, sizeof(v), 1);
	tsb_pes(&b, V_PID, 0xe0U, 3U, v, sizeof(v), 1);
	tsb_at(&b, tsb_count(&b) - 1U)[3] |= 0x80U;
	run(b.data, b.len, 188U, ALL_POPS, 1, 0, &o);
	CHECK(o.done);
	CHECK(o.nv == 0U && o.na == 0U);
	out_free(&o);
	tsb_free(&b);
	return 0;
}

/* A PES header cut over two packets by an adaptation field (the first packet
 * carries only `first` bytes of the PES) must give the output of the same
 * PES in one piece. */
struct split_ctx {
	struct expect e;
	int video;
	uint64_t pts0;
	uint64_t pts1;
};

static int check_split(const struct out *o, void *vctx)
{
	const struct split_ctx *c = (const struct split_ctx *)vctx;
	const uint64_t *pts = c->video ? o->vpts : o->apts;
	uint32_t n = c->video ? o->nv : o->na;

	if (check_es(o, vctx) != 0)
		return 1;
	CHECK(n == 2U);
	CHECK(pts[0] == c->pts0 && pts[1] == c->pts1);
	CHECK(o->malformed == 0U && o->truncated_pes == 0U);
	return 0;
}

static int test_split_header(void)
{
	uint8_t es[2][700];
	uint8_t both[1400];
	uint32_t first;
	uint32_t kind;

	fill_es(es[0], sizeof(es[0]), 11U);
	fill_es(es[1], sizeof(es[1]), 12U);
	memcpy(both, es[0], sizeof(es[0]));
	memcpy(both + sizeof(es[0]), es[1], sizeof(es[1]));
	for (kind = 0U; kind < 4U; kind++) {
		int video = (kind & 1U) == 0U;
		int bounded = (kind & 2U) != 0U;

		for (first = 1U; first <= 16U; first++) {
			struct tsb b;
			struct split_ctx c;
			uint32_t pid = video ? V_PID : A_PID;
			uint8_t sid = video ? 0xe0U : 0xc0U;

			tsb_init(&b, 188U);
			tsb_psi(&b);
			tsb_pes_split(&b, pid, sid, 100U, es[0], sizeof(es[0]),
			              bounded, first);
			tsb_pes(&b, pid, sid, 200U, es[1], sizeof(es[1]), bounded);
			c.video = video;
			c.pts0 = 100U;
			c.pts1 = 200U;
			c.e.video = video ? both : 0;
			c.e.video_len = video ? sizeof(both) : 0U;
			c.e.audio = video ? 0 : both;
			c.e.audio_len = video ? 0U : sizeof(both);
			if (each_config(b.data, b.len, check_split, &c) != 0) {
				fprintf(stderr, "  %s %s, first packet %u bytes\n",
				        video ? "video" : "audio",
				        bounded ? "bounded" : "unbounded", first);
				return 1;
			}
			/* Split a PES that has no timestamp too, and the
			 * second one, behind a PES that is not split. */
			tsb_free(&b);
			tsb_init(&b, 188U);
			tsb_psi(&b);
			tsb_pes(&b, pid, sid, 100U, es[0], sizeof(es[0]), bounded);
			tsb_pes_split(&b, pid, sid, 200U, es[1], sizeof(es[1]),
			              bounded, first);
			if (each_config(b.data, b.len, check_split, &c) != 0) {
				fprintf(stderr, "  %s %s, second split %u\n",
				        video ? "video" : "audio",
				        bounded ? "bounded" : "unbounded", first);
				return 1;
			}
			tsb_free(&b);
		}
	}
	return 0;
}

/* A header that is split and then wrong, or never completed. */
static int test_split_header_damage(void)
{
	uint8_t a[600];
	uint8_t v[600];
	uint32_t first;
	struct tsb b;
	struct expect e;
	struct out o;
	uint32_t at;

	fill_es(a, sizeof(a), 1U);
	fill_es(v, sizeof(v), 2U);
	e.video = v;
	e.video_len = sizeof(v);
	e.audio = 0;
	e.audio_len = 0U;

	/* A bad start code byte, in the first packet or the second: that PES is
	 * dropped as malformed and the next one is intact. */
	for (first = 3U; first <= 5U; first++) {
		uint32_t bad;

		for (bad = 0U; bad < 3U; bad++) {
			tsb_init(&b, 188U);
			tsb_psi(&b);
			at = tsb_count(&b);
			tsb_pes_split(&b, V_PID, 0xe0U, 1U, a, sizeof(a), 1, first);
			if (bad < first)
				tsb_at(&b, at)[188U - first + bad] ^= 0x10U;
			else
				tsb_at(&b, at + 1U)[188U - 184U + bad - first] ^= 0x10U;
			tsb_pes(&b, V_PID, 0xe0U, 2U, v, sizeof(v), 1);
			run(b.data, b.len, 188U, ALL_POPS, 1, 0, &o);
			if (check_es(&o, &e) != 0 || o.nv != 1U || o.vpts[0] != 2U ||
			    o.malformed == 0U) {
				fprintf(stderr, "  bad start code byte %u, first %u\n", bad,
				        first);
				out_free(&o);
				return 1;
			}
			out_free(&o);
			tsb_free(&b);
		}
	}

	/* A stream id of the wrong kind, seen once split: skipped silently. */
	for (first = 1U; first <= 6U; first++) {
		tsb_init(&b, 188U);
		tsb_psi(&b);
		tsb_pes_split(&b, V_PID, 0xc0U, 1U, a, sizeof(a), 1, first);
		tsb_pes(&b, V_PID, 0xe0U, 2U, v, sizeof(v), 1);
		run(b.data, b.len, 188U, ALL_POPS, 1, 0, &o);
		if (check_es(&o, &e) != 0 || o.nv != 1U || o.vpts[0] != 2U) {
			fprintf(stderr, "  wrong stream id, first %u\n", first);
			out_free(&o);
			return 1;
		}
		out_free(&o);
		tsb_free(&b);
	}

	/* A header cut off by the end of input, or by the next PES. */
	for (first = 1U; first <= 5U; first++) {
		int bounded;
		uint8_t cc0;

		for (bounded = 0; bounded < 2; bounded++) {
			tsb_init(&b, 188U);
			tsb_psi(&b);
			tsb_pes(&b, V_PID, 0xe0U, 1U, v, sizeof(v), bounded);
			at = tsb_count(&b);
			cc0 = b.cc[V_PID];
			tsb_pes_split(&b, V_PID, 0xe0U, 2U, a, sizeof(a), bounded,
			              first);
			b.len = (at + 1U) * b.stride;
			b.cc[V_PID] = (uint8_t)(cc0 + 1U);
			run(b.data, b.len, 188U, ALL_POPS, 1, 0, &o);
			if (check_es(&o, &e) != 0 || o.nv != 1U || o.vpts[0] != 1U ||
			    o.truncated_pes != 1U || o.malformed == 0U) {
				fprintf(stderr, "  cut at end, first %u bounded %d\n", first,
				        bounded);
				out_free(&o);
				return 1;
			}
			out_free(&o);

			/* The same cut, then a whole PES. */
			tsb_pes(&b, V_PID, 0xe0U, 3U, v, sizeof(v), bounded);
			run(b.data, b.len, 188U, ALL_POPS, 1, 0, &o);
			e.video = cat(v, sizeof(v), v, sizeof(v));
			e.video_len = 2U * sizeof(v);
			if (check_es(&o, &e) != 0 || o.nv != 2U || o.vpts[1] != 3U ||
			    o.truncated_pes != 1U) {
				fprintf(stderr, "  cut then PES, first %u bounded %d\n",
				        first, bounded);
				out_free(&o);
				free((void *)e.video);
				return 1;
			}
			out_free(&o);
			free((void *)e.video);
			e.video = v;
			e.video_len = sizeof(v);
			tsb_free(&b);
		}
	}
	return 0;
}

/* The PS demux is unchanged for Program Streams: the TS hook is off. */
static int test_program_stream_untouched(void)
{
	struct out o;

	run(zz9k_ts_mpeg2_ps_fixture, zz9k_ts_mpeg2_ps_fixture_len, 2048U,
	    ALL_POPS, 0, 0, &o);
	CHECK(o.done && o.nv != 0U && o.na != 0U);
	out_free(&o);
	/* A Program Stream fed to the TS demux yields nothing and drains. */
	run(zz9k_ts_mpeg2_ps_fixture, zz9k_ts_mpeg2_ps_fixture_len, 2048U,
	    ALL_POPS, 1, 0, &o);
	CHECK(o.done && o.nv == 0U && o.na == 0U);
	out_free(&o);
	return 0;
}

int main(void)
{
	struct {
		const char *name;
		int (*fn)(void);
	} tests[] = {
		{"real streams equal their Program Stream", test_real_streams},
		{"PES over 64 KiB", test_pes_over_64k},
		{"interleaved audio and video", test_interleave},
		{"end of input", test_end_of_input},
		{"PAT/PMT selection", test_pmt_selection},
		{"missing PMT", test_missing_pmt},
		{"damaged streams", test_damage},
		{"discontinuity indicator", test_discontinuity_indicator},
		{"backpressure", test_backpressure},
		{"audio drop policy", test_audio_drop_policy},
		{"M2TS", test_m2ts},
		{"foreign payloads", test_foreign_payloads},
		{"PES header split over packets", test_split_header},
		{"PES header damaged or cut", test_split_header_damage},
		{"Program Stream path", test_program_stream_untouched},
	};
	unsigned i;
	int failed = 0;

	for (i = 0U; i < sizeof(tests) / sizeof(tests[0]); i++) {
		int r = tests[i].fn();

		printf("%s: %s\n", r == 0 ? "ok" : "FAIL", tests[i].name);
		if (r != 0)
			failed++;
	}
	return failed != 0;
}
