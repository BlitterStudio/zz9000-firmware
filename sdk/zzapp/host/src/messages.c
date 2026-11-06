/*
 * Two 64-by-32 rings, matching the card. Without --msg-script, zz_msg_send
 * loops back into the queue zz_msg_poll reads, so a host app can talk to
 * itself. With --msg-script, script lines are host-to-app messages and
 * zz_msg_send is recorded as app-to-host and is not fed back.
 *
 * Script lines: 64 hex digits, a quoted string padded with NULs to 32 bytes,
 * or "stop". "stop" asks the runner to stop once every injected message has
 * been sent back. '#' comments and blank lines are ignored.
 *
 * SPDX-License-Identifier: MIT
 */
#include "host_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct script_msg {
    uint8_t bytes[HOST_MSG_BYTES];
};

static uint8_t h2a[HOST_MSG_CAP][HOST_MSG_BYTES];
static uint8_t a2h[HOST_MSG_CAP][HOST_MSG_BYTES];
static uint32_t h2a_head, h2a_tail;
static uint32_t a2h_head, a2h_tail;
static struct script_msg *script;
static uint32_t script_count;
static uint32_t script_injected;
static int script_mode;
static int script_stop;
static uint32_t recorded;
static FILE *msg_out;

static int ring_full(uint32_t head, uint32_t tail)
{
    return head - tail >= HOST_MSG_CAP;
}

static int ring_push(uint8_t ring[][HOST_MSG_BYTES], uint32_t *head,
                     uint32_t tail, const void *msg)
{
    if (ring_full(*head, tail))
        return 0;
    memcpy(ring[*head & (HOST_MSG_CAP - 1u)], msg, HOST_MSG_BYTES);
    *head += 1u;
    return 1;
}

static int ring_pop(uint8_t ring[][HOST_MSG_BYTES], uint32_t head,
                    uint32_t *tail, void *msg)
{
    if (head == *tail)
        return 0;
    memcpy(msg, ring[*tail & (HOST_MSG_CAP - 1u)], HOST_MSG_BYTES);
    *tail += 1u;
    return 1;
}

static int hex_nibble(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int parse_hex(const char *s, uint8_t out[HOST_MSG_BYTES])
{
    uint32_t i;

    for (i = 0; i < HOST_MSG_BYTES; i++) {
        int hi = hex_nibble((unsigned char)s[i * 2u]);
        int lo = hex_nibble((unsigned char)s[i * 2u + 1u]);
        if (hi < 0 || lo < 0)
            return 0;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return s[HOST_MSG_BYTES * 2u] == 0;
}

static int parse_quoted(const char *s, uint8_t out[HOST_MSG_BYTES])
{
    size_t n;

    if (s[0] != '"')
        return 0;
    n = strlen(s);
    if (n < 2u || s[n - 1u] != '"')
        return 0;
    memset(out, 0, HOST_MSG_BYTES);
    n -= 2u;
    if (n > HOST_MSG_BYTES)
        n = HOST_MSG_BYTES;
    memcpy(out, s + 1, n);
    return 1;
}

static int script_add(const uint8_t bytes[HOST_MSG_BYTES])
{
    struct script_msg *grown;

    grown = (struct script_msg *)realloc(script,
                                         (script_count + 1u) * sizeof *script);
    if (!grown)
        return 0;
    script = grown;
    memcpy(script[script_count].bytes, bytes, HOST_MSG_BYTES);
    script_count++;
    return 1;
}

static int load_script(const char *path)
{
    FILE *fp;
    char line[160];
    uint32_t lineno = 0;

    fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "zzapp-host: cannot open message script %s\n", path);
        return -1;
    }
    while (fgets(line, sizeof line, fp)) {
        char *p = line;
        size_t n;
        uint8_t msg[HOST_MSG_BYTES];

        lineno++;
        n = strlen(p);
        while (n && (p[n - 1u] == '\n' || p[n - 1u] == '\r'))
            p[--n] = 0;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == 0 || *p == '#')
            continue;
        if (strcmp(p, "stop") == 0) {
            script_stop = 1;
            continue;
        }
        if (!parse_hex(p, msg) && !parse_quoted(p, msg)) {
            fprintf(stderr, "zzapp-host: bad message script line %u\n", lineno);
            fclose(fp);
            return -1;
        }
        if (!script_add(msg)) {
            fclose(fp);
            return -1;
        }
    }
    fclose(fp);
    return 0;
}

static void record_sent(const void *msg)
{
    uint32_t i;
    const uint8_t *p = (const uint8_t *)msg;

    recorded++;
    if (!msg_out)
        return;
    for (i = 0; i < HOST_MSG_BYTES; i++)
        fprintf(msg_out, "%02x", p[i]);
    fputc('\n', msg_out);
    fflush(msg_out);
}

static int api_msg_send(const void *msg)
{
    if (!msg)
        return ZZ_EINVAL;
    if (script_mode) {
        if (!ring_push(a2h, &a2h_head, a2h_tail, msg))
            return ZZ_EBUSY;
        record_sent(msg);
        /* Nothing drains a2h; the message is already recorded. Release
         * the slot now, or the 65th send wedges with ZZ_EBUSY forever. */
        a2h_tail = a2h_head;
        if (script_stop && script_injected == script_count &&
            recorded >= script_count)
            host_request_stop();
        return ZZ_OK;
    }
    if (!ring_push(h2a, &h2a_head, h2a_tail, msg))
        return ZZ_EBUSY;
    return ZZ_OK;
}

static int api_msg_poll(void *msg)
{
    if (!msg)
        return ZZ_EINVAL;
    if (!ring_pop(h2a, h2a_head, &h2a_tail, msg))
        return 0;
    return 1;
}

void host_messages_pump(void)
{
    while (script_mode && script_injected < script_count &&
           !ring_full(h2a_head, h2a_tail)) {
        ring_push(h2a, &h2a_head, h2a_tail, script[script_injected].bytes);
        script_injected++;
    }
    if (script_mode && script_stop && script_injected == script_count &&
        recorded >= script_count)
        host_request_stop();
}

int host_messages_init(void)
{
    const struct host_options *opt = host_options();

    h2a_head = h2a_tail = 0;
    a2h_head = a2h_tail = 0;
    recorded = 0;
    script_injected = 0;
    script_mode = opt->msg_script != 0;
    if (opt->msg_script && load_script(opt->msg_script) != 0)
        return -1;
    if (opt->msg_out) {
        msg_out = fopen(opt->msg_out, "wb");
        if (!msg_out) {
            fprintf(stderr, "zzapp-host: cannot open %s\n", opt->msg_out);
            return -1;
        }
    }
    if (script_mode && script_stop && script_count == 0u)
        host_request_stop();
    host_messages_pump();
    return 0;
}

void host_messages_shutdown(void)
{
    if (msg_out) {
        fclose(msg_out);
        msg_out = 0;
    }
    free(script);
    script = 0;
    script_count = 0;
}

void host_messages_bind(zzapp_api *api)
{
    api->msg_send = api_msg_send;
    api->msg_poll = api_msg_poll;
}
