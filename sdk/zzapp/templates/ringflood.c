/*
 * Message ring load for the card's ring conformance test. The app starts
 * QUIET: it answers PING messages and sends nothing else, parked in
 * zz_msg_wait, so a ping measures the ring and the wake-up alone. On CTRL
 * FLOOD it sends sequence-numbered, pattern-checked 32-byte messages as fast
 * as the app-to-host ring takes them and bumps a counter word in a
 * host-visible block on every loop iteration. On CTRL QUIET it stops. The
 * 68k side is zz9k-app-ringtest (sdk/tools). On the PC with the SDL host
 * runner, `ringflood [data-message-count]` leaves the flood after that many
 * DATA messages and exits (0 or no argument runs until the launcher stops
 * the app).
 *
 * Messages are eight little-endian 32-bit words:
 *   INFO  1, 1, counter block offset (0xffffffff if none), block bytes, time
 *   DATA  2, seq from 1, time us, counter, pattern words 0..3
 *   DONE  3, data messages sent, time us, final counter, ring-full retries,
 *         pongs sent
 *   PING  5, id, host stamp low, host stamp high              (host to app)
 *   PONG  6, id, host stamp low, host stamp high, time us, counter
 *   CTRL  7, id, mode (0 quiet, 1 flood)                      (host to app)
 *   ACK   8, id, mode, data messages sent so far, counter
 * An ACK follows every DATA message sent before the mode change. The
 * pattern word i of message seq is the murmur3 finalizer (xor-shift 16,
 * multiply 0x85ebca6b, xor-shift 13, multiply 0xc2b2ae35, xor-shift 16) of
 * seq * 4 + i. The counter word is stored big-endian, so the 68k reads it as
 * a native long. The counter is written and a barrier issued before the
 * message that carries the same value is sent.
 *
 * SPDX-License-Identifier: MIT
 */
#include "zzapp.h"
#include "zzapp_note.h"

ZZAPP_NOTE(0, 0)

#define KIND_INFO 1u
#define KIND_DATA 2u
#define KIND_DONE 3u
#define KIND_PING 5u
#define KIND_PONG 6u
#define KIND_CTRL 7u
#define KIND_ACK 8u
#define MODE_QUIET 0u
#define MODE_FLOOD 1u
#define NO_COUNTER 0xffffffffu
#define BLOCK_BYTES 4096u

static uint32_t get_word(const uint8_t *m, unsigned word)
{
    const uint8_t *p = m + word * 4u;

    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void put_word(uint8_t *m, unsigned word, uint32_t v)
{
    uint8_t *p = m + word * 4u;

    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t pattern(uint32_t seq, unsigned i)
{
    uint32_t h = seq * 4u + i;

    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

static uint32_t now_us(void)
{
    return (uint32_t)zz_time_us();
}

static void clear(uint8_t *m)
{
    unsigned i;

    for (i = 0; i < 32u; i++)
        m[i] = 0;
}

/* Sends m, retrying while the ring is full, until stop or timeout_us. */
static int send_retry(const uint8_t *m, uint32_t timeout_us)
{
    uint64_t deadline = zz_time_us() + timeout_us;
    int rc;

    while ((rc = zz_msg_send(m)) == ZZ_EBUSY) {
        if (zz_time_us() > deadline)
            return rc;
        zz_yield();
    }
    return rc;
}

static uint32_t parse_count(int argc, char **argv)
{
    uint32_t n = 0;
    const char *s;

    if (argc < 2)
        return 0;
    for (s = argv[1]; *s >= '0' && *s <= '9'; s++)
        n = n * 10u + (uint32_t)(*s - '0');
    return n;
}

int zz_main(int argc, char **argv)
{
    uint32_t limit = parse_count(argc, argv);
    volatile uint32_t *block;
    uint32_t block_offset = NO_COUNTER;
    uint32_t mode = MODE_QUIET;
    uint32_t seq = 0;
    uint32_t counter = 0;
    uint32_t busy = 0;
    uint32_t pongs = 0;
    int pend_valid = 0;
    int pend_is_pong = 0;
    uint8_t msg[32];
    uint8_t in[32];
    uint8_t pend[32];
    unsigned i;
    int rc;

    block = (volatile uint32_t *)zz_alloc(BLOCK_BYTES, BLOCK_BYTES,
                                          ZZ_ALLOC_HOST_VISIBLE);
    if (block) {
        uint32_t offset;

        if (zz_host_address((const void *)block, &offset) == ZZ_OK) {
            block_offset = offset;
            *block = 0;
        } else {
            zz_free((void *)block);
            block = 0;
        }
    }

    clear(msg);
    put_word(msg, 0, KIND_INFO);
    put_word(msg, 1, 1u);
    put_word(msg, 2, block_offset);
    put_word(msg, 3, block ? BLOCK_BYTES : 0u);
    put_word(msg, 4, now_us());
    if (send_retry(msg, 5000000u) != ZZ_OK)
        return 20;

    while (!zz_should_stop() && !(mode == MODE_FLOOD && limit && seq >= limit)) {
        if (mode == MODE_FLOOD) {
            counter++;
            if (block) {
                *block = __builtin_bswap32(counter);
                __sync_synchronize();
            }
        } else if (!pend_valid) {
            /* Quiet: park until the host sends something. */
            rc = zz_msg_wait(100000u);
            if (rc == ZZ_ETIMEDOUT)
                continue;
            if (rc != ZZ_OK)
                break; /* stop, or a corrupt ring */
        }
        if (!pend_valid) {
            rc = zz_msg_poll(in);
            if (rc < 0)
                return 20;
            if (rc == 1 && get_word(in, 0) == KIND_PING) {
                clear(pend);
                put_word(pend, 0, KIND_PONG);
                put_word(pend, 1, get_word(in, 1));
                put_word(pend, 2, get_word(in, 2));
                put_word(pend, 3, get_word(in, 3));
                put_word(pend, 4, now_us());
                put_word(pend, 5, counter);
                pend_valid = 1;
                pend_is_pong = 1;
            } else if (rc == 1 && get_word(in, 0) == KIND_CTRL &&
                       get_word(in, 2) <= MODE_FLOOD) {
                mode = get_word(in, 2);
                clear(pend);
                put_word(pend, 0, KIND_ACK);
                put_word(pend, 1, get_word(in, 1));
                put_word(pend, 2, mode);
                put_word(pend, 3, seq);
                put_word(pend, 4, counter);
                pend_valid = 1;
                pend_is_pong = 0;
            }
        }
        if (pend_valid) {
            rc = zz_msg_send(pend);
            if (rc == ZZ_OK) {
                pend_valid = 0;
                if (pend_is_pong)
                    pongs++;
            } else if (rc == ZZ_EBUSY) {
                busy++;
                zz_yield();
            } else {
                return 20;
            }
            continue;
        }
        if (mode != MODE_FLOOD)
            continue;
        clear(msg);
        put_word(msg, 0, KIND_DATA);
        put_word(msg, 1, seq + 1u);
        put_word(msg, 2, now_us());
        put_word(msg, 3, counter);
        for (i = 0; i < 4u; i++)
            put_word(msg, 4u + i, pattern(seq + 1u, i));
        rc = zz_msg_send(msg);
        if (rc == ZZ_OK) {
            seq++;
        } else if (rc == ZZ_EBUSY) {
            busy++;
            zz_yield();
        } else {
            return 20;
        }
    }

    /* A reply still pending is sent before DONE, so its count is right. */
    if (pend_valid && send_retry(pend, 200000u) == ZZ_OK && pend_is_pong)
        pongs++;
    clear(msg);
    put_word(msg, 0, KIND_DONE);
    put_word(msg, 1, seq);
    put_word(msg, 2, now_us());
    put_word(msg, 3, counter);
    put_word(msg, 4, busy);
    put_word(msg, 5, pongs);
    rc = send_retry(msg, limit ? 5000000u : 500000u);
    /* The counter block stays allocated: the host may sample it until it
     * reads DONE, and the runtime reclaims it at exit. */
    return rc == ZZ_OK || zz_should_stop() ? 0 : 20;
}
