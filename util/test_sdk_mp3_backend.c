/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "../ZZ9000_proto.sdk/ZZ9000OS/src/mp3/mp3_backend_config.h"

#define MINIMP3_IMPLEMENTATION 1
#include "../ZZ9000_proto.sdk/ZZ9000OS/src/mp3/minimp3_ex.h"

int main(void) {
#ifndef ZZ9K_MP3_BACKEND_MINIMP3
	printf("missing ZZ9K_MP3_BACKEND_MINIMP3\n");
	return 1;
#endif
#ifdef MINIMP3_ONLY_MP3
	printf("unexpected MINIMP3_ONLY_MP3\n");
	return 2;
#endif
#ifndef MINIMP3_NO_STDIO
	printf("missing MINIMP3_NO_STDIO\n");
	return 3;
#endif
#ifdef MINIMP3_FLOAT_OUTPUT
	printf("unexpected MINIMP3_FLOAT_OUTPUT\n");
	return 4;
#endif

	mp3dec_t dec;
	mp3dec_frame_info_t info;
	short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
	unsigned char empty[1] = {0};

	memset(&dec, 0xa5, sizeof(dec));
	memset(&info, 0, sizeof(info));
	memset(pcm, 0, sizeof(pcm));
	mp3dec_init(&dec);

	if (mp3dec_decode_frame(&dec, empty, 0, pcm, &info) != 0) {
		printf("empty decode produced samples\n");
		return 5;
	}
	if (info.frame_bytes != 0) {
		printf("empty decode consumed bytes\n");
		return 6;
	}
	if (MINIMP3_BUF_SIZE < 16 * 1024) {
		printf("mp3 stream buffer too small\n");
		return 7;
	}

	printf("mp3 backend config ok\n");
	return 0;
}
