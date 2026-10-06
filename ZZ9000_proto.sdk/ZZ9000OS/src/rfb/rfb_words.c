/* The control words, built and read.  See rfb_words.h for the vocabulary.
 *
 * No stdio: the Amiga side of this runs inside httpd, which is a Shell command
 * with 4 KB of stack, and a printf pulls in a formatter for six numbers that
 * are all small unsigned decimals.
 *
 * SPDX-License-Identifier: MIT
 */

#include "rfb/rfb_words.h"

/* ------------------------------------------------------------- building --- */

static rfb_u32 put_num(char *out, rfb_u32 cap, rfb_u32 at, rfb_u32 v)
{
    char digits[10];
    rfb_u32 n = 0;

    do {
        digits[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);

    while (n > 0u) {
        if (at >= cap)
            return cap + 1u;        /* overflow, and the caller checks */
        out[at++] = digits[--n];
    }
    return at;
}

static rfb_u32 put_char(char *out, rfb_u32 cap, rfb_u32 at, char c)
{
    if (at >= cap)
        return cap + 1u;
    out[at++] = c;
    return at;
}

/* A hotspot can sit left of or above the corner of the sprite, so the one
   number in this vocabulary that can be negative needs its sign written. */
static rfb_u32 put_signed(char *out, rfb_u32 cap, rfb_u32 at, rfb_s16 v)
{
    if (v < 0) {
        at = put_char(out, cap, at, '-');
        return put_num(out, cap, at, (rfb_u32)(-(rfb_s32)v));
    }
    return put_num(out, cap, at, (rfb_u32)v);
}

rfb_u32 rfb_word_geom(char *out, rfb_u32 cap, const rfb_geom *g)
{
    static const char kw[] = "geom";
    rfb_u32 at = 0;
    rfb_u32 i;

    if (!out || cap == 0u || !g)
        return 0;

    for (i = 0; kw[i] != '\0'; i++)
        at = put_char(out, cap, at, kw[i]);

    at = put_char(out, cap, at, ' ');
    at = put_num(out, cap, at, g->width);
    at = put_char(out, cap, at, ' ');
    at = put_num(out, cap, at, g->height);
    at = put_char(out, cap, at, ' ');
    at = put_num(out, cap, at, g->depth);
    at = put_char(out, cap, at, ' ');
    at = put_num(out, cap, at, g->bytes_per_row);
    at = put_char(out, cap, at, ' ');
    at = put_num(out, cap, at, g->tile_w);
    at = put_char(out, cap, at, ' ');
    at = put_num(out, cap, at, g->tile_h);
    /* And what a byte of the frames after it means.  Last, because it is the
       number that this vocabulary gained, not one it started with. */
    at = put_char(out, cap, at, ' ');
    at = put_num(out, cap, at, g->format);

    if (at >= cap)                  /* the terminator must fit too */
        return 0;

    out[at] = '\0';
    return at;
}

rfb_u32 rfb_word_pal(char *out, rfb_u32 cap, const rfb_u8 *rgb,
                     rfb_u32 colours)
{
    static const char hex[] = "0123456789abcdef";
    static const char kw[] = "pal ";
    rfb_u32 at = 0;
    rfb_u32 i;

    if (!out || cap == 0u || !rgb)
        return 0;

    if (4u + 6u * colours + 1u > cap)
        return 0;

    for (i = 0; kw[i] != '\0'; i++)
        out[at++] = kw[i];

    for (i = 0; i < colours * 3u; i++) {
        out[at++] = hex[(rgb[i] >> 4) & 0x0fu];
        out[at++] = hex[rgb[i] & 0x0fu];
    }

    out[at] = '\0';
    return at;
}


/*
 * The pointer image.  Refused, and not truncated, when the shape is past what
 * the vocabulary carries: a viewer handed half a sprite draws a wrong pointer
 * and cannot tell, where one handed nothing keeps its own.
 */
rfb_u32 rfb_word_ptr(char *out, rfb_u32 cap, const rfb_pointer *p,
                     const rfb_u8 *rgb, const rfb_u8 *bits)
{
    static const char hex[] = "0123456789abcdef";
    static const char kw[] = "ptr";
    rfb_u32 colours;
    rfb_u32 nbits;
    rfb_u32 at = 0;
    rfb_u32 i;

    if (!out || cap == 0u || !p || !rgb || !bits)
        return 0;

    if (p->width == 0u || p->width > RFB_PTR_MAX_W ||
        p->height == 0u || p->height > RFB_PTR_MAX_H ||
        p->depth == 0u || p->depth > RFB_PTR_MAX_DEPTH ||
        p->x_scale == 0u || p->y_scale == 0u)
        return 0;

    colours = (1u << p->depth) - 1u;
    nbits   = (rfb_u32)p->depth * RFB_PTR_ROW_BYTES(p->width) * p->height;

    for (i = 0; kw[i] != '\0'; i++)
        at = put_char(out, cap, at, kw[i]);

    at = put_char(out, cap, at, ' ');
    at = put_num(out, cap, at, p->width);
    at = put_char(out, cap, at, ' ');
    at = put_num(out, cap, at, p->height);
    at = put_char(out, cap, at, ' ');
    at = put_num(out, cap, at, p->depth);
    at = put_char(out, cap, at, ' ');
    at = put_num(out, cap, at, p->x_scale);
    at = put_char(out, cap, at, ' ');
    at = put_num(out, cap, at, p->y_scale);
    at = put_char(out, cap, at, ' ');
    at = put_signed(out, cap, at, p->hot_x);
    at = put_char(out, cap, at, ' ');
    at = put_signed(out, cap, at, p->hot_y);
    at = put_char(out, cap, at, ' ');

    if (at > cap || at + 2u * (3u * colours + nbits) + 2u > cap)
        return 0;

    for (i = 0; i < 3u * colours; i++) {
        out[at++] = hex[(rgb[i] >> 4) & 0x0fu];
        out[at++] = hex[rgb[i] & 0x0fu];
    }

    out[at++] = ' ';

    for (i = 0; i < nbits; i++) {
        out[at++] = hex[(bits[i] >> 4) & 0x0fu];
        out[at++] = hex[bits[i] & 0x0fu];
    }

    out[at] = '\0';
    return at;
}

/* -------------------------------------------------------------- reading --- */

/*
 * A word is a keyword and then decimal numbers separated by runs of spaces.
 * The cursor is a pair of indices into the frame as it arrived, which is not
 * terminated.  A text frame carries a length and nothing else, and a copy into
 * a buffer, so that string functions can run on it, is one copy per mouse
 * move.
 */
typedef struct {
    const char *s;
    rfb_u32     n;
    rfb_u32     at;
} cursor;

static void skip_spaces(cursor *c)
{
    while (c->at < c->n && (c->s[c->at] == ' ' || c->s[c->at] == '\t'))
        c->at++;
}

/* The keyword, if it is this one and is followed by a separator or the end. */
static int take_keyword(cursor *c, const char *want)
{
    rfb_u32 at = c->at;
    rfb_u32 i = 0;

    while (want[i] != '\0') {
        if (at >= c->n || c->s[at] != want[i])
            return 0;
        at++;
        i++;
    }

    if (at < c->n && c->s[at] != ' ' && c->s[at] != '\t')
        return 0;

    c->at = at;
    return 1;
}

/*
 * One signed decimal.  Bounded at six digits: every number in this vocabulary
 * is a screen coordinate, a rawkey code, a qualifier mask or a wheel notch,
 * and none of them is bigger than that.  A longer one comes from a client that
 * does not send what this reads, so the word is refused whole.
 */
static int take_int(cursor *c, rfb_s32 *out)
{
    rfb_s32 v = 0;
    int neg = 0;
    int digits = 0;

    skip_spaces(c);

    if (c->at < c->n && (c->s[c->at] == '-' || c->s[c->at] == '+')) {
        neg = (c->s[c->at] == '-');
        c->at++;
    }

    while (c->at < c->n && c->s[c->at] >= '0' && c->s[c->at] <= '9') {
        if (digits >= 6)
            return 0;
        v = v * 10 + (rfb_s32)(c->s[c->at] - '0');
        c->at++;
        digits++;
    }

    if (digits == 0)
        return 0;

    *out = neg ? -v : v;
    return 1;
}

/* Nothing but spaces left.  A word with something after its numbers is not one
 * of these, and the part that parsed is not acted on. */
static int at_end(cursor *c)
{
    skip_spaces(c);
    return c->at >= c->n;
}

int rfb_word_parse(const char *w, rfb_u32 len, rfb_input *ev)
{
    cursor c;

    if (!w || !ev)
        return 0;

    ev->kind = RFB_IN_NONE;
    ev->a = 0;
    ev->b = 0;
    ev->c = 0;

    c.s = w;
    c.n = len;
    c.at = 0;

    skip_spaces(&c);

    if (take_keyword(&c, "refresh")) {
        if (!at_end(&c))
            return 0;
        ev->kind = RFB_IN_REFRESH;
        return 1;
    }

    if (take_keyword(&c, "reset")) {
        if (!at_end(&c))
            return 0;
        ev->kind = RFB_IN_RESET;
        return 1;
    }

    if (take_keyword(&c, "m")) {
        if (!take_int(&c, &ev->a) || !take_int(&c, &ev->b) ||
            !take_int(&c, &ev->c) || !at_end(&c))
            return 0;
        ev->kind = RFB_IN_POINTER;
        return 1;
    }

    if (take_keyword(&c, "w")) {
        if (!take_int(&c, &ev->a) || !take_int(&c, &ev->b) || !at_end(&c))
            return 0;
        ev->kind = RFB_IN_WHEEL;
        return 1;
    }

    if (take_keyword(&c, "kd") || take_keyword(&c, "ku")) {
        rfb_u8 kind = (w[c.at - 1] == 'd') ? (rfb_u8)RFB_IN_KEYDOWN
                                           : (rfb_u8)RFB_IN_KEYUP;

        if (!take_int(&c, &ev->a) || !take_int(&c, &ev->b) || !at_end(&c))
            return 0;
        ev->kind = kind;
        return 1;
    }

    return 0;
}
