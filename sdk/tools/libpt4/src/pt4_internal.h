/* libpt4 internals. zlib license, see LICENSE. */
#ifndef PT4_INTERNAL_H
#define PT4_INTERNAL_H

#include "../include/pt4.h"

#define PT4_MAXCH     32
#define PT4_MAXSMP    63
#define PAULA_CLOCK    3546895UL
#define ECLOCK_PAL     709379UL
#define CIA_BASE_PAL   1773447UL
#define MIX_MINPER     55
#define ADDR_SCRATCH   16u     /* chip_ScratchBuffer: 32 zero bytes */
#define ADDR_FIRST     64u     /* first sample byte in the arena */

extern const uint16_t pt4_periods[616];

/* one DYN envelope */
typedef struct {
    int nseg, loopseg;
    uint16_t *len;     /* nseg words: segment length in words */
    uint16_t *vol;     /* nseg: 0..64 */
    uint32_t *off;     /* nseg: byte offset of each segment from the sample start */
} pt4_env;

typedef struct {
    char     name[23];
    uint16_t length, repeat, replen;   /* words, header after load rules */
    uint8_t  finetune, volume;         /* finetune byte as stored (bit 7 = envelope bit) */
    uint32_t addr;                     /* arena address of the data, 0 = none */
    int      chunked;                  /* envelope kept (dat_RpChunkedSamples) */
    pt4_env env;
} pt4_sample;

/* channel state as the replayer's n_* structure */
typedef struct {
    uint16_t note;        /* cell word 0 */
    uint8_t  cmd, cmdlo;  /* cell bytes 2, 3 */
    uint32_t start;
    uint16_t length;
    uint32_t loopstart;
    uint16_t replen;
    uint16_t period;
    uint8_t  finetune, volume;
    uint16_t volscale;
    uint8_t  toneportdirec, toneportspeed;
    uint16_t wantedperiod;
    uint8_t  vibratocmd, vibratopos, tremolocmd, tremolopos;
    uint8_t  wavecontrol, glissfunk, sampleoffset, pattpos, loopcount, funkoffset;
    uint32_t wavestart;
    uint16_t reallength;
    uint32_t samplestart;
    uint16_t samplelen;
    uint8_t  samplenum, chunked, dmawait;
    uint16_t irqvol, volmul;
    uint16_t chanbit;
    /* chunk list (pt_BuildChunkList, cut/seek by pt_RpStartChunked) */
    int      ck_smp;           /* sample, 0 = empty list */
    int      ck_first;         /* first chunk index */
    uint32_t ck_firstaddr;
    uint16_t ck_firstlen;
    int      ck_last;          /* last chunk index (incl.), -1 = empty */
    uint16_t ck_lastlen;
    int      ck_loop;          /* loop chunk index, -1 = none */
} pt4_chan;

/* shadow Paula registers of a voice */
typedef struct {
    uint16_t per, vol, len;
    uint32_t lc;
} pt4_aud;

/* mixer / paula voice */
typedef struct {
    int      active, back;
    uint32_t pos;          /* integer address */
    uint32_t frac;         /* 16 bit fraction */
    uint32_t end;
    uint32_t loopstart, looplen;
    uint32_t step;         /* 16.16 */
    uint16_t period;       /* period the step was computed for */
    uint16_t vol;
    /* paula semantics: loop latched at the trigger */
    int      chunkmode;    /* playing a chunk list */
    int      ck_idx;       /* current chunk */
    uint16_t ck_vol;       /* volume of the playing chunk */
} pt4_voice;

typedef struct {
    /* replayer globals */
    int      playing;
    uint32_t counter, speed;
    uint16_t tempo;
    uint32_t songpos, patternpos;
    uint8_t  breakrow, pbreakflag, posjumpflag, pattdelaytime, pattdelaycount, lowmask;
    uint8_t  efx8[8][4];        /* slide, tremor y, tremor cnt, retrig interval */
    uint8_t  blend[4];
    pt4_chan ch[PT4_MAXCH];
    pt4_aud  aud[PT4_MAXCH];
    pt4_voice v[PT4_MAXCH];
    /* timing */
    uint32_t spt;               /* samples per tick, 16.16 */
    uint16_t spt_bpm;
    uint32_t frac;              /* 16 bit carry */
    uint32_t tick_left;
    /* song end */
    uint8_t  visited[128];
    int      ended;
    int      cur_order, cur_row;
    uint64_t ticks_eclk;        /* elapsed time in E clock cycles */
    uint16_t dbg_per[PT4_MAXCH], dbg_vol[PT4_MAXCH];   /* test trace: arpeggio/vibrato period, tremolo volume of this tick, 0xffff = none */
} pt4_state;

struct pt4_song {
    pt4_alloc al;
    char     title[21];
    char     id[6];
    int      nch;            /* channels in the file */
    int      lay;            /* channels per stored row: 4, 8 or 32 */
    int      mode4;          /* replayer row step 16 bytes (4ch, wide) */
    int      wide;
    int      songlen;
    uint8_t  positions[128];
    int      npat;           /* stored patterns */
    uint8_t *pat;            /* npat * 64 * lay * 4 bytes */
    pt4_sample smp[PT4_MAXSMP + 1];
    int      nsmp;           /* 31 + PT4X count */
    int      ext;
    uint8_t *arena;
    uint32_t arena_size;
    uint8_t *pristine;       /* copy for EFx (funk) when used, else NULL */
    int      uses_funk;
    long     length_ms;
    /* options */
    int      opt_path, path;
    int      opt_frac0;
    int      opt_stereo, opt_interp, opt_dyn_mixer, opt_notes_ext, opt_master, opt_row0, opt_latch, opt_tempo;
    int      loop;
    int      mute[PT4_MAXCH];
    unsigned rate;
    int      dry;            /* no voices (length scan / seek) */
    pt4_state st;
};

/* api.c */
size_t pt4__render(pt4_song *s, int32_t *lr, int8_t **pairs, size_t frames, unsigned rate);
/* load.c */
int  pt4__load(pt4_song *s, const uint8_t *d, size_t len);
/* replay.c */
void pt4__reset(pt4_song *s);
void pt4__tick(pt4_song *s);
uint32_t pt4__cia_period(unsigned bpm);
/* voice.c */
void pt4__calc_spt(pt4_song *s, unsigned bpm);
void pt4__update_voices(pt4_song *s);
void pt4__mix(pt4_song *s, int32_t *lr, int8_t **pairs, uint32_t off, uint32_t n);
void *pt4__alloc(pt4_song *s, size_t n);
void *pt4__malloc(size_t n);
void pt4__free(pt4_song *s, void *p);

#define PT4_RD8(s, a) ((uint32_t)(a) < (s)->arena_size ? (s)->arena[(uint32_t)(a)] : (uint8_t)0)

#endif
