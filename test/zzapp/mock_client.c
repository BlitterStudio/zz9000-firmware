/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mock_client.h"

#include <stdlib.h>
#include <string.h>

/* ---- The heap -------------------------------------------------------------- */

typedef struct {
    void *ptr;
    uint32_t card;
    uint32_t bytes;
    uint32_t align;
} heap_block_t;

mock_heap_stats_t mock_heap;
void (*mock_heap_on_release)(void *ptr, uint32_t card);
void (*mock_heap_on_freed)(uint32_t card);
static heap_block_t blocks[MOCK_HEAP_BLOCKS];
static uint32_t next_card;

static int heap_alloc(void *ctx, uint32_t bytes, uint32_t align, uint32_t lo,
                      uint32_t hi, void **ptr, uint32_t *card)
{
    uint32_t i;
    void *p;

    (void)ctx;
    (void)lo;
    (void)hi;
    mock_heap.last_bytes = bytes;
    mock_heap.last_align = align;
    if (mock_heap.fail_after == 0)
        return -1;
    if (mock_heap.fail_after > 0)
        mock_heap.fail_after--;
    if (mock_heap.capacity != 0u &&
        mock_heap.live_bytes + bytes > mock_heap.capacity)
        return -1;
    for (i = 0u; i < MOCK_HEAP_BLOCKS && blocks[i].ptr; i++)
        ;
    if (i == MOCK_HEAP_BLOCKS)
        return -1;
    p = aligned_alloc(align, bytes);
    if (!p)
        return -1;
    memset(p, 0xA5, bytes); /* a heap does not zero: the firmware must */
    next_card = (next_card + align - 1u) & ~(align - 1u);
    blocks[i].ptr = p;
    blocks[i].card = next_card;
    blocks[i].bytes = bytes;
    blocks[i].align = align;
    next_card += bytes + 0x2000u;
    mock_heap.allocs++;
    mock_heap.live++;
    mock_heap.live_bytes += bytes;
    if (mock_heap.live_bytes > mock_heap.peak_live_bytes)
        mock_heap.peak_live_bytes = mock_heap.live_bytes;
    *ptr = p;
    *card = blocks[i].card;
    return 0;
}

static int heap_release(void *ctx, void *ptr, uint32_t card)
{
    uint32_t i;

    (void)ctx;
    if (mock_heap_on_release)
        mock_heap_on_release(ptr, card);
    for (i = 0u; i < MOCK_HEAP_BLOCKS; i++) {
        if (blocks[i].ptr == ptr && blocks[i].card == card) {
            mock_heap.live--;
            mock_heap.live_bytes -= blocks[i].bytes;
            mock_heap.frees++;
            free(ptr);
            memset(&blocks[i], 0, sizeof(blocks[i]));
            if (mock_heap_on_freed)
                mock_heap_on_freed(card);
            return 0;
        }
    }
    mock_heap.bad_frees++;
    return -1;
}

zzapp_xport_env_t mock_heap_env(void)
{
    zzapp_xport_env_t e;

    memset(&e, 0, sizeof(e));
    e.alloc = heap_alloc;
    e.release = heap_release;
    e.token_adjust = MOCK_HEAP_TOKEN_ADJUST;
    return e;
}

void mock_heap_reset(void)
{
    uint32_t i;

    for (i = 0u; i < MOCK_HEAP_BLOCKS; i++)
        if (blocks[i].ptr)
            free(blocks[i].ptr);
    memset(blocks, 0, sizeof(blocks));
    memset(&mock_heap, 0, sizeof(mock_heap));
    mock_heap.fail_after = -1;
    mock_heap_on_release = NULL;
    mock_heap_on_freed = NULL;
    next_card = 0x08200000u;
}

void *mock_heap_map(uint32_t token, uint32_t len)
{
    uint32_t card = token + MOCK_HEAP_TOKEN_ADJUST;
    uint32_t i;

    for (i = 0u; i < MOCK_HEAP_BLOCKS; i++)
        if (blocks[i].ptr && card >= blocks[i].card &&
            card - blocks[i].card <= blocks[i].bytes &&
            len <= blocks[i].bytes - (card - blocks[i].card))
            return (uint8_t *)blocks[i].ptr + (card - blocks[i].card);
    return NULL;
}

uint32_t mock_heap_token(const void *ptr)
{
    uint32_t i;
    const uint8_t *p = (const uint8_t *)ptr;

    for (i = 0u; i < MOCK_HEAP_BLOCKS; i++)
        if (blocks[i].ptr && p >= (const uint8_t *)blocks[i].ptr &&
            p < (const uint8_t *)blocks[i].ptr + blocks[i].bytes)
            return blocks[i].card - MOCK_HEAP_TOKEN_ADJUST +
                   (uint32_t)(p - (const uint8_t *)blocks[i].ptr);
    return 0u;
}

/* ---- Registry -------------------------------------------------------------- */

static mock_client *registry[MOCK_CLIENT_MAX];
uint32_t mock_client_yields;
void (*mock_client_on_yield)(uint32_t n);

void mock_client_init(mock_client *c)
{
    memset(c, 0, sizeof(*c));
    c->read_remaining = ~0u;
}

int mock_client_register(mock_client *c)
{
    uint32_t i;

    for (i = 0u; i < MOCK_CLIENT_MAX; i++) {
        if (registry[i] == c)
            return 0;
    }
    for (i = 0u; i < MOCK_CLIENT_MAX; i++) {
        if (!registry[i]) {
            registry[i] = c;
            return 0;
        }
    }
    return -1;
}

void mock_client_unregister(mock_client *c)
{
    uint32_t i;

    for (i = 0u; i < MOCK_CLIENT_MAX; i++)
        if (registry[i] == c)
            registry[i] = NULL;
}

void mock_client_registry_clear(void)
{
    memset(registry, 0, sizeof(registry));
    mock_client_yields = 0u;
    mock_client_on_yield = NULL;
}

void mock_client_serve_all(void)
{
    uint32_t i;

    for (i = 0u; i < MOCK_CLIENT_MAX; i++)
        if (registry[i])
            (void)mock_client_serve(registry[i]);
}

void mock_client_yield_hook(void)
{
    mock_client_yields++;
    if (mock_client_on_yield)
        mock_client_on_yield(mock_client_yields);
    mock_client_serve_all();
}

/* ---- Claim and bind -------------------------------------------------------- */

int mock_client_attach(mock_client *c, uint32_t cb_token)
{
    volatile uint8_t *cb = mock_heap_map(cb_token, AAR_CB_BYTES);
    volatile uint8_t *ring;
    uint32_t t;
    void *base;
    mock_mailbox_fn mailbox = c->mailbox;
    int (*kick)(uint32_t, uint32_t) = c->kick;

    c->attached = 0;
    if (!cb || aar_cb_check(cb) != AAR_OK)
        return AAR_E_IO;
    c->cb = cb;
    c->handle = aar_get_be32(cb + AAR_CB_HANDLE);
    t = aar_get_be32(cb + AAR_CB_SQ_DESC + AAR_QD_TOKEN);
    base = mock_heap_map(t, aar_get_be32(cb + AAR_CB_SQ_DESC + AAR_QD_OFFSET) +
                            AAR_QUEUE_DEPTH_DEFAULT * AAR_ENTRY_BYTES);
    if (!base)
        return AAR_E_IO;
    ring = (volatile uint8_t *)base +
           aar_get_be32(cb + AAR_CB_SQ_DESC + AAR_QD_OFFSET);
    if (aar_cb_queue_bind(&c->sq, (volatile void *)cb, ring, AAR_QUEUE_SQ) !=
        AAR_OK)
        return AAR_E_IO;
    t = aar_get_be32(cb + AAR_CB_CQ_DESC + AAR_QD_TOKEN);
    base = mock_heap_map(t, aar_get_be32(cb + AAR_CB_CQ_DESC + AAR_QD_OFFSET) +
                            AAR_QUEUE_DEPTH_DEFAULT * AAR_ENTRY_BYTES);
    if (!base)
        return AAR_E_IO;
    ring = (volatile uint8_t *)base +
           aar_get_be32(cb + AAR_CB_CQ_DESC + AAR_QD_OFFSET);
    if (aar_cb_queue_bind(&c->cq, (volatile void *)cb, ring, AAR_QUEUE_CQ) !=
        AAR_OK)
        return AAR_E_IO;
    c->attached = 1;
    c->msgs_posted = 0u;
    c->pend_n = 0u;
    c->mailbox = mailbox;
    c->kick = kick;
    return AAR_OK;
}

int mock_client_attach_reply(mock_client *c, const uint8_t *reply,
                             uint16_t reply_len)
{
    if (reply_len < ZZAPP_CLAIM_REPLY_BYTES)
        return AAR_E_IO;
    if (zzapp_get_be32(reply + ZZAPP_CLAIM_CB_BYTES) < AAR_CB_BYTES)
        return AAR_E_IO;
    if (mock_client_attach(c, zzapp_get_be32(reply + ZZAPP_CLAIM_CB_TOKEN)) !=
        AAR_OK)
        return AAR_E_IO;
    return zzapp_get_be32(reply + ZZAPP_CLAIM_HANDLE) == c->handle ? AAR_OK
                                                                   : AAR_E_IO;
}

void mock_client_detach(mock_client *c)
{
    c->attached = 0;
    c->cb = NULL;
    c->pend_n = 0u;
}

/* ---- The mailbox ----------------------------------------------------------- */

uint16_t mock_client_call(mock_client *c, uint16_t op, uint8_t payload[48],
                          uint16_t req_len, uint8_t reply[48],
                          uint16_t *reply_len)
{
    uint8_t scratch_req[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint8_t scratch_reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint16_t rlen = 0u;
    uint16_t status;

    if (!payload)
        payload = scratch_req;
    if (!reply)
        reply = scratch_reply;
    if (op != ZZAPP_OP_QUERY && op != ZZAPP_OP_CLAIM && op != ZZAPP_OP_LIST) {
        zzapp_build_handle(payload, c->handle);
        req_len = ZZAPP_MAILBOX_PAYLOAD_BYTES;
    }
    if (!c->mailbox)
        return SDK_STATUS_UNSUPPORTED;
    status = c->mailbox(op, payload, req_len, reply, &rlen);
    if (reply_len)
        *reply_len = rlen;
    return status;
}

uint16_t mock_client_claim(mock_client *c)
{
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint16_t rlen = 0u;
    uint16_t status = mock_client_call(c, ZZAPP_OP_CLAIM, NULL, 0u, reply, &rlen);

    if (status != SDK_STATUS_OK)
        return status;
    return mock_client_attach_reply(c, reply, rlen) == AAR_OK
               ? SDK_STATUS_OK : SDK_STATUS_IO_ERROR;
}

uint16_t mock_client_load_begin(mock_client *c, uint32_t caps,
                                uint8_t reply[48], uint16_t *reply_len)
{
    uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };

    zzapp_build_load_begin_request(req, c->handle, caps);
    return mock_client_call(c, ZZAPP_OP_LOAD_BEGIN, req,
                            ZZAPP_MAILBOX_PAYLOAD_BYTES, reply, reply_len);
}

uint16_t mock_client_start(mock_client *c, uint32_t argc)
{
    uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };

    zzapp_build_start_app_request(req, c->handle, argc);
    return mock_client_call(c, ZZAPP_OP_START, req,
                            ZZAPP_MAILBOX_PAYLOAD_BYTES, NULL, NULL);
}

uint16_t mock_client_stop(mock_client *c)
{
    return mock_client_call(c, ZZAPP_OP_STOP, NULL, 0u, NULL, NULL);
}

uint16_t mock_client_release(mock_client *c)
{
    return mock_client_call(c, ZZAPP_OP_RELEASE, NULL, 0u, NULL, NULL);
}

uint16_t mock_client_kick_op(mock_client *c, uint32_t flags)
{
    uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };

    c->kicks++;
    c->kick_flags_or |= flags;
    zzapp_build_kick_request(req, c->handle, flags);
    return mock_client_call(c, ZZAPP_OP_KICK, req,
                            ZZAPP_MAILBOX_PAYLOAD_BYTES, NULL, NULL);
}

uint16_t mock_client_list(mock_client *c, uint32_t cursor, uint8_t reply[48])
{
    uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint16_t rlen = 0u;

    zzapp_build_list_request(req, cursor);
    return mock_client_call(c, ZZAPP_OP_LIST, req, 4U,
                            reply, &rlen);
}

uint16_t mock_client_query(mock_client *c, uint8_t reply[48])
{
    uint16_t rlen = 0u;

    return mock_client_call(c, ZZAPP_OP_QUERY, NULL, 0u, reply, &rlen);
}

/* ---- Control block --------------------------------------------------------- */

uint32_t mock_client_cb_get(const mock_client *c, uint32_t off)
{
    return c->cb ? aar_get_be32(c->cb + off) : 0u;
}

void mock_client_cb_put(mock_client *c, uint32_t off, uint32_t value)
{
    if (c->cb)
        aar_put_be32(c->cb + off, value);
}

void mock_client_set_limits(mock_client *c, uint32_t private_mib,
                            uint32_t shared_mib, uint32_t weight)
{
    mock_client_cb_put(c, AAR_CB_PRIVATE_MIB, private_mib);
    mock_client_cb_put(c, AAR_CB_SHARED_MIB, shared_mib);
    mock_client_cb_put(c, AAR_CB_WEIGHT, weight);
}

void mock_client_renew_lease(mock_client *c)
{
    mock_client_cb_put(c, AAR_CB_LEASE_COUNTER,
                       mock_client_cb_get(c, AAR_CB_LEASE_COUNTER) + 1u);
}

void mock_client_request_stop(mock_client *c)
{
    mock_client_cb_put(c, AAR_CB_STOP_REQUEST, 1u);
}

void mock_client_set_present(mock_client *c, uint32_t present)
{
    mock_client_cb_put(c, AAR_CB_CLIENT_PRESENT, present);
}

/* ---- Producing -------------------------------------------------------------- */

int mock_client_post_event(mock_client *c, const aar_event *ev)
{
    int rc;

    if (!c->attached)
        return AAR_E_IO;
    rc = aar_queue_post_event(&c->cq, ev, NULL, NULL);
    if (rc == AAR_E_BUSY) {
        mock_client_cb_put(c, AAR_CB_EVENTS_DROPPED,
                           mock_client_cb_get(c, AAR_CB_EVENTS_DROPPED) + 1u);
        mock_client_cb_put(c, AAR_CB_CQ_FULL,
                           mock_client_cb_get(c, AAR_CB_CQ_FULL) + 1u);
    }
    return rc;
}

int mock_client_post_msg(mock_client *c, const void *msg)
{
    if (!c->attached)
        return AAR_E_IO;
    return aar_cb_post_msg(&c->cq, c->cb, &c->msgs_posted, msg, NULL, NULL);
}

int mock_client_post_msg_raw(mock_client *c, const void *msg)
{
    int rc;

    if (!c->attached)
        return AAR_E_IO;
    rc = aar_queue_post_msg(&c->cq, msg, NULL, NULL);
    if (rc == AAR_OK)
        c->msgs_posted++;
    return rc;
}

int mock_client_post_completion(mock_client *c, const aar_completion *comp)
{
    if (!c->attached)
        return AAR_E_IO;
    return aar_queue_post_completion(&c->cq, comp, NULL, NULL);
}

static void pend_add(mock_client *c, const aar_completion *comp, uint32_t due)
{
    if (c->pend_n < MOCK_CLIENT_PENDING) {
        c->pend[c->pend_n].comp = *comp;
        c->pend[c->pend_n].due = due;
        c->pend_n++;
    }
}

int mock_client_complete(mock_client *c, uint32_t id, int32_t result,
                         const uint32_t out[AAR_ENTRY_OUTS], uint16_t flags)
{
    uint32_t n;
    aar_completion comp;

    memset(&comp, 0, sizeof(comp));
    for (n = 0u; n < c->hist_count && n < MOCK_CLIENT_HISTORY; n++) {
        mock_client_hist_t *h = &c->hist[(c->hist_count - 1u - n) %
                                         MOCK_CLIENT_HISTORY];

        if (h->req.id == id && !h->completed) {
            comp.op = h->req.op;
            h->completed = 1;
            h->result = result;
            break;
        }
    }
    if (n == c->hist_count || n == MOCK_CLIENT_HISTORY)
        return AAR_E_NOENT;
    comp.id = id;
    comp.result = result;
    comp.flags = flags;
    if (out)
        memcpy(comp.out, out, sizeof(comp.out));
    pend_add(c, &comp, c->serves);
    return AAR_OK;
}

uint32_t mock_client_pending(const mock_client *c)
{
    return c->pend_n;
}

uint32_t mock_client_cq_room(const mock_client *c)
{
    uint32_t used;

    if (!c->attached || aar_queue_used(&c->cq, &used) != AAR_OK)
        return 0u;
    return c->cq.depth - used;
}

/* ---- Consuming --------------------------------------------------------------- */

void *mock_client_buf(const aar_request *req)
{
    uint8_t *p;

    if (req->buf == 0u)
        return NULL;
    p = mock_heap_map(req->buf, req->buf_off + req->buf_len);
    return p ? p + req->buf_off : NULL;
}

static uint32_t bytes_per_pixel(uint32_t fmt)
{
    return fmt == AAR_FMT_BGRA8888 ? 4u : fmt == AAR_FMT_RGB565 ? 2u : 1u;
}

static void answer_default(mock_client *c, const aar_request *r,
                           aar_completion *comp, mock_verdict_t *verdict)
{
    uint8_t *buf = mock_client_buf(r);
    uint32_t n, i;

    switch (r->op) {
    case AAR_OP_FILE_READ:
        n = r->arg[AAR_A_FILE_LEN];
        if (n > r->buf_len)
            n = r->buf_len;
        if (c->read_chunk_cap && n > c->read_chunk_cap)
            n = c->read_chunk_cap;
        if (c->read_remaining != ~0u && n > c->read_remaining)
            n = c->read_remaining;
        if (!buf && n) {
            comp->result = AAR_E_INVAL;
            break;
        }
        for (i = 0u; i < n; i++)
            buf[i] = (uint8_t)(c->produced + i);
        c->produced += n;
        if (c->read_remaining != ~0u)
            c->read_remaining -= n;
        comp->out[AAR_R_FILE_COUNT] = n;
        break;
    case AAR_OP_FILE_WRITE:
        n = r->arg[AAR_A_FILE_LEN];
        if (n > r->buf_len)
            n = r->buf_len;
        if (!buf && n) {
            comp->result = AAR_E_INVAL;
            break;
        }
        for (i = 0u; i < n && c->written_len < MOCK_CLIENT_WRITTEN; i++)
            c->written[c->written_len++] = buf[i];
        comp->out[AAR_R_FILE_COUNT] = n;
        break;
    case AAR_OP_FILE_SEEK:
        comp->out[AAR_R_FILE_POS_HI] = r->arg[AAR_A_FILE_OFF_HI];
        comp->out[AAR_R_FILE_POS_LO] = r->arg[AAR_A_FILE_OFF_LO];
        break;
    case AAR_OP_FILE_STAT:
        comp->out[AAR_R_STAT_SIZE_LO] = c->stat_size;
        comp->out[AAR_R_STAT_MTIME_LO] = c->stat_mtime;
        comp->out[AAR_R_STAT_MODE] = c->stat_mode;
        break;
    case AAR_OP_LIST_OPEN:
        c->next_name = 0u;
        break;
    case AAR_OP_LIST_NEXT:
        if (c->next_name < MOCK_CLIENT_NAMES && c->names[c->next_name] && buf) {
            strncpy((char *)buf, c->names[c->next_name], r->buf_len);
            if (r->buf_len)
                buf[r->buf_len - 1u] = 0;
            c->next_name++;
            comp->out[AAR_R_LIST_MODE] = c->stat_mode;
        } else {
            comp->result = AAR_E_NOENT;
        }
        break;
    case AAR_OP_CONSOLE_WRITE:
        n = r->buf_len;
        if (c->console_out_chunk_cap && n > c->console_out_chunk_cap)
            n = c->console_out_chunk_cap;
        if (!buf && n) {
            comp->result = AAR_E_INVAL;
            break;
        }
        for (i = 0u; i < n && c->console_out_len < MOCK_CLIENT_CONSOLE; i++)
            c->console_out[c->console_out_len++] = buf[i];
        comp->out[AAR_R_CONSOLE_COUNT] = n;
        break;
    case AAR_OP_CONSOLE_READ:
        if (c->console_in_pos < c->console_in_len) {
            n = c->console_in_len - c->console_in_pos;
            if (n > r->buf_len)
                n = r->buf_len;
            if (!buf) {
                comp->result = AAR_E_INVAL;
                break;
            }
            memcpy(buf, c->console_in + c->console_in_pos, n);
            c->console_in_pos += n;
            comp->out[AAR_R_CONSOLE_COUNT] = n;
        } else if (c->console_in_eof) {
            comp->out[AAR_R_CONSOLE_COUNT] = 0u;
        } else {
            *verdict = MOCK_HOLD;   /* the keyboard has nothing yet */
        }
        break;
    case AAR_OP_DISPLAY_OPEN: {
        uint32_t fmt = r->arg[AAR_A_DISPLAY_OPEN_FORMAT];

        comp->out[AAR_R_DISPLAY_OPEN_ID] = 1u;
        comp->out[AAR_R_DISPLAY_OPEN_PITCH] =
            r->arg[AAR_A_DISPLAY_OPEN_W] * bytes_per_pixel(fmt);
        comp->out[AAR_R_DISPLAY_OPEN_LAYOUT] =
            fmt == AAR_FMT_BGRA8888 ? AAR_LAYOUT_BGRA8888 :
            fmt == AAR_FMT_CLUT8 ? AAR_LAYOUT_CLUT8 : AAR_LAYOUT_RGB565_BE;
        comp->out[AAR_R_DISPLAY_OPEN_FRONT] = c->display_front;
        comp->out[AAR_R_DISPLAY_OPEN_BACK] = c->display_back;
        comp->out[AAR_R_DISPLAY_OPEN_SPARE] = c->display_spare;
        comp->flags = (uint16_t)c->display_dispcaps;
        break;
    }
    case AAR_OP_DISPLAY_FLIP:
        comp->flags = (uint16_t)c->flip_flags;
        comp->out[AAR_R_DISPLAY_FLIP_DROPPED] = c->display_dropped;
        break;
    case AAR_OP_DISPLAY_PALETTE:
        c->palette_first = r->arg[AAR_A_DISPLAY_PALETTE_FIRST];
        c->palette_count = r->arg[AAR_A_DISPLAY_PALETTE_COUNT];
        if (buf)
            for (i = 0u; i < c->palette_count && c->palette_first + i < 256u &&
                         i * 4u + 4u <= r->buf_len; i++)
                c->palette[c->palette_first + i] =
                    zzapp_get_be32(buf + i * 4u);
        break;
    case AAR_OP_AUDIO_OPEN:
        comp->out[AAR_R_AUDIO_OPEN_RATE] = c->audio_granted_rate
            ? c->audio_granted_rate : r->arg[AAR_A_AUDIO_OPEN_RATE];
        comp->out[AAR_R_AUDIO_OPEN_CHANNELS] = r->arg[AAR_A_AUDIO_OPEN_CHANNELS];
        comp->out[AAR_R_AUDIO_OPEN_RING] = r->buf;
        comp->out[AAR_R_AUDIO_OPEN_RING_BYTES] = r->buf_len;
        comp->out[AAR_R_AUDIO_OPEN_FRAMES] =
            r->buf_len > AAR_AUDIO_RING_HDR_BYTES
                ? (r->buf_len - AAR_AUDIO_RING_HDR_BYTES) / 4u : 0u;
        break;
    default:
        break;   /* OPEN, CLOSE, LIST_CLOSE, DISPLAY_CLOSE, AUDIO_CLOSE: OK */
    }
}

static void note_path(mock_client_hist_t *h)
{
    const uint8_t *buf;
    uint32_t n;

    if (h->req.op != AAR_OP_FILE_OPEN && h->req.op != AAR_OP_FILE_STAT &&
        h->req.op != AAR_OP_LIST_OPEN)
        return;
    buf = mock_client_buf(&h->req);
    if (!buf)
        return;
    n = h->req.buf_len < sizeof(h->path) - 1u ? h->req.buf_len
                                               : (uint32_t)sizeof(h->path) - 1u;
    memcpy(h->path, buf, n);
    h->path[n] = 0;
}

static void flush_pending(mock_client *c)
{
    uint32_t i = 0u;
    int blocked = 0;

    while (i < c->pend_n) {
        if (c->pend[i].due > c->serves) {
            i++;
            continue;
        }
        if (blocked || mock_client_post_completion(c, &c->pend[i].comp) !=
                           AAR_OK) {
            if (!blocked)
                c->cq_full_seen++;
            blocked = 1;   /* completions go in order, behind a full CQ */
            i++;
            continue;
        }
        memmove(&c->pend[i], &c->pend[i + 1u],
                (c->pend_n - i - 1u) * sizeof(c->pend[0]));
        c->pend_n--;
    }
}

static void take_request(mock_client *c, const volatile uint8_t *slot)
{
    mock_client_hist_t *h = &c->hist[c->hist_count % MOCK_CLIENT_HISTORY];
    aar_completion comp;
    mock_verdict_t verdict = MOCK_DEFAULT;
    uint32_t op;

    memset(h, 0, sizeof(*h));
    aar_entry_get_request(slot, &h->req);
    h->serve_no = c->serves;
    op = h->req.op;
    c->hist_count++;
    if (op < MOCK_CLIENT_OPS)
        c->op_count[op]++;
    note_path(h);

    memset(&comp, 0, sizeof(comp));
    comp.op = h->req.op;
    comp.id = h->req.id;
    if (c->on_request)
        verdict = c->on_request(c, &h->req, &comp);
    if (verdict == MOCK_DEFAULT) {
        verdict = MOCK_COMPLETE;
        answer_default(c, &h->req, &comp, &verdict);
    }
    if (verdict == MOCK_COMPLETE) {
        if (op < MOCK_CLIENT_OPS && c->forced[op])
            comp.result = c->forced_result[op];
        h->completed = 1;
        h->result = comp.result;
        pend_add(c, &comp, c->serves + (op < MOCK_CLIENT_OPS ? c->delay[op] : 0u));
    }
}

int mock_client_serve(mock_client *c)
{
    int n = 0;

    if (!c->attached || c->frozen)
        return 0;
    /* A claim that was released took its pair with it: a client that still
     * holds the pointers has nothing to serve (a real one would not touch
     * the memory again after its release). */
    if (!mock_heap_token((const void *)c->cb)) {
        c->attached = 0;
        return 0;
    }
    c->serves++;
    for (;;) {
        const volatile uint8_t *slot = NULL;
        uint16_t op;
        int rc;

        if (c->drain_limit && (uint32_t)n >= c->drain_limit)
            break;
        rc = aar_queue_peek(&c->sq, NULL, &slot);
        if (rc < 0) {
            c->sq_io_errors++;
            return rc;
        }
        if (rc == 0)
            break;
        op = aar_get_be16(slot + AAR_ENTRY_OP);
        if (op == AAR_OP_MSG) {
            if (c->inbox_tail - c->inbox_head < MOCK_CLIENT_INBOX) {
                aar_entry_get_msg(slot, c->inbox[c->inbox_tail %
                                                 MOCK_CLIENT_INBOX]);
                c->inbox_tail++;
            } else {
                c->inbox_overflow++;
            }
        } else if (op == AAR_OP_LOG) {
            aar_request r;
            const uint8_t *buf;

            aar_entry_get_request(slot, &r);
            buf = mock_client_buf(&r);
            c->logs_seen++;
            c->last_log_len = 0u;
            if (buf && r.buf_len <= AAR_LOG_MAX) {
                memcpy(c->last_log, buf, r.buf_len);
                c->last_log[r.buf_len] = 0;
                c->last_log_len = r.buf_len;
            }
        } else {
            take_request(c, slot);
        }
        aar_queue_pop(&c->sq, NULL);
        n++;
    }
    flush_pending(c);
    return n;
}

int mock_client_sq_used(const mock_client *c)
{
    uint32_t used;

    if (!c->attached || aar_queue_used(&c->sq, &used) != AAR_OK)
        return AAR_E_IO;
    return (int)used;
}

int mock_client_get_msg(mock_client *c, void *msg)
{
    if (c->inbox_head == c->inbox_tail)
        return 0;
    memcpy(msg, c->inbox[c->inbox_head % MOCK_CLIENT_INBOX], AAR_MSG_BYTES);
    c->inbox_head++;
    return 1;
}

const mock_client_hist_t *mock_client_hist_at(const mock_client *c, uint32_t n)
{
    uint32_t first = c->hist_count > MOCK_CLIENT_HISTORY
                         ? c->hist_count - MOCK_CLIENT_HISTORY : 0u;

    if (n >= c->hist_count || n < first)
        return NULL;
    return &c->hist[n % MOCK_CLIENT_HISTORY];
}

const mock_client_hist_t *mock_client_last(const mock_client *c, uint16_t op)
{
    uint32_t n;

    for (n = c->hist_count; n > 0u; n--) {
        const mock_client_hist_t *h = mock_client_hist_at(c, n - 1u);

        if (!h)
            break;
        if (h->req.op == op)
            return h;
    }
    return NULL;
}

uint32_t mock_client_count(const mock_client *c, uint16_t op)
{
    return op < MOCK_CLIENT_OPS ? c->op_count[op] : 0u;
}
