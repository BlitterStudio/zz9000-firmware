CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c99
SANITIZE_CFLAGS ?= -O1 -g -Wall -Wextra -Werror -std=c99 \
	-fsanitize=address,undefined -fno-omit-frame-pointer
FIRMWARE_SRC := ../../ZZ9000_proto.sdk/ZZ9000OS/src

.PHONY: test clean

test: build/video_codec_test build/media_timeline_test build/media_codec_test \
	build/media_codec_test_sanitize
	build/video_codec_test
	build/media_timeline_test
	build/media_codec_test
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		build/media_codec_test_sanitize

build/video_codec_test: video_codec_test.c mpeg1_ps_fixture.inc \
	$(FIRMWARE_SRC)/sdk_video_plmpeg.c \
	$(FIRMWARE_SRC)/sdk_media_timeline.c \
	$(FIRMWARE_SRC)/sdk_video_yuy2.c \
	$(FIRMWARE_SRC)/sdk_video_backend.h \
	$(FIRMWARE_SRC)/sdk_video_yuy2.h \
	$(FIRMWARE_SRC)/third_party/pl_mpeg/pl_mpeg.h
	mkdir -p build
	$(CC) $(CFLAGS) -DSDK_VIDEO_HOST_TEST=1 -I$(FIRMWARE_SRC) \
		-o $@ video_codec_test.c $(FIRMWARE_SRC)/sdk_video_plmpeg.c \
		$(FIRMWARE_SRC)/sdk_media_timeline.c \
		$(FIRMWARE_SRC)/sdk_video_yuy2.c

build/media_timeline_test: media_timeline_test.c \
	$(FIRMWARE_SRC)/sdk_media_timeline.c \
	$(FIRMWARE_SRC)/sdk_media_timeline.h
	mkdir -p build
	$(CC) $(CFLAGS) -I$(FIRMWARE_SRC) -o $@ \
		media_timeline_test.c $(FIRMWARE_SRC)/sdk_media_timeline.c

build/media_codec_test: media_codec_test.c mpeg1_mp2_ps_fixture.inc \
	mpeg1_mp2_mono32_ps_fixture.inc \
	mpeg1_mp2_stereo48_ps_fixture.inc \
	mpeg1_ps_fixture.inc \
	$(FIRMWARE_SRC)/sdk_video_plmpeg.c \
	$(FIRMWARE_SRC)/sdk_media_timeline.c \
	$(FIRMWARE_SRC)/sdk_video_yuy2.c \
	$(FIRMWARE_SRC)/sdk_video_backend.h \
	$(FIRMWARE_SRC)/third_party/pl_mpeg/pl_mpeg.h
	mkdir -p build
	$(CC) $(CFLAGS) -DSDK_VIDEO_HOST_TEST=1 -I$(FIRMWARE_SRC) \
		-o $@ media_codec_test.c $(FIRMWARE_SRC)/sdk_video_plmpeg.c \
		$(FIRMWARE_SRC)/sdk_media_timeline.c \
		$(FIRMWARE_SRC)/sdk_video_yuy2.c

build/media_codec_test_sanitize: media_codec_test.c \
	mpeg1_mp2_ps_fixture.inc mpeg1_mp2_mono32_ps_fixture.inc \
	mpeg1_mp2_stereo48_ps_fixture.inc mpeg1_ps_fixture.inc \
	$(FIRMWARE_SRC)/sdk_video_plmpeg.c \
	$(FIRMWARE_SRC)/sdk_media_timeline.c \
	$(FIRMWARE_SRC)/sdk_video_yuy2.c \
	$(FIRMWARE_SRC)/sdk_video_backend.h \
	$(FIRMWARE_SRC)/third_party/pl_mpeg/pl_mpeg.h
	mkdir -p build
	$(CC) $(SANITIZE_CFLAGS) -DSDK_VIDEO_HOST_TEST=1 -I$(FIRMWARE_SRC) \
		-o $@ media_codec_test.c $(FIRMWARE_SRC)/sdk_video_plmpeg.c \
		$(FIRMWARE_SRC)/sdk_media_timeline.c \
		$(FIRMWARE_SRC)/sdk_video_yuy2.c

clean:
	rm -rf build
