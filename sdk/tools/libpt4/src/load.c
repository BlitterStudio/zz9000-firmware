/* libpt4 loader: bytes -> module, as the PT 4.1 loader (io/fileio.asm, io/formats.asm,
   io/wide41.asm, io/dyn41.asm, engine/smpx41.asm). zlib license, see LICENSE. */
#include <string.h>
#include "pt4_internal.h"

/* virtual file: reads past the end give zeros (pt_FmtRead pads and flags "truncated") */
typedef struct {
    const uint8_t *d;
    size_t len;
    size_t fp;
    int trunc;
} rd_t;

static size_t rd_read(rd_t *r, uint8_t *dst, size_t n, int pad)
{
    size_t got = 0;
    if (r->fp < r->len) {
        got = r->len - r->fp;
        if (got > n) got = n;
        if (dst) memcpy(dst, r->d + r->fp, got);
    }
    if (got < n) {
        if (pad) {
            if (dst) memset(dst + got, 0, n - got);
            r->trunc = 1;
        }
    }
    r->fp += pad ? n : got;
    return got;
}

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static int id_is(const uint8_t *id, const char *s) { return memcmp(id, s, 4) == 0; }

/* sample name: plain loop (vbcc 0.9h -O2 lost the destination of an inlined 22-byte memcpy in the PT4X loop) */
static void copy_name(char *dst, const uint8_t *src)
{
    int k;
    for (k = 0; k < 22; k++) dst[k] = (char)src[k];
}

static int has_footer(const uint8_t *d, size_t len)
{
    unsigned v;
    if (len < 8 + 600) return 0;
    if (memcmp(d + len - 8, "PT41", 4) != 0) return 0;
    v = be16(d + len - 4);
    return v >= 0x0400 && v < 0x0500;
}

/* pt_WideId: 9CHN, 09CH..32CH, 09CN..32CN (TakeTracker) -> channels, else 0 */
static int wide_id(const uint8_t *a)
{
    int c = a[0] - '0', c2;
    if (c < 0 || c > 9) return 0;
    if (a[1] == 'C') {
        if (a[2] == 'H' && a[3] == 'N' && c == 9) return 9;
        return 0;
    }
    if (a[2] != 'C' || (a[3] != 'H' && a[3] != 'N')) return 0;
    c2 = a[1] - '0';
    if (c2 < 0 || c2 > 9) return 0;
    c = c * 10 + c2;
    return (c >= 9 && c <= 32) ? c : 0;
}

/* pt_FmtNarrowId: CD61, TDZ1..4, 1CHN..7CHN -> channels, else 0 */
static int narrow_id(const uint8_t *a)
{
    if (id_is(a, "CD61")) return 6;
    if (a[0] == 'T' && a[1] == 'D') {
        if (a[2] == 'Z' && a[3] >= '1' && a[3] <= '4') return a[3] - '0';
        return 0;
    }
    if (a[1] == 'C' && a[2] == 'H' && a[3] == 'N' && a[0] >= '1' && a[0] <= '7') return a[0] - '0';
    return 0;
}

enum { K_NONE, K_STD4, K_STD8, K_FLT8, K_NARROW, K_WIDE, K_15 };

/* id at 1080 -> kind, channels */
static int classify(const uint8_t *id, int *nch)
{
    int n;
    *nch = 4;
    if ((n = wide_id(id)) != 0) { *nch = n; return K_WIDE; }
    if ((n = narrow_id(id)) != 0) {
        *nch = n;
        return n == 4 ? K_STD4 : K_NARROW;
    }
    if (id_is(id, "M.K.") || id_is(id, "M!K!") || id_is(id, "!PM!") || id_is(id, "FLT4") || id_is(id, "EXO4"))
        return K_STD4;
    if (id_is(id, "8CHN") || id_is(id, "OCTA") || id_is(id, "CD81")) { *nch = 8; return K_STD8; }
    if (id_is(id, "FLT8")) { *nch = 8; return K_FLT8; }
    return K_NONE;
}

/* pt_ModIdOk41: M.K. M!K! FLT4 FLT8 EXO4 !PM! CD81 OCTA CD61, TDZ + any byte, 1CHN..9CHN, two digits + CH/CN */
static int modid_ok(const uint8_t *a)
{
    static const char *const ids[] = { "M.K.", "M!K!", "FLT4", "FLT8", "EXO4", "!PM!", "CD81", "OCTA", "CD61" };
    size_t i;
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
        if (memcmp(a, ids[i], 4) == 0) return 1;
    if (a[0] == 'T' && a[1] == 'D' && a[2] == 'Z') return 1;
    if (a[1] == 'C' && a[2] == 'H' && a[3] == 'N') return a[0] >= '1' && a[0] <= '9';
    if (a[2] == 'C' && (a[3] == 'H' || a[3] == 'N'))
        return a[0] >= '0' && a[0] <= '9' && a[1] >= '0' && a[1] <= '9';
    return 0;
}

/* foreign magic at offset 0 (pt_DosLibDiskOp chain); a known module id at 1080 wins (pt_MagicMod41),
   except for a hunk file */
static int rejected_at_0(const uint8_t *d, size_t len)
{
    static const char *const ids[] = { "OCTA", "MMD0", "MMD1", "MMD2", "MMD3", "SymM", "DIGI", "TFMX", "FC14", "SOAR" };
    size_t i;
    if (len < 4) return 1;
    if (d[0] == 0 && d[1] == 0 && d[2] == 3 && d[3] == 0xf3) return 1;
    if (len >= 1084 && modid_ok(d + 1080)) return 0;
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
        if (memcmp(d, ids[i], 4) == 0) return 1;
    return 0;
}

/* PT asks "15 or 31 samples" for an unknown id; the library takes the 15-sample layout only
   if the header looks sane */
static int plausible_15(const uint8_t *d, size_t len)
{
    int i, songlen, maxpat = 0;
    unsigned long need;
    if (len < 600) return 0;
    for (i = 0; i < 20; i++)
        if (d[i] != 0 && (d[i] < 32 || d[i] > 126)) return 0;
    songlen = d[470];
    if (songlen == 0 || songlen > 128) return 0;
    for (i = 0; i < 128; i++) {
        if (d[472 + i] > 63) return 0;
        if (d[472 + i] > maxpat) maxpat = d[472 + i];
    }
    need = 600UL + (unsigned long)(maxpat + 1) * 1024UL;
    for (i = 0; i < 15; i++) {
        const uint8_t *h = d + 20 + 30 * i;
        if (h[25] > 64 || (h[24] & 0x70)) return 0;
        need += (unsigned long)be16(h + 22) * 2UL;
    }
    return len + 2048 >= need;   /* small tolerance for cut files */
}

typedef struct {
    uint16_t length, repeat, replen;
    uint8_t ft, vol;
} hdr_t;

static void get_hdr(hdr_t *h, const uint8_t *p)
{
    h->length = be16(p + 22);
    h->ft = p[24];
    h->vol = p[25];
    h->repeat = be16(p + 26);
    h->replen = be16(p + 28);
}

/* pt_FmtClampSmp sample rules (samples 1..31, 15-sample modules, PT4X entries) */
static void sanitize_hdr(hdr_t *h)
{
    uint32_t len = h->length, rep, end;
    if (h->vol > 64) h->vol = 64;
    h->ft &= 0x8f;
    if (h->replen == 0) h->replen = 1;
    rep = h->repeat;
    if (rep >= len) {
        h->repeat = 0;
        h->replen = 1;
        return;
    }
    end = rep + h->replen;
    if (end > len) {
        h->replen = (uint16_t)(h->replen - (end - len));
        if (h->replen == 0) h->replen = 1;
    }
}

/* Parse the whole file. s == NULL: detect only (no allocation). Returns kind or K_NONE;
   *ext gets the extension flags seen. */
static int parse(const uint8_t *d0, size_t len0, pt4_song *s, int *ext, int patcap, uint32_t *arena_end)
{
    rd_t r;
    uint8_t hb[1084];
    uint8_t id[4];
    hdr_t h[PT4_MAXSMP + 1];
    int kind, nch, lay, i, j, filepats, stored, songlen, maxp, cur, nsmx = 0;
    size_t hdrsize, patbytes, smpoff;
    uint32_t arena_pos = ADDR_FIRST;
    int x = 0;

    if (!d0) return K_NONE;
    memset(&r, 0, sizeof(r));
    r.d = d0;
    r.len = len0;
    if (has_footer(d0, len0)) {
        x |= PT4_X_FOOTER;
        r.len -= 8;
    }
    if (rejected_at_0(r.d, r.len)) return K_NONE;
    memset(id, 0, 4);
    if (r.len >= 1084) memcpy(id, r.d + 1080, 4);
    kind = classify(id, &nch);
    if (kind == K_NONE) {
        if (!plausible_15(r.d, r.len)) return K_NONE;
        kind = K_15;
    }
    memset(hb, 0, sizeof(hb));
    memset(h, 0, sizeof(h));
    if (kind == K_15) {
        memcpy(hb, r.d, 470);
        memcpy(hb + 950, r.d + 470, 130);
        r.fp = 600;
        hdrsize = 600;
        memcpy(hb + 1080, "M.K.", 4);
        for (i = 16; i <= 31; i++) hb[20 + 30 * (i - 1) + 29] = 1;   /* empty headers, replen 1 */
    } else {
        if (r.len < 1084) return K_NONE;
        memcpy(hb, r.d, 1084);
        r.fp = 1084;
        hdrsize = 1084;
    }
    for (i = 1; i <= 31; i++) {
        get_hdr(&h[i], hb + 20 + 30 * (i - 1));
        if (s) copy_name(s->smp[i].name, hb + 20 + 30 * (i - 1));
        sanitize_hdr(&h[i]);   /* pt_FmtClampSmp, also for 15 samples (pt_Fmt15Clamp41) */
    }
    /* positions: FLT8 halves them */
    if (kind == K_FLT8)
        for (i = 0; i < 128; i++) hb[952 + i] >>= 1;
    songlen = hb[950];
    if (songlen == 0) songlen = 1;
    if (songlen > 128) songlen = 128;
    maxp = 0;
    for (i = 0; i < 128; i++)
        if (hb[952 + i] > maxp) maxp = hb[952 + i];
    filepats = maxp + 1;
    stored = filepats;
    if (kind == K_WIDE && patcap > 100) patcap = 100;
    if (stored > patcap) {
        stored = patcap;
        for (i = 0; i < 128; i++)
            if (hb[952 + i] >= patcap) hb[952 + i] = 0;
    }
    switch (kind) {
    case K_STD8: case K_FLT8: lay = 8; patbytes = 2048; break;
    case K_NARROW: lay = 8; patbytes = (size_t)nch * 256; break;
    case K_WIDE: lay = 32; patbytes = (size_t)nch * 256; break;
    default: lay = 4; patbytes = 1024; break;
    }
    if (kind == K_WIDE) x |= PT4_X_WIDE;

    if (s) {
        s->nch = nch;
        s->lay = lay;
        s->wide = kind == K_WIDE;
        s->mode4 = lay != 8;
        s->songlen = songlen;
        memcpy(s->positions, hb + 952, 128);
        s->npat = stored;
        memcpy(s->title, hb, 20);
        s->title[20] = 0;
        if (kind == K_15) memcpy(s->id, "15smp", 6);
        else { memcpy(s->id, id, 4); s->id[4] = 0; for (i = 0; i < 4; i++) if (s->id[i] < 32 || s->id[i] > 126) s->id[i] = '?'; }
        s->pat = (uint8_t *)pt4__alloc(s, (size_t)stored * 64 * (size_t)lay * 4);
        if (!s->pat) return -1;
        memset(s->pat, 0, (size_t)stored * 64 * (size_t)lay * 4);
    }
    /* patterns */
    {
        int p, row, c;
        uint8_t cell[4];
        for (p = 0; p < stored; p++) {
            if (kind == K_FLT8) {
                /* two 4ch patterns -> one 8ch pattern, row by row */
                for (j = 0; j < 2; j++)
                    for (row = 0; row < 64; row++)
                        for (c = 0; c < 4; c++) {
                            rd_read(&r, cell, 4, 1);
                            if (s) memcpy(s->pat + (((size_t)p * 64 + (size_t)row) * 8 + (size_t)(j * 4 + c)) * 4, cell, 4);
                        }
            } else {
                for (row = 0; row < 64; row++)
                    for (c = 0; c < (kind == K_STD4 || kind == K_15 ? 4 : (kind == K_STD8 ? 8 : nch)); c++) {
                        rd_read(&r, cell, 4, 1);
                        if (s) memcpy(s->pat + (((size_t)p * 64 + (size_t)row) * (size_t)lay + (size_t)c) * 4, cell, 4);
                    }
            }
        }
    }
    smpoff = hdrsize + (size_t)filepats * patbytes;
    r.fp = smpoff;
    if (s) {
        /* pt_FmtScanPeriods: sample numbers > 63 -> 0 (wide: only the 4ch view, the replayer clamps) */
        size_t ncell = (size_t)stored * 64 * (size_t)lay, k;
        for (k = 0; k < ncell; k++) {
            uint8_t *c = s->pat + k * 4;
            int smp = (c[0] & 0xf0) | (c[2] >> 4);
            if (kind == K_WIDE && (k % 32) >= 4) continue;
            if (smp > 63) { c[0] &= 0x0f; c[2] &= 0x0f; }
            if ((c[2] & 0x0f) == 8 && (c[3] & 0x0f) && ((0xde >> (c[3] >> 4)) & 1)) x |= PT4_X_8XY;   /* x = 1,2,3,4,6,7: footer bit 0 rule */
            if ((c[2] & 0x0f) == 0x0e && (c[3] & 0xf0) == 0xf0 && (c[3] & 0x0f)) s->uses_funk = 1;
        }
    }

    /* samples 1..31, then PT4X entries 32.. */
    cur = 1;
    for (;;) {
        hdr_t *hh = &h[cur];
        if (hh->length) {
            size_t n = (size_t)hh->length * 2;
            if (s) {
                s->smp[cur].addr = arena_pos;
                rd_read(&r, s->arena ? s->arena + arena_pos : NULL, n, 1);
            } else {
                rd_read(&r, NULL, n, 1);
            }
            arena_pos += (uint32_t)n;
            if (hh->ft & 0x80) {
                uint8_t lb[4];
                uint32_t L;
                rd_read(&r, lb, 4, 1);
                L = be32(lb);
                if (L != 0) {
                    if (L < 8 || L > 65536 || (L & 1)) {
                        r.fp -= 4;            /* junk bit: 4 bytes back to the sample stream */
                        hh->ft &= 0x7f;
                    } else {
                        /* read L bytes, validate */
                        size_t at = r.fp;
                        uint8_t e4[4];
                        int nseg, ok = 1, k;
                        rd_read(&r, e4, 4, 1);
                        nseg = (int16_t)be16(e4);
                        if (nseg <= 0 || (uint32_t)nseg > (L - 4) / 4) ok = 0;
                        for (k = 0; ok && k < nseg; k++) {
                            uint8_t sg[4];
                            rd_read(&r, sg, 4, 1);
                            if (be16(sg + 2) > 64) ok = 0;
                        }
                        r.fp = at + L;
                        if (r.fp > r.len) r.trunc = 1;
                        if (!ok) {
                            r.fp = at - 4;
                            hh->ft &= 0x7f;
                        } else {
                            x |= PT4_X_DYN;
                            if (s) {
                                pt4_env *e = &s->smp[cur].env;
                                uint8_t sg[4];
                                size_t save = r.fp;
                                e->len = (uint16_t *)pt4__alloc(s, (size_t)nseg * (2 * sizeof(uint16_t) + sizeof(uint32_t)) + 4);
                                if (!e->len) return -1;
                                e->vol = e->len + nseg;
                                e->off = (uint32_t *)(void *)(e->len + 2 * nseg + (nseg & 1));
                                e->nseg = nseg;
                                r.fp = at + 2;
                                rd_read(&r, sg, 2, 1);
                                e->loopseg = be16(sg);
                                for (k = 0; k < nseg; k++) {
                                    rd_read(&r, sg, 4, 1);
                                    e->len[k] = be16(sg);
                                    e->vol[k] = be16(sg + 2);
                                    e->off[k] = k ? e->off[k - 1] + (uint32_t)e->len[k - 1] * 2 : 0;
                                }
                                r.fp = save;
                                s->smp[cur].chunked = 1;
                            }
                        }
                    }
                }
            }
        }
        /* pt_CheckRowDataSize */
        if (hh->replen == 0) hh->replen = 1;
        cur++;
        /* pt_SmxNext */
        if (cur == 32) {
            uint8_t tag[8];
            if (rd_read(&r, tag, 8, 0) == 8 && memcmp(tag, "PT4X", 4) == 0 && be32(tag + 4) >= 1 && be32(tag + 4) <= 32) {
                uint8_t hd[32 * 30];
                nsmx = (int)be32(tag + 4);
                memset(hd, 0, sizeof(hd));
                rd_read(&r, hd, (size_t)nsmx * 30, 0);
                for (i = 0; i < nsmx; i++) {
                    get_hdr(&h[32 + i], hd + 30 * i);
                    sanitize_hdr(&h[32 + i]);
                    if (s) copy_name(s->smp[32 + i].name, hd + 30 * i);
                }
                x |= PT4_X_PT4X;
            } else {
                break;
            }
        }
        if (cur > 31 + nsmx) break;
    }
    if (r.trunc) x |= PT4_X_TRUNC;
    if (ext) *ext = x;
    if (s) {
        s->nsmp = 31 + nsmx;
        for (i = 1; i <= PT4_MAXSMP; i++) {
            pt4_sample *sm = &s->smp[i];
            sm->name[22] = 0;
            for (j = 0; j < 22; j++)
                if (sm->name[j] && (sm->name[j] < 32 || sm->name[j] > 126)) sm->name[j] = ' ';
            sm->length = h[i].length;
            sm->repeat = h[i].repeat;
            sm->replen = h[i].replen;
            sm->finetune = h[i].ft;
            sm->volume = h[i].vol;
            /* pt_SampleClearLoop: first word of the data = 0 */
            if (sm->addr && s->arena) { s->arena[sm->addr] = 0; s->arena[sm->addr + 1] = 0; }
        }
        s->ext = x;
    }
    if (arena_end) *arena_end = arena_pos;
    return kind;
}

int pt4_detect(const void *data, size_t len)
{
    int ext = 0, kind;
    if (!data) return PT4_NONE;
    kind = parse((const uint8_t *)data, len, NULL, &ext, 100, NULL);
    if (kind == K_NONE || kind < 0) return PT4_NONE;
    if (ext & (PT4_X_FOOTER | PT4_X_PT4X | PT4_X_DYN)) return PT4_EXT;
    return PT4_PLAIN_MOD;
}

/* two passes: the first one sizes the arena */
int pt4__load(pt4_song *s, const uint8_t *d, size_t len)
{
    int ext = 0, kind;
    uint32_t total = 0;
    kind = parse(d, len, NULL, &ext, 100, &total);
    if (kind == K_NONE || kind < 0) return PT4_ERR_FORMAT;
    s->arena_size = total + 16;
    s->arena = (uint8_t *)pt4__alloc(s, s->arena_size);
    if (!s->arena) return PT4_ERR_MEMORY;
    memset(s->arena, 0, s->arena_size);
    kind = parse(d, len, s, &ext, 100, NULL);
    if (kind < 0) return PT4_ERR_MEMORY;
    return PT4_OK;
}
