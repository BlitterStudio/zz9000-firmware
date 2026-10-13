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

build/media_timeline_test: media_timeline_test.c \
	$(FIRMWARE_SRC)/sdk_media_timeline.c \
	$(FIRMWARE_SRC)/sdk_media_profile.c \
	$(FIRMWARE_SRC)/sdk_media_timeline.h
	mkdir -p build
	$(CC) $(CFLAGS) -I$(FIRMWARE_SRC) -o $@ \
		media_timeline_test.c $(FIRMWARE_SRC)/sdk_media_timeline.c

REPO_ROOT         := ../..
BUILD_DEPS_DIR    := $(FIRMWARE_SRC)/../build/deps
LIBMPEG2_HOST_LIB := $(BUILD_DEPS_DIR)/libmpeg2/0.5.1/host/libmpeg2.a
LIBA52_HOST_LIB   := $(BUILD_DEPS_DIR)/a52dec/0.7.4/host/liba52.a
DVD_INCLUDES      := -I$(BUILD_DEPS_DIR)/libmpeg2/0.5.1/host/include \
                     -I$(BUILD_DEPS_DIR)/a52dec/0.7.4/host/include
# The host codec archives use the firmware's short-enum ABI (see
# build_dvd_codecs.sh); everything linked with them must match.
DVD_ABI           := -fshort-enums

$(LIBMPEG2_HOST_LIB): $(REPO_ROOT)/build_dvd_codecs.sh
	cd $(REPO_ROOT) && bash ./build_dvd_codecs.sh --host

$(LIBA52_HOST_LIB): $(LIBMPEG2_HOST_LIB)

# The MPEG-1/MPEG-2 Program Stream backend (libmpeg2 video, minimp3 MPEG
# audio) and everything it links.
MPEG_SRCS := $(FIRMWARE_SRC)/sdk_video_mpeg2.c \
	$(FIRMWARE_SRC)/webm_parse.c \
	$(FIRMWARE_SRC)/webm_window.c \
	$(FIRMWARE_SRC)/sdk_dvd_ps.c \
	$(FIRMWARE_SRC)/sdk_dvd_ac3.c \
	$(FIRMWARE_SRC)/sdk_dvd_lpcm.c \
	$(FIRMWARE_SRC)/mp3/decode_mp3.c \
	$(FIRMWARE_SRC)/sdk_media_timeline.c \
	$(FIRMWARE_SRC)/sdk_media_profile.c \
	$(FIRMWARE_SRC)/sdk_video_yuy2.c \
	$(FIRMWARE_SRC)/sdk_mpeg2_neon.c
MPEG_LIBS := $(LIBMPEG2_HOST_LIB) $(LIBA52_HOST_LIB)
# The firmware headers those sources include: a change to one must rebuild
# every test binary below.
MPEG_HDRS := $(FIRMWARE_SRC)/sdk_video_backend.h \
	$(FIRMWARE_SRC)/sdk_dvd_ps.h \
	$(FIRMWARE_SRC)/sdk_dvd_ac3.h \
	$(FIRMWARE_SRC)/sdk_dvd_lpcm.h \
	$(FIRMWARE_SRC)/sdk_media_timeline.h \
	$(FIRMWARE_SRC)/sdk_media_profile.h \
	$(FIRMWARE_SRC)/sdk_video_yuy2.h \
	$(FIRMWARE_SRC)/sdk_mpeg2_neon.h \
	$(FIRMWARE_SRC)/mp3/mp3.h
MPEG_DEPS := $(MPEG_SRCS) $(MPEG_HDRS) $(MPEG_LIBS)
MPEG_FLAGS := $(DVD_ABI) -DSDK_VIDEO_HOST_TEST=1 -DSDK_MEDIA_HOST_TEST=1 \
	-I$(FIRMWARE_SRC) $(DVD_INCLUDES)

MPEG1_FIXTURES := mpeg1_ps_fixture.inc \
	mpeg1_crop_ps_fixture.inc \
	mpeg1_mp2_ps_fixture.inc \
	mpeg1_mp2_mono32_ps_fixture.inc \
	mpeg1_mp2_stereo48_ps_fixture.inc

DVD_FIXTURES := dvd_pal_25p_fixture.inc \
	dvd_pal_tff_fixture.inc \
	dvd_ntsc_30_fixture.inc \
	dvd_ntsc_film_32_fixture.inc \
	dvd_ac3_51_fixture.inc \
	dvd_ac3_192_fixture.inc \
	dvd_ac3_dual_fixture.inc

build/video_codec_test: video_codec_test.c $(MPEG1_FIXTURES) $(MPEG_DEPS)
	mkdir -p build
	$(CC) $(CFLAGS) $(MPEG_FLAGS) -o $@ video_codec_test.c $(MPEG_SRCS) \
		$(MPEG_LIBS) -lm

build/media_codec_test: media_codec_test.c $(MPEG1_FIXTURES) $(MPEG_DEPS)
	mkdir -p build
	$(CC) $(CFLAGS) $(MPEG_FLAGS) -o $@ media_codec_test.c $(MPEG_SRCS) \
		$(MPEG_LIBS) -lm

build/media_codec_test_sanitize: media_codec_test.c $(MPEG1_FIXTURES) \
	$(MPEG_DEPS)
	mkdir -p build
	$(CC) $(SANITIZE_CFLAGS) $(MPEG_FLAGS) -o $@ media_codec_test.c \
		$(MPEG_SRCS) $(MPEG_LIBS) -lm

build/dvd_codec_test: dvd_codec_test.c $(DVD_FIXTURES) $(MPEG_DEPS)
	mkdir -p build
	$(CC) $(CFLAGS) $(MPEG_FLAGS) -o $@ dvd_codec_test.c $(MPEG_SRCS) \
		$(MPEG_LIBS) -lm

build/dvd_codec_test_sanitize: dvd_codec_test.c $(DVD_FIXTURES) \
	$(MPEG_DEPS)
	mkdir -p build
	$(CC) $(SANITIZE_CFLAGS) $(MPEG_FLAGS) -o $@ dvd_codec_test.c \
		$(MPEG_SRCS) $(MPEG_LIBS) -lm

build/dvd_bench: dvd_bench.c $(DVD_FIXTURES) $(MPEG_DEPS)
	mkdir -p build
	$(CC) $(CFLAGS) $(MPEG_FLAGS) -o $@ dvd_bench.c $(MPEG_SRCS) \
		$(MPEG_LIBS) -lm
clean:
	rm -rf build
