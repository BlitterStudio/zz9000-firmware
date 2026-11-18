/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * The client side of the AAR transport on the ZZ9000: claim an app, map its
 * control block and queue pair, load, start and stop it, serve the requests
 * the app posts (the shape of AARServiceNext and AARServiceReply), post input
 * events and messages, renew the lease, read the audio ring, release the
 * claim. This is the module ZZRun and the SDK tools link, and the one the
 * resident aar.library (slice 8) will wrap. It replaces the ZZ9KApp* calls of
 * zz9k.library and the launcher window of the earlier protocol.
 *
 * The module is source, linked statically. It has no global state: every call
 * takes the claim (aar_xport) or, before there is one, the board. All of the
 * platform sits behind aar_xport_board (mailbox calls, mapping with cache
 * inhibit), so the same code runs on the 68k with the real card and on a
 * host with a fake board (sdk/tests/aar_xport_zz9000_test.c, which pairs it
 * with the firmware's zzapp_xport.c). Producer and consumer paths are the
 * aar_queue_* reference model of aar/transport.h unchanged; the barrier hooks
 * are NULL on the 68k.
 *
 * Wire. Mailbox operations are the private wire of zzapp_proto.h (this header
 * includes it with the window protocol shim off). Queue entries, the control
 * block and message credits are aar/transport.h.
 *
 * Threading. A claim belongs to one task. aar_xport_lease_renew() and
 * aar_xport_request_stop() are the two calls that may run from a timer or an
 * interrupt (they only store one control block word). Everything else runs in
 * the owner's task context. The module does not lock.
 *
 * Release and stale handles. aar_xport_release() ends the claim; the card then
 * returns the control block, the queues and every service buffer, so the
 * module stops touching them first (every map is dropped, every later call
 * answers AAR_E_STALE). The same happens when any mailbox call answers
 * AAR_E_STALE: that is the firmware saying the claim was revoked (lease lost,
 * quarantine) and its memory may be reused.
 *
 * Required of the program on the 68k: zz9k.library (revision 36 or later) and
 * mmu.library 41 or later (the mapping has to be cache-inhibited), both
 * opened by aar_xport_amiga_board_open().
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AAR_XPORT_ZZ9000_H
#define AAR_XPORT_ZZ9000_H

#include <stdint.h>

#include "aar/app.h"
#include "aar/transport.h"

#include "zzapp_proto.h"

#if defined(__amigaos__) || defined(__amiga__) || defined(__AMIGA__)
#define AAR_XPORT_AMIGA 1
#else
#define AAR_XPORT_AMIGA 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- The board: what the module needs of the platform --------------------- */

/* One mailbox reply. `status` is ZZ9K_STATUS_* of the reply (0 is OK); the
 * payload holds the reply bytes whatever the status (a refusal carries the
 * AAR_REASON_* word at 0). */
typedef struct aar_xport_reply {
	int32_t status;
	uint32_t len;
	uint8_t payload[ZZAPP_MAILBOX_PAYLOAD_BYTES];
} aar_xport_reply;

/* Called by a long mailbox call about once a millisecond while it waits. It
 * renews the lease; a non-zero return cancels the wait. */
typedef int (*aar_xport_tick_fn)(void *user);

typedef struct aar_xport_board {
	void *user;
	/*
	 * One mailbox call with a request payload of `len` bytes (at most 48).
	 * `long_op` is non-zero for the operations the card answers late (load,
	 * start): the call then waits up to two minutes, calling `tick` while it
	 * does, and retries a BUSY answer. Returns the status of the exchange:
	 * 0 when a reply arrived and its status is OK, else the ZZ9K_STATUS_*
	 * that says why (a reply status, or the transport's own failure).
	 * `reply` is filled whenever a reply arrived.
	 */
	int (*call)(void *user, uint16_t opcode, const uint8_t *payload,
		    uint32_t len, int long_op, aar_xport_tick_fn tick,
		    void *tick_user, aar_xport_reply *reply);
	/*
	 * A mailbox request nobody waits for (the doorbell). AAR_OK when it was
	 * sent, AAR_E_BUSY when the previous one is still in flight (the module
	 * coalesces and sends again later), another AAR_E_* when it cannot.
	 */
	int (*post)(void *user, uint16_t opcode, const uint8_t *payload,
		    uint32_t len);
	/*
	 * The CPU pointer of [token, token + bytes), made cache-inhibited, or
	 * NULL when it is out of the board. `token` is a board offset (an
	 * aar_shared haddr: the card address less ADDR_ADJ); `bytes` is a
	 * multiple of 8 KiB and the range lies in one block the card allocated
	 * 8 KiB aligned and sized. The module calls it once per range it needs.
	 */
	volatile uint8_t *(*map)(void *user, uint32_t token, uint32_t bytes);
	/* Barrier hooks of aar_queue_*; NULL on the 68k. */
	aar_barrier_fn acquire;
	aar_barrier_fn release;
} aar_xport_board;

/* ---- Query, list, administrative release (no claim needed) ---------------- */

typedef struct aar_xport_query_info {
	uint32_t abi_major, abi_minor;
	uint32_t state;
	uint32_t caps;                  /* ZZAPP_CAP_* */
	uint32_t hostheap_largest;      /* largest free host-visible block */
	uint32_t queue_depth;           /* depth of a new claim's queues */
	uint32_t commit_free, commit_total;
	uint32_t records_free;
	uint32_t tables_free, tables_total;
	uint32_t hostheap_free;
} aar_xport_query_info;

/* ZZAPP_OP_QUERY. AAR_OK, or AAR_E_IO/AAR_E_NOSYS... from the mailbox. */
int aar_xport_query(const aar_xport_board *board, aar_xport_query_info *info);

typedef struct aar_xport_list_info {
	uint32_t handle;                /* 0: no further claim */
	uint32_t state;                 /* AAR_STATE_* */
	uint32_t committed;
	uint32_t index;                 /* pass index + 1 as the next cursor */
	uint32_t cpu_ms, service_ms;
	uint8_t block_reason;           /* ZZAPP_BLOCK_* */
	uint8_t end_reason;             /* AAR_END_* */
	uint16_t flags;                 /* ZZAPP_LIST_FLAG_* */
	char name[ZZAPP_LIST_NAME_BYTES + 1];
} aar_xport_list_info;

/* ZZAPP_OP_LIST: the next live claim at or after `cursor`. 1 with *info
 * filled, 0 when there is none, a negative AAR_E_* on failure. */
int aar_xport_list(const aar_xport_board *board, uint32_t cursor,
		   aar_xport_list_info *info);

/* ZZAPP_OP_RELEASE of any claim by its handle (aar-run status, quarantine
 * clean-up). AAR_OK, AAR_E_STALE for a handle that is not live. */
int aar_xport_release_handle(const aar_xport_board *board, uint32_t handle);

/* ---- The claim ----------------------------------------------------------- */

#define AAR_XPORT_MAPS        16u
#define AAR_XPORT_INBOX       64u   /* messages held for aar_xport_get_message */

typedef struct aar_xport_stats {
	uint32_t requests;              /* service requests handed out */
	uint32_t replies;               /* completions posted */
	uint32_t reply_busy;            /* completions that found the CQ full */
	uint32_t bad_buf;               /* requests whose buffer did not map */
	uint32_t bad_entries;           /* SQ entries of an op the client does not take */
	uint32_t msgs_taken;            /* messages moved into the inbox */
	uint32_t msgs_overflow;         /* messages lost to a full inbox */
	uint32_t msgs_sent;
	uint32_t logs;                  /* log records delivered to the sink */
	uint32_t logs_unclaimed;        /* log records dropped, no sink */
	uint32_t events_posted;
	uint32_t events_merged;         /* mouse events folded into another */
	uint32_t events_dropped;        /* events lost to a full CQ */
	uint32_t kicks;                 /* doorbells sent */
	uint32_t kicks_coalesced;       /* doorbells folded into a pending one */
	uint32_t maps;                  /* ranges mapped */
} aar_xport_stats;

/* The claim. The fields are private: use the functions. It is plain storage
 * (a few KiB) so the caller decides where it lives. */
typedef struct aar_xport {
	aar_xport_board board;
	uint32_t handle;                /* 0 once released or revoked */
	volatile uint8_t *cb;           /* control block, NULL once detached */
	uint32_t cb_token;
	aar_queue sq, cq;
	uint32_t msg_posted;            /* messages posted; AAR_CB_MSG_CONSUMED is the other half */
	uint32_t lease;
	uint32_t reason;                /* last AAR_REASON_* of a refusal */
	struct { uint32_t token, bytes; volatile uint8_t *ptr; } maps[AAR_XPORT_MAPS];
	uint32_t map_next;
	/* the load buffer */
	volatile uint8_t *load;
	uint32_t load_bytes, chunk_max, argv_off;
	uint8_t load_begun, started;
	/* messages from the app */
	uint8_t inbox[AAR_XPORT_INBOX][AAR_MSG_BYTES];
	uint32_t inbox_head, inbox_tail;
	void (*log_sink)(void *user, const void *data, uint32_t len);
	void *log_user;
	/* events */
	aar_event mouse;                /* coalesced, not yet posted */
	uint8_t mouse_pending;
	/* doorbell */
	uint32_t kick_pending;
	/* the audio ring */
	volatile uint8_t *audio;
	uint32_t audio_frames, audio_channels;
	aar_xport_stats stats;
} aar_xport;

/*
 * Query, CLAIM, map the control block and both queues (from the descriptors
 * in the control block, not from assumptions), check magic, version and
 * handle, write AAR_CB_CLIENT_PRESENT. AAR_OK, or:
 *   AAR_E_NORESOURCE  the card refused the claim; *reason is AAR_REASON_*
 *   AAR_E_NOSYS       not a Zorro III board with the queue runtime
 *   AAR_E_IO          the control block did not check or did not map
 * A claim that cannot be completed is released again. *reason may be NULL.
 */
int aar_xport_open(aar_xport *x, const aar_xport_board *board,
		   uint32_t *reason);
/* The AAR_REASON_* of the last refusal (load begin, load end), else 0. */
uint32_t aar_xport_reason(const aar_xport *x);
/* The claim's handle, 0 once it is gone. */
uint32_t aar_xport_handle(const aar_xport *x);
/* 1 while the claim is live (not released, not revoked). */
int aar_xport_live(const aar_xport *x);

/* ---- Limits, load, start, stop, release ---------------------------------- */

/* Writes AAR_CB_PRIVATE_MIB, AAR_CB_SHARED_MIB and AAR_CB_WEIGHT; the card
 * reads them at load begin. 0 selects the default. AAR_E_BUSY once the load
 * has begun, AAR_E_INVAL above 4095 MiB. */
int aar_xport_set_limits(aar_xport *x, uint32_t private_mib,
			 uint32_t shared_mib, uint32_t weight);

/*
 * LOAD_BEGIN: the card allocates the load buffer, the module maps it.
 * `client_caps` is ZZAPP_CLIENT_*. On success *staging is the 64 KiB image
 * staging area and *chunk_max the largest chunk (ZZAPP_CHUNK_MAX). AAR_E_NORESOURCE
 * (see aar_xport_reason) when the card refuses (limits, host heap).
 */
int aar_xport_load_begin(aar_xport *x, uint32_t client_caps,
			 volatile uint8_t **staging, uint32_t *chunk_max);
/* LOAD_CHUNK: `len` bytes now in the staging area go to `file_off` of the
 * image. */
int aar_xport_load_chunk(aar_xport *x, uint32_t file_off, uint32_t len);
/* LOAD_END: the card checks and maps the image. Long: it renews the lease. */
int aar_xport_load_end(aar_xport *x);
/* begin, chunks, end for an image in memory. */
int aar_xport_load_memory(aar_xport *x, const void *data, uint32_t len,
			  uint32_t client_caps);
#if AAR_XPORT_AMIGA
/* The same for a file (dos.library). */
int aar_xport_load_file(aar_xport *x, const char *path, uint32_t client_caps);
#endif
/* START with argv (at most ZZAPP_ARGV_MAX strings of ZZAPP_ARGV_STR_MAX - 1
 * bytes): written into the load buffer, which the card frees. */
int aar_xport_start(aar_xport *x, const char *const *argv, uint32_t argc);

/* Asks the app to stop: writes AAR_CB_STOP_REQUEST (callable from a timer). */
void aar_xport_request_stop(aar_xport *x);
/* ZZAPP_OP_STOP: the card stops the app now. */
int aar_xport_stop(aar_xport *x);
/* ZZAPP_OP_RELEASE: the card stops the app if it still runs and returns the
 * control block, queues and every service buffer. Drops every map first.
 * Idempotent. Messages and the final status are readable until this call. */
int aar_xport_release(aar_xport *x);

/* ---- Status, from the control block (spec 9.7) ----------------------------- */

typedef struct aar_xport_status {
	uint32_t handle;
	uint32_t state;                 /* AAR_STATE_* */
	uint32_t end_reason;            /* AAR_END_* */
	int32_t exit_code;
	uint32_t flags;                 /* AAR_CBF_* */
	uint64_t progress;              /* load progress, bytes */
	uint32_t lease_period_ms;
	uint32_t fault_class;           /* AAR_FAULT_* */
	uint32_t fault_syndrome, fault_pstate;
	uint64_t fault_pc, fault_sp, fault_lr, fault_addr;
	uint64_t load_base;
	uint64_t shared_used, private_committed;
	uint32_t sq_full, log_dropped;  /* platform counters */
	uint32_t events_dropped, cq_full; /* the client's own */
	uint32_t msg_consumed;
} aar_xport_status;

/* AAR_OK, or AAR_E_STALE once the claim is gone. Readable after the app
 * ended, until the claim is released. */
int aar_xport_status_get(aar_xport *x, aar_xport_status *st);

/* ---- Lease ---------------------------------------------------------------- */

/* Advances AAR_CB_LEASE_COUNTER (never 0). One store: call it from a timer
 * or interrupt, about every 100 ms while the app runs (the lease is 2 s,
 * 10 s with ZZAPP_CLIENT_LEASE_EXTENDED, 30 s while loading). Long mailbox
 * calls renew it themselves. AAR_E_STALE once the claim is gone. */
int aar_xport_lease_renew(aar_xport *x);

/* ---- Service requests (AARServiceNext / AARServiceReply) ------------------ */

/* A request the app posted, and the completion the client answers with. */
typedef struct aar_xport_req {
	uint16_t op;                    /* AAR_OP_* */
	uint16_t flags;
	uint32_t id;
	uint32_t arg[AAR_ENTRY_ARGS];
	uint32_t buf;                   /* token of the payload buffer, 0: none */
	uint32_t buf_off;
	uint32_t buf_len;
	volatile uint8_t *ptr;          /* buf + buf_off, mapped; NULL when buf is 0 */
	/* the completion: fill before aar_xport_service_reply() */
	int32_t result;                 /* AAR_OK or AAR_E_* */
	uint16_t reply_flags;           /* e.g. ZZAPP_FLIPF_SUBMITTED, AAR_DISPCAP_* */
	uint32_t out[AAR_ENTRY_OUTS];
} aar_xport_req;

/*
 * The next request on the SQ: 1 with *req filled (out[], result, reply_flags
 * zeroed), 0 when none, AAR_E_IO on corrupt indices. Messages and logs in
 * front of it are moved aside (to the inbox, to the log sink), so a request
 * is never stuck behind them. A request whose buffer does not map is answered
 * AAR_E_INVAL here and not returned. Also sends a pending doorbell and posts
 * a coalesced mouse event the CQ had no room for.
 */
int aar_xport_service_next(aar_xport *x, aar_xport_req *req);
/*
 * Completes `req`: posts the completion on the CQ, then rings the doorbell
 * (KICK, coalesced). Write any payload bytes the completion names first.
 * AAR_OK; AAR_E_BUSY when the CQ is full (counted in AAR_CB_CQ_FULL; try
 * again, the app drains it); AAR_E_IO on corrupt indices; AAR_E_STALE.
 */
int aar_xport_service_reply(aar_xport *x, const aar_xport_req *req);
/* Called when the CQ was full or the doorbell was busy: sends what is
 * pending. service_next calls it; a loop that only posts events may too. */
void aar_xport_flush(aar_xport *x);
/* Where AAR_OP_LOG records go (one call per record, `data` valid only during
 * the call). NULL: logs are dropped and counted. */
void aar_xport_set_log_sink(aar_xport *x,
			    void (*sink)(void *user, const void *data,
					 uint32_t len),
			    void *user);

/* ---- Messages -------------------------------------------------------------- */

/* A message to the app (AAR_MSG_BYTES), subject to its credit
 * (aar_cb_post_msg). AAR_OK; AAR_E_BUSY when the credit or the queue is
 * exhausted; AAR_E_IO; AAR_E_STALE. */
int aar_xport_send_message(aar_xport *x, const void *msg);
/* A message from the app: 1 with `msg` filled (AAR_MSG_BYTES), 0 when none,
 * AAR_E_IO. Moves messages in front of the next request into the inbox
 * first. */
int aar_xport_get_message(aar_xport *x, void *msg);
/* Messages waiting in the inbox. */
uint32_t aar_xport_message_count(const aar_xport *x);

/* ---- Events ---------------------------------------------------------------- */

/*
 * Posts an input event. Mouse motion is coalesced: events with the same
 * button state are summed (dx, dy, saturating) into one pending event, posted
 * when the CQ has room, so a flood costs one entry, not one per event. Any
 * other event first posts the pending mouse event, so the order of keys and
 * clicks is kept. AAR_OK when the event is posted or merged; AAR_E_BUSY when
 * it was dropped because the CQ is full (counted in AAR_CB_EVENTS_DROPPED);
 * AAR_E_STALE.
 */
int aar_xport_post_event(aar_xport *x, const aar_event *ev);
/* Posts the pending mouse event if the CQ has room. 1 when nothing is
 * pending afterwards. */
int aar_xport_flush_events(aar_xport *x);

/* ---- Doorbell ---------------------------------------------------------------- */

/* KICK with ZZAPP_KICK_* flags, coalesced with one still in flight.
 * service_reply rings it for completions; nothing else needs to. */
void aar_xport_kick(aar_xport *x, uint32_t flags);

/* ---- Shared buffers, audio ---------------------------------------------------- */

/* The CPU pointer of [token + off, token + off + len), mapped cache-inhibited
 * once per block and kept for the life of the claim. NULL when it cannot be
 * mapped or the claim is gone. A token is a board offset: a buffer the card
 * named in a request, the load buffer, a counter block the app published. */
volatile uint8_t *aar_xport_map(aar_xport *x, uint32_t token, uint32_t off,
				uint32_t len);

/* Binds the audio ring named by AAR_OP_AUDIO_OPEN (token and bytes of the
 * request, frames of the completion) and checks its header. */
int aar_xport_audio_attach(aar_xport *x, uint32_t token, uint32_t bytes,
			   uint32_t frames);
void aar_xport_audio_detach(aar_xport *x);
/*
 * Reads up to `frames` frames of the ring into `dst` (interleaved signed
 * 16-bit samples in CPU byte order, channels from the ring header) and
 * advances read_frame. *got is the frames read; a read that finds fewer than
 * asked counts one underrun in the ring header. AAR_OK; AAR_E_INVAL without
 * an attached ring; AAR_E_IO when the ring header is corrupt.
 */
int aar_xport_audio_read(aar_xport *x, int16_t *dst, uint32_t frames,
			 uint32_t *got);
/* Frames waiting in the ring. */
uint32_t aar_xport_audio_available(aar_xport *x);

/* ---- Statistics ------------------------------------------------------------------ */

const aar_xport_stats *aar_xport_stats_get(const aar_xport *x);

#if AAR_XPORT_AMIGA
/* ---- The 68k board ------------------------------------------------------------- */

/*
 * Opens what the board needs: zz9k.library (explicit base, not the global
 * ZZ9KBase), expansion.library to find the Zorro III board, mmu.library for
 * the cache-inhibit mapping, a timer for the long calls. AAR_OK, AAR_E_NOSYS
 * when a library or the board is missing. The board is then passed to
 * aar_xport_open(). Not reentrant: one board per program.
 */
int aar_xport_amiga_board_open(aar_xport_board *board);
void aar_xport_amiga_board_close(aar_xport_board *board);
/* The board's base address and size, for converting the address of client
 * memory (a P96 bitmap) into a token: token = address - base. */
uint32_t aar_xport_amiga_board_base(const aar_xport_board *board);
uint32_t aar_xport_amiga_board_size(const aar_xport_board *board);
#endif

#ifdef __cplusplus
}
#endif

#endif /* AAR_XPORT_ZZ9000_H */
