/*
 * Host model of the Amiga client of the AAR transport (spec sections 6 and
 * 10, aar/transport.h). It is what ZZRun and the SDK tools do on the 68k, on
 * the reference queue model: CLAIM, map the control block by token, bind the
 * two queues from the descriptors, drain the submission queue and answer each
 * op type, post completions, events and credited messages, ring the doorbell,
 * release, and renew the lease.
 *
 * The firmware side of a test (zzapp_services.c, zzapp_xport.c) allocates the
 * pair and the service buffers through the injected host-visible heap below,
 * and the client reaches every buffer the way the 68k does: board token to
 * memory. Nothing here links the firmware: the mailbox and the doorbell reach
 * it through hooks, so a suite wires them to zzapp_mailbox_dispatch and
 * zzapp_xport_kick as it needs.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZAPP_MOCK_CLIENT_H
#define ZZAPP_MOCK_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "aar/app.h"
#include "aar/transport.h"
#include "zzapp_proto.h"
#include "zzapp_services.h"
#include "zzapp_xport.h"

/* ---- The host-visible heap the firmware allocates from -------------------- */

#define MOCK_HEAP_BLOCKS      512u
/* The card address of a block less this is its token. */
#define MOCK_HEAP_TOKEN_ADJUST 0x08000000u

typedef struct {
    uint32_t allocs, frees, bad_frees;
    uint32_t live, live_bytes;
    uint32_t peak_live_bytes;
    uint32_t last_bytes, last_align;
    int32_t  fail_after;   /* allocations that still succeed; -1: never fail */
    uint32_t capacity;     /* live bytes the heap holds; 0: unlimited */
} mock_heap_stats_t;

extern mock_heap_stats_t mock_heap;
/* Runs at the start of every release, before the block is freed: a test
 * checks the order of a teardown here. */
extern void (*mock_heap_on_release)(void *ptr, uint32_t card);

/* Frees every block and clears the counters. The caller installs the heap:
 *   zzapp_xport_env_t e = mock_heap_env(); zzapp_xport_init(&e); */
void mock_heap_reset(void);
/* The environment of the heap (alloc and release, token adjust, no hooks). */
zzapp_xport_env_t mock_heap_env(void);
/* What the 68k does with a token: the memory `len` bytes at board offset
 * `token`, or NULL when it is not inside one block. */
void *mock_heap_map(uint32_t token, uint32_t len);
/* The token of an ARM pointer inside a block, 0 when it is in none. */
uint32_t mock_heap_token(const void *ptr);

/* ---- The client ----------------------------------------------------------- */

#define MOCK_CLIENT_OPS       0x48u  /* op codes below this are modelled */
#define MOCK_CLIENT_HISTORY   1024u
#define MOCK_CLIENT_MAX       128u   /* clients serve_all() walks */
#define MOCK_CLIENT_INBOX     256u
#define MOCK_CLIENT_PENDING   128u
#define MOCK_CLIENT_NAMES     8u
#define MOCK_CLIENT_CONSOLE   0x10000u
#define MOCK_CLIENT_WRITTEN   0x40000u

typedef struct {
    aar_request req;
    int32_t     result;     /* of the completion, once completed */
    int         completed;
    uint32_t    serve_no;   /* the serve() call that saw it */
    char        path[128];  /* OPEN, STAT, LIST_OPEN: the path in the buffer */
} mock_client_hist_t;

struct mock_client;

/* What an on_request hook decides. `comp` arrives with the op and id of the
 * request echoed and everything else zero. */
typedef enum {
    MOCK_DEFAULT = 0,    /* the built-in answer for the op */
    MOCK_COMPLETE,       /* post the completion `comp` filled in */
    MOCK_HOLD,           /* no completion now; mock_client_complete() later */
    MOCK_DROP            /* never complete it (a lost request) */
} mock_verdict_t;

typedef mock_verdict_t (*mock_client_request_fn)(struct mock_client *c,
                                                 const aar_request *req,
                                                 aar_completion *comp);

/* The mailbox as the client sees it: op, request payload and length in, reply
 * and its length out; returns the status (SDK_STATUS_*). */
typedef uint16_t (*mock_mailbox_fn)(uint16_t op, const uint8_t *req,
                                    uint16_t req_len, uint8_t *reply,
                                    uint16_t *reply_len);

typedef struct mock_client {
    /* Binding. */
    uint32_t handle;
    volatile uint8_t *cb;
    aar_queue sq, cq;
    int attached;

    /* The client's own message credit count (messages it posted). */
    uint32_t msgs_posted;

    /* Behaviour. */
    int frozen;             /* serve() does nothing: a hung or lost client */
    uint32_t drain_limit;   /* SQ entries served per serve(); 0: all */
    uint32_t delay[MOCK_CLIENT_OPS];      /* serves before op completes */
    uint8_t  forced[MOCK_CLIENT_OPS];     /* forced_result applies */
    int32_t  forced_result[MOCK_CLIENT_OPS];
    mock_client_request_fn on_request;    /* runs before the default */
    void *user;

    /* Default answers. */
    uint32_t read_remaining;  /* bytes a READ may still return; ~0u: endless */
    uint32_t read_chunk_cap;  /* most bytes one READ returns; 0: as asked */
    uint32_t produced;        /* pattern cursor: byte n of the stream is n */
    uint32_t stat_size, stat_mode, stat_mtime;
    const char *names[MOCK_CLIENT_NAMES]; /* LIST_NEXT names, NULL ends */
    uint32_t next_name;
    uint8_t  console_in[0x1000];
    uint32_t console_in_len, console_in_pos;
    int      console_in_eof;           /* 0 bytes once drained */
    uint8_t  console_out[MOCK_CLIENT_CONSOLE];
    uint32_t console_out_len;
    uint32_t console_out_chunk_cap;    /* bytes of a write taken; 0: all */
    uint8_t  written[MOCK_CLIENT_WRITTEN];
    uint32_t written_len;
    uint32_t display_front, display_back, display_spare; /* tokens */
    uint32_t display_dispcaps;
    uint32_t flip_flags;               /* flags of a FLIP completion */
    uint32_t display_dropped;          /* FLIP out DROPPED */
    uint32_t palette[256];
    uint32_t palette_first, palette_count;
    uint32_t audio_granted_rate;       /* 0: grant what was asked */

    /* What it has seen. */
    mock_client_hist_t hist[MOCK_CLIENT_HISTORY];
    uint32_t hist_count;               /* free-running */
    uint32_t op_count[MOCK_CLIENT_OPS];
    uint32_t serves;
    uint32_t logs_seen, log_dropped_seen;
    char     last_log[AAR_LOG_MAX + 1u];
    uint32_t last_log_len;
    uint8_t  inbox[MOCK_CLIENT_INBOX][AAR_MSG_BYTES]; /* app-to-client messages */
    uint32_t inbox_head, inbox_tail;
    uint32_t inbox_overflow;
    uint32_t cq_full_seen;             /* completions that waited for room */
    uint32_t sq_io_errors;

    /* Completions that wait for their delay or for CQ room. */
    struct {
        aar_completion comp;
        uint32_t due;
    } pend[MOCK_CLIENT_PENDING];
    uint32_t pend_n;

    /* Reaching the firmware. */
    mock_mailbox_fn mailbox;
    int (*kick)(uint32_t handle, uint32_t flags);
    uint32_t kicks, kick_flags_or;
} mock_client;

/* ---- Registry, so a services yield hook can serve every client ------------ */

void mock_client_init(mock_client *c);
/* Adds `c` to the registry that mock_client_serve_all() walks. */
int mock_client_register(mock_client *c);
void mock_client_unregister(mock_client *c);
void mock_client_registry_clear(void);
/* void(void) hook for zzapp_services_test_set_yield(): serves every client
 * and counts the call in mock_client_yields; on_yield runs first when set. */
void mock_client_yield_hook(void);
extern uint32_t mock_client_yields;
extern void (*mock_client_on_yield)(uint32_t n);
void mock_client_serve_all(void);

/* ---- Claim and bind ------------------------------------------------------- */

/* Map the control block at `cb_token`, check it, bind both queues from the
 * descriptors (token and offset), take the handle from the block. AAR_OK or
 * AAR_E_IO. */
int mock_client_attach(mock_client *c, uint32_t cb_token);
/* The same from a CLAIM reply (handle, token, bytes). */
int mock_client_attach_reply(mock_client *c, const uint8_t *reply,
                             uint16_t reply_len);
void mock_client_detach(mock_client *c);

/* ---- The mailbox ---------------------------------------------------------- */

/* Build the 48-byte payload for `op`, set the handle (unless the op takes
 * none), call the hook. Return the status; the reply is filled when given. */
uint16_t mock_client_call(mock_client *c, uint16_t op, uint8_t payload[48],
                          uint16_t req_len, uint8_t reply[48],
                          uint16_t *reply_len);
uint16_t mock_client_claim(mock_client *c);      /* CLAIM, then attach */
uint16_t mock_client_load_begin(mock_client *c, uint32_t caps,
                                uint8_t reply[48], uint16_t *reply_len);
uint16_t mock_client_start(mock_client *c, uint32_t argc);
uint16_t mock_client_stop(mock_client *c);
uint16_t mock_client_release(mock_client *c);
uint16_t mock_client_kick_op(mock_client *c, uint32_t flags);
uint16_t mock_client_list(mock_client *c, uint32_t cursor, uint8_t reply[48]);
uint16_t mock_client_query(mock_client *c, uint8_t reply[48]);

/* ---- Control block -------------------------------------------------------- */

uint32_t mock_client_cb_get(const mock_client *c, uint32_t off);
void mock_client_cb_put(mock_client *c, uint32_t off, uint32_t value);
/* Limits, written before the load. */
void mock_client_set_limits(mock_client *c, uint32_t private_mib,
                            uint32_t shared_mib, uint32_t weight);
/* The lease counter advances by one (the renewal of a 68k timer). */
void mock_client_renew_lease(mock_client *c);
void mock_client_request_stop(mock_client *c);
void mock_client_set_present(mock_client *c, uint32_t present);

/* ---- Producing (the client side of the CQ) -------------------------------- */

/* AAR_OK, AAR_E_BUSY when the CQ is full, AAR_E_IO on corrupt indices. */
int mock_client_post_event(mock_client *c, const aar_event *ev);
/* A credited message: AAR_E_BUSY when the credit or the queue says so. */
int mock_client_post_msg(mock_client *c, const void *msg);
/* The same without the credit check (a client that broke the rule). */
int mock_client_post_msg_raw(mock_client *c, const void *msg);
int mock_client_post_completion(mock_client *c, const aar_completion *comp);
/* Complete a held request now (a completion with the request's op and id,
 * `result`, and `out`). */
int mock_client_complete(mock_client *c, uint32_t id, int32_t result,
                         const uint32_t out[AAR_ENTRY_OUTS], uint16_t flags);
/* Pending completions that wait for room or delay. */
uint32_t mock_client_pending(const mock_client *c);
/* Free entries in the CQ now. */
uint32_t mock_client_cq_room(const mock_client *c);

/* ---- Consuming (the client side of the SQ) -------------------------------- */

/* Drain the SQ, answer every request, flush the completions that are due.
 * Returns the number of SQ entries consumed (negative: AAR_E_IO). */
int mock_client_serve(mock_client *c);
/* Entries in the SQ now. */
int mock_client_sq_used(const mock_client *c);
/* The oldest app-to-client message: 1 and *msg filled, 0 when none. */
int mock_client_get_msg(mock_client *c, void *msg);
/* History helpers. */
const mock_client_hist_t *mock_client_last(const mock_client *c, uint16_t op);
uint32_t mock_client_count(const mock_client *c, uint16_t op);
/* Replays an entry of the history (0 oldest); NULL when out of range. */
const mock_client_hist_t *mock_client_hist_at(const mock_client *c, uint32_t n);
/* The bytes of a buffer a request names, mapped by token. */
void *mock_client_buf(const aar_request *req);

#endif
