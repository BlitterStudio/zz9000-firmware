/* libpt4 public API. zlib license, see LICENSE. */
#include <string.h>
#include "pt4_internal.h"

#define LEN_LIMIT_ECLK ((uint64_t)ECLOCK_PAL * 7200u)   /* 2 hours */
#define BLOCK 128

static void resolve_path(pt4_song *s)
{
    if (s->opt_path != PT4_PATH_AUTO) s->path = s->opt_path;
    else if (s->wide) s->path = PT4_PATH_WIDE;
    else if (s->lay == 4) s->path = PT4_PATH_PAULA;
    else s->path = PT4_PATH_MIXER;
}

static void restart(pt4_song *s)
{
    resolve_path(s);
    pt4__reset(s);
    s->st.frac = (uint32_t)s->opt_frac0;
}

/* one tick: replayer, timing; returns samples of this tick at s->rate */
static uint32_t do_tick(pt4_song *s, int voices)
{
    pt4_state *st = &s->st;
    unsigned old = st->tempo, use;
    uint32_t n;
    pt4__tick(s);
    use = (s->path == PT4_PATH_PAULA || s->opt_latch) ? old : st->tempo;
    if (st->spt_bpm != use || st->spt == 0) pt4__calc_spt(s, use);
    st->frac += st->spt;
    n = st->frac >> 16;
    st->frac &= 0xffff;
    st->ticks_eclk += pt4__cia_period(use);
    if (voices) pt4__update_voices(s);
    return n;
}

static void scan_length(pt4_song *s)
{
    unsigned save_rate = s->rate;
    s->rate = 44100;
    restart(s);
    while (!s->st.ended && s->st.ticks_eclk < LEN_LIMIT_ECLK) {
        uint64_t before = s->st.ticks_eclk;
        do_tick(s, 0);
        if (!s->st.playing) s->st.ticks_eclk = before;   /* F00: that tick is not played */
    }
    s->length_ms = s->st.ended ? (long)(s->st.ticks_eclk * 1000u / ECLOCK_PAL) : -1;
    s->rate = save_rate;
    restart(s);
}

pt4_song *pt4_open(const void *data, size_t len, const pt4_alloc *alloc, int *err)
{
    pt4_song *s;
    pt4_alloc al;
    int e;
    if (err) *err = PT4_OK;
    if (!data || len == 0) {
        if (err) *err = PT4_ERR_ARG;
        return NULL;
    }
    memset(&al, 0, sizeof(al));
    if (alloc && alloc->alloc && alloc->free) al = *alloc;
    s = (pt4_song *)(al.alloc ? al.alloc(sizeof(pt4_song), al.user) : pt4__malloc(sizeof(pt4_song)));
    if (!s) {
        if (err) *err = PT4_ERR_MEMORY;
        return NULL;
    }
    memset(s, 0, sizeof(*s));
    s->al = al;
    s->opt_master = 64;
    s->opt_tempo = 125;
    s->rate = 44100;
    e = pt4__load(s, (const uint8_t *)data, len);
    if (e == PT4_OK && s->uses_funk) {
        s->pristine = (uint8_t *)pt4__alloc(s, s->arena_size);
        if (!s->pristine) e = PT4_ERR_MEMORY;
        else memcpy(s->pristine, s->arena, s->arena_size);
    }
    if (e != PT4_OK) {
        pt4_close(s);
        if (err) *err = e;
        return NULL;
    }
    scan_length(s);
    return s;
}

void pt4_close(pt4_song *s)
{
    int i;
    if (!s) return;
    for (i = 0; i <= PT4_MAXSMP; i++) pt4__free(s, s->smp[i].env.len);
    pt4__free(s, s->pat);
    pt4__free(s, s->arena);
    pt4__free(s, s->pristine);
    pt4__free(s, s);
}

/* core render into lr (int32 stereo, 16 bit scale) and/or pairs */
size_t pt4__render(pt4_song *s, int32_t *lr, int8_t **pairs, size_t frames, unsigned rate)
{
    pt4_state *st = &s->st;
    size_t done = 0;
    int k;
    if (rate < 8000) rate = 8000;
    if (rate > 192000) rate = 192000;
    if (rate != s->rate) {
        s->rate = rate;
        st->spt = 0;
        for (k = 0; k < PT4_MAXCH; k++) st->v[k].period = 0;
    }
    while (done < frames) {
        uint32_t n;
        if (st->tick_left == 0) {
            if (st->ended) {
                if (!s->loop) break;
                if (!st->playing) {
                    unsigned r = s->rate;
                    restart(s);
                    s->rate = r;
                } else {
                    memset(st->visited, 0, sizeof(st->visited));
                    st->visited[st->songpos & 0x7f] = 1;
                    st->ended = 0;
                }
            }
            st->tick_left = do_tick(s, 1);
            if (!st->playing && !s->loop) {   /* F00 */
                st->tick_left = 0;
                break;
            }
            if (st->tick_left == 0) continue;
        }
        n = st->tick_left;
        if (n > frames - done) n = (uint32_t)(frames - done);
        pt4__mix(s, lr, pairs, (uint32_t)done, n);
        done += n;
        st->tick_left -= n;
    }
    return done;
}

size_t pt4_render_s16(pt4_song *s, int16_t *out, size_t frames, unsigned rate)
{
    int32_t buf[2 * BLOCK];
    size_t done = 0, i;
    if (!s || !out) return 0;
    while (done < frames) {
        size_t want = frames - done, got;
        if (want > BLOCK) want = BLOCK;
        got = pt4__render(s, buf, NULL, want, rate);
        for (i = 0; i < 2 * got; i++) {
            int32_t x = buf[i];
            if (x > 32767) x = 32767;
            if (x < -32768) x = -32768;
            out[2 * done + i] = (int16_t)x;
        }
        done += got;
        if (got < want) break;
    }
    return done;
}

size_t pt4_render_pairs(pt4_song *s, int8_t *pairs[4], size_t frames, unsigned rate)
{
    if (!s || !pairs || !pairs[0] || !pairs[1] || !pairs[2] || !pairs[3]) return 0;
    return pt4__render(s, NULL, pairs, frames, rate);
}

/* seek = silent play to ms * rate / 1000 frames: voices, envelopes and
   the tick remainder advance exactly as when rendering */
int pt4_seek_ms(pt4_song *s, long ms)
{
    uint64_t left;
    unsigned r;
    if (!s || ms < 0) return PT4_ERR_ARG;
    r = s->rate;
    restart(s);
    s->rate = r;
    left = (uint64_t)ms * r / 1000u;
    while (left) {
        size_t want = left > 65536u ? 65536u : (size_t)left;
        size_t got = pt4__render(s, NULL, NULL, want, r);
        left -= got;
        if (got < want) break;
    }
    return PT4_OK;
}

int pt4_set_rate(pt4_song *s, unsigned rate)
{
    int k;
    if (!s || rate < 8000 || rate > 192000) return PT4_ERR_ARG;
    if (rate != s->rate) {
        s->rate = rate;
        s->st.spt = 0;
        for (k = 0; k < PT4_MAXCH; k++) s->st.v[k].period = 0;
    }
    return PT4_OK;
}

long pt4_length_ms(const pt4_song *s) { return s ? s->length_ms : -1; }

void pt4_position(const pt4_song *s, int *order, int *row)
{
    if (order) *order = s ? s->st.cur_order : 0;
    if (row) *row = s ? s->st.cur_row : 0;
}

int pt4_ended(const pt4_song *s) { return s ? s->st.ended : 1; }
void pt4_set_loop(pt4_song *s, int on) { if (s) s->loop = on ? 1 : 0; }

void pt4_mute(pt4_song *s, int chan, int on)
{
    if (s && chan >= 0 && chan < PT4_MAXCH) s->mute[chan] = on ? 1 : 0;
}

int pt4_set_option(pt4_song *s, int opt, int v)
{
    if (!s) return PT4_ERR_ARG;
    switch (opt) {
    case PT4_OPT_PATH:
        if (v < PT4_PATH_AUTO || v > PT4_PATH_WIDE) return PT4_ERR_ARG;
        s->opt_path = v;
        if (s->st.ticks_eclk == 0) restart(s);
        return PT4_OK;
    case PT4_OPT_STEREO: if (v < 0 || v > 4) return PT4_ERR_ARG; s->opt_stereo = v; return PT4_OK;
    case PT4_OPT_INTERP: s->opt_interp = v ? 1 : 0; return PT4_OK;
    case PT4_OPT_DYN_MIXER: s->opt_dyn_mixer = v ? 1 : 0; return PT4_OK;
    case PT4_OPT_NOTES_EXT: s->opt_notes_ext = v ? 1 : 0; return PT4_OK;
    case PT4_OPT_MASTER_VOL: if (v < 0 || v > 64) return PT4_ERR_ARG; s->opt_master = v; return PT4_OK;
    case PT4_OPT_START_ROW0:
        s->opt_row0 = v ? 1 : 0;
        if (s->st.ticks_eclk == 0) restart(s);
        return PT4_OK;
    case PT4_OPT_TEMPO_LATCH: s->opt_latch = v ? 1 : 0; return PT4_OK;
    case 100:   /* test only: start value of the 16 bit samples-per-tick fraction (PT keeps it from before play start) */
        s->opt_frac0 = v & 0xffff;
        if (s->st.ticks_eclk == 0) restart(s);
        return PT4_OK;
    case PT4_OPT_TEMPO:
        if (v < 32 || v > 255) return PT4_ERR_ARG;
        s->opt_tempo = v;
        if (s->st.ticks_eclk == 0) restart(s);
        return PT4_OK;
    default: return PT4_ERR_ARG;
    }
}

int pt4_get_option(const pt4_song *s, int opt)
{
    if (!s) return -1;
    switch (opt) {
    case PT4_OPT_PATH: return s->opt_path == PT4_PATH_AUTO ? s->path : s->opt_path;
    case PT4_OPT_STEREO: return s->opt_stereo;
    case PT4_OPT_INTERP: return s->opt_interp;
    case PT4_OPT_DYN_MIXER: return s->opt_dyn_mixer;
    case PT4_OPT_NOTES_EXT: return s->opt_notes_ext;
    case PT4_OPT_MASTER_VOL: return s->opt_master;
    case PT4_OPT_START_ROW0: return s->opt_row0;
    case PT4_OPT_TEMPO_LATCH: return s->opt_latch;
    case PT4_OPT_TEMPO: return s->opt_tempo;
    default: return -1;
    }
}

int pt4_channels(const pt4_song *s) { return s ? s->nch : 0; }
const char *pt4_title(const pt4_song *s) { return s ? s->title : ""; }
const char *pt4_format_id(const pt4_song *s) { return s ? s->id : ""; }
int pt4_num_samples(const pt4_song *s) { return s ? s->nsmp : 0; }
int pt4_extensions(const pt4_song *s) { return s ? s->ext : 0; }
int pt4_num_orders(const pt4_song *s) { return s ? s->songlen : 0; }
int pt4_num_patterns(const pt4_song *s) { return s ? s->npat : 0; }

const char *pt4_sample_name(const pt4_song *s, int i)
{
    if (!s || i < 1 || i > PT4_MAXSMP) return NULL;
    return s->smp[i].name;
}

int pt4_pattern_at(const pt4_song *s, int order)
{
    if (!s || order < 0 || order >= s->songlen) return -1;
    return s->positions[order];
}

static const uint8_t *cellp(const pt4_song *s, int pattern, int row, int chan)
{
    if (!s || pattern < 0 || pattern >= s->npat || row < 0 || row > 63 || chan < 0 || chan >= s->nch) return NULL;
    return s->pat + (((size_t)pattern * 64 + (size_t)row) * (size_t)s->lay + (size_t)chan) * 4;
}

int pt4_pattern_period(const pt4_song *s, int pattern, int row, int chan)
{
    const uint8_t *c = cellp(s, pattern, row, chan);
    if (!c) return -1;
    return ((c[0] & 15) << 8) | c[1];
}

int pt4_pattern_cell(const pt4_song *s, int pattern, int row, int chan, int *note, int *sample, int *effect, int *param)
{
    const uint8_t *c = cellp(s, pattern, row, chan);
    int per, i, best = 0;
    long bd = 0x7fffffffL;
    if (!c) return -1;
    per = ((c[0] & 15) << 8) | c[1];
    if (per) {
        for (i = 0; i < 36; i++) {
            long d = (long)per - (long)pt4_periods[i];
            if (d < 0) d = -d;
            if (d < bd) { bd = d; best = i + 1; }
        }
    }
    if (note) *note = best;
    if (sample) *sample = (c[0] & 0xf0) | (c[2] >> 4);
    if (effect) *effect = c[2] & 15;
    if (param) *param = c[3];
    return 0;
}
