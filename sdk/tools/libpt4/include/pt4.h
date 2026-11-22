/*
 * libpt4 - ProTracker 4.1 module replayer, portable C99
 *
 * ProTracker 4.1 by Tom Beyer, Oliver Ickler, Haavard Pedersen and
 * Akustikrausch (Andreas Wendorf); based on ProTracker by Lars Hamre /
 * Amiga Freelancers, 3.15 CryptoBurners.
 *
 * zlib license, see LICENSE in this directory.
 *
 * Loads every module id the PT 4.1 loader accepts (M.K., M!K!, 1CHN-9CHN,
 * 10CH-32CH, 8CHN, OCTA, CD81, FLT4/FLT8, TDZx, CD61, 15 samples) with the
 * 4.1 extensions (PT4X samples 32-63, DYN envelopes, 8xy sub-effects 81y-87y)
 * and renders stereo PCM at any rate. Format and playback rules:
 * docs/PT41_FORMAT.md in the ProTracker 4.1 repository.
 *
 * Every call works on its own pt4_song; there is no global mutable state,
 * so independent songs may be used from different threads. The library
 * never prints and never touches files.
 */
#ifndef PT4_H
#define PT4_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PT4_VERSION_MAJOR 1
#define PT4_VERSION_MINOR 0

/* pt4_detect results */
#define PT4_NONE      0   /* not a module PT 4.1 loads */
#define PT4_PLAIN_MOD 1   /* a module other players can read too */
#define PT4_EXT       2   /* uses 4.1 extensions: 'PT41' footer, PT4X block or DYN envelope */

/* error codes (pt4_open *err) */
#define PT4_OK          0
#define PT4_ERR_ARG    -1
#define PT4_ERR_FORMAT -2
#define PT4_ERR_MEMORY -3

/* output paths (pt4_set_option PT4_OPT_PATH) */
#define PT4_PATH_AUTO  0  /* as PT: 4 channels -> PAULA, 5..8 -> MIXER, 9..32 -> WIDE */
#define PT4_PATH_PAULA 1  /* Paula semantics, hard LRRL panning */
#define PT4_PATH_MIXER 2  /* PT 4.1 020 mixer (8 voices, pairs n/n+4), bit exact */
#define PT4_PATH_WIDE  3  /* every voice on its own, AmiGUS panning (wide modules) */

/* options (pt4_set_option / pt4_get_option) */
#define PT4_OPT_PATH        1  /* PT4_PATH_*, applies after the next pt4_seek_ms(0) or at once if not yet rendered */
#define PT4_OPT_STEREO      2  /* stereo width in quarters as AHI: 0 = 100 % (hard) .. 4 = mono; default 0 */
#define PT4_OPT_INTERP      3  /* 1 = linear interpolation (not PT), default 0 */
#define PT4_OPT_DYN_MIXER   4  /* 1 = DYN envelopes also in the mixer path (PT ignores them there), default 0 */
#define PT4_OPT_NOTES_EXT   5  /* 1 = setup NOTES EXT: cell periods outside 113..856 kept, 1xx/2xx/3xx reach 57..1712;
                                   default 0 (wide modules always use it, as PT) */
#define PT4_OPT_MASTER_VOL  6  /* 0..64, default 64 */
#define PT4_OPT_START_ROW0  7  /* 1 = first tick reads row 0 (PT: row 0 on the speed-th tick), default 0 */
#define PT4_OPT_TEMPO_LATCH 8  /* tempo change timing: 0 = as the path in PT (mixer: same tick, Paula: next tick), 1 = always next tick */
#define PT4_OPT_TEMPO       9  /* start tempo (setup TEMPO), 32..255, default 125; applies from the next restart */

/* extension flags (pt4_extensions) */
#define PT4_X_8XY     1   /* cells with 8xy, x = 1,2,3,4,6,7, y > 0 (the 4.1 sub-effects; footer bit 0 rule) */
#define PT4_X_PT4X    2   /* PT4X block (samples 32..63) */
#define PT4_X_DYN     4   /* DYN envelopes */
#define PT4_X_FOOTER  8   /* 'PT41' footer present */
#define PT4_X_TRUNC  16   /* file shorter than its header says; missing data is silence */
#define PT4_X_WIDE   32   /* 9..32 channels */

typedef struct pt4_song pt4_song;

typedef struct pt4_alloc {
    void *(*alloc)(size_t size, void *user);
    void  (*free)(void *ptr, void *user);
    void *user;
} pt4_alloc;

/* Classify a buffer. Reads only, never allocates. */
int pt4_detect(const void *data, size_t len);

/* Load a module from memory. The buffer is copied; it may be freed after the call.
   alloc may be NULL (malloc/free). err may be NULL. */
pt4_song *pt4_open(const void *data, size_t len, const pt4_alloc *alloc, int *err);
void       pt4_close(pt4_song *song);

/* Render interleaved stereo frames at samplerate (8000..192000 Hz, may change between calls).
   Returns the frames written; less than requested only at the song end (loop off). */
size_t pt4_render_f32(pt4_song *song, float *out, size_t frames, unsigned samplerate);
size_t pt4_render_s16(pt4_song *song, int16_t *out, size_t frames, unsigned samplerate);

/* Reference output: the four signed 8 bit pair streams of the mixer path (pair p = voices p+1 and p+5).
   pairs[0..3] must each hold frames bytes. */
size_t pt4_render_pairs(pt4_song *song, int8_t *pairs[4], size_t frames, unsigned samplerate);

/* Position */
int  pt4_seek_ms(pt4_song *song, long ms);          /* restart, silent play to ms*rate/1000 frames (bit exact); 0 = ok */
int  pt4_set_rate(pt4_song *song, unsigned rate);   /* rate for seeking before the first render, 8000..192000 */
long pt4_length_ms(const pt4_song *song);           /* -1 unknown (no end within 2 hours) */
void pt4_position(const pt4_song *song, int *order, int *row);
int  pt4_ended(const pt4_song *song);               /* song end reached (F00 or an order played again) */
void pt4_set_loop(pt4_song *song, int on);          /* on: keep playing after the end as PT does; default off */
void pt4_mute(pt4_song *song, int chan, int on);    /* chan 0-based; output only, the replayer keeps running */

/* Options, see PT4_OPT_* */
int  pt4_set_option(pt4_song *song, int opt, int value);   /* 0 = ok */
int  pt4_get_option(const pt4_song *song, int opt);

/* Module info */
int         pt4_channels(const pt4_song *song);
const char *pt4_title(const pt4_song *song);              /* 20 chars max, 0-terminated */
const char *pt4_format_id(const pt4_song *song);          /* id as in the file, e.g. "M.K.", "8CHN", "15smp" */
int         pt4_num_samples(const pt4_song *song);        /* 31, or 31 + PT4X entries */
const char *pt4_sample_name(const pt4_song *song, int i); /* i = 1..63; NULL if out of range */
int         pt4_extensions(const pt4_song *song);         /* PT4_X_* */
int         pt4_num_orders(const pt4_song *song);
int         pt4_pattern_at(const pt4_song *song, int order);
int         pt4_num_patterns(const pt4_song *song);
/* note: 0 = none, 1..36 = C-1..B-3 (nearest table note for other periods);
   sample 0..63, effect 0..15, param 0..255. Returns 0, or -1 if out of range. */
int pt4_pattern_cell(const pt4_song *song, int pattern, int row, int chan,
                      int *note, int *sample, int *effect, int *param);
/* raw period of a cell (0 = none), -1 if out of range */
int pt4_pattern_period(const pt4_song *song, int pattern, int row, int chan);

#ifdef __cplusplus
}
#endif

#endif
