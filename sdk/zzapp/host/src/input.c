/*
 * SDL keyboard, mouse and joystick events become ZZ_EV_* with Amiga raw key
 * codes. The up bit is not stored in code; the event type distinguishes it,
 * matching ZZRun. Qualifiers sit in buttons using the Amiga IEQUALIFIER bits.
 * Ctrl-D/E/F (raw 0x22/0x12/0x23) also enqueue ZZ_EV_BREAK with codes 4/5/6.
 * Window close and Ctrl-C enqueue one ZZ_EV_STOP and set the stop flag.
 * The queue holds 256 events and drops the newest when full.
 *
 * --input FILE injects scripted events before the app starts. Lines:
 *   key down|up <raw> [qualifiers]
 *   mouse <dx> <dy> [buttons]
 *   mousebtn <1|2|3> down|up
 *   joy <port> <dx> <dy> <buttons>
 *   break <4|5|6>
 *   stop
 *   resize <w> <h>
 * Numbers accept a 0x prefix. '#' starts a comment.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include "host_internal.h"

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOST_QUAL_LSHIFT 0x0001u
#define HOST_QUAL_RSHIFT 0x0002u
#define HOST_QUAL_CAPS 0x0004u
#define HOST_QUAL_CTRL 0x0008u
#define HOST_QUAL_LALT 0x0010u
#define HOST_QUAL_RALT 0x0020u
#define HOST_QUAL_LCMD 0x0040u
#define HOST_QUAL_RCMD 0x0080u
#define HOST_QUAL_NUMPAD 0x0100u
#define HOST_QUAL_REPEAT 0x0200u

static zzapp_event queue[HOST_INPUT_CAP];
static uint32_t q_head, q_tail;
static uint32_t drops;
static uint16_t mouse_buttons;
static int stop_event_sent;
static SDL_Joystick *joys[2];
static uint32_t joy_last[2];
static int joy_valid[2];

struct raw_map {
    SDL_Scancode scan;
    uint16_t raw;
    int keypad;
};

static const struct raw_map raw_table[] = {
    {SDL_SCANCODE_GRAVE, 0x00u, 0},
    {SDL_SCANCODE_1, 0x01u, 0},
    {SDL_SCANCODE_2, 0x02u, 0},
    {SDL_SCANCODE_3, 0x03u, 0},
    {SDL_SCANCODE_4, 0x04u, 0},
    {SDL_SCANCODE_5, 0x05u, 0},
    {SDL_SCANCODE_6, 0x06u, 0},
    {SDL_SCANCODE_7, 0x07u, 0},
    {SDL_SCANCODE_8, 0x08u, 0},
    {SDL_SCANCODE_9, 0x09u, 0},
    {SDL_SCANCODE_0, 0x0au, 0},
    {SDL_SCANCODE_MINUS, 0x0bu, 0},
    {SDL_SCANCODE_EQUALS, 0x0cu, 0},
    {SDL_SCANCODE_BACKSLASH, 0x0du, 0},
    {SDL_SCANCODE_KP_0, 0x0fu, 1},
    {SDL_SCANCODE_Q, 0x10u, 0},
    {SDL_SCANCODE_W, 0x11u, 0},
    {SDL_SCANCODE_E, 0x12u, 0},
    {SDL_SCANCODE_R, 0x13u, 0},
    {SDL_SCANCODE_T, 0x14u, 0},
    {SDL_SCANCODE_Y, 0x15u, 0},
    {SDL_SCANCODE_U, 0x16u, 0},
    {SDL_SCANCODE_I, 0x17u, 0},
    {SDL_SCANCODE_O, 0x18u, 0},
    {SDL_SCANCODE_P, 0x19u, 0},
    {SDL_SCANCODE_LEFTBRACKET, 0x1au, 0},
    {SDL_SCANCODE_RIGHTBRACKET, 0x1bu, 0},
    {SDL_SCANCODE_KP_1, 0x1du, 1},
    {SDL_SCANCODE_KP_2, 0x1eu, 1},
    {SDL_SCANCODE_KP_3, 0x1fu, 1},
    {SDL_SCANCODE_A, 0x20u, 0},
    {SDL_SCANCODE_S, 0x21u, 0},
    {SDL_SCANCODE_D, 0x22u, 0},
    {SDL_SCANCODE_F, 0x23u, 0},
    {SDL_SCANCODE_G, 0x24u, 0},
    {SDL_SCANCODE_H, 0x25u, 0},
    {SDL_SCANCODE_J, 0x26u, 0},
    {SDL_SCANCODE_K, 0x27u, 0},
    {SDL_SCANCODE_L, 0x28u, 0},
    {SDL_SCANCODE_SEMICOLON, 0x29u, 0},
    {SDL_SCANCODE_APOSTROPHE, 0x2au, 0},
    {SDL_SCANCODE_NONUSBACKSLASH, 0x30u, 0},
    {SDL_SCANCODE_Z, 0x31u, 0},
    {SDL_SCANCODE_X, 0x32u, 0},
    {SDL_SCANCODE_C, 0x33u, 0},
    {SDL_SCANCODE_V, 0x34u, 0},
    {SDL_SCANCODE_B, 0x35u, 0},
    {SDL_SCANCODE_N, 0x36u, 0},
    {SDL_SCANCODE_M, 0x37u, 0},
    {SDL_SCANCODE_COMMA, 0x38u, 0},
    {SDL_SCANCODE_PERIOD, 0x39u, 0},
    {SDL_SCANCODE_SLASH, 0x3au, 0},
    {SDL_SCANCODE_KP_PERIOD, 0x3cu, 1},
    {SDL_SCANCODE_KP_7, 0x3du, 1},
    {SDL_SCANCODE_KP_8, 0x3eu, 1},
    {SDL_SCANCODE_KP_9, 0x3fu, 1},
    {SDL_SCANCODE_SPACE, 0x40u, 0},
    {SDL_SCANCODE_BACKSPACE, 0x41u, 0},
    {SDL_SCANCODE_TAB, 0x42u, 0},
    {SDL_SCANCODE_KP_ENTER, 0x43u, 1},
    {SDL_SCANCODE_RETURN, 0x44u, 0},
    {SDL_SCANCODE_ESCAPE, 0x45u, 0},
    {SDL_SCANCODE_DELETE, 0x46u, 0},
    {SDL_SCANCODE_KP_MINUS, 0x4au, 1},
    {SDL_SCANCODE_UP, 0x4cu, 0},
    {SDL_SCANCODE_DOWN, 0x4du, 0},
    {SDL_SCANCODE_RIGHT, 0x4eu, 0},
    {SDL_SCANCODE_LEFT, 0x4fu, 0},
    {SDL_SCANCODE_F1, 0x50u, 0},
    {SDL_SCANCODE_F2, 0x51u, 0},
    {SDL_SCANCODE_F3, 0x52u, 0},
    {SDL_SCANCODE_F4, 0x53u, 0},
    {SDL_SCANCODE_F5, 0x54u, 0},
    {SDL_SCANCODE_F6, 0x55u, 0},
    {SDL_SCANCODE_F7, 0x56u, 0},
    {SDL_SCANCODE_F8, 0x57u, 0},
    {SDL_SCANCODE_F9, 0x58u, 0},
    {SDL_SCANCODE_F10, 0x59u, 0},
    {SDL_SCANCODE_KP_DIVIDE, 0x5cu, 1},
    {SDL_SCANCODE_KP_MULTIPLY, 0x5du, 1},
    {SDL_SCANCODE_KP_PLUS, 0x5eu, 1},
    {SDL_SCANCODE_LSHIFT, 0x60u, 0},
    {SDL_SCANCODE_RSHIFT, 0x61u, 0},
    {SDL_SCANCODE_CAPSLOCK, 0x62u, 0},
    {SDL_SCANCODE_LCTRL, 0x63u, 0},
    {SDL_SCANCODE_RCTRL, 0x63u, 0},
    {SDL_SCANCODE_LALT, 0x64u, 0},
    {SDL_SCANCODE_RALT, 0x65u, 0},
    {SDL_SCANCODE_LGUI, 0x66u, 0},
    {SDL_SCANCODE_RGUI, 0x67u, 0},
};

static int raw_from_scan(SDL_Scancode scan, uint16_t *raw, int *keypad)
{
    size_t i;

    for (i = 0; i < sizeof raw_table / sizeof raw_table[0]; i++) {
        if (raw_table[i].scan == scan) {
            *raw = raw_table[i].raw;
            *keypad = raw_table[i].keypad;
            return 1;
        }
    }
    return 0;
}

static uint16_t qualifiers(SDL_Keymod mod, int keypad, int repeat)
{
    uint16_t q = 0;

    if (mod & KMOD_LSHIFT)
        q |= HOST_QUAL_LSHIFT;
    if (mod & KMOD_RSHIFT)
        q |= HOST_QUAL_RSHIFT;
    if (mod & KMOD_CAPS)
        q |= HOST_QUAL_CAPS;
    if (mod & KMOD_CTRL)
        q |= HOST_QUAL_CTRL;
    if (mod & KMOD_LALT)
        q |= HOST_QUAL_LALT;
    if (mod & KMOD_RALT)
        q |= HOST_QUAL_RALT;
    if (mod & KMOD_LGUI)
        q |= HOST_QUAL_LCMD;
    if (mod & KMOD_RGUI)
        q |= HOST_QUAL_RCMD;
    if (keypad)
        q |= HOST_QUAL_NUMPAD;
    if (repeat)
        q |= HOST_QUAL_REPEAT;
    return q;
}

static uint16_t break_for_raw(uint16_t raw, uint16_t qual)
{
    if ((qual & HOST_QUAL_CTRL) == 0u)
        return 0;
    if (raw == 0x22u)
        return ZZ_BREAK_CTRL_D;
    if (raw == 0x12u)
        return ZZ_BREAK_CTRL_E;
    if (raw == 0x23u)
        return ZZ_BREAK_CTRL_F;
    return 0;
}

static void push_event(uint16_t type, uint16_t code, int16_t dx, int16_t dy,
                       uint16_t buttons)
{
    zzapp_event *ev;

    if (q_head - q_tail >= HOST_INPUT_CAP) {
        drops++;
        return;
    }
    ev = &queue[q_head & (HOST_INPUT_CAP - 1u)];
    memset(ev, 0, sizeof *ev);
    ev->type = type;
    ev->code = code;
    ev->dx = dx;
    ev->dy = dy;
    ev->buttons = buttons;
    ev->time_ms = host_time_ms();
    q_head++;
}

static void note_stop(void)
{
    host_request_stop();
    if (!stop_event_sent) {
        push_event(ZZ_EV_STOP, 0, 0, 0, 0);
        stop_event_sent = 1;
    }
}

static void on_key(const SDL_KeyboardEvent *key)
{
    uint16_t raw = 0;
    int keypad = 0;
    uint16_t type;
    uint16_t qual;
    uint16_t brk;

    if (!raw_from_scan(key->keysym.scancode, &raw, &keypad))
        return;
    type = key->type == SDL_KEYUP ? ZZ_EV_KEY_UP : ZZ_EV_KEY_DOWN;
    qual = qualifiers(key->keysym.mod, keypad, key->repeat);
    push_event(type, raw, 0, 0, qual);
    brk = type == ZZ_EV_KEY_DOWN ? break_for_raw(raw, qual) : 0;
    if (brk)
        push_event(ZZ_EV_BREAK, brk, 0, 0, qual);
}

static void on_button(const SDL_MouseButtonEvent *btn)
{
    uint16_t bit = 0;
    uint16_t id = 0;

    if (btn->button == SDL_BUTTON_LEFT) {
        bit = 1u;
        id = 1u;
    } else if (btn->button == SDL_BUTTON_RIGHT) {
        bit = 2u;
        id = 2u;
    } else if (btn->button == SDL_BUTTON_MIDDLE) {
        bit = 4u;
        id = 3u;
    } else {
        return;
    }
    if (btn->type == SDL_MOUSEBUTTONDOWN)
        mouse_buttons |= bit;
    else
        mouse_buttons = (uint16_t)(mouse_buttons & ~bit);
    push_event(ZZ_EV_MOUSE, id, 0, 0, mouse_buttons);
}

static int axis_dir(Sint16 v)
{
    if (v < -16000)
        return -1;
    if (v > 16000)
        return 1;
    return 0;
}

static void poll_joysticks(void)
{
    int n;

    for (n = 0; n < 2; n++) {
        SDL_Joystick *joy = joys[n];
        int dx = 0;
        int dy = 0;
        int fire;
        uint32_t state;
        uint16_t port;

        if (!joy || !SDL_JoystickGetAttached(joy))
            continue;
        if (SDL_JoystickNumHats(joy) > 0) {
            Uint8 hat = SDL_JoystickGetHat(joy, 0);
            if (hat & SDL_HAT_LEFT)
                dx = -1;
            else if (hat & SDL_HAT_RIGHT)
                dx = 1;
            if (hat & SDL_HAT_UP)
                dy = -1;
            else if (hat & SDL_HAT_DOWN)
                dy = 1;
        } else if (SDL_JoystickNumAxes(joy) >= 2) {
            dx = axis_dir(SDL_JoystickGetAxis(joy, 0));
            dy = axis_dir(SDL_JoystickGetAxis(joy, 1));
        }
        fire = SDL_JoystickNumButtons(joy) > 0 &&
               SDL_JoystickGetButton(joy, 0);
        state = ((uint32_t)(dx + 1) << 8) | (uint32_t)(dy + 1) |
                (fire ? 0x10000u : 0u);
        if (joy_valid[n] && state == joy_last[n])
            continue;
        joy_last[n] = state;
        joy_valid[n] = 1;
        /* SDL joystick 0 is the Amiga game port (port 1). */
        port = n == 0 ? 1u : 0u;
        push_event(ZZ_EV_JOY, port, (int16_t)dx, (int16_t)dy, fire ? 1u : 0u);
    }
}

static int parse_i(const char *s, long *out)
{
    char *end = 0;
    long v;

    if (!s || !*s)
        return 0;
    v = strtol(s, &end, 0);
    if (!end || *end)
        return 0;
    *out = v;
    return 1;
}

static int load_input(const char *path)
{
    FILE *fp;
    char line[160];
    uint32_t lineno = 0;

    fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "zzapp-host: cannot open input script %s\n", path);
        return -1;
    }
    while (fgets(line, sizeof line, fp)) {
        char *tok[6];
        int ntok = 0;
        char *p = line;
        size_t n = strlen(p);
        long a = 0, b = 0, c = 0, d = 0;

        lineno++;
        while (n && (p[n - 1u] == '\n' || p[n - 1u] == '\r'))
            p[--n] = 0;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == 0 || *p == '#')
            continue;
        while (ntok < 6 && *p) {
            tok[ntok++] = p;
            while (*p && *p != ' ' && *p != '\t')
                p++;
            if (*p) {
                *p++ = 0;
                while (*p == ' ' || *p == '\t')
                    p++;
            }
        }
        if (ntok >= 3 && strcmp(tok[0], "key") == 0 && parse_i(tok[2], &a)) {
            uint16_t qual = 0;
            if (ntok >= 4 && parse_i(tok[3], &b))
                qual = (uint16_t)b;
            if (strcmp(tok[1], "down") == 0)
                push_event(ZZ_EV_KEY_DOWN, (uint16_t)a, 0, 0, qual);
            else if (strcmp(tok[1], "up") == 0)
                push_event(ZZ_EV_KEY_UP, (uint16_t)a, 0, 0, qual);
            else
                goto bad;
        } else if (ntok >= 3 && strcmp(tok[0], "mouse") == 0 &&
                   parse_i(tok[1], &a) && parse_i(tok[2], &b)) {
            uint16_t buttons = 0;
            if (ntok >= 4 && parse_i(tok[3], &c))
                buttons = (uint16_t)c;
            push_event(ZZ_EV_MOUSE, 0, (int16_t)a, (int16_t)b, buttons);
        } else if (ntok >= 3 && strcmp(tok[0], "mousebtn") == 0 &&
                   parse_i(tok[1], &a)) {
            uint16_t bit = a == 1 ? 1u : a == 2 ? 2u : a == 3 ? 4u : 0u;
            if (!bit || (strcmp(tok[2], "down") != 0 && strcmp(tok[2], "up") != 0))
                goto bad;
            if (strcmp(tok[2], "down") == 0)
                mouse_buttons |= bit;
            else
                mouse_buttons = (uint16_t)(mouse_buttons & ~bit);
            push_event(ZZ_EV_MOUSE, (uint16_t)a, 0, 0, mouse_buttons);
        } else if (ntok >= 5 && strcmp(tok[0], "joy") == 0 &&
                   parse_i(tok[1], &a) && parse_i(tok[2], &b) &&
                   parse_i(tok[3], &c) && parse_i(tok[4], &d)) {
            push_event(ZZ_EV_JOY, (uint16_t)a, (int16_t)b, (int16_t)c,
                       (uint16_t)d);
        } else if (ntok >= 2 && strcmp(tok[0], "break") == 0 &&
                   parse_i(tok[1], &a)) {
            push_event(ZZ_EV_BREAK, (uint16_t)a, 0, 0, 0);
        } else if (ntok >= 3 && strcmp(tok[0], "resize") == 0 &&
                   parse_i(tok[1], &a) && parse_i(tok[2], &b)) {
            push_event(ZZ_EV_RESIZE, 0, (int16_t)a, (int16_t)b, 0);
        } else if (ntok == 1 && strcmp(tok[0], "stop") == 0) {
            note_stop();
        } else {
            goto bad;
        }
        continue;
    bad:
        fprintf(stderr, "zzapp-host: bad input script line %u\n", lineno);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static int api_input_poll(zzapp_event *out, uint32_t max, uint32_t *out_n)
{
    uint32_t n = 0;

    if (!out_n || (max && !out))
        return ZZ_EINVAL;
    while (n < max && q_tail != q_head) {
        out[n++] = queue[q_tail & (HOST_INPUT_CAP - 1u)];
        q_tail++;
    }
    *out_n = n;
    return ZZ_OK;
}

void host_input_pump(void)
{
    SDL_Event ev;

    if (host_stop_event_pending())
        note_stop();
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT)
            note_stop();
        else if (ev.type == SDL_WINDOWEVENT &&
                 ev.window.event == SDL_WINDOWEVENT_CLOSE)
            note_stop();
        else if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP)
            on_key(&ev.key);
        else if (ev.type == SDL_MOUSEBUTTONDOWN ||
                 ev.type == SDL_MOUSEBUTTONUP)
            on_button(&ev.button);
        else if (ev.type == SDL_MOUSEMOTION &&
                 (ev.motion.xrel || ev.motion.yrel))
            push_event(ZZ_EV_MOUSE, 0, (int16_t)ev.motion.xrel,
                       (int16_t)ev.motion.yrel, mouse_buttons);
    }
    poll_joysticks();
    (void)drops;
}

int host_input_selftest(void)
{
    uint16_t raw = 0;
    int keypad = 0;
    SDL_Event ev;
    zzapp_event got[4];
    uint32_t n = 0;
    int saw_key = 0;
    int saw_break = 0;
    uint32_t i;

    if (!raw_from_scan(SDL_SCANCODE_D, &raw, &keypad) || raw != 0x22u)
        return 1;
    if (!raw_from_scan(SDL_SCANCODE_E, &raw, &keypad) || raw != 0x12u)
        return 2;
    if (!raw_from_scan(SDL_SCANCODE_F, &raw, &keypad) || raw != 0x23u)
        return 3;
    if (!raw_from_scan(SDL_SCANCODE_ESCAPE, &raw, &keypad) || raw != 0x45u)
        return 4;
    if (!raw_from_scan(SDL_SCANCODE_UP, &raw, &keypad) || raw != 0x4cu)
        return 5;
    memset(&ev, 0, sizeof ev);
    ev.type = SDL_KEYDOWN;
    ev.key.type = SDL_KEYDOWN;
    ev.key.state = SDL_PRESSED;
    ev.key.keysym.scancode = SDL_SCANCODE_D;
    ev.key.keysym.mod = KMOD_LCTRL;
    if (SDL_PushEvent(&ev) < 0)
        return 6;
    host_input_pump();
    if (api_input_poll(got, 4u, &n) != ZZ_OK)
        return 7;
    for (i = 0; i < n; i++) {
        if (got[i].type == ZZ_EV_KEY_DOWN && got[i].code == 0x22u)
            saw_key = 1;
        if (got[i].type == ZZ_EV_BREAK && got[i].code == ZZ_BREAK_CTRL_D)
            saw_break = 1;
    }
    if (!saw_key || !saw_break)
        return 8;
    return 0;
}

int host_input_init(void)
{
    const struct host_options *opt = host_options();
    int i;

    q_head = q_tail = 0;
    drops = 0;
    mouse_buttons = 0;
    stop_event_sent = 0;
    for (i = 0; i < 2; i++) {
        joys[i] = 0;
        joy_valid[i] = 0;
        if (i < SDL_NumJoysticks())
            joys[i] = SDL_JoystickOpen(i);
    }
    if (opt->input_path && load_input(opt->input_path) != 0)
        return -1;
    return 0;
}

void host_input_shutdown(void)
{
    int i;

    for (i = 0; i < 2; i++) {
        if (joys[i])
            SDL_JoystickClose(joys[i]);
        joys[i] = 0;
    }
}

void host_input_bind(zzapp_api *api)
{
    api->input_poll = api_input_poll;
}
