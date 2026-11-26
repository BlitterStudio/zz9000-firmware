/*
 * DECODE_MP3 end-of-input behaviour: what the one-shot decoder reports for
 * clean MP3 files, files with ID3v2 / ID3v1 / APEv2 tags, truncated files,
 * trailing junk, and output buffers that fill first. Drives the production
 * decode_mp3.c (init, samples, bytes consumed, mp3_expect_end_ok) over real
 * encoder output and variants of it built in memory; sdk_mailbox.c cannot be
 * linked into a host test, so the last checks read its source to pin that
 * mp3_decode_compute() applies the same functions in the same order.
 *
 * Fixtures (ffmpeg with libmp3lame, no ID3v2 unless noted):
 *   tone.mp3                  2 s, 44.1 kHz stereo CBR 128k, ID3v2.4 tag, Info frame
 *   tone_vbr_mono_48k.mp3     0.5 s, 48 kHz mono VBR (-q:a 4), Xing frame
 *   tone_noxing_44k.mp3       0.4 s, 44.1 kHz stereo CBR 128k, no Xing frame
 * The expected frame counts are the PCM frames ffmpeg decodes from each file
 * (it applies the same encoder delay and padding trim from the Xing/Info
 * frame as minimp3).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp3/mp3.h"

#define PCM_CAP (1U << 20)

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

static struct mp3_decode_ctx g_ctx;
static uint8_t g_pcm[PCM_CAP];
static uint8_t g_clean_pcm[PCM_CAP];

struct outcome {
	int init_rc;
	size_t written;
	unsigned long consumed;
	enum mp3_input_state state;
	int expect_end_ok;
	int end; /* RESULT_END as mp3_decode_compute() derives it */
	size_t start_offset;
	int vbr_tag;
};

/* The sequence mp3_decode_compute() runs, up to the reply fields. */
static void run(const uint8_t *in, size_t len, size_t cap,
                struct outcome *o)
{
	memset(o, 0, sizeof(*o));
	memset(g_pcm, 0xa5, sizeof(g_pcm));
	o->init_rc = decode_mp3_init(&g_ctx, (uint8_t *)(uintptr_t)in, len);
	if (o->init_rc)
		return;
	o->written = (size_t)decode_mp3_samples(&g_ctx, g_pcm, (int)(cap / 2U));
	o->consumed = mp3_get_bytes_consumed(&g_ctx);
	o->state = mp3_input_state(&g_ctx);
	o->expect_end_ok = mp3_expect_end_ok(&g_ctx, o->written, cap);
	o->end = o->consumed >= len;
	o->start_offset = (size_t)g_ctx.mp3d.start_offset;
	o->vbr_tag = g_ctx.mp3d.vbr_tag_found;
}

struct buf {
	uint8_t *p;
	size_t n;
};

static uint8_t *must_alloc(size_t n)
{
	uint8_t *p = malloc(n ? n : 1U);

	if (!p) {
		printf("FATAL: out of memory\n");
		exit(2);
	}
	return p;
}

static struct buf load(const char *dir, const char *name)
{
	char path[512];
	struct buf b;
	FILE *f;
	long n;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "rb");
	if (!f) {
		printf("FATAL: cannot open %s\n", path);
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b.p = must_alloc((size_t)n);
	b.n = (size_t)n;
	if (fread(b.p, 1U, b.n, f) != b.n) {
		printf("FATAL: short read %s\n", path);
		exit(2);
	}
	fclose(f);
	return b;
}

/* prefix + body[body_skip .. body_skip + body_len) + suffix */
static struct buf cat(const uint8_t *prefix, size_t prefix_len,
                      const uint8_t *body, size_t body_len,
                      const uint8_t *suffix, size_t suffix_len)
{
	struct buf b;

	b.n = prefix_len + body_len + suffix_len;
	b.p = must_alloc(b.n);
	if (prefix_len)
		memcpy(b.p, prefix, prefix_len);
	memcpy(b.p + prefix_len, body, body_len);
	if (suffix_len)
		memcpy(b.p + prefix_len + body_len, suffix, suffix_len);
	return b;
}

/* ID3v2.3 header with one TIT2 frame, padded to `total` bytes. */
static size_t make_id3v2(uint8_t *out, size_t total)
{
	size_t n = total - 10U;

	memset(out, 0, total);
	memcpy(out, "ID3\x03\x00\x00", 6);
	out[6] = (uint8_t)((n >> 21) & 0x7f);
	out[7] = (uint8_t)((n >> 14) & 0x7f);
	out[8] = (uint8_t)((n >> 7) & 0x7f);
	out[9] = (uint8_t)(n & 0x7f);
	memcpy(out + 10, "TIT2", 4);
	out[17] = 11;
	memcpy(out + 21, "measure tag", 11);
	return total;
}

static size_t make_id3v1(uint8_t *out)
{
	memset(out, 0, 128);
	memcpy(out, "TAG", 3);
	memcpy(out + 3, "Title", 5);
	memcpy(out + 33, "Artist", 6);
	memcpy(out + 93, "2026", 4);
	out[127] = 0xff;
	return 128;
}

/* "TAG+" extended block (227 bytes) that precedes the ID3v1 tag. */
static size_t make_id3v1_ext(uint8_t *out)
{
	memset(out, 0, 227);
	memcpy(out, "TAG+", 4);
	return 227;
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* APEv2 tag with one item. size and flags are overridable so the tests can
 * write footers that lie. */
static size_t make_ape(uint8_t *out, int with_header, uint32_t size_override,
                       int override_size)
{
	static const uint8_t item[] = {
		7, 0, 0, 0, 0, 0, 0, 0, 'T', 'i', 't', 'l', 'e', 0,
		'm', 'e', 'a', 's', 'u', 'r', 'e'
	};
	uint32_t size = (uint32_t)sizeof(item) + 32U;
	size_t n = 0;
	uint32_t flags = with_header ? 0x80000000UL : 0U;

	if (override_size)
		size = size_override;
	if (with_header) {
		memcpy(out + n, "APETAGEX", 8);
		put_le32(out + n + 8, 2000);
		put_le32(out + n + 12, size);
		put_le32(out + n + 16, 1);
		put_le32(out + n + 20, flags | 0x20000000UL);
		memset(out + n + 24, 0, 8);
		n += 32;
	}
	memcpy(out + n, item, sizeof(item));
	n += sizeof(item);
	memcpy(out + n, "APETAGEX", 8);
	put_le32(out + n + 8, 2000);
	put_le32(out + n + 12, size);
	put_le32(out + n + 16, 1);
	put_le32(out + n + 20, flags);
	memset(out + n + 24, 0, 8);
	n += 32;
	return n;
}

/* Expected: decodes exactly like the clean file and reports a clean end. */
static void expect_clean(const char *what, const struct buf *in,
                         size_t clean_written)
{
	struct outcome o;

	run(in->p, in->n, PCM_CAP, &o);
	CHECK(o.init_rc == 0, "%s: init failed (%d)", what, o.init_rc);
	CHECK(o.written == clean_written, "%s: wrote %lu, clean file %lu", what,
	      (unsigned long)o.written, (unsigned long)clean_written);
	CHECK(memcmp(g_pcm, g_clean_pcm, clean_written) == 0,
	      "%s: PCM differs from the clean file", what);
	CHECK(o.consumed == in->n, "%s: consumed %lu of %lu", what, o.consumed,
	      (unsigned long)in->n);
	CHECK(o.end, "%s: RESULT_END not set", what);
	CHECK(o.state == MP3_INPUT_COMPLETE, "%s: state %d, want COMPLETE", what,
	      (int)o.state);
	CHECK(o.expect_end_ok, "%s: EXPECT_END rejected a complete stream", what);
}

/* Expected: EXPECT_END rejects it, and the lenient decode still ends with
 * every byte used (RESULT_END set) while delivering less PCM. */
static void expect_damaged(const char *what, const struct buf *in,
                           size_t clean_written)
{
	struct outcome o;

	run(in->p, in->n, PCM_CAP, &o);
	CHECK(o.init_rc == 0, "%s: init failed (%d)", what, o.init_rc);
	CHECK(!o.expect_end_ok, "%s: EXPECT_END accepted a damaged end", what);
	CHECK(o.state == MP3_INPUT_DAMAGED, "%s: state %d, want DAMAGED", what,
	      (int)o.state);
	CHECK(o.written < clean_written, "%s: wrote %lu, clean file %lu", what,
	      (unsigned long)o.written, (unsigned long)clean_written);
	CHECK(o.consumed == in->n && o.end,
	      "%s: lenient decode consumed %lu of %lu, end=%d", what,
	      o.consumed, (unsigned long)in->n, o.end);
}

/* Offsets just past each frame, walking from the first frame at start. */
static size_t frame_ends(const struct buf *b, size_t start, size_t *ends,
                         size_t max)
{
	mp3dec_t dec;
	mp3dec_frame_info_t info;
	size_t pos = start;
	size_t n = 0;

	mp3dec_init(&dec);
	while (pos < b->n && n < max) {
		int samples = mp3dec_decode_frame(&dec, b->p + pos,
		                                  (int)(b->n - pos), NULL, &info);

		if (samples <= 0 || info.frame_bytes <= 0)
			break;
		pos += (size_t)info.frame_bytes;
		ends[n++] = pos;
	}
	return n;
}

static void test_fixture(const char *dir, const char *name,
                         size_t expected_frames, unsigned channels,
                         int expect_vbr_tag)
{
	struct buf clean = load(dir, name);
	struct outcome o;
	size_t clean_written = expected_frames * channels * 2U;
	size_t start;
	size_t audio_len;
	uint8_t id3v2[2048];
	uint8_t id3v1[128];
	uint8_t id3v1x[227];
	uint8_t ape_hdr[256];
	uint8_t ape_bare[256];
	uint8_t tail[2048];
	size_t id3v2_len = make_id3v2(id3v2, sizeof(id3v2));
	size_t id3v1_len = make_id3v1(id3v1);
	size_t id3v1x_len = make_id3v1_ext(id3v1x);
	size_t ape_hdr_len = make_ape(ape_hdr, 1, 0, 0);
	size_t ape_bare_len = make_ape(ape_bare, 0, 0, 0);
	struct buf v;
	size_t tail_len;
	char what[128];
	unsigned i;

	printf("fixture %s (%lu bytes)\n", name, (unsigned long)clean.n);

	/* clean input, and the reference PCM for every variant */
	run(clean.p, clean.n, PCM_CAP, &o);
	CHECK(o.init_rc == 0, "%s: init failed", name);
	CHECK(mp3_get_hz(&g_ctx) > 0 && (unsigned)mp3_get_channels(&g_ctx) ==
	      channels, "%s: rate %d channels %d", name, mp3_get_hz(&g_ctx),
	      mp3_get_channels(&g_ctx));
	CHECK(o.vbr_tag == expect_vbr_tag, "%s: vbr tag %d", name, o.vbr_tag);
	CHECK(o.written == clean_written, "%s: wrote %lu bytes, want %lu", name,
	      (unsigned long)o.written, (unsigned long)clean_written);
	CHECK(o.consumed == clean.n && o.end && o.state == MP3_INPUT_COMPLETE &&
	      o.expect_end_ok, "%s: clean file: consumed %lu/%lu end=%d state=%d "
	      "ok=%d", name, o.consumed, (unsigned long)clean.n, o.end,
	      (int)o.state, o.expect_end_ok);
	memcpy(g_clean_pcm, g_pcm, o.written);
	start = o.start_offset;
	audio_len = clean.n;

	/* tags the decoder skips cleanly */
	v = cat(id3v2, id3v2_len, clean.p, clean.n, NULL, 0);
	expect_clean("ID3v2 prefix", &v, clean_written);
	free(v.p);
	v = cat(NULL, 0, clean.p, clean.n, id3v1, id3v1_len);
	expect_clean("ID3v1 suffix", &v, clean_written);
	free(v.p);
	memcpy(tail, id3v1x, id3v1x_len);
	memcpy(tail + id3v1x_len, id3v1, id3v1_len);
	v = cat(NULL, 0, clean.p, clean.n, tail, id3v1x_len + id3v1_len);
	expect_clean("TAG+ and ID3v1 suffix", &v, clean_written);
	free(v.p);
	v = cat(NULL, 0, clean.p, clean.n, ape_hdr, ape_hdr_len);
	expect_clean("APEv2 suffix with header", &v, clean_written);
	free(v.p);
	v = cat(NULL, 0, clean.p, clean.n, ape_bare, ape_bare_len);
	expect_clean("APEv2 suffix without header", &v, clean_written);
	free(v.p);
	memcpy(tail, ape_bare, ape_bare_len);
	memcpy(tail + ape_bare_len, id3v1, id3v1_len);
	v = cat(NULL, 0, clean.p, clean.n, tail, ape_bare_len + id3v1_len);
	expect_clean("APEv2 then ID3v1 suffix", &v, clean_written);
	free(v.p);
	memcpy(tail, id3v1, id3v1_len);
	memcpy(tail + id3v1_len, ape_hdr, ape_hdr_len);
	v = cat(NULL, 0, clean.p, clean.n, tail, id3v1_len + ape_hdr_len);
	expect_clean("ID3v1 then APEv2 suffix", &v, clean_written);
	free(v.p);
	memcpy(tail, ape_bare, ape_bare_len);
	memcpy(tail + ape_bare_len, id3v1, id3v1_len);
	v = cat(id3v2, id3v2_len, clean.p, clean.n, tail,
	        ape_bare_len + id3v1_len);
	expect_clean("ID3v2 prefix with APEv2 and ID3v1 suffix", &v,
	             clean_written);
	free(v.p);

	/* ends the decoder cannot use */
	memset(tail, 0, sizeof(tail));
	for (i = 1; i <= 3; i += 2) {
		/* 1 byte, and 3 bytes that are the start of a frame header */
		if (i == 3) {
			tail[0] = 0xff;
			tail[1] = 0xfb;
			tail[2] = 0x90;
		}
		snprintf(what, sizeof(what), "%u trailing byte%s", i,
		         i == 1 ? "" : "s");
		v = cat(NULL, 0, clean.p, clean.n, tail, i);
		expect_damaged(what, &v, clean_written);
		free(v.p);
	}
	memset(tail, 0x5a, 100);
	v = cat(NULL, 0, clean.p, clean.n, tail, 100);
	expect_damaged("100 trailing junk bytes", &v, clean_written);
	free(v.p);
	v = cat(NULL, 0, clean.p, clean.n - 1U, NULL, 0);
	expect_damaged("input cut by 1 byte", &v, clean_written);
	free(v.p);
	v = cat(NULL, 0, clean.p, clean.n - 100U, NULL, 0);
	expect_damaged("input cut by 100 bytes", &v, clean_written);
	free(v.p);
	v = cat(NULL, 0, clean.p, clean.n - 100U, id3v1, id3v1_len);
	expect_damaged("input cut by 100 bytes, then ID3v1", &v, clean_written);
	free(v.p);

	/* Whole frames cut off. A Xing or Info header declares the length, so
	 * that is damage. A stream without one declares nothing: the cut is
	 * indistinguishable from a complete file and is accepted. */
	{
		static size_t ends[4096];
		size_t nframes = frame_ends(&clean, start, ends, 4096U);
		unsigned cut;

		CHECK(nframes > 3U && ends[nframes - 1U] == clean.n,
		      "%s: frame walk found %lu frames, last ends at %lu of %lu",
		      name, (unsigned long)nframes,
		      (unsigned long)(nframes ? ends[nframes - 1U] : 0U),
		      (unsigned long)clean.n);
		for (cut = 1; cut <= 2 && nframes > 3U; cut++) {
			size_t keep = ends[nframes - 1U - cut];

			snprintf(what, sizeof(what), "last %u frame%s cut off", cut,
			         cut == 1 ? "" : "s");
			v = cat(NULL, 0, clean.p, keep, NULL, 0);
			if (expect_vbr_tag) {
				expect_damaged(what, &v, clean_written);
			} else {
				run(v.p, v.n, PCM_CAP, &o);
				CHECK(o.state == MP3_INPUT_COMPLETE && o.expect_end_ok &&
				      o.written < clean_written,
				      "%s: no length header: state %d ok=%d wrote %lu",
				      what, (int)o.state, o.expect_end_ok,
				      (unsigned long)o.written);
			}
			free(v.p);
			if (!expect_vbr_tag)
				continue;
			snprintf(what, sizeof(what), "last %u frame%s cut off, then "
			         "ID3v1", cut, cut == 1 ? "" : "s");
			v = cat(NULL, 0, clean.p, keep, id3v1, id3v1_len);
			expect_damaged(what, &v, clean_written);
			free(v.p);
			/* a full output buffer still means a partial decode */
			v = cat(NULL, 0, clean.p, keep, NULL, 0);
			run(v.p, v.n, clean_written / 2U, &o);
			CHECK(o.state == MP3_INPUT_MORE && o.expect_end_ok && !o.end,
			      "%s, half output: state %d ok=%d end=%d", what,
			      (int)o.state, o.expect_end_ok, o.end);
			free(v.p);
		}
	}
	/* Lyrics3v2 is not skipped, so it counts as trailing junk. */
	memcpy(tail, "LYRICSBEGINLYRICSEND000000LYRICS200", 35);
	tail_len = 35;
	v = cat(NULL, 0, clean.p, clean.n, tail, tail_len);
	expect_damaged("Lyrics3v2 suffix", &v, clean_written);
	free(v.p);
	/* APEv2 footers that cannot be real tags stay in the audio region. */
	for (i = 0; i < 2; i++) {
		static const uint32_t bogus[2] = { 0xfffffff0UL, 16U };
		uint8_t foot[256];
		size_t foot_len = make_ape(foot, 0, bogus[i], 1);

		snprintf(what, sizeof(what), "APEv2 footer claiming %lu bytes",
		         (unsigned long)bogus[i]);
		v = cat(NULL, 0, clean.p, clean.n, foot, foot_len);
		expect_damaged(what, &v, clean_written);
		free(v.p);
	}

	/* the output buffer decides where the decode stops */
	run(clean.p, clean.n, clean_written / 2U, &o);
	CHECK(o.written == clean_written / 2U, "half output: wrote %lu",
	      (unsigned long)o.written);
	CHECK(memcmp(g_pcm, g_clean_pcm, o.written) == 0,
	      "half output: PCM differs from the clean file");
	CHECK(o.state == MP3_INPUT_MORE && o.expect_end_ok,
	      "half output: state %d ok=%d, want MORE and accepted",
	      (int)o.state, o.expect_end_ok);
	CHECK(o.consumed > 0U && o.consumed < clean.n && !o.end,
	      "half output: consumed %lu of %lu end=%d", o.consumed,
	      (unsigned long)clean.n, o.end);
	CHECK(o.consumed >= start, "half output: consumed %lu before the first "
	      "frame at %lu", o.consumed, (unsigned long)start);

	run(clean.p, clean.n, clean_written, &o);
	CHECK(o.written == clean_written && o.end && o.expect_end_ok &&
	      o.state == MP3_INPUT_COMPLETE && o.consumed == clean.n,
	      "output exactly full: wrote %lu consumed %lu end=%d state=%d ok=%d",
	      (unsigned long)o.written, o.consumed, o.end, (int)o.state,
	      o.expect_end_ok);

	/* Output two bytes short: the last frame is only partly delivered, so
	 * it is not consumed and END stays clear. */
	run(clean.p, clean.n, clean_written - 2U, &o);
	CHECK(o.written == clean_written - 2U && !o.end && o.consumed < clean.n &&
	      o.expect_end_ok && o.state == MP3_INPUT_MORE,
	      "output two bytes short: wrote %lu consumed %lu end=%d state=%d "
	      "ok=%d", (unsigned long)o.written, o.consumed, o.end, (int)o.state,
	      o.expect_end_ok);
	/* The same with an ID3v1 tag: the tag is not consumed either. */
	v = cat(NULL, 0, clean.p, clean.n, id3v1, id3v1_len);
	run(v.p, v.n, clean_written - 2U, &o);
	CHECK(!o.end && o.consumed < clean.n && o.expect_end_ok,
	      "ID3v1, output two bytes short: consumed %lu end=%d ok=%d",
	      o.consumed, o.end, o.expect_end_ok);
	free(v.p);

	/* More frames than the Xing/Info header counts: the decoder stops at
	 * the counted end with bytes left over, and the output is not full. */
	if (expect_vbr_tag) {
		v = cat(clean.p, clean.n, clean.p + start, audio_len - start, NULL,
		        0);
		run(v.p, v.n, PCM_CAP, &o);
		CHECK(o.written == clean_written, "extra frames: wrote %lu, want %lu",
		      (unsigned long)o.written, (unsigned long)clean_written);
		CHECK(o.state == MP3_INPUT_MORE && !o.expect_end_ok && !o.end &&
		      o.consumed == clean.n,
		      "extra frames: state %d ok=%d end=%d consumed %lu of %lu "
		      "(%lu before the extra frames)", (int)o.state,
		      o.expect_end_ok, o.end, o.consumed, (unsigned long)v.n,
		      (unsigned long)clean.n);
		free(v.p);
	}

	free(clean.p);
}

static void test_no_frames(void)
{
	static uint8_t junk[4096];
	struct outcome o;

	memset(junk, 0x5a, sizeof(junk));
	run(junk, sizeof(junk), PCM_CAP, &o);
	/* init succeeds with no frame found; mp3_decode_compute() rejects the
	 * zero rate */
	CHECK(o.init_rc != 0 || mp3_get_hz(&g_ctx) == 0,
	      "input with no frames reports a stream rate");
}

/* mp3_decode_compute() must carry the flag from the request into the check,
 * and run the check before it touches the output. */
static void test_compute_source(const char *path)
{
	FILE *f = fopen(path, "rb");
	char *src;
	long n;
	const char *fn;
	const char *check;
	const char *swap;

	if (!f) {
		printf("FATAL: cannot open %s\n", path);
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	src = (char *)must_alloc((size_t)n + 1U);
	if (fread(src, 1U, (size_t)n, f) != (size_t)n) {
		printf("FATAL: short read %s\n", path);
		exit(2);
	}
	src[n] = 0;
	fclose(f);

	fn = strstr(src, "static uint16_t mp3_decode_compute(");
	CHECK(fn != NULL, "mp3_decode_compute not found in %s", path);
	if (fn) {
		check = strstr(fn, "(p->flags & SDK_AUDIO_DECODE_FLAG_EXPECT_END)");
		swap = strstr(fn, "byteswap_pcm16(");
		CHECK(check != NULL, "mp3_decode_compute does not test EXPECT_END");
		CHECK(check && strstr(check, "mp3_expect_end_ok(ctx, bytes_written, "
		                              "p->dst_cap)") != NULL,
		      "mp3_decode_compute does not call mp3_expect_end_ok");
		CHECK(check && swap && check < swap,
		      "the EXPECT_END check must come before the output swap");
		CHECK(check && strstr(check, "SDK_STATUS_BAD_REQUEST") != NULL &&
		      strstr(check, "SDK_STATUS_BAD_REQUEST") < swap,
		      "EXPECT_END failure is not BAD_REQUEST");
		/* The decoder writes the destination before any later return, so
		 * every return after decode_mp3_samples() must follow a cache
		 * flush of it. This is a source-structure check: sdk_mailbox.c
		 * cannot be linked into a host test to observe the mock. */
		{
			const char *dec = strstr(fn, "decode_mp3_samples(");
			const char *end = strstr(fn, "\nstatic uint16_t handle_decode_mp3");
			const char *flush = dec ? strstr(dec, "Xil_DCacheFlushRange(") :
			                          NULL;
			const char *ret = dec;
			int returns = 0;

			CHECK(dec && end && flush, "decode/flush not found in compute");
			while (dec && end && flush &&
			       (ret = strstr(ret, "return SDK_STATUS_")) && ret < end) {
				returns++;
				CHECK(flush < ret, "a return after the decode at offset %ld "
				      "precedes the destination flush", (long)(ret - fn));
				ret++;
			}
			CHECK(returns >= 2, "expected the BAD_REQUEST and OK returns "
			      "after the decode, found %d", returns);
			CHECK(check && (flush = strstr(check, "Xil_DCacheFlushRange(")) &&
			      flush < strstr(check, "return SDK_STATUS_BAD_REQUEST"),
			      "EXPECT_END failure does not flush the destination");
		}
	}
	CHECK(strstr(src, "p.flags = flags;") != NULL,
	      "handle_decode_mp3 does not pass the flags to the decode");
	free(src);
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		printf("usage: %s <fixtures dir> <sdk_mailbox.c>\n", argv[0]);
		return 2;
	}
	test_fixture(argv[1], "tone.mp3", 88200U, 2U, 1);
	test_fixture(argv[1], "tone_vbr_mono_48k.mp3", 24000U, 1U, 1);
	test_fixture(argv[1], "tone_noxing_44k.mp3", 19584U, 2U, 0);
	test_no_frames();
	test_compute_source(argv[2]);
	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("ok\n");
	return 0;
}
