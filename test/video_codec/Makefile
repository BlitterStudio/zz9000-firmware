CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c99
FIRMWARE_SRC := ../../ZZ9000_proto.sdk/ZZ9000OS/src

.PHONY: test clean

test: build/video_codec_test
	build/video_codec_test

build/video_codec_test: video_codec_test.c mpeg1_ps_fixture.inc \
	$(FIRMWARE_SRC)/sdk_video_plmpeg.c \
	$(FIRMWARE_SRC)/sdk_video_yuy2.c \
	$(FIRMWARE_SRC)/sdk_video_backend.h \
	$(FIRMWARE_SRC)/sdk_video_yuy2.h \
	$(FIRMWARE_SRC)/third_party/pl_mpeg/pl_mpeg.h
	mkdir -p build
	$(CC) $(CFLAGS) -DSDK_VIDEO_HOST_TEST=1 -I$(FIRMWARE_SRC) \
		-o $@ video_codec_test.c $(FIRMWARE_SRC)/sdk_video_plmpeg.c \
		$(FIRMWARE_SRC)/sdk_video_yuy2.c

clean:
	rm -rf build
