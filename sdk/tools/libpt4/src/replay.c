/* libpt4 replayer: one call = one tick of pt_RpMusic (engine/replayer.asm, engine/efx8.asm,
   engine/smpx41.asm, engine/songctl.asm:pt_BuildChunkList). Names follow the asm labels.
   zlib license, see LICENSE. */
#include <string.h>
#include "pt4_internal.h"

static const uint8_t vib_table[32] = {
    0x00, 0x18, 0x31, 0x4a, 0x61, 0x78, 0x8d, 0xa1, 0xb4, 0xc5, 0xd4, 0xe0, 0xeb, 0xf4, 0xfa, 0xfd,
    0xff, 0xfd, 0xfa, 0xf4, 0xeb, 0xe0, 0xd4, 0xc5, 0xb4, 0xa1, 0x8d, 0x78, 0x61, 0x4a, 0x31, 0x18
};
static const uint8_t funk_table[16] = { 0, 5, 6, 7, 8, 10, 11, 13, 16, 19, 22, 26, 32, 43, 64, 128 };

#define NOTE_MIN  113
#define NOTE_MAX  856
#define NOTE_MINX 57
#define NOTE_MAXX 1712

/* table word at row base (finetune & 15) + index, reading on past the row like the asm */
static uint16_t per_at(int ft, int idx)
{
    int i = (ft & 15) * 37 + idx;
    if (i < 0 || i >= 616) return 0;
    return pt4_periods[i];
}

static uint16_t cmdword(const pt4_chan *c) { return (uint16_t)((c->cmd << 8) | c->cmdlo); }

/* pt_NoteExt41: notes C-0..B-4 with setup NOTES EXT or in a wide module */
static int notes_ext(const pt4_song *s) { return s->opt_notes_ext || s->wide; }

/* mix_Active: mixer or card voices (not Paula DMA) */
static int mixed(const pt4_song *s) { return s->path != PT4_PATH_PAULA; }

/* pt_PerRaw41: ext range keeps a period outside C-1..B-3 instead of the table value p */
static uint16_t per_raw(const pt4_song *s, const pt4_chan *c, uint16_t p)
{
    uint16_t q = c->period & 0x0fff;
    if (notes_ext(s) && (q < NOTE_MIN || q > NOTE_MAX)) return q;
    return p;
}

uint32_t pt4__cia_period(unsigned bpm)
{
    if (bpm == 0) bpm = 125;
    if (bpm < 32) bpm = 32;
    return (uint32_t)(CIA_BASE_PAL / bpm);
}

/* ---- volume ---- */
static void set_volume(pt4_song *s, int n)   /* pt_RpSetVolume */
{
    pt4_chan *c = &s->st.ch[n];
    pt4_aud *a = &s->st.aud[n];
    uint16_t v = (uint16_t)(((uint32_t)c->volume * c->volscale + 32) >> 6);
    if (!c->chunked) {
        a->vol = v;
        return;
    }
    c->irqvol = v;
    if (c->volmul == 0) {
        a->vol = 0;
    } else {
        uint16_t w = (uint16_t)(((uint32_t)v * c->volmul + 32) & 0xffff) >> 6;
        a->vol = w ? w : 1;
    }
}

/* ---- chunk list (pt_BuildChunkList + pt_RpStartChunked seek/cut) ---- */
static uint32_t chunk_addr(const pt4_song *s, int smp, int i)
{
    const pt4_sample *sm = &s->smp[smp];
    uint32_t a = sm->addr;
    int k;
    for (k = 0; k < i; k++) a += (uint32_t)sm->env.len[k] * 2;
    return a;
}

static void build_chunks(pt4_song *s, pt4_chan *c)
{
    int smp = c->samplenum;
    const pt4_sample *sm = &s->smp[smp];
    c->ck_smp = 0;
    c->ck_first = 0;
    c->ck_last = -1;
    c->ck_loop = -1;
    if (smp == 0 || smp > PT4_MAXSMP || !sm->chunked || sm->env.nseg <= 0) return;
    c->ck_smp = smp;
    c->ck_last = sm->env.nseg - 1;
    c->ck_firstaddr = sm->addr;
    c->ck_firstlen = sm->env.len[0];
    c->ck_lastlen = sm->env.len[c->ck_last];
    /* loopseg k: back to chunk k; k >= nseg would hang/garble PT -> no loop */
    if (sm->env.loopseg > 0 && sm->env.loopseg < sm->env.nseg) c->ck_loop = sm->env.loopseg;
}

static void start_dma(pt4_song *s, int n);
static void start_chunked(pt4_song *s, int n)
{
    pt4_chan *c = &s->st.ch[n];
    pt4_aud *a = &s->st.aud[n];
    build_chunks(s, c);
    c->irqvol = (uint16_t)(((uint32_t)c->volume * (uint32_t)s->opt_master + 32) >> 6);
    c->dmawait = 0;
    c->chunked = 1;
    if (c->sampleoffset && c->ck_smp) {
        /* seek the chunk holding n_start, shorten it, cut the list at n_length */
        int i = 0, last = c->ck_last;
        uint32_t st = c->start, sum = 0;
        while (i <= last && chunk_addr(s, c->ck_smp, i) <= st) i++;
        if (i > 0) i--;
        {
            uint32_t ca = chunk_addr(s, c->ck_smp, i);
            uint16_t l = s->smp[c->ck_smp].env.len[i];
            l = (uint16_t)(l - (uint16_t)((st - ca) >> 1));
            c->ck_first = i;
            c->ck_firstaddr = st;
            c->ck_firstlen = l;
        }
        for (; i <= last; i++) {
            uint16_t l = (i == c->ck_first) ? c->ck_firstlen : s->smp[c->ck_smp].env.len[i];
            sum = (uint16_t)(sum + l);
            if (sum >= c->length) {
                uint16_t cut = (uint16_t)(l - (uint16_t)(sum - c->length));
                c->ck_last = i;
                c->ck_lastlen = cut;
                if (i == c->ck_first) c->ck_firstlen = cut;
                c->ck_loop = -1;
                break;
            }
        }
    }
    a->per = 1;
    a->vol = 0;
    if (c->ck_smp) {
        a->lc = c->ck_firstaddr;
        a->len = c->ck_firstlen;
    }
}

static void start_dma(pt4_song *s, int n)   /* pt_RpStartDma */
{
    pt4_chan *c = &s->st.ch[n];
    pt4_aud *a = &s->st.aud[n];
    c->dmawait = 1;
    if (c->length <= 2 || c->start == 0) {   /* pt_RpUseSilence */
        c->start = ADDR_SCRATCH;
        c->loopstart = ADDR_SCRATCH;
        c->replen = 16;
        c->length = 16;
    }
    a->per = 1;
    a->lc = c->start;
    a->len = c->length;
}

static void do_retrig(pt4_song *s, int n)   /* pt_RpDoRetrig */
{
    pt4_chan *c = &s->st.ch[n];
    pt4_aud *a = &s->st.aud[n];
    if (c->chunked) { start_chunked(s, n); return; }
    c->dmawait = 1;
    a->per = 1;
    a->lc = c->start;
    a->len = c->length;
}

/* ---- effects ---- */
static void sample_offset(pt4_song *s, int n)
{
    pt4_chan *c = &s->st.ch[n];
    int32_t o;
    if (c->cmdlo) c->sampleoffset = c->cmdlo;
    o = (int32_t)c->sampleoffset << 7;
    if (mixed(s) ? o >= (int32_t)c->length : o >= (int32_t)(int16_t)c->length) {   /* pt_OfsCmp41: mixed BCC, Paula BGE */
        c->length = 1;
        return;
    }
    c->length = (uint16_t)(c->samplelen - (uint16_t)o);
    c->start = c->samplestart + (uint32_t)o * 2;
}

static void vol_slide_up(pt4_chan *c, int x)
{
    c->volume = (uint8_t)(c->volume + x);
    if ((int8_t)(uint8_t)(c->volume - 0x40) >= 0) c->volume = 0x40;
}
static void vol_slide_down(pt4_chan *c, int x)
{
    c->volume = (uint8_t)(c->volume - x);
    if ((int8_t)c->volume < 0) c->volume = 0;
}
static void volume_slide(pt4_chan *c)
{
    int x = c->cmdlo >> 4;
    if (x) vol_slide_up(c, x);
    else vol_slide_down(c, c->cmdlo & 15);
}

static void position_jump(pt4_song *s, int n)
{
    s->st.songpos = (uint8_t)(s->st.ch[n].cmdlo - 1);
    s->st.breakrow = 0;
    s->st.posjumpflag = 1;
}

static void pattern_break(pt4_song *s, int n)
{
    int lo = s->st.ch[n].cmdlo;
    int r = (lo >> 4) * 10;
    r = (r & 0xff00) | ((r + (lo & 15)) & 0xff);
    if (!s->mode4) {
        r = (r << 1) & 0xffff;
        if (r > 0x7e) { s->st.breakrow = 0; s->st.posjumpflag = 1; return; }
    } else if ((r & 0xff) > 0x3f) {
        s->st.breakrow = 0;
        s->st.posjumpflag = 1;
        return;
    }
    s->st.breakrow = (uint8_t)r;
    s->st.posjumpflag = 1;
}

static void set_speed(pt4_song *s, int n)
{
    int x = s->st.ch[n].cmdlo;
    if (x == 0) {
        s->st.playing = 0;
        s->st.ended = 1;
        return;
    }
    if (x >= 32) {
        s->st.tempo = (uint16_t)x;
        return;
    }
    s->st.counter = 0;
    s->st.speed = (uint32_t)x;
}

static void volume_change(pt4_chan *c)
{
    c->volume = c->cmdlo > 64 ? 64 : c->cmdlo;
}

static void update_funk(pt4_song *s, int n)   /* pt_RpUpdateFunk */
{
    pt4_chan *c = &s->st.ch[n];
    int sp = c->glissfunk >> 4;
    uint32_t a, lim;
    if (!sp) return;
    c->funkoffset = (uint8_t)(c->funkoffset + funk_table[sp]);
    if (!(c->funkoffset & 0x80)) return;
    c->funkoffset = 0;
    lim = c->loopstart + (uint32_t)c->replen * 2;
    a = c->wavestart + 1;
    if (!(a < lim)) a = c->loopstart;
    c->wavestart = a;
    if (a >= ADDR_SCRATCH && a < s->arena_size) s->arena[a] = (uint8_t)~s->arena[a];
}

static void ecommands(pt4_song *s, int n);

static void porta_up(pt4_song *s, int n)
{
    pt4_chan *c = &s->st.ch[n];
    int x = c->cmdlo & s->st.lowmask;
    uint16_t p;
    s->st.lowmask = 0xff;
    c->period = (uint16_t)(c->period - x);
    p = c->period & 0x0fff;
    if (p < (notes_ext(s) ? NOTE_MINX : NOTE_MIN))
        c->period = (uint16_t)((c->period & 0xf000) | (notes_ext(s) ? NOTE_MINX : NOTE_MIN));
    s->st.aud[n].per = c->period & 0x0fff;
}

static void porta_down(pt4_song *s, int n)
{
    pt4_chan *c = &s->st.ch[n];
    int x = c->cmdlo & s->st.lowmask;
    uint16_t p;
    s->st.lowmask = 0xff;
    c->period = (uint16_t)(c->period + x);
    p = c->period & 0x0fff;
    if (!(p < (notes_ext(s) ? NOTE_MAXX : NOTE_MAX)))
        c->period = (uint16_t)((c->period & 0xf000) | (notes_ext(s) ? NOTE_MAXX : NOTE_MAX));
    s->st.aud[n].per = c->period & 0x0fff;
}

static void set_tone_porta(pt4_song *s, int n)
{
    pt4_chan *c = &s->st.ch[n];
    uint16_t want = c->note & 0x0fff;
    int i = 0;
    int16_t tgt;
    while (i < 37 && !(want >= per_at(c->finetune, i))) i++;
    if (i >= 37) i = 35;
    if ((c->finetune & 8) && i) i--;
    tgt = (int16_t)per_at(c->finetune, i);
    if (notes_ext(s) && want && (want < NOTE_MIN || want > NOTE_MAX)) tgt = (int16_t)want;   /* pt_TpTarget41 */
    c->wantedperiod = (uint16_t)tgt;
    c->toneportdirec = 0;
    if (tgt == (int16_t)c->period) c->wantedperiod = 0;
    else if (tgt < (int16_t)c->period) c->toneportdirec = 1;
}

static void tone_port_nochange(pt4_song *s, int n)
{
    pt4_chan *c = &s->st.ch[n];
    uint16_t p;
    if (!c->wantedperiod) return;
    if (!c->toneportdirec) {
        c->period = (uint16_t)(c->period + c->toneportspeed);
        if (!((int16_t)c->wantedperiod > (int16_t)c->period)) { c->period = c->wantedperiod; c->wantedperiod = 0; }
    } else {
        c->period = (uint16_t)(c->period - c->toneportspeed);
        if (!((int16_t)c->wantedperiod < (int16_t)c->period)) { c->period = c->wantedperiod; c->wantedperiod = 0; }
    }
    p = c->period;
    if (c->glissfunk & 0x0f) {
        int i = 0;
        while (i < 37 && !(p >= per_at(c->finetune, i))) i++;
        if (i >= 37) i = 35;
        p = per_at(c->finetune, i);
    }
    s->st.aud[n].per = per_raw(s, c, p);
}

static void tone_portamento(pt4_song *s, int n)
{
    pt4_chan *c = &s->st.ch[n];
    if (c->cmdlo) {
        c->toneportspeed = c->cmdlo;
        c->cmdlo = 0;
    }
    tone_port_nochange(s, n);
}

static void vibrato2(pt4_song *s, int n)
{
    pt4_chan *c = &s->st.ch[n];
    int idx = (c->vibratopos >> 2) & 0x1f, w = c->wavecontrol & 3;
    unsigned d;
    uint16_t p;
    if (w == 0) d = vib_table[idx];
    else {
        unsigned v = (unsigned)(idx << 3) & 0xff;
        if (w == 1) d = ((int8_t)c->vibratopos >= 0) ? v : (0xff - v) & 0xff;
        else d = 0xff;
    }
    d = (d * (c->vibratocmd & 15)) >> 7;
    p = c->period;
    if ((int8_t)c->vibratopos < 0) p = (uint16_t)(p - d);
    else p = (uint16_t)(p + d);
    s->st.aud[n].per = p;
    s->st.dbg_per[n] = p;
    c->vibratopos = (uint8_t)(c->vibratopos + ((c->vibratocmd >> 2) & 0x3c));
}

static void vibrato(pt4_song *s, int n)
{
    pt4_chan *c = &s->st.ch[n];
    if (c->cmdlo) {
        uint8_t v = c->vibratocmd;
        if (c->cmdlo & 0x0f) v = (uint8_t)((v & 0xf0) | (c->cmdlo & 0x0f));
        if (c->cmdlo & 0xf0) v = (uint8_t)((v & 0x0f) | (c->cmdlo & 0xf0));
        c->vibratocmd = v;
    }
    vibrato2(s, n);
}

/* returns 1: the volume write after the effect is skipped (7xy) */
static int tremolo(pt4_song *s, int n)
{
    pt4_chan *c = &s->st.ch[n];
    int idx, w;
    unsigned d;
    int v;
    if (c->cmdlo) {
        uint8_t t = c->tremolocmd;
        if (c->cmdlo & 0x0f) t = (uint8_t)((t & 0xf0) | (c->cmdlo & 0x0f));
        if (c->cmdlo & 0xf0) t = (uint8_t)((t & 0x0f) | (c->cmdlo & 0xf0));
        c->tremolocmd = t;
    }
    idx = (c->tremolopos >> 2) & 0x1f;
    w = (c->wavecontrol >> 4) & 3;
    if (w == 0) d = vib_table[idx];
    else {
        unsigned x = (unsigned)(idx << 3) & 0xff;
        if (w == 1) d = ((int8_t)c->vibratopos >= 0) ? x : (0xff - x) & 0xff;   /* 2.3D: tests the vibrato position */
        else d = 0xff;
    }
    d = (d * (c->tremolocmd & 15)) >> 6;
    v = c->volume;
    if ((int8_t)c->tremolopos < 0) v -= (int)d;
    else v += (int)d;
    if (v < 0) v = 0;
    if (v > 64) v = 64;
    s->st.aud[n].vol = (uint16_t)(((uint32_t)v * (uint32_t)s->opt_master + 32) >> 6);
    s->st.dbg_vol[n] = s->st.aud[n].vol;
    c->tremolopos = (uint8_t)(c->tremolopos + ((c->tremolocmd >> 2) & 0x3c));
    return 1;
}

static void arpeggio(pt4_song *s, int n)
{
    pt4_chan *c = &s->st.ch[n];
    int r = (int)(s->st.counter % 3), x, i;
    uint16_t p;
    if (r == 0) {
        p = per_raw(s, c, c->period);
        s->st.aud[n].per = p;
        s->st.dbg_per[n] = p;
        return;
    }
    x = r == 1 ? (c->cmdlo >> 4) : (c->cmdlo & 15);
    for (i = 0; i < 37; i++) {
        p = per_at(c->finetune, i + x);
        if (c->period >= per_at(c->finetune, i)) {
            p = per_raw(s, c, p);
            s->st.aud[n].per = p;
            s->st.dbg_per[n] = p;
            return;
        }
    }
}

/* ---- 8xy ---- */
static uint8_t *efx8_ptr(pt4_song *s, int n) { return s->st.efx8[s->st.ch[n].chanbit & 7]; }

static void efx8_backwards(pt4_song *s, int n, int y)
{
    pt4_voice *v;
    if (s->path == PT4_PATH_PAULA) return;   /* mix_Active off: no effect on the Paula path */
    v = &s->st.v[s->wide ? n : (s->st.ch[n].chanbit & 7)];
    if (!v->active) return;
    if (y) {
        v->back = 1;
        if (v->looplen == 0) v->loopstart = s->st.ch[n].start;
    } else {
        v->back = 0;
    }
}

static void efx8_set(pt4_song *s, int n)   /* pt_Efx8Set, y > 0 */
{
    pt4_chan *c = &s->st.ch[n];
    uint8_t *e = efx8_ptr(s, n);
    int x = c->cmdlo >> 4, y = c->cmdlo & 15;
    if (y == 15) y = 0;
    switch (x) {
    case 1: efx8_backwards(s, n, y); break;
    case 2: e[0] = (uint8_t)y; break;
    case 3: e[0] = (uint8_t)(-y); break;
    case 4: s->st.blend[c->chanbit & 3] = (uint8_t)y; break;
    case 6: e[3] = (uint8_t)y; break;
    case 7:
        if (e[1] != y) { e[1] = (uint8_t)y; e[2] = 0; }
        break;
    default: break;
    }
}

static void efx8_tick(pt4_song *s, int n)   /* pt_Efx8Tick */
{
    pt4_chan *c = &s->st.ch[n];
    uint8_t *e = efx8_ptr(s, n);
    if (e[0]) {
        int v = (int)c->volume + (int8_t)e[0];
        if (v < 0) v = 0;
        if (v > 64) v = 64;
        c->volume = (uint8_t)v;
    }
    if (e[3] && (s->st.counter % e[3]) == 0) {
        int v = (int)c->volume - 4;
        c->volume = (uint8_t)(v < 0 ? 0 : v);
        do_retrig(s, n);
    }
    if (e[1]) {
        e[2]++;
        if (!((uint8_t)(e[1] * 2) > e[2])) e[2] = 0;
    }
}

static void efx8_set_volume(pt4_song *s, int n)   /* pt_Efx8SetVolume */
{
    uint8_t *e;
    set_volume(s, n);
    e = efx8_ptr(s, n);
    if (e[1] && !(e[1] > e[2]) && !s->st.ch[n].chunked) s->st.aud[n].vol = 0;
}

/* ---- E commands ---- */
static void retrig_note(pt4_song *s, int n)
{
    pt4_chan *c = &s->st.ch[n];
    int x = c->cmdlo & 15;
    if (!x) return;
    if (s->st.counter == 0 && (c->note & 0x0fff)) return;
    if ((s->st.counter & 0xffff) % (unsigned)x) return;
    do_retrig(s, n);
}

static void ecommands(pt4_song *s, int n)
{
    pt4_chan *c = &s->st.ch[n];
    int x = c->cmdlo >> 4, y = c->cmdlo & 15;
    switch (x) {
    case 0: break;   /* E0x: LED filter, not modelled */
    case 1:
        if (s->st.counter) return;
        s->st.lowmask = 0x0f;
        porta_up(s, n);
        break;
    case 2:
        if (s->st.counter) return;
        s->st.lowmask = 0x0f;
        porta_down(s, n);
        break;
    case 3: c->glissfunk = (uint8_t)((c->glissfunk & 0xf0) | y); break;
    case 4: c->wavecontrol = (uint8_t)((c->wavecontrol & 0xf0) | y); break;
    case 5: c->finetune = (uint8_t)y; break;
    case 6:
        if (s->st.counter) return;
        if (!y) {
            if (!s->mode4) c->pattpos = (uint8_t)((s->st.patternpos >> 4) & 0x7e);
            else c->pattpos = (uint8_t)((s->st.patternpos >> 4) & 0x3f);
            return;
        }
        if (c->loopcount) {
            c->loopcount--;
            if (!c->loopcount) return;
        } else {
            c->loopcount = (uint8_t)y;
        }
        s->st.breakrow = c->pattpos;
        s->st.pbreakflag = 1;
        break;
    case 7: c->wavecontrol = (uint8_t)((c->wavecontrol & 0x0f) | (y << 4)); break;
    case 9: retrig_note(s, n); break;
    case 10:
        if (s->st.counter) return;
        vol_slide_up(c, y);
        break;
    case 11:
        if (s->st.counter) return;
        vol_slide_down(c, y);
        break;
    case 12:
        if ((uint32_t)y == s->st.counter) c->volume = 0;
        break;
    case 13:
        if ((uint32_t)y == s->st.counter && (c->note & 0x0fff)) do_retrig(s, n);
        break;
    case 14:
        if (s->st.counter) return;
        if (s->st.pattdelaycount) return;
        s->st.pattdelaytime = (uint8_t)(y + 1);
        break;
    case 15:
        if (s->st.counter) return;
        c->glissfunk = (uint8_t)((c->glissfunk & 0x0f) | (y << 4));
        if (y) update_funk(s, n);
        break;
    default: break;
    }
}

/* tick 0 effects: pt_RpCheckMoreEfx */
static void check_more_efx(pt4_song *s, int n)
{
    pt4_chan *c = &s->st.ch[n];
    switch (c->cmd & 15) {
    case 9: sample_offset(s, n); break;
    case 0xb: position_jump(s, n); break;
    case 0xd: pattern_break(s, n); break;
    case 0xe: ecommands(s, n); break;
    case 0xf: set_speed(s, n); break;
    case 0xc: volume_change(c); break;
    default: s->st.aud[n].per = c->period; break;
    }
}

static void check_more_efx2(pt4_song *s, int n)
{
    switch (s->st.ch[n].cmd & 15) {
    case 0xb: position_jump(s, n); break;
    case 0xd: pattern_break(s, n); break;
    case 0xe: ecommands(s, n); break;
    case 0xf: set_speed(s, n); break;
    default: break;
    }
}

/* per tick effects: pt_RpCheckEfxChan; returns nothing, writes the volume */
static void check_efx_chan(pt4_song *s, int n)
{
    pt4_chan *c = &s->st.ch[n];
    int skipvol = 0;
    update_funk(s, n);
    if (!s->wide) efx8_tick(s, n);   /* wide: 8xx is panning, no sub-effects */
    if (cmdword(c) & 0x0fff) {
        switch (c->cmd & 15) {
        case 0: arpeggio(s, n); break;
        case 1: porta_up(s, n); break;
        case 2: porta_down(s, n); break;
        case 3: tone_portamento(s, n); break;
        case 4: vibrato(s, n); break;
        case 5: tone_port_nochange(s, n); volume_slide(c); break;
        case 6: vibrato2(s, n); volume_slide(c); break;
        case 0xe: ecommands(s, n); break;
        default:
            s->st.aud[n].per = c->period;
            switch (c->cmd & 15) {
            case 7: skipvol = tremolo(s, n); break;
            case 0xa: volume_slide(c); break;
            case 8:
                if ((c->cmdlo & 15) && !s->wide) efx8_set(s, n);
                break;   /* 8x0: background colour; wide: panning, ignored */
            default: break;
            }
            break;
        }
    }
    if (!skipvol) efx8_set_volume(s, n);
}

/* ---- notes ---- */
static void smx_note(pt4_song *s, int n, int smp)   /* pt_SmxNote + pt_RpReadNote sample part */
{
    pt4_chan *c = &s->st.ch[n];
    const pt4_sample *sm;
    if (smp > PT4_MAXSMP) smp = PT4_MAXSMP;   /* pt_SmxPhys */
    sm = &s->smp[smp];
    c->chunked = (uint8_t)(sm->chunked ? 1 : 0);
    c->samplenum = (uint8_t)smp;
    c->samplestart = sm->addr;
    c->start = sm->addr;
    c->samplelen = sm->length;
    c->length = sm->length;
    c->reallength = sm->length;
    c->finetune = sm->finetune;
    c->volume = sm->volume;
    if (sm->repeat) {
        /* pt_LoopSet41: mixed adds the offset as a long, Paula keeps ASL.W (wraps above 64 KB) */
        c->loopstart = c->start + (mixed(s) ? (uint32_t)sm->repeat * 2 : (uint32_t)((uint16_t)(sm->repeat << 1)));
        c->wavestart = c->loopstart;
        c->length = (uint16_t)(sm->repeat + sm->replen);
        c->replen = sm->replen;
    } else {
        c->loopstart = c->start;
        c->wavestart = c->start;
        c->replen = sm->replen;
    }
}

static void set_period(pt4_song *s, int n)   /* pt_RpSetPeriod and on */
{
    pt4_chan *c = &s->st.ch[n];
    uint16_t p = c->note & 0x0fff;
    int i = 0;
    while (i < 37 && !(p >= pt4_periods[i])) i++;
    c->period = per_at(c->finetune, i);
    if (notes_ext(s) && (p < NOTE_MIN || p > NOTE_MAX)) c->period = p;   /* pt_PerLookup41 */
    if ((cmdword(c) & 0x0ff0) == 0x0ed0) {
        check_more_efx(s, n);
        return;
    }
    if (!(c->wavecontrol & 4)) c->vibratopos = 0;
    if (!(c->wavecontrol & 0x40)) c->tremolopos = 0;
    if (c->chunked) start_chunked(s, n);
    else start_dma(s, n);
    check_more_efx2(s, n);
}

static void play_voice(pt4_song *s, int n, const uint8_t *cell)   /* pt_RpPlayVoice */
{
    pt4_chan *c = &s->st.ch[n];
    int smp;
    if (!(c->note | c->cmd | c->cmdlo)) s->st.aud[n].per = c->period;   /* TST.L (A6): the previous cell */
    c->note = (uint16_t)((cell[0] << 8) | cell[1]);
    c->cmd = cell[2];
    c->cmdlo = cell[3];
    smp = (cell[0] & 0xf0) | (cell[2] >> 4);
    if (smp) smx_note(s, n, smp);
    /* pt_RpSetRegs */
    if (!(c->note & 0x0fff)) {
        check_more_efx(s, n);
        return;
    }
    if ((cmdword(c) & 0x0ff0) == 0x0e50) {
        c->finetune = c->cmdlo & 15;
        set_period(s, n);
        return;
    }
    switch (c->cmd & 15) {
    case 3: case 5:
        set_tone_porta(s, n);
        check_more_efx(s, n);
        return;
    case 0xc:
        volume_change(c);
        break;
    case 9:
        check_more_efx(s, n);
        break;
    default: break;
    }
    set_period(s, n);
}

/* ---- row / position ---- */
static int nchan(const pt4_song *s) { return s->wide ? PT4_MAXCH : s->lay; }

static void no_new_all(pt4_song *s)
{
    int n, k = nchan(s);
    for (n = 0; n < k; n++) check_efx_chan(s, n);
}

static void next_position(pt4_song *s)
{
    pt4_state *st = &s->st;
    st->patternpos = (uint32_t)st->breakrow << 4;
    st->breakrow = 0;
    st->posjumpflag = 0;
    st->songpos = (st->songpos + 1) & 0x7f;
    if (st->songpos >= (uint32_t)s->songlen) st->songpos = 0;
    if (st->visited[st->songpos]) st->ended = 1;
    st->visited[st->songpos] = 1;
}

static void next_row(pt4_song *s)
{
    pt4_state *st = &s->st;
    uint32_t rowb = s->mode4 ? 16 : 32;
    st->patternpos += rowb;
    if (st->pattdelaytime) {
        st->pattdelaycount = st->pattdelaytime;
        st->pattdelaytime = 0;
    }
    if (st->pattdelaycount) {
        st->pattdelaycount--;
        if (st->pattdelaycount) st->patternpos -= rowb;
    }
    if (st->pbreakflag) {
        st->pbreakflag = 0;
        st->patternpos = (uint32_t)st->breakrow << 4;
        st->breakrow = 0;
    }
    if (st->patternpos == (s->mode4 ? 1024u : 2048u)) next_position(s);
}

static void get_new_note(pt4_song *s)
{
    pt4_state *st = &s->st;
    int pat = s->positions[st->songpos & 0x7f], n, k = nchan(s);
    uint32_t row = st->patternpos / (s->mode4 ? 16u : 32u);
    const uint8_t *rowp = NULL;
    static const uint8_t zero[4] = { 0, 0, 0, 0 };
    st->cur_order = (int)st->songpos;
    st->cur_row = (int)row;
    if (pat < s->npat && row < 64) rowp = s->pat + ((size_t)pat * 64 + row) * (size_t)s->lay * 4;
    for (n = 0; n < k; n++) {
        play_voice(s, n, rowp && n < s->lay ? rowp + n * 4 : zero);
        set_volume(s, n);
    }
}

void pt4__tick(pt4_song *s)
{
    pt4_state *st = &s->st;
    int n, k = nchan(s);
    if (!st->playing) return;
    memset(st->dbg_per, 0xff, sizeof(st->dbg_per));
    memset(st->dbg_vol, 0xff, sizeof(st->dbg_vol));
    for (n = 0; n < k; n++) st->ch[n].volscale = (uint16_t)s->opt_master;
    st->counter++;
    if (st->counter >= st->speed) {
        st->counter = 0;
        if (st->pattdelaycount) no_new_all(s);
        else get_new_note(s);
        next_row(s);
    } else {
        no_new_all(s);
    }
    while (st->posjumpflag) next_position(s);
}

void pt4__reset(pt4_song *s)
{
    pt4_state *st = &s->st;
    int n;
    memset(st, 0, sizeof(*st));
    st->playing = 1;
    st->speed = 6;
    st->tempo = (uint16_t)s->opt_tempo;
    st->lowmask = 0xff;
    st->counter = s->opt_row0 ? 5 : 0;
    st->visited[0] = 1;
    for (n = 0; n < PT4_MAXCH; n++) {
        st->ch[n].chanbit = (uint16_t)(s->wide ? (n & 3) : n);
        st->ch[n].volscale = (uint16_t)s->opt_master;
        st->ch[n].ck_last = -1;
        st->ch[n].ck_loop = -1;
    }
    if (s->pristine && s->arena) memcpy(s->arena, s->pristine, s->arena_size);
}
