CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c99
SANITIZE_CFLAGS ?= -O1 -g -Wall -Wextra -Werror -std=c99 \
	-fsanitize=address,undefined -fno-omit-frame-pointer
FIRMWARE_SRC := ../../ZZ9000_proto.sdk/ZZ9000OS/src

.PHONY: test clean

test: build/video_codec_test build/media_timeline_test build/media_codec_test \
	build/media_codec_test_sanitize build/dvd_codec_test build/dvd_codec_test_sanitize
	build/video_codec_test
	build/media_timeline_test
	build/media_codec_test
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		build/media_codec_test_sanitize
	build/dvd_codec_test
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		build/dvd_codec_test_sanitize

build/video_codec_test: video_codec_test.c mpeg1_ps_fixture.inc \
	mpeg2_backend_stub.c \
	$(FIRMWARE_SRC)/sdk_video_plmpeg.c \
	$(FIRMWARE_SRC)/sdk_media_timeline.c \
	$(FIRMWARE_SRC)/sdk_media_profile.c \
	$(FIRMWARE_SRC)/sdk_video_yuy2.c \
	$(FIRMWARE_SRC)/sdk_video_backend.h \
	$(FIRMWARE_SRC)/sdk_media_profile.h \
	$(FIRMWARE_SRC)/sdk_video_yuy2.h \
	$(FIRMWARE_SRC)/third_party/pl_mpeg/pl_mpeg.h
	mkdir -p build
	$(CC) $(CFLAGS) -DSDK_VIDEO_HOST_TEST=1 -DSDK_MEDIA_HOST_TEST=1 \
		-I$(FIRMWARE_SRC) \
		-o $@ video_codec_test.c mpeg2_backend_stub.c \
		$(FIRMWARE_SRC)/sdk_video_plmpeg.c \
		$(FIRMWARE_SRC)/sdk_media_timeline.c \
		$(FIRMWARE_SRC)/sdk_media_profile.c \
		$(FIRMWARE_SRC)/sdk_video_yuy2.c

build/media_timeline_test: media_timeline_test.c \
	$(FIRMWARE_SRC)/sdk_media_timeline.c \
	$(FIRMWARE_SRC)/sdk_media_profile.c \
	$(FIRMWARE_SRC)/sdk_media_timeline.h
	mkdir -p build
	$(CC) $(CFLAGS) -I$(FIRMWARE_SRC) -o $@ \
		media_timeline_test.c $(FIRMWARE_SRC)/sdk_media_timeline.c

build/media_codec_test: media_codec_test.c mpeg1_mp2_ps_fixture.inc \
	mpeg1_mp2_mono32_ps_fixture.inc \
	mpeg1_mp2_stereo48_ps_fixture.inc \
	mpeg1_ps_fixture.inc \
	mpeg2_backend_stub.c \
	$(FIRMWARE_SRC)/sdk_video_plmpeg.c \
	$(FIRMWARE_SRC)/sdk_media_timeline.c \
	$(FIRMWARE_SRC)/sdk_media_profile.c \
	$(FIRMWARE_SRC)/sdk_video_yuy2.c \
	$(FIRMWARE_SRC)/sdk_video_backend.h \
	$(FIRMWARE_SRC)/third_party/pl_mpeg/pl_mpeg.h
	mkdir -p build
	$(CC) $(CFLAGS) -DSDK_VIDEO_HOST_TEST=1 -DSDK_MEDIA_HOST_TEST=1 -I$(FIRMWARE_SRC) \
		-o $@ media_codec_test.c mpeg2_backend_stub.c \
		$(FIRMWARE_SRC)/sdk_video_plmpeg.c \
		$(FIRMWARE_SRC)/sdk_media_timeline.c \
		$(FIRMWARE_SRC)/sdk_media_profile.c \
		$(FIRMWARE_SRC)/sdk_video_yuy2.c

build/media_codec_test_sanitize: media_codec_test.c \
	mpeg1_mp2_ps_fixture.inc mpeg1_mp2_mono32_ps_fixture.inc \
	mpeg1_mp2_stereo48_ps_fixture.inc mpeg1_ps_fixture.inc \
	mpeg2_backend_stub.c \
	$(FIRMWARE_SRC)/sdk_video_plmpeg.c \
	$(FIRMWARE_SRC)/sdk_media_timeline.c \
	$(FIRMWARE_SRC)/sdk_media_profile.c \
	$(FIRMWARE_SRC)/sdk_video_yuy2.c \
	$(FIRMWARE_SRC)/sdk_video_backend.h \
	$(FIRMWARE_SRC)/third_party/pl_mpeg/pl_mpeg.h
	mkdir -p build
	$(CC) $(SANITIZE_CFLAGS) -DSDK_VIDEO_HOST_TEST=1 -DSDK_MEDIA_HOST_TEST=1 -I$(FIRMWARE_SRC) \
		-o $@ media_codec_test.c mpeg2_backend_stub.c \
		$(FIRMWARE_SRC)/sdk_video_plmpeg.c \
		$(FIRMWARE_SRC)/sdk_media_timeline.c \
		$(FIRMWARE_SRC)/sdk_media_profile.c \
		$(FIRMWARE_SRC)/sdk_video_yuy2.c

REPO_ROOT         := ../..
BUILD_DEPS_DIR    := $(FIRMWARE_SRC)/../build/deps
LIBMPEG2_HOST_LIB := $(BUILD_DEPS_DIR)/libmpeg2/0.5.1/host/libmpeg2.a
LIBA52_HOST_LIB   := $(BUILD_DEPS_DIR)/a52dec/0.7.4/host/liba52.a
DVD_INCLUDES      := -I$(BUILD_DEPS_DIR)/libmpeg2/0.5.1/host/include \
                     -I$(BUILD_DEPS_DIR)/a52dec/0.7.4/host/include

$(LIBMPEG2_HOST_LIB): $(REPO_ROOT)/build_dvd_codecs.sh
	cd $(REPO_ROOT) && bash ./build_dvd_codecs.sh --host

$(LIBA52_HOST_LIB): $(LIBMPEG2_HOST_LIB)

DVD_SRCS := $(FIRMWARE_SRC)/sdk_video_mpeg2.c \
	$(FIRMWARE_SRC)/sdk_dvd_ps.c \
	$(FIRMWARE_SRC)/sdk_dvd_ac3.c \
	$(FIRMWARE_SRC)/sdk_dvd_lpcm.c \
	$(FIRMWARE_SRC)/sdk_video_plmpeg.c \
	$(FIRMWARE_SRC)/sdk_media_timeline.c \
	$(FIRMWARE_SRC)/sdk_media_profile.c \
	$(FIRMWARE_SRC)/sdk_video_yuy2.c

DVD_FIXTURES := dvd_pal_25p_fixture.inc \
	dvd_pal_tff_fixture.inc \
	dvd_ntsc_30_fixture.inc \
	dvd_ntsc_film_32_fixture.inc \
	dvd_ac3_51_fixture.inc

build/dvd_codec_test: dvd_codec_test.c $(DVD_FIXTURES) $(DVD_SRCS) \
	$(LIBMPEG2_HOST_LIB) $(LIBA52_HOST_LIB)
	mkdir -p build
	$(CC) $(CFLAGS) -DSDK_VIDEO_HOST_TEST=1 -DSDK_MEDIA_HOST_TEST=1 \
		-I$(FIRMWARE_SRC) $(DVD_INCLUDES) \
		-o $@ dvd_codec_test.c $(DVD_SRCS) \
		$(LIBMPEG2_HOST_LIB) $(LIBA52_HOST_LIB) -lm

build/dvd_codec_test_sanitize: dvd_codec_test.c $(DVD_FIXTURES) $(DVD_SRCS) \
	$(LIBMPEG2_HOST_LIB) $(LIBA52_HOST_LIB)
	mkdir -p build
	$(CC) $(SANITIZE_CFLAGS) -DSDK_VIDEO_HOST_TEST=1 -DSDK_MEDIA_HOST_TEST=1 \
		-I$(FIRMWARE_SRC) $(DVD_INCLUDES) \
		-o $@ dvd_codec_test.c $(DVD_SRCS) \
		$(LIBMPEG2_HOST_LIB) $(LIBA52_HOST_LIB) -lm

build/dvd_bench: dvd_bench.c $(DVD_FIXTURES) $(DVD_SRCS) \
	$(LIBMPEG2_HOST_LIB) $(LIBA52_HOST_LIB)
	mkdir -p build
	$(CC) $(CFLAGS) -DSDK_VIDEO_HOST_TEST=1 -DSDK_MEDIA_HOST_TEST=1 \
		-I$(FIRMWARE_SRC) $(DVD_INCLUDES) \
		-o $@ dvd_bench.c $(DVD_SRCS) \
		$(LIBMPEG2_HOST_LIB) $(LIBA52_HOST_LIB) -lm
clean:
	rm -rf build
