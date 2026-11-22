/* libpt4 voices and output: pt_MixUpdateVoice / pt_MixPair (engine/mixer.asm) for the mixer
   path, Paula semantics (sys/init.asm:pt_RpAudInt) for the Paula path. zlib license, see LICENSE. */
#include <stdlib.h>
#include "pt4_internal.h"

void *pt4__malloc(size_t n) { return malloc(n ? n : 1); }

void *pt4__alloc(pt4_song *s, size_t n)
{
    if (n == 0) n = 1;
    return s->al.alloc ? s->al.alloc(n, s->al.user) : malloc(n);
}

void pt4__free(pt4_song *s, void *p)
{
    if (!p) return;
    if (s->al.free) s->al.free(p, s->al.user);
    else free(p);
}

/* pt_MixCalcSpt (CIA mode): samples per tick as 16.16, fraction in 16 bits */
void pt4__calc_spt(pt4_song *s, unsigned bpm)
{
    uint64_t t = (uint64_t)s->rate * pt4__cia_period(bpm);
    uint32_t in = (uint32_t)(t / ECLOCK_PAL), rem = (uint32_t)(t % ECLOCK_PAL), hi, lo;
    hi = (rem << 8) / ECLOCK_PAL;
    rem = (rem << 8) % ECLOCK_PAL;
    lo = (rem << 8) / ECLOCK_PAL;
    s->st.spt = (in << 16) | (hi << 8) | lo;
    s->st.spt_bpm = (uint16_t)bpm;
}

static int nvoices(const pt4_song *s)
{
    if (s->path == PT4_PATH_WIDE) return s->wide ? PT4_MAXCH : s->lay;
    if (s->path == PT4_PATH_MIXER) return 8;
    return s->lay;
}

static uint16_t scale_irq(uint16_t irqvol, uint16_t cv)   /* pt_RpAudIntScaleVol */
{
    uint16_t v;
    if (!irqvol || !cv) return 0;
    v = (uint16_t)(((uint32_t)irqvol * cv + 32) >> 6);
    return v ? v : 1;
}

/* chunk geometry of channel c */
static uint32_t ck_addr(const pt4_song *s, const pt4_chan *c, int i)
{
    if (i == c->ck_first) return c->ck_firstaddr;
    return s->smp[c->ck_smp].addr + s->smp[c->ck_smp].env.off[i];
}
static uint16_t ck_len(const pt4_song *s, const pt4_chan *c, int i)
{
    if (i == c->ck_first) return c->ck_firstlen;
    if (i == c->ck_last) return c->ck_lastlen;
    return s->smp[c->ck_smp].env.len[i];
}

static void ck_enter(pt4_song *s, int n, pt4_voice *v, int i)
{
    pt4_chan *c = &s->st.ch[n];
    v->ck_idx = i;
    v->pos = ck_addr(s, c, i);
    v->end = v->pos + (uint32_t)ck_len(s, c, i) * 2;
    v->ck_vol = s->smp[c->ck_smp].env.vol[i];
    c->volmul = v->ck_vol;
    s->st.aud[n].vol = scale_irq(c->irqvol, v->ck_vol);
    v->vol = s->st.aud[n].vol > 64 ? 64 : s->st.aud[n].vol;   /* the chunk interrupt writes AUDxVOL at once */
}

/* end of a chunk: next one, loop or silence; over = bytes past the end */
static void ck_next(pt4_song *s, int n, pt4_voice *v, uint32_t over)
{
    pt4_chan *c = &s->st.ch[n];
    int guard = 0;
    for (;;) {
        int i = v->ck_idx;
        if (i >= c->ck_last) {
            if (c->ck_loop < 0 || c->ck_loop > c->ck_last) {
                v->active = 0;
                c->volmul = 0;
                return;
            }
            i = c->ck_loop;
        } else {
            i++;
        }
        ck_enter(s, n, v, i);
        if (v->end - v->pos > over) {
            v->pos += over;
            return;
        }
        over = 0;
        if (++guard > c->ck_last + 2) {   /* only empty chunks left */
            v->active = 0;
            return;
        }
    }
}

static int use_chunks(const pt4_song *s)
{
    return s->path == PT4_PATH_PAULA || s->path == PT4_PATH_WIDE || s->opt_dyn_mixer;
}

void pt4__update_voices(pt4_song *s)
{
    int n, nv = nvoices(s);
    int paula = s->path == PT4_PATH_PAULA;
    for (n = 0; n < nv; n++) {
        pt4_chan *c = &s->st.ch[n];
        pt4_aud *a = &s->st.aud[n];
        pt4_voice *v = &s->st.v[n];
        int trig = 0;
        uint16_t vol, per;
        if (c->dmawait) {
            c->dmawait = 0;
            trig = 1;
        }
        if (a->per == 1) {
            trig = 1;
            a->per = c->period;
        }
        if (trig) {
            if (c->chunked && use_chunks(s) && c->ck_smp) {
                v->active = 1;
                v->back = 0;
                v->frac = 0;
                v->period = 0;
                v->chunkmode = 1;
                ck_enter(s, n, v, c->ck_first);
                if (v->end == v->pos) ck_next(s, n, v, 0);
            } else if (c->start == 0 || c->length <= 2) {
                v->active = 0;
                v->chunkmode = 0;
            } else {
                v->pos = c->start;
                v->frac = 0;
                v->end = c->start + (uint32_t)c->length * 2;
                v->active = 1;
                v->back = 0;
                v->period = 0;
                v->chunkmode = 0;
                a->vol = (uint16_t)(((uint32_t)c->volume * c->volscale + 32) >> 6);
                if (paula) {   /* loop registers written once after the trigger */
                    v->loopstart = c->loopstart;
                    v->looplen = c->replen > 1 ? (uint32_t)c->replen * 2 : 0;
                    if (c->replen == 1) v->looplen = 2;
                }
            }
        }
        if (!paula && !v->chunkmode) {   /* mixer: loop info every tick */
            v->loopstart = c->loopstart;
            v->looplen = c->replen > 1 ? (uint32_t)c->replen * 2 : 0;
        }
        /* volume */
        vol = a->vol;
        if (c->chunked && !paula && !v->chunkmode) vol = c->irqvol;
        if (vol > 64) vol = 64;
        if (!paula) {   /* 84y blend (mixer and card voice logic) */
            uint8_t y = s->st.blend[c->chanbit & 3];
            if (y) {
                if ((c->chanbit & 7) >= 4) vol = (uint16_t)((vol * y) / 15);
                else vol = (uint16_t)((vol * (15 - y)) / 15);
            }
        }
        v->vol = vol;
        /* period */
        per = a->per;
        if (per < 2) per = c->period;
        if (s->path == PT4_PATH_WIDE) {   /* card (pt_AgusRate): no step clamp, no rate below period 5 */
            if (per >= 5 && per != v->period) {
                uint32_t hz = PAULA_CLOCK / per;
                if (hz > 192000) hz = 192000;   /* rate register $3fffffff: at most one sample per 192 kHz clock */
                v->period = per;
                v->step = (uint32_t)(((uint64_t)hz << 16) / s->rate);
            }
            if (trig && v->period < 5) v->active = 0;   /* pt_AgusStart: a voice without a rate stays off */
        } else if (per >= 2 && per != v->period) {
            uint32_t p = per < MIX_MINPER ? MIX_MINPER : per;
            v->period = per;
            v->step = (uint32_t)(((PAULA_CLOCK / p) << 16) / s->rate);
        }
    }
}

static int asr3(int x) { return x >= 0 ? (x >> 3) : -((-x + 7) >> 3); }

/* T[v][s] = (s*v) >> 7, arithmetic */
static int tvol(int smp, int vol)
{
    int x = smp * vol;
    return x >= 0 ? (x >> 7) : -((-x + 127) >> 7);
}

static int read_smp(const pt4_song *s, const pt4_voice *v)
{
    int a = (int8_t)PT4_RD8(s, v->pos);
    if (s->opt_interp) {
        int b = (int8_t)PT4_RD8(s, v->back ? v->pos - 1 : v->pos + 1);
        a += (int)(((long)(b - a) * (long)v->frac) >> 16);
    }
    return a;
}

/* advance one output sample, then wrap (pt_MixVoiceWrap) */
static void advance(pt4_song *s, int n, pt4_voice *v)
{
    uint32_t f;
    if (!v->back) {
        f = v->frac + (v->step & 0xffff);
        v->pos += (v->step >> 16) + (f >> 16);
        v->frac = f & 0xffff;
        if (v->pos < v->end) return;
        if (v->chunkmode) {
            ck_next(s, n, v, v->pos - v->end);
            return;
        }
        if (v->looplen == 0) { v->active = 0; return; }
        {
            uint32_t over = v->pos - v->end;
            if (over >= v->looplen) over = 0;
            v->pos = v->loopstart + over;
            v->end = v->loopstart + v->looplen;
        }
    } else {
        uint32_t sub = v->step & 0xffff;
        uint32_t ip = v->step >> 16;
        if (v->frac < sub) {
            v->frac = v->frac + 0x10000 - sub;
            ip++;
        } else {
            v->frac -= sub;
        }
        if (ip > v->pos) {   /* below address 0: treat as far below the loop start */
            v->pos = 0;
            v->frac = 0;
        } else {
            v->pos -= ip;
        }
        if (v->pos >= v->loopstart) return;
        if (v->looplen == 0) { v->active = 0; return; }
        {
            uint32_t under = v->loopstart - v->pos;
            if (under >= v->looplen) under = 0;
            v->end = v->loopstart + v->looplen;
            v->pos = v->end - under - 1;
        }
    }
}

/* mix n frames at lr[2*off..]: int32 stereo in 16 bit scale; pairs (mixer path) optional */
void pt4__mix(pt4_song *s, int32_t *lr, int8_t **pairs, uint32_t off, uint32_t n)
{
    uint32_t i;
    int k, nv = nvoices(s);
    int sw = s->opt_stereo;
    if (s->path == PT4_PATH_MIXER) {
        for (i = 0; i < n; i++) {
            int r[4], L, R, d;
            for (k = 0; k < 4; k++) {
                pt4_voice *a = &s->st.v[k], *b = &s->st.v[k + 4];
                int ta = 0, tb = 0;
                if (a->active) {
                    ta = s->mute[k] ? 0 : tvol(read_smp(s, a), a->vol);
                    advance(s, k, a);
                }
                if (b->active) {
                    tb = s->mute[k + 4] ? 0 : tvol(read_smp(s, b), b->vol);
                    advance(s, k + 4, b);
                }
                r[k] = (int8_t)(uint8_t)(ta + tb);
                if (pairs) pairs[k][off + i] = (int8_t)r[k];
            }
            if (lr) {
                L = r[0] + r[3];
                R = r[1] + r[2];
                d = L - R;
                L -= asr3(d * sw);
                R += asr3(d * sw);
                lr[2 * (off + i)] = L * 128;
                lr[2 * (off + i) + 1] = R * 128;
            }
        }
        return;
    }
    for (i = 0; i < n; i++) {
        int32_t L = 0, R = 0, d;
        for (k = 0; k < nv; k++) {
            pt4_voice *v = &s->st.v[k];
            int x;
            if (!v->active) continue;
            x = s->mute[k] ? 0 : read_smp(s, v) * (int)v->vol;
            advance(s, k, v);
            if ((k & 3) == 0 || (k & 3) == 3) L += x;
            else R += x;
        }
        if (nv > 4) {
            L = L * 4 / nv;
            R = R * 4 / nv;
        }
        d = L - R;
        L -= asr3(d * sw);
        R += asr3(d * sw);
        if (lr) {
            lr[2 * (off + i)] = L;
            lr[2 * (off + i) + 1] = R;
        }
        if (pairs) for (k = 0; k < 4; k++) pairs[k][off + i] = 0;
    }
}
