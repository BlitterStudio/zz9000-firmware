/*
 * Host tests for the WebM demux bounds: hostile element sizes, Segment
 * sizes past 32 bits and header prefixes split at every byte.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ebml_build.h"
#include "webm_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOSTILE 0xFFFFFFF4ULL

/* An element header claiming `size` bytes, followed by a little filler so
 * the parent has bytes after the header. */
static void bhostile(struct buf *b, uint32_t id, uint64_t size)
{
	static const uint8_t fill[4] = {0, 0, 0, 0};

	bid(b, id);
	bsize64(b, size);
	bput(b, fill, sizeof(fill));
}

/* Hostile sizes must fail closed with WEBM_ERR_LAYOUT: never wrap an
 * offset, re-parse earlier bytes or loop. `where` picks the element that
 * carries the hostile size. */
enum {
	H_TRACKS_CHILD,
	H_INFO_CHILD,
	H_TRACKENTRY_CHILD,
	H_SEGMENT_CHILD,
	H_CLUSTER_CHILD,
	H_CLUSTER,
	H_GROUP,
	H_GROUP_CHILD,
	H_GROUP_BLOCK64,
	H_SIMPLE64,
	H_COUNT
};

static struct buf make_hostile(int where, int seg_mode)
{
	struct buf seg, info, tracks, track, video, cluster, group;
	uint8_t scale[3] = {0x0F, 0x42, 0x40};
	uint8_t frame[3] = {0x10, 0, 0};

	memset(&seg, 0, sizeof(seg));
	memset(&info, 0, sizeof(info));
	memset(&tracks, 0, sizeof(tracks));
	memset(&track, 0, sizeof(track));
	memset(&video, 0, sizeof(video));
	memset(&cluster, 0, sizeof(cluster));
	memset(&group, 0, sizeof(group));
	belem(&info, 0x2AD7B1, scale, 3);
	if (where == H_INFO_CHILD)
		bhostile(&info, 0x4D80, HOSTILE);
	bnest(&seg, 0x1549A966, &info);
	bu16(&video, 0xB0, 160);
	bu16(&video, 0xBA, 120);
	bu8(&track, 0xD7, 1);
	bu8(&track, 0x83, 1);
	belem(&track, 0x86, "V_VP8", 5);
	bnest(&track, 0xE0, &video);
	if (where == H_TRACKENTRY_CHILD)
		bhostile(&track, 0x536E, HOSTILE);
	bnest(&tracks, 0xAE, &track);
	if (where == H_TRACKS_CHILD)
		bhostile(&tracks, 0xAE, HOSTILE);
	bnest(&seg, 0x1654AE6B, &tracks);
	if (where == H_SEGMENT_CHILD)
		bhostile(&seg, 0xEC, HOSTILE);
	bu8(&cluster, 0xE7, 0);
	add_block(&cluster, 0xA3, 1, 0, 0x80, frame, 3);
	if (where == H_CLUSTER_CHILD)
		bhostile(&cluster, 0xEC, HOSTILE);
	if (where == H_GROUP)
		bhostile(&cluster, 0xA0, HOSTILE);
	if (where == H_SIMPLE64)
		bhostile(&cluster, 0xA3, 0x100000010ULL);
	if (where == H_GROUP_CHILD || where == H_GROUP_BLOCK64) {
		if (where == H_GROUP_CHILD)
			bhostile(&group, 0xEC, HOSTILE);
		else
			bhostile(&group, 0xA1, 0x100000010ULL);
		add_block(&group, 0xA1, 1, 1, 0, frame, 3);
		bnest(&cluster, 0xA0, &group);
	}
	if (where == H_CLUSTER) {
		bhostile(&seg, 0x1F43B675, HOSTILE);
		free(cluster.p);
	} else {
		bnest(&seg, 0x1F43B675, &cluster);
	}
	return make_file(&seg, seg_mode);
}

static int test_hostile_sizes(void)
{
	static struct webm_demux d;
	static struct got g;
	int where, mode, failed = 0;

	for (where = 0; where < H_COUNT; where++) {
		for (mode = SEG_SIZED; mode <= SEG_UNKNOWN; mode++) {
			int eof;

			/* A Cluster in an unbounded stream counts as unknown
			 * size instead (covered by test_segment_sizes). */
			if (where == H_CLUSTER && mode == SEG_UNKNOWN)
				continue;
			for (eof = 0; eof < 2; eof++) {
				struct buf file = make_hostile(where, mode);
				int rc = collect(&file, eof, &d, &g);

				if (rc != -1 || d.error != WEBM_ERR_LAYOUT) {
					fprintf(stderr,
						"hostile %d seg=%d eof=%d: rc=%d err=%d n=%u\n",
						where, mode, eof, rc, d.error, g.n);
					failed = 1;
				}
				free(file.p);
			}
		}
	}
	return failed;
}

/* A Segment whose declared end does not fit 32 bits is bounded by the
 * input (or unbounded), not by a wrapped offset. */
static int test_segment_sizes(void)
{
	static struct webm_demux d;
	static struct got g;
	int mode, eof, failed = 0;

	for (mode = SEG_4GB; mode <= SEG_EDGE; mode++) {
		for (eof = 0; eof < 2; eof++) {
			struct buf seg, cluster, file;
			uint8_t frame[3] = {0x10, 0, 0};
			int rc;

			memset(&seg, 0, sizeof(seg));
			memset(&cluster, 0, sizeof(cluster));
			add_head(&seg, 0);
			bu8(&cluster, 0xE7, 0);
			add_block(&cluster, 0xA3, 1, 0, 0x80, frame, 3);
			bnest(&seg, 0x1F43B675, &cluster);
			file = make_file(&seg, mode);
			rc = collect(&file, eof, &d, &g);
			if (g.n != 1U || d.segment_end != (eof ? file.n : 0U) ||
			    (eof ? rc != 0 : d.error != WEBM_ERR_NEED)) {
				fprintf(stderr,
					"segment mode=%d eof=%d: rc=%d err=%d n=%u end=%u\n",
					mode, eof, rc, d.error, g.n,
					d.segment_end);
				failed = 1;
			}
			free(file.p);
		}
	}
	/* A 4 GB Cluster in an unbounded Segment runs like unknown size. */
	{
		struct buf seg, file;
		uint8_t frame[3] = {0x10, 0, 0};
		int rc;

		memset(&seg, 0, sizeof(seg));
		add_head(&seg, 0);
		bid(&seg, 0x1F43B675);
		bsize64(&seg, 0x100000010ULL);
		bu8(&seg, 0xE7, 7);
		add_block(&seg, 0xA3, 1, 0, 0x80, frame, 3);
		file = make_file(&seg, SEG_UNKNOWN);
		rc = collect(&file, 0, &d, &g);
		if (g.n != 1U || g.tc[0] != 7 || d.error != WEBM_ERR_NEED) {
			fprintf(stderr, "4 GB cluster: rc=%d err=%d n=%u\n", rc,
				d.error, g.n);
			failed = 1;
		}
		free(file.p);
	}
	return failed;
}

/* Every prefix of a valid header is "need more input", never an error:
 * a client may split the header across writes at any byte. */
static int test_header_prefixes(void)
{
	static struct webm_demux d;
	static uint8_t scratch[8192];
	struct buf seg, cluster, file;
	uint8_t frame[3] = {0x10, 0, 0};
	uint32_t len;
	int failed = 0;

	memset(&seg, 0, sizeof(seg));
	memset(&cluster, 0, sizeof(cluster));
	add_head(&seg, 0);
	bu8(&cluster, 0xE7, 0);
	add_block(&cluster, 0xA3, 1, 0, 0x80, frame, 3);
	bnest(&seg, 0x1F43B675, &cluster);
	file = make_file(&seg, SEG_UNKNOWN);
	for (len = 0U; len < file.n; len++) {
		int rc = open_file(file.p, len, 0, &d, scratch, sizeof(scratch));

		if (rc != 0 && d.error != WEBM_ERR_NEED) {
			fprintf(stderr, "prefix %u/%u: err=%d\n", len, file.n,
				d.error);
			failed = 1;
		}
	}
	free(file.p);
	return failed;
}

int main(void)
{
	int failed = 0;

	failed |= test_hostile_sizes();
	failed |= test_segment_sizes();
	failed |= test_header_prefixes();
	if (failed)
		fprintf(stderr, "webm_bounds_test failed\n");
	return failed ? 1 : 0;
}
