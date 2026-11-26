/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * The ZZ9000 platform of aar.library. See aar_zz9000.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include <libraries/aar.h>
#include <libraries/aar_zz9000.h>

#include "aar_core.h"
#include "aar_plat.h"
#include "aar_zz9000.h"

#if AAR_ZZ9000_AMIGA
#include <proto/exec.h>
#endif
#include "zz9k/abi.h"

#define ZZ_SHARED_GRANULE   4096u
/* The largest limit LOAD_BEGIN grants (zzapp_proto.h): AARInfo reports it,
 * and AARSetTagList refuses a larger AAR_TAG_SHARED_MIB. */
#define ZZ_SHARED_MAX_MIB   ZZAPP_SHARED_MAX_MIB
#define ZZ_STALL_LIMIT_MS   30000u /* ZZAPP_APP_STALL_US of zzapp_runtime.h */
#define ZZ_STOP_GRACE_MS    500u   /* ZZAPP_STOP_GRACE_US */
#define ZZ_CB_BYTES_MAX     0x100000u

/* ---- The constants of the platform ----------------------------------------- */

static const struct aar_plat_info zz_info = {
	sizeof(struct aar_plat_info),
	AAR_PLATFORM_ZZ9000,
	AAR_PROFILEF_A32,
	AAR_GROUP_INPUT | AAR_GROUP_SERVICES,
	AAR_INFOF_ISOLATED | AAR_INFOF_SHARED_COHERENT,
	0u,                       /* max_apps: limited by resources */
	ZZAPP_IMAGE_MAX_BYTES,
	ZZAPP_ARGV_MAX,
	ZZAPP_ARGV_STR_MAX,
	ZZ_SHARED_GRANULE,
	ZZ_STALL_LIMIT_MS,
	ZZ_STOP_GRACE_MS,
	AAR_SHARED_DEFAULT_MIB,
	ZZ_SHARED_MAX_MIB,
	ZZAPP_WEIGHT_MAX,
};

/* The constants above are the firmware's. The ones the wire header does not
 * carry are restated here and checked against what the card reports (the host
 * test compares them with aar_info of the runtime). */
_Static_assert(ZZ_STALL_LIMIT_MS >= AAR_STALL_LIMIT_MIN_MS, "stall limit");
_Static_assert(ZZAPP_KICK_CQ_POSTED == AAR_PLAT_KICK_CQ &&
	       ZZAPP_KICK_SQ_SPACE == AAR_PLAT_KICK_SQ, "doorbell bits");
_Static_assert(ZZAPP_CAP_Z3 == AAR_ZZ9000_CAP_Z3 &&
	       ZZAPP_CAP_DISPLAY == AAR_ZZ9000_CAP_DISPLAY &&
	       ZZAPP_CAP_AUDIO == AAR_ZZ9000_CAP_AUDIO &&
	       ZZAPP_CAP_GL == AAR_ZZ9000_CAP_GL &&
	       ZZAPP_CAP_CLUT8 == AAR_ZZ9000_CAP_CLUT8, "capability bits");
_Static_assert(ZZAPP_FLIPF_SUBMITTED == AAR_CFLAG_FLIP_SUBMITTED,
	       "flip submitted flag");
_Static_assert(ZZAPP_LIST_NAME_BYTES < 24, "list name");

/* ---- State ------------------------------------------------------------------ */

struct zz_state {
	aar_zz_board board;
	int attached;
	int abi_ok;                 /* QUERY said: ABI and Z3 as required */
	aar_barrier_fn acquire, release;
	uint32_t events_token;      /* QUERY: the event block, 0: none */
	volatile uint8_t *events;   /* its mapping; set last, NULL: no events */
	uint8_t events_sampled;     /* the counts below are valid */
	uint32_t events_global;     /* the global count at the last change */
	uint32_t events_group[ZZAPP_EVENT_GROUPS];
};

static struct zz_state zz;

/* The storage of one claim, in aar_plat_claim.priv. */
struct zz_claim {
	volatile uint8_t *load;     /* the load buffer, NULL once gone */
	uint32_t chunk_max, argv_off;
	uint8_t load_begun, started;
	uint8_t news_asked;         /* claim_news answered 1 once */
	uint32_t news_seen;         /* the count that answer saw */
	volatile uint8_t bell_busy; /* try-lock of the doorbell below */
	uint8_t bell_inflight;
	aar_zz_bell bell;
};

/* ---- Small helpers ------------------------------------------------------- */

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* A mailbox status as an AAR error. */
static int status_to_aar(int status)
{
	switch (status) {
	case ZZ9K_STATUS_OK:          return AAR_OK;
	case ZZ9K_STATUS_BUSY:        return AAR_E_BUSY;
	case ZZ9K_STATUS_UNSUPPORTED: return AAR_E_NOSYS;
	case ZZ9K_STATUS_BAD_REQUEST: return AAR_E_INVAL;
	case ZZ9K_STATUS_BAD_HANDLE:  return AAR_E_STALE;
	case ZZ9K_STATUS_NO_MEMORY:   return AAR_E_NOMEM;
	case ZZ9K_STATUS_TIMEOUT:     return AAR_E_TIMEDOUT;
	case ZZ9K_STATUS_NOT_FOUND:   return AAR_E_NOENT;
	default:                      return AAR_E_IO;
	}
}

static struct zz_state *state_of(struct aar_plat *p)
{
	return (struct zz_state *)p->user;
}

static struct zz_claim *claim_of(const struct aar_plat_claim *c)
{
	return (struct zz_claim *)c->priv;
}

/* ---- Mailbox ------------------------------------------------------------- */

static int board_call(const struct zz_state *s, uint16_t op,
		      const uint8_t *payload, uint32_t len, int long_op,
		      aar_zz_reply *reply)
{
	int status;

	memset(reply, 0, sizeof(*reply));
	if (!s->attached || !s->board.call)
		return AAR_E_NOSYS;
	status = s->board.call(s->board.user, op, payload, len, long_op,
			       reply);
	return status_to_aar(status);
}

/* A refusal reply carries the AAR_REASON_* word and, from a card that sends
 * it, the AAR_E_* code of the refusal (ZZAPP_REFUSAL_CODE). The mailbox status
 * maps PROFILE and INVAL to one value and NOGROUP to NOSYS, so the code wins
 * when the reply has one the library knows; otherwise the status mapping
 * stands, as for a card that sends no code. */
static int refusal(int rc, const aar_zz_reply *reply, uint32_t *reason)
{
	uint32_t why, code;

	if (rc == AAR_OK || rc == AAR_E_STALE || reply->len < 4u)
		return rc;
	why = be32(reply->payload);
	if (why != AAR_REASON_NONE && why <= AAR_REASON_LIMIT) {
		if (reason)
			*reason = why;
		return AAR_E_NORESOURCE;
	}
	if (reply->len < ZZAPP_REFUSAL_CODE_REPLY_BYTES)
		return rc;
	code = be32(reply->payload + ZZAPP_REFUSAL_CODE);
	if (code == (uint32_t)-AAR_E_INVAL || code == (uint32_t)-AAR_E_NOMEM ||
	    code == (uint32_t)-AAR_E_PROFILE || code == (uint32_t)-AAR_E_NOGROUP)
		return -(int)code;
	return rc;
}

static void handle_request(uint32_t handle,
			   uint8_t payload[ZZAPP_MAILBOX_PAYLOAD_BYTES])
{
	memset(payload, 0, ZZAPP_MAILBOX_PAYLOAD_BYTES);
	zzapp_build_handle(payload, handle);
}

/* One mailbox call on a claim. The card saying the handle is gone means the
 * claim was revoked: the load buffer it held is no longer ours to touch. */
static int claim_call(const struct zz_state *s, struct aar_plat_claim *c,
		      uint16_t op, uint8_t *payload, uint32_t len, int long_op,
		      aar_zz_reply *reply)
{
	aar_zz_reply local;
	struct zz_claim *zc = claim_of(c);
	int rc;

	if (!reply)
		reply = &local;
	rc = board_call(s, op, payload, len, long_op, reply);
	if (rc == AAR_E_STALE) {
		zc->load = NULL;
		zc->load_begun = 0u;
	}
	return rc;
}

/* ---- Query -------------------------------------------------------------------- */

/* The card's event block (zzapp_proto.h) is mapped by the first QUERY that
 * names one (the claim's, in the owner's context), cache-inhibited like the
 * control blocks, and read by the dispatcher: one word per pass, the group
 * words only when it moved. The sampled values are kept in `zz` and a claim
 * compares against them without touching the card. */
static void events_map(struct zz_state *s)
{
	volatile uint8_t *ptr;

	if (s->events || s->events_token == 0u || !s->board.map)
		return;
	if (s->board.map(s->board.user, s->events_token, ZZAPP_EVENT_BLOCK_BYTES,
			 &ptr) != AAR_OK || !ptr)
		return;
	s->events_sampled = 0u;
	s->events = ptr;
}

struct zz_query {
	uint32_t abi_major, abi_minor, events, caps, hostheap_largest;
	uint32_t queue_depth, commit_free, commit_total, records_free;
	uint32_t tables_free, tables_total, hostheap_free;
};

static int query_raw(struct zz_state *s, struct zz_query *qi)
{
	aar_zz_reply reply;
	const uint8_t *p = reply.payload;
	int rc;

	memset(qi, 0, sizeof(*qi));
	rc = board_call(s, ZZAPP_OP_QUERY, NULL, 0u, 0, &reply);
	if (rc != AAR_OK)
		return rc;
	if (reply.len < ZZAPP_MAILBOX_PAYLOAD_BYTES)
		return AAR_E_NOSYS;
	qi->abi_major = be32(p + ZZAPP_QUERY_ABI_MAJOR);
	qi->abi_minor = be32(p + ZZAPP_QUERY_ABI_MINOR);
	qi->events = be32(p + ZZAPP_QUERY_EVENTS);
	qi->caps = be32(p + ZZAPP_QUERY_CAPS);
	qi->hostheap_largest = be32(p + ZZAPP_QUERY_HOSTHEAP_LARGEST);
	qi->queue_depth = be32(p + ZZAPP_QUERY_QUEUE_DEPTH);
	qi->commit_free = be32(p + ZZAPP_QUERY_COMMIT_FREE);
	qi->commit_total = be32(p + ZZAPP_QUERY_COMMIT_TOTAL);
	qi->records_free = be32(p + ZZAPP_QUERY_RECORDS_FREE);
	qi->tables_free = be32(p + ZZAPP_QUERY_TABLES_FREE);
	qi->tables_total = be32(p + ZZAPP_QUERY_TABLES_TOTAL);
	qi->hostheap_free = be32(p + ZZAPP_QUERY_HOSTHEAP_FREE);
	/* The 0.6 app table has a different slot layout; do not claim an older
	 * card or negotiate a newer private mailbox layout here. */
	if (qi->abi_major != ZZAPP_ABI_MAJOR || qi->abi_minor != ZZAPP_ABI_MINOR ||
	    !(qi->caps & ZZAPP_CAP_Z3)) {
		s->abi_ok = 0;
		return AAR_E_NOSYS;
	}
	s->abi_ok = 1;
	if (!s->events) {
		s->events_token = qi->events;
		events_map(s);
	}
	return AAR_OK;
}

static int zz_query(struct aar_plat *p, struct aar_plat_query *q)
{
	struct zz_state *s = state_of(p);
	struct zz_query qi;
	int rc = query_raw(s, &qi);

	if (rc != AAR_OK)
		return rc;
	memset(q, 0, sizeof(*q));
	if (qi.caps & ZZAPP_CAP_DISPLAY)
		q->groups |= AAR_GROUP_DISPLAY;
	if (qi.caps & ZZAPP_CAP_AUDIO)
		q->groups |= AAR_GROUP_AUDIO;
	if (qi.caps & ZZAPP_CAP_GL)
		q->groups |= AAR_GROUP_GL;
	q->queue_depth = qi.queue_depth;
	q->platform[AAR_ZZ9000_INFO_RECORDS_FREE] = qi.records_free;
	q->platform[AAR_ZZ9000_INFO_TABLES_FREE] = qi.tables_free;
	q->platform[AAR_ZZ9000_INFO_TABLES_TOTAL] = qi.tables_total;
	q->platform[AAR_ZZ9000_INFO_COMMIT_FREE] = qi.commit_free;
	q->platform[AAR_ZZ9000_INFO_COMMIT_TOTAL] = qi.commit_total;
	q->platform[AAR_ZZ9000_INFO_HOSTHEAP_FREE] = qi.hostheap_free;
	q->platform[AAR_ZZ9000_INFO_HOSTHEAP_LARGEST] = qi.hostheap_largest;
	q->platform[AAR_ZZ9000_INFO_CAPS] = qi.caps;
	return AAR_OK;
}

/* ---- Claim, release ----------------------------------------------------------- */

static int release_handle(const struct zz_state *s, uint32_t handle)
{
	uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	aar_zz_reply reply;

	handle_request(handle, req);
	return board_call(s, ZZAPP_OP_RELEASE, req, sizeof(req), 0, &reply);
}

static int zz_claim(struct aar_plat *p, struct aar_plat_claim *c,
		    uint32_t *reason)
{
	struct zz_state *s = state_of(p);
	aar_zz_reply reply;
	uint32_t token, bytes;
	int rc;

	*reason = AAR_REASON_NONE;
	if (!s->attached)
		return AAR_E_NOSYS;
	if (s->board.prepare) {
		rc = s->board.prepare(s->board.user);
		if (rc != AAR_OK)
			return rc;
	}
	if (!s->abi_ok) {
		struct zz_query qi;

		rc = query_raw(s, &qi);
		if (rc != AAR_OK)
			return rc;
	}
	rc = board_call(s, ZZAPP_OP_CLAIM, NULL, 0u, 0, &reply);
	if (rc != AAR_OK)
		return refusal(rc, &reply, reason);
	if (reply.len < ZZAPP_CLAIM_REPLY_BYTES)
		return AAR_E_IO;
	c->handle = be32(reply.payload + ZZAPP_CLAIM_HANDLE);
	token = be32(reply.payload + ZZAPP_CLAIM_CB_TOKEN);
	bytes = be32(reply.payload + ZZAPP_CLAIM_CB_BYTES);
	if (c->handle == 0u)
		return AAR_E_IO;
	if (token == 0u || bytes < AAR_CB_BYTES || bytes > ZZ_CB_BYTES_MAX) {
		/* The claim exists on the card; give it back. */
		(void)release_handle(s, c->handle);
		c->handle = 0u;
		return AAR_E_IO;
	}
	c->cb_token = token;
	return AAR_OK;
}

/* The doorbell try-lock. The owner and the dispatcher may ring or retire the
 * doorbell of one claim; the loser gives up. On the 68k a Forbid() protects
 * the test and set of one byte and nothing else. */
#if AAR_ZZ9000_AMIGA
#define BELL_ENTER() Forbid()
#define BELL_LEAVE() Permit()
#else
#define BELL_ENTER() ((void)0)
#define BELL_LEAVE() ((void)0)
#endif

static int bell_try(struct zz_claim *zc)
{
#if AAR_ZZ9000_AMIGA
	int got = 0;

	BELL_ENTER();
	if (!zc->bell_busy) {
		zc->bell_busy = 1u;
		got = 1;
	}
	BELL_LEAVE();
	return got;
#else
	return !__atomic_exchange_n(&zc->bell_busy, 1u, __ATOMIC_ACQUIRE);
#endif
}

static void bell_unlock(struct zz_claim *zc)
{
#if AAR_ZZ9000_AMIGA
	zc->bell_busy = 0u;
#else
	__atomic_store_n(&zc->bell_busy, 0u, __ATOMIC_RELEASE);
#endif
}

/* The platform's pause between two tries of a short wait. */
static void bell_pause(const struct zz_state *s);

static int zz_release(struct aar_plat *p, struct aar_plat_claim *c)
{
	struct zz_state *s = state_of(p);
	struct zz_claim *zc = claim_of(c);
	uint32_t handle = c->handle;
	int rc;

	if (handle == 0u)
		return AAR_OK;
	/* Drop every pointer first: the card frees the memory behind them. */
	zc->load = NULL;
	zc->load_begun = 0u;
	zc->started = 0u;
	c->handle = 0u;
	/* A doorbell still in flight points into the claim; withdraw it. */
	while (!bell_try(zc))
		bell_pause(s);
	if (zc->bell_inflight && s->board.bell_cancel)
		s->board.bell_cancel(s->board.user, &zc->bell);
	zc->bell_inflight = 0u;
	bell_unlock(zc);
	rc = release_handle(s, handle);
	/* A handle the card no longer knows is released as far as we care. */
	return rc == AAR_E_STALE ? AAR_OK : rc;
}

/* ---- Load, start, stop -------------------------------------------------------- */

static uint32_t client_caps_of(const struct aar_plat_load_opts *o)
{
	uint32_t caps = 0u;

	if (!o)
		return 0u;
	if (o->lease_extended)
		caps |= ZZAPP_CLIENT_LEASE_EXTENDED;
	if (o->audio_route == AAR_AUDIO_ROUTE_AHI)
		caps |= ZZAPP_CLIENT_AUDIO_AHI;
	if (o->client_caps & AAR_CCAP_DISPLAY_CLUT8)
		caps |= ZZAPP_CLIENT_CLUT8;
	if (o->client_caps & AAR_CCAP_DISPLAY_PAN_ACK)
		caps |= ZZAPP_CLIENT_SCANOUT_ACK;
	if (o->client_caps & AAR_CCAP_DISPLAY_LATEST)
		caps |= ZZAPP_CLIENT_LATEST;
	return caps;
}

static int zz_load_begin(struct aar_plat *p, struct aar_plat_claim *c,
			 const struct aar_plat_load_opts *opts,
			 volatile uint8_t **staging, uint32_t *chunk_max,
			 uint32_t *reason)
{
	struct zz_state *s = state_of(p);
	struct zz_claim *zc = claim_of(c);
	uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	aar_zz_reply reply;
	uint32_t token, size;
	volatile uint8_t *ptr;
	int rc;

	if (c->handle == 0u)
		return AAR_E_STALE;
	if (zc->load_begun || zc->started)
		return AAR_E_BUSY;
	if (opts && (opts->client_caps & ~AAR_CCAP_KNOWN))
		return AAR_E_INVAL;
	*reason = AAR_REASON_NONE;
	memset(req, 0, sizeof(req));
	zzapp_build_load_begin_request(req, c->handle, client_caps_of(opts));
	rc = claim_call(s, c, ZZAPP_OP_LOAD_BEGIN, req, sizeof(req), 1, &reply);
	if (rc != AAR_OK)
		return refusal(rc, &reply, reason);
	if (reply.len < ZZAPP_LOAD_BEGIN_REPLY_BYTES)
		return AAR_E_IO;
	token = be32(reply.payload + ZZAPP_LOAD_REPLY_TOKEN);
	size = be32(reply.payload + ZZAPP_LOAD_REPLY_SIZE);
	zc->chunk_max = be32(reply.payload + ZZAPP_LOAD_REPLY_CHUNK_MAX);
	zc->argv_off = be32(reply.payload + ZZAPP_LOAD_REPLY_ARGV_OFF);
	if (zc->chunk_max == 0u || zc->chunk_max > ZZAPP_CHUNK_MAX ||
	    zc->chunk_max > zc->argv_off ||
	    zc->argv_off > size || size - zc->argv_off < ZZAPP_LOAD_ARGV_BYTES)
		return AAR_E_IO;
	if (!s->board.map || token == 0u)
		return AAR_E_IO;
	rc = s->board.map(s->board.user, token, size, &ptr);
	if (rc != AAR_OK || !ptr)
		return AAR_E_IO;
	zc->load = ptr;
	zc->load_begun = 1u;
	*staging = ptr;
	*chunk_max = zc->chunk_max;
	return AAR_OK;
}

static int zz_load_chunk(struct aar_plat *p, struct aar_plat_claim *c,
			 uint32_t file_off, uint32_t len)
{
	struct zz_claim *zc = claim_of(c);
	uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES];

	if (c->handle == 0u)
		return AAR_E_STALE;
	if (!zc->load_begun || zc->started)
		return AAR_E_INVAL;
	if (len < ZZAPP_CHUNK_MIN || len > zc->chunk_max ||
	    file_off > ZZAPP_IMAGE_MAX_BYTES - len)
		return AAR_E_INVAL;
	memset(req, 0, sizeof(req));
	zzapp_build_load_chunk_request(req, c->handle, file_off, len);
	return claim_call(state_of(p), c, ZZAPP_OP_LOAD_CHUNK, req,
			  sizeof(req), 0, NULL);
}

static int zz_load_end(struct aar_plat *p, struct aar_plat_claim *c,
		       uint32_t *reason)
{
	struct zz_claim *zc = claim_of(c);
	uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	aar_zz_reply reply;
	int rc;

	if (c->handle == 0u)
		return AAR_E_STALE;
	if (!zc->load_begun || zc->started)
		return AAR_E_INVAL;
	handle_request(c->handle, req);
	rc = claim_call(state_of(p), c, ZZAPP_OP_LOAD_END, req, sizeof(req), 1,
			&reply);
	return refusal(rc, &reply, reason);
}

static int zz_start(struct aar_plat *p, struct aar_plat_claim *c,
		    const char *const *argv, uint32_t argc)
{
	struct zz_claim *zc = claim_of(c);
	uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	volatile uint8_t *area;
	uint32_t at = ZZAPP_ARGV_STR_OFF, i, n, k;
	int rc;

	if (c->handle == 0u)
		return AAR_E_STALE;
	if (!zc->load_begun || zc->started || !zc->load)
		return AAR_E_INVAL;
	if (argc > ZZAPP_ARGV_MAX || (argc != 0u && !argv))
		return AAR_E_INVAL;
	area = zc->load + zc->argv_off;
	for (i = 0u; i < ZZAPP_LOAD_ARGV_BYTES; i++)
		area[i] = 0u;
	for (i = 0u; i < argc; i++) {
		if (!argv[i])
			return AAR_E_INVAL;
		n = (uint32_t)strlen(argv[i]);
		if (n >= ZZAPP_ARGV_STR_MAX ||
		    at + n + 1u > ZZAPP_LOAD_ARGV_BYTES)
			return AAR_E_INVAL;
		/* Offsets are from the start of the load buffer. */
		aar_put_be32(area + ZZAPP_ARGV_OFFS_OFF + i * 4u,
			     zc->argv_off + at);
		for (k = 0u; k <= n; k++)
			area[at + k] = (uint8_t)argv[i][k];
		at += n + 1u;
	}
	aar_put_be32(area + ZZAPP_ARGV_ARGC_OFF, argc);
	memset(req, 0, sizeof(req));
	zzapp_build_start_app_request(req, c->handle, argc);
	rc = claim_call(state_of(p), c, ZZAPP_OP_START, req, sizeof(req), 1,
			NULL);
	if (rc == AAR_OK) {
		/* The card frees the load buffer after it copied argv. */
		zc->started = 1u;
		zc->load_begun = 0u;
		zc->load = NULL;
	}
	return rc;
}

static int zz_stop(struct aar_plat *p, struct aar_plat_claim *c)
{
	uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES];

	if (c->handle == 0u)
		return AAR_E_STALE;
	handle_request(c->handle, req);
	return claim_call(state_of(p), c, ZZAPP_OP_STOP, req, sizeof(req), 0,
			  NULL);
}

/* ---- Shared memory ------------------------------------------------------------- */

static int zz_map(struct aar_plat *p, uint32_t token, uint32_t bytes,
		  volatile uint8_t **ptr, uint32_t *mapf)
{
	struct zz_state *s = state_of(p);
	int rc;

	if (!s->attached || !s->board.map)
		return AAR_E_NOSYS;
	if (token == 0u || bytes == 0u)
		return AAR_E_INVAL;
	if (s->board.prepare) {
		rc = s->board.prepare(s->board.user);
		if (rc != AAR_OK)
			return rc;
	}
	rc = s->board.map(s->board.user, token, bytes, ptr);
	if (rc != AAR_OK)
		return rc;
	*mapf = AAR_MAPF_UNCACHED;
	return AAR_OK;
}

static int zz_token(struct aar_plat *p, const volatile void *ptr,
		    uint32_t bytes, uint32_t *token)
{
	struct zz_state *s = state_of(p);

	if (!s->attached || !s->board.token)
		return AAR_E_NOSYS;
	return s->board.token(s->board.user, ptr, bytes, token);
}

/* ---- Doorbell ------------------------------------------------------------------- */

static int zz_kick(struct aar_plat *p, struct aar_plat_claim *c,
		   uint32_t bits)
{
	struct zz_state *s = state_of(p);
	struct zz_claim *zc = claim_of(c);
	uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	int rc;

	bits &= ZZAPP_KICK_KNOWN;
	if (!bits)
		return AAR_OK;
	if (c->handle == 0u)
		return AAR_E_STALE;
	if (!s->board.bell_post)
		return AAR_E_NOSYS;
	if (!bell_try(zc))
		return AAR_E_BUSY;
	if (zc->bell_inflight) {
		if (!s->board.bell_done(s->board.user, &zc->bell)) {
			bell_unlock(zc);
			return AAR_E_BUSY;
		}
		zc->bell_inflight = 0u;
	}
	memset(req, 0, sizeof(req));
	zzapp_build_kick_request(req, c->handle, bits);
	memset(&zc->bell, 0, sizeof(zc->bell));
	rc = s->board.bell_post(s->board.user, &zc->bell, ZZAPP_OP_KICK, req,
				sizeof(req));
	if (rc == AAR_OK)
		zc->bell_inflight = 1u;
	bell_unlock(zc);
	return rc;
}

/* 1 while the doorbell request is still in flight, so the dispatcher keeps
 * polling this claim; 0 when there is nothing left to retire. */
static int zz_poll(struct aar_plat *p, struct aar_plat_claim *c)
{
	struct zz_state *s = state_of(p);
	struct zz_claim *zc = claim_of(c);

	if (c->handle == 0u || !zc->bell_inflight)
		return 0;
	if (!bell_try(zc))
		return 1;
	if (zc->bell_inflight && s->board.bell_done(s->board.user, &zc->bell))
		zc->bell_inflight = 0u;
	bell_unlock(zc);
	return zc->bell_inflight != 0u;
}

/* ---- Event counts ----------------------------------------------------------------- */

static int zz_events_sample(struct aar_plat *p)
{
	struct zz_state *s = state_of(p);
	volatile uint8_t *ev = s->events;
	uint32_t global, i;

	if (!ev)
		return AAR_PLAT_EVENTS_NONE;
	global = aar_get_be32(ev);
	if (s->events_sampled && global == s->events_global)
		return AAR_PLAT_EVENTS_QUIET;
	for (i = 0u; i < ZZAPP_EVENT_GROUPS; i++)
		s->events_group[i] = aar_get_be32(ev + 4u * (1u + i));
	s->events_global = global;
	s->events_sampled = 1u;
	return AAR_PLAT_EVENTS_NEWS;
}

static int zz_claim_news(struct aar_plat *p, struct aar_plat_claim *c)
{
	struct zz_state *s = state_of(p);
	struct zz_claim *zc = claim_of(c);
	uint32_t group = (c->handle & 0xffffu) / ZZAPP_EVENT_GROUP_RECORDS;
	uint32_t now;

	/* Nothing sampled yet: take it as news, the queues decide. */
	if (!s->events_sampled)
		return 1;
	now = group < ZZAPP_EVENT_GROUPS ? s->events_group[group] :
					   s->events_global;
	if (zc->news_asked && zc->news_seen == now)
		return 0;
	zc->news_asked = 1u;
	zc->news_seen = now;
	return 1;
}

/* ---- Barriers ------------------------------------------------------------------- */

#if AAR_ZZ9000_AMIGA
#define ZZ_BARRIER_ACQUIRE NULL
#define ZZ_BARRIER_RELEASE NULL
#else
static void barrier_acquire(void)
{
	if (zz.acquire)
		zz.acquire();
}

static void barrier_release(void)
{
	if (zz.release)
		zz.release();
}
#define ZZ_BARRIER_ACQUIRE barrier_acquire
#define ZZ_BARRIER_RELEASE barrier_release
#endif

/* ---- The extension table: every claim on the card -------------------------------- */

static LONG ext_list(ULONG cursor, AARZZ9000List *out)
{
	uint8_t req[ZZAPP_LIST_CURSOR + 4u];
	aar_zz_reply reply;
	const uint8_t *p = reply.payload;
	AARZZ9000List info;
	uint32_t reasons, size;
	int rc;

	if (!out || out->size < 4u)
		return AAR_E_INVAL;
	size = out->size;
	put32(req, cursor);
	rc = board_call(&zz, ZZAPP_OP_LIST, req, sizeof(req), 0, &reply);
	if (rc != AAR_OK)
		return rc;
	if (reply.len < ZZAPP_LIST_REPLY_BYTES)
		return AAR_E_IO;
	memset(&info, 0, sizeof(info));
	info.size = size;
	info.handle = be32(p + ZZAPP_LIST_HANDLE);
	if (info.handle != 0u) {
		info.state = be32(p + ZZAPP_LIST_STATE);
		info.committed = be32(p + ZZAPP_LIST_COMMITTED);
		info.index = be32(p + ZZAPP_LIST_INDEX);
		info.cpu_ms = be32(p + ZZAPP_LIST_CPU_MS);
		info.service_ms = be32(p + ZZAPP_LIST_SERVICE_MS);
		reasons = be32(p + ZZAPP_LIST_REASONS);
		info.block_reason = reasons >> 24;
		info.end_reason = (reasons >> 16) & 0xffu;
		info.flags = reasons & 0xffffu;
		memcpy(info.name, p + ZZAPP_LIST_NAME, ZZAPP_LIST_NAME_BYTES);
		info.name[ZZAPP_LIST_NAME_BYTES] = 0;
	}
	memcpy(out, &info, size < sizeof(info) ? size : sizeof(info));
	return info.handle != 0u ? 1 : 0;
}

static LONG ext_release_handle(ULONG handle)
{
	return release_handle(&zz, handle);
}

static aar_client_ext_zz9000 zz_ext = {
	sizeof(aar_client_ext_zz9000),
	0u,
	ext_list,
	ext_release_handle,
	/* The two that take a handle act on the core's own state. The log sink
	 * is not in this table: AARSetLogSink is a call of the base library. */
	aar_core_ext_stats,
	aar_core_ext_lease_pause,
};

static const void *zz_ext_get(struct aar_plat *p, uint32_t ext_id)
{
	(void)p;
	return ext_id == AAR_EXT_ZZ9000_CLIENT ? &zz_ext : NULL;
}

/* ---- The platform object ----------------------------------------------------------- */

static int board_attach(struct zz_state *s);
static void board_detach(struct zz_state *s);

static int zz_attach(struct aar_plat *p)
{
	struct zz_state *s = state_of(p);
	int rc;

	if (s->attached)
		return AAR_OK;
	rc = board_attach(s);
	if (rc != AAR_OK)
		return rc;
	s->attached = 1;
	s->abi_ok = 0;
	return AAR_OK;
}

static void zz_detach(struct aar_plat *p)
{
	struct zz_state *s = state_of(p);

	if (!s->attached)
		return;
	s->attached = 0;
	s->events = NULL;
	s->events_token = 0u;
	s->events_sampled = 0u;
	board_detach(s);
}

static const struct aar_plat_ops zz_ops = {
	sizeof(struct aar_plat_ops),
	sizeof(struct zz_claim),
	zz_attach,
	zz_detach,
	zz_query,
	zz_claim,
	zz_release,
	zz_load_begin,
	zz_load_chunk,
	zz_load_end,
	zz_start,
	zz_stop,
	zz_map,
	zz_token,
	zz_kick,
	zz_poll,
	zz_ext_get,
	ZZ_BARRIER_ACQUIRE,
	ZZ_BARRIER_RELEASE,
	NULL,   /* cache_publish: the mappings are cache-inhibited */
	NULL,   /* cache_acquire */
	zz_events_sample,
	zz_claim_news,
};

static struct aar_plat zz_plat = {
	AAR_PLAT_VERSION,
	0u,
	&zz_info,
	&zz_ops,
	&zz,
};

struct aar_plat *aar_plat_get(void)
{
	return &zz_plat;
}

/* ====================================================================== */
/* The 68k board                                                            */
/* ====================================================================== */

#if AAR_ZZ9000_AMIGA

#include <devices/timer.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/semaphores.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/zz9k.h>
#include "zz9k/library_vectors.h"
#include "zz9k/request.h"

#define ZZ_LONG_OP_MS         120000UL
#define ZZ_MIN_REVISION       36
#define ZZ_POLL_MS            20UL     /* the wait of a deferred reply */

#define MMU_MAPP_CACHEINHIBIT 0x00000040UL
#define MMU_MAPP_COPYBACK     0x00002000UL
#define MMU_MAPP_IMPRECISE    0x00200000UL
#define MMU_MAPP_NONSERIALIZED 0x20000000UL
#define MMU_CACHE_MASK (MMU_MAPP_CACHEINHIBIT | MMU_MAPP_COPYBACK | \
			MMU_MAPP_IMPRECISE | MMU_MAPP_NONSERIALIZED)

struct mmu_context;

typedef struct zz_amiga {
	struct Library *zz9k;
	struct Library *volatile mmu; /* opened at the first claim or map; set last */
	struct mmu_context *context;
	uint32_t page;
	struct SignalSemaphore mmu_lock_sem;
	struct MsgPort *timer_port;
	struct timerequest *timer;    /* template; GetSysTime needs its device */
	uint32_t base, size;
} zz_amiga;

/* One doorbell request, in the claim. */
struct zz_amiga_bell {
	ZZ9KAsyncRequest async;
	ZZ9KRequest request;
};

_Static_assert(sizeof(struct zz_amiga_bell) <= sizeof(aar_zz_bell),
	       "aar_zz_bell is too small for the 68k doorbell request");

/* The vectors of mmu.library and expansion.library are called through the
 * explicit base: the library has no global bases of its own for them. */
static struct mmu_context *mmu_default_context(struct Library *mmu)
{
	register struct mmu_context *d0 __asm("d0");
	register struct Library *a6 __asm("a6") = mmu;

	__asm volatile("jsr -150(a6)" : "=r"(d0) : "r"(a6) :
		       "cc", "memory", "d1", "a0", "a1");
	return d0;
}

static uint32_t mmu_page_size(struct Library *mmu, struct mmu_context *ctx)
{
	register uint32_t d0 __asm("d0");
	register struct mmu_context *a0 __asm("a0") = ctx;
	register struct Library *a6 __asm("a6") = mmu;

	__asm volatile("jsr -48(a6)" : "=r"(d0), "+r"(a0) : "r"(a6) :
		       "cc", "memory", "d1", "a1");
	return d0;
}

static int mmu_set_properties(struct Library *mmu, struct mmu_context *ctx,
			      uint32_t flags, uint32_t mask, uint32_t address,
			      uint32_t size)
{
	register uint32_t d0 __asm("d0") = size;
	register uint32_t d1 __asm("d1") = flags;
	register uint32_t d2 __asm("d2") = mask;
	register struct mmu_context *a0 __asm("a0") = ctx;
	register void *a1 __asm("a1") = (void *)(uintptr_t)address;
	register void *a2 __asm("a2") = 0;
	register struct Library *a6 __asm("a6") = mmu;

	__asm volatile("jsr -84(a6)" : "+r"(d0), "+r"(d1), "+r"(d2), "+r"(a0),
		       "+r"(a1), "+r"(a2) : "r"(a6) : "cc", "memory");
	return d0 != 0U;
}

static void mmu_lock(struct Library *mmu, struct mmu_context *ctx)
{
	register struct mmu_context *a0 __asm("a0") = ctx;
	register struct Library *a6 __asm("a6") = mmu;

	__asm volatile("jsr -72(a6)" : "+r"(a0) : "r"(a6) :
		       "cc", "memory", "d0", "d1", "a1", "a2");
}

static void mmu_unlock(struct Library *mmu, struct mmu_context *ctx)
{
	register struct mmu_context *a0 __asm("a0") = ctx;
	register struct Library *a6 __asm("a6") = mmu;

	__asm volatile("jsr -78(a6)" : "+r"(a0) : "r"(a6) :
		       "cc", "memory", "d0", "d1", "a1", "a2");
}

static int mmu_rebuild(struct Library *mmu, struct mmu_context *ctx)
{
	register uint32_t d0 __asm("d0");
	register struct mmu_context *a0 __asm("a0") = ctx;
	register struct Library *a6 __asm("a6") = mmu;

	__asm volatile("jsr -96(a6)" : "=r"(d0), "+r"(a0) : "r"(a6) :
		       "cc", "memory", "d1", "a1", "a2");
	return d0 != 0U;
}

/* expansion.library FindConfigDev, LVO -72. */
static struct ConfigDev *find_config_dev(struct Library *expansion,
					 uint32_t manufacturer, uint32_t product)
{
	register uint32_t d0 __asm("d0") = manufacturer;
	register uint32_t d1 __asm("d1") = product;
	register struct ConfigDev *a0 __asm("a0") = 0;
	register struct Library *a6 __asm("a6") = expansion;

	__asm volatile("jsr -72(a6)" : "+r"(d0), "+r"(d1), "+r"(a0) :
		       "r"(a6) : "cc", "memory", "a1");
	return (struct ConfigDev *)(uintptr_t)d0;
}

/* Cache-inhibits whole pages over [address, address + bytes): write back and
 * invalidate under the old mapping, set the property, rebuild the tree. The
 * pages stay inhibited: nothing restores them (the restore costs about 0.7 s
 * a range), so a later opener cannot make a live claim's block copyback
 * again. */
static int amiga_inhibit(zz_amiga *a, uint32_t address, uint32_t bytes)
{
	uint32_t first = address & ~(a->page - 1u);
	uint32_t last = (address + bytes + a->page - 1u) & ~(a->page - 1u);

	CacheClearE((APTR)(uintptr_t)address, bytes, CACRF_ClearD);
	mmu_lock(a->mmu, a->context);
	if (!mmu_set_properties(a->mmu, a->context, MMU_MAPP_CACHEINHIBIT,
				MMU_CACHE_MASK, first, last - first) ||
	    !mmu_rebuild(a->mmu, a->context)) {
		mmu_unlock(a->mmu, a->context);
		return 0;
	}
	mmu_unlock(a->mmu, a->context);
	return 1;
}

/* mmu.library is opened by the first task that needs a mapping and kept
 * until the library detaches. */
static int amiga_prepare(void *user)
{
	zz_amiga *a = (zz_amiga *)user;
	struct Library *mmu;

	if (a->mmu)
		return AAR_OK;
	ObtainSemaphore(&a->mmu_lock_sem);
	if (!a->mmu) {
		mmu = OpenLibrary((CONST_STRPTR)"mmu.library", 41);
		if (mmu) {
			struct mmu_context *ctx = mmu_default_context(mmu);
			uint32_t page = ctx ? mmu_page_size(mmu, ctx) : 0u;

			if (page == 0u || (page & (page - 1u)) != 0u) {
				CloseLibrary(mmu);
			} else {
				a->context = ctx;
				a->page = page;
				a->mmu = mmu;
			}
		}
	}
	ReleaseSemaphore(&a->mmu_lock_sem);
	return a->mmu ? AAR_OK : AAR_E_NOSYS;
}

static int amiga_map(void *user, uint32_t token, uint32_t bytes,
		     volatile uint8_t **ptr)
{
	zz_amiga *a = (zz_amiga *)user;
	uint32_t address;
	int rc;

	rc = amiga_prepare(a);
	if (rc != AAR_OK)
		return rc;
	if (a->size != 0u && (token >= a->size || bytes > a->size - token))
		return AAR_E_INVAL;
	if (bytes == 0u || token > UINT32_MAX - a->base - bytes)
		return AAR_E_INVAL;
	address = a->base + token;
	if (!amiga_inhibit(a, address, bytes))
		return AAR_E_NOMEM;
	*ptr = (volatile uint8_t *)(uintptr_t)address;
	return AAR_OK;
}

static int amiga_token(void *user, const volatile void *ptr, uint32_t bytes,
		       uint32_t *token)
{
	zz_amiga *a = (zz_amiga *)user;
	uint32_t address = (uint32_t)(uintptr_t)ptr;

	if (a->size == 0u || address < a->base || address - a->base >= a->size ||
	    (bytes != 0u && bytes > a->size - (address - a->base)))
		return AAR_E_INVAL;
	*token = address - a->base;
	return AAR_OK;
}

static uint32_t amiga_now_ms(zz_amiga *a)
{
	struct timeval tv;
	register struct Device *a6 __asm("a6") = a->timer->tr_node.io_Device;
	register struct timeval *a0 __asm("a0") = &tv;

	/* timer.device GetSysTime, LVO -66. */
	__asm volatile("jsr -66(a6)" : "+r"(a0) : "r"(a6) :
		       "cc", "memory", "d0", "d1", "a1");
	return tv.tv_secs * 1000UL + tv.tv_micro / 1000UL;
}

/* A pause that a plain Task can take as well as a Process: a timer request of
 * the caller's own, on the open device of the template. */
struct amiga_sleeper {
	struct MsgPort *port;
	struct timerequest *req;
};

static int sleeper_open(zz_amiga *a, struct amiga_sleeper *z)
{
	z->port = CreateMsgPort();
	z->req = z->port ? (struct timerequest *)CreateIORequest(
				   z->port, sizeof(struct timerequest)) : NULL;
	if (!z->req) {
		if (z->port)
			DeleteMsgPort(z->port);
		return 0;
	}
	z->req->tr_node.io_Device = a->timer->tr_node.io_Device;
	z->req->tr_node.io_Unit = a->timer->tr_node.io_Unit;
	return 1;
}

static void sleeper_pause(struct amiga_sleeper *z, uint32_t ms)
{
	z->req->tr_node.io_Command = TR_ADDREQUEST;
	z->req->tr_time.tv_secs = ms / 1000UL;
	z->req->tr_time.tv_micro = (ms % 1000UL) * 1000UL;
	(void)DoIO((struct IORequest *)z->req);
}

static void sleeper_close(struct amiga_sleeper *z)
{
	DeleteIORequest((struct IORequest *)z->req);
	DeleteMsgPort(z->port);
}

static void bell_pause(const struct zz_state *s)
{
	struct amiga_sleeper z;

	if (s->board.user && sleeper_open((zz_amiga *)s->board.user, &z)) {
		sleeper_pause(&z, 2u);
		sleeper_close(&z);
	}
}

static void request_fill(ZZ9KRequest *request, uint16_t opcode,
			 const uint8_t *payload, uint32_t len)
{
	zz9k_request_init(request, opcode);
	request->entry.payload_len = (uint16_t)len;
	if (payload && len)
		memcpy(request->entry.payload.inline_data, payload, len);
}

static void reply_fill(aar_zz_reply *out, const ZZ9KMailboxEntry *in)
{
	out->status = (int32_t)in->status;
	out->len = in->payload_len < ZZAPP_MAILBOX_PAYLOAD_BYTES ?
		   in->payload_len : ZZAPP_MAILBOX_PAYLOAD_BYTES;
	memcpy(out->payload, in->payload.inline_data, out->len);
}

static int amiga_call(void *user, uint16_t opcode, const uint8_t *payload,
		      uint32_t len, int long_op, aar_zz_reply *reply)
{
	zz_amiga *a = (zz_amiga *)user;
	ZZ9KRequest request;
	ZZ9KMailboxEntry r;
	int status;

	request_fill(&request, opcode, payload, len);
	memset(&r, 0, sizeof(r));
	if (!long_op) {
		status = ZZ9KCallAtBase(a->zz9k, &request, &r,
					ZZ9K_DEFAULT_TIMEOUT_TICKS);
	} else {
		/* Deferred reply: the card answers when the work is done. Poll,
		 * retry a BUSY (one deferred operation per claim) for as long
		 * as the operation may take. The lease is not this call's
		 * business: the library's interrupt keeps it. */
		struct amiga_sleeper z;
		uint32_t started = amiga_now_ms(a);

		if (!sleeper_open(a, &z))
			return ZZ9K_STATUS_NO_MEMORY;
		for (;;) {
			ZZ9KAsyncRequest async;
			uint32_t op_started = amiga_now_ms(a);

			memset(&async, 0, sizeof(async));
			status = ZZ9KCallAsyncAtBase(a->zz9k, &async, &request,
						     NULL, NULL);
			if (status == ZZ9K_STATUS_QUEUED) {
				while (!async.completed) {
					uint32_t done = 0u;

					status = ZZ9KPollAtBase(a->zz9k, 1u,
								&done);
					if (status != ZZ9K_STATUS_OK) {
						/* The request lives on this
						 * stack frame: unlink it from
						 * the library's pending list
						 * before it goes. */
						(void)ZZ9KCancelAsyncAtBase(
							a->zz9k, &async);
						sleeper_close(&z);
						return status;
					}
					if (async.completed)
						break;
					if (amiga_now_ms(a) - op_started >=
					    ZZ_LONG_OP_MS) {
						(void)ZZ9KCancelAsyncAtBase(
							a->zz9k, &async);
						sleeper_close(&z);
						return ZZ9K_STATUS_TIMEOUT;
					}
					sleeper_pause(&z, ZZ_POLL_MS);
				}
				r = async.reply;
				status = async.status;
			}
			if (status != ZZ9K_STATUS_BUSY &&
			    !(status == ZZ9K_STATUS_OK &&
			      r.status == ZZ9K_STATUS_BUSY))
				break;
			if (amiga_now_ms(a) - started >= ZZ_LONG_OP_MS)
				break;
			sleeper_pause(&z, ZZ_POLL_MS);
		}
		sleeper_close(&z);
	}
	reply_fill(reply, &r);
	if (status == ZZ9K_STATUS_OK && r.status != ZZ9K_STATUS_OK)
		status = (int)r.status;
	return status;
}

static int amiga_bell_post(void *user, aar_zz_bell *bell, uint16_t opcode,
			   const uint8_t *payload, uint32_t len)
{
	zz_amiga *a = (zz_amiga *)user;
	struct zz_amiga_bell *b = (struct zz_amiga_bell *)bell;
	int status;

	request_fill(&b->request, opcode, payload, len);
	memset(&b->async, 0, sizeof(b->async));
	status = ZZ9KCallAsyncAtBase(a->zz9k, &b->async, &b->request, NULL,
				     NULL);
	if (status != ZZ9K_STATUS_QUEUED)
		return status == ZZ9K_STATUS_BUSY ? AAR_E_BUSY : AAR_E_IO;
	return AAR_OK;
}

static int amiga_bell_done(void *user, aar_zz_bell *bell)
{
	zz_amiga *a = (zz_amiga *)user;
	struct zz_amiga_bell *b = (struct zz_amiga_bell *)bell;
	uint32_t done = 0u;

	if (!b->async.completed)
		(void)ZZ9KPollAtBase(a->zz9k, 1u, &done);
	return b->async.completed != 0;
}

static void amiga_bell_cancel(void *user, aar_zz_bell *bell)
{
	zz_amiga *a = (zz_amiga *)user;
	struct zz_amiga_bell *b = (struct zz_amiga_bell *)bell;

	if (!b->async.completed)
		(void)ZZ9KCancelAsyncAtBase(a->zz9k, &b->async);
}

static void amiga_close(zz_amiga *a)
{
	if (a->timer) {
		CloseDevice((struct IORequest *)a->timer);
		DeleteIORequest((struct IORequest *)a->timer);
	}
	if (a->timer_port)
		DeleteMsgPort(a->timer_port);
	if (a->mmu)
		CloseLibrary(a->mmu);
	if (a->zz9k)
		CloseLibrary(a->zz9k);
	FreeVec(a);
}

static int board_attach(struct zz_state *s)
{
	zz_amiga *a;
	struct Library *expansion;
	struct ConfigDev *dev;

	memset(&s->board, 0, sizeof(s->board));
	a = (zz_amiga *)AllocVec(sizeof(*a), MEMF_PUBLIC | MEMF_CLEAR);
	if (!a)
		return AAR_E_NOMEM;
	InitSemaphore(&a->mmu_lock_sem);
	a->zz9k = OpenLibrary((CONST_STRPTR)ZZ9K_LIBRARY_NAME, 0);
	if (!a->zz9k || a->zz9k->lib_Revision < ZZ_MIN_REVISION)
		goto fail;
	expansion = OpenLibrary((CONST_STRPTR)"expansion.library", 0);
	if (!expansion)
		goto fail;
	dev = find_config_dev(expansion, ZZ9K_MNT_MANUFACTURER,
			      ZZ9K_PRODUCT_Z3);
	if (dev) {
		a->base = (uint32_t)dev->cd_BoardAddr;
		a->size = dev->cd_BoardSize;
	}
	CloseLibrary(expansion);
	if (!dev)
		goto fail;
	a->timer_port = CreateMsgPort();
	if (a->timer_port)
		a->timer = (struct timerequest *)CreateIORequest(
			a->timer_port, sizeof(struct timerequest));
	if (!a->timer || OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK,
				    (struct IORequest *)a->timer, 0) != 0) {
		if (a->timer) {
			DeleteIORequest((struct IORequest *)a->timer);
			a->timer = NULL;
		}
		goto fail;
	}
	s->board.user = a;
	s->board.call = amiga_call;
	s->board.bell_post = amiga_bell_post;
	s->board.bell_done = amiga_bell_done;
	s->board.bell_cancel = amiga_bell_cancel;
	s->board.prepare = amiga_prepare;
	s->board.map = amiga_map;
	s->board.token = amiga_token;
	return AAR_OK;

fail:
	amiga_close(a);
	return AAR_E_NOSYS;
}

static void board_detach(struct zz_state *s)
{
	if (s->board.user)
		amiga_close((zz_amiga *)s->board.user);
	memset(&s->board, 0, sizeof(s->board));
}

#else /* host */

#include <sched.h>

static aar_zz_board host_board;

void aar_zz9000_host_board(const aar_zz_board *board)
{
	if (board)
		host_board = *board;
	else
		memset(&host_board, 0, sizeof(host_board));
}

static void bell_pause(const struct zz_state *s)
{
	(void)s;
	sched_yield();
}

static int board_attach(struct zz_state *s)
{
	if (!host_board.call)
		return AAR_E_NOSYS;
	s->board = host_board;
	s->acquire = host_board.acquire;
	s->release = host_board.release;
	return AAR_OK;
}

static void board_detach(struct zz_state *s)
{
	memset(&s->board, 0, sizeof(s->board));
}

#endif /* AAR_ZZ9000_AMIGA */
