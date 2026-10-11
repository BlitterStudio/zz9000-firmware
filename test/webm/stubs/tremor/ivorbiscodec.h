/*
 * Host-test stand-in for the Tremor declarations sdk_video_webm.c uses.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef STUB_IVORBISCODEC_H
#define STUB_IVORBISCODEC_H

#include <stdint.h>

typedef int32_t ogg_int32_t;
typedef int64_t ogg_int64_t;

typedef struct {
	unsigned char *packet;
	long bytes;
	long b_o_s;
	long e_o_s;
	ogg_int64_t granulepos;
	ogg_int64_t packetno;
} ogg_packet;

typedef struct {
	int version;
	int channels;
	long rate;
	void *codec_setup;
} vorbis_info;

typedef struct {
	int comments;
} vorbis_comment;

typedef struct {
	int unused;
} vorbis_dsp_state;

typedef struct {
	int unused;
} vorbis_block;

void vorbis_info_init(vorbis_info *vi);
void vorbis_info_clear(vorbis_info *vi);
void vorbis_comment_init(vorbis_comment *vc);
void vorbis_comment_clear(vorbis_comment *vc);
int vorbis_synthesis_headerin(vorbis_info *vi, vorbis_comment *vc,
			      ogg_packet *op);
int vorbis_synthesis_init(vorbis_dsp_state *v, vorbis_info *vi);
int vorbis_block_init(vorbis_dsp_state *v, vorbis_block *vb);
int vorbis_block_clear(vorbis_block *vb);
void vorbis_dsp_clear(vorbis_dsp_state *v);
int vorbis_synthesis(vorbis_block *vb, ogg_packet *op);
int vorbis_synthesis_blockin(vorbis_dsp_state *v, vorbis_block *vb);
int vorbis_synthesis_pcmout(vorbis_dsp_state *v, ogg_int32_t ***pcm);
int vorbis_synthesis_read(vorbis_dsp_state *v, int samples);

#endif
