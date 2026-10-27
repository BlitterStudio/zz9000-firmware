CC ?= cc
AR ?= ar
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -O2
ROOT := ../..
FIRMWARE := $(ROOT)/ZZ9000_proto.sdk/ZZ9000OS/src
CPPFLAGS := -I$(FIRMWARE) -DMINIGL_HOST_TEST

MW_OBJS := mw_parse.o mw_texture.o mw_draw.o mw_ctx.o mw_service.o \
           minigl_raster.o minigl_lighting.o minigl_present.o \
           minigl_rix_backend.o rix_cmd.o rix_texture.o rix_setup.o rix_bin.o rix_drv.o

TESTS := test_mw_validate test_mw_state test_mw_draw test_mw_texture test_mw_host_api test_mw_e2e test_mw_rasterix_mapper

AMIBERRY_DIR ?= $(ROOT)/../amiberry-minigl-library-mw-e2e
ifeq ($(wildcard $(AMIBERRY_DIR)/src/amiberry_transport_zz9000.c),)
AMIBERRY_DIR := $(ROOT)/../amiberry-minigl-library
endif
MOCK_AMIGA := ./mock_amiga

.PHONY: all test asan clean

all: libmw_host.a $(TESTS)

test: all
	./test_mw_validate
	./test_mw_state
	./test_mw_draw
	./test_mw_texture
	./test_mw_host_api
	./test_mw_e2e
	./test_mw_rasterix_mapper

asan: CFLAGS += -fsanitize=address,undefined -g
asan: clean test

libmw_host.a: $(MW_OBJS)
	$(AR) rcs $@ $^

%.o: $(FIRMWARE)/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

test_mw_validate: test_mw_validate.c libmw_host.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@

test_mw_state: test_mw_state.c libmw_host.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@

test_mw_draw: test_mw_draw.c libmw_host.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@

test_mw_texture: test_mw_texture.c libmw_host.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@

test_mw_host_api: test_mw_host_api.c libmw_host.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@
test_mw_e2e: test_mw_e2e.c scheduler_arm_host.o amiberry_transport_zz9000.o libmw_host.a
	$(CC) $(CPPFLAGS) -I$(MOCK_AMIGA) -I$(AMIBERRY_DIR)/include -I$(AMIBERRY_DIR)/src $(CFLAGS) $^ -lpthread -lm -o $@
test_mw_rasterix_mapper: test_mw_rasterix_mapper.c libmw_host.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@
rix_v9_capture: rix_v9_capture.c libmw_host.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@

scheduler_arm_host.o: $(FIRMWARE)/scheduler_arm.c $(FIRMWARE)/scheduler_core1_minigl_init.h
	$(CC) $(CPPFLAGS) -DTASKQ_HOST_TEST $(CFLAGS) -c $< -o $@

amiberry_transport_zz9000.o: $(AMIBERRY_DIR)/src/amiberry_transport_zz9000.c
	$(CC) -I$(MOCK_AMIGA) -I$(AMIBERRY_DIR)/include -I$(AMIBERRY_DIR)/src $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o libmw_host.a $(TESTS)
