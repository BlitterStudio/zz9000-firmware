CC ?= cc
AR ?= ar
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -O2
ROOT := ../..
FIRMWARE := $(ROOT)/ZZ9000_proto.sdk/ZZ9000OS/src
# Keep host coverage on the same backend inventory as the module build.
MINIGL_MODULE_BACKEND ?= $(FIRMWARE)
CPPFLAGS := -I$(FIRMWARE) -DMINIGL_HOST_TEST

MW_OBJS := minigl_platform.o mw_parse.o mw_texture.o mw_draw.o mw_ctx.o mw_service.o sdk_palette.o \
           minigl_raster.o minigl_lighting.o minigl_present.o \
           minigl_rix_backend.o rix_cmd.o rix_texture.o rix_setup.o rix_drv.o
POOL_OBJS := $(addprefix pool_,$(MW_OBJS)) pool_card_pool.o \
             pool_sdk_smp_lock.o pool_host_card_pool.o

TESTS := test_mw_validate test_mw_state test_mw_draw test_mw_texture test_mw_host_api \
         test_mw_card_pool test_mw_rasterix_mapper test_mw_transport_guard

.PHONY: all test asan clean

all: libmw_host.a $(TESTS)

test: all
	./test_mw_validate
	./test_mw_state
	./test_mw_draw
	./test_mw_texture
	./test_mw_host_api
	./test_mw_card_pool
	./test_mw_transport_guard
	./test_mw_rasterix_mapper

asan: CFLAGS += -fsanitize=address,undefined -g
asan: clean test

libmw_host.a: $(MW_OBJS)
	$(AR) rcs $@ $^

%.o: $(MINIGL_MODULE_BACKEND)/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

test_mw_validate: test_mw_validate.c libmw_host.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@

test_mw_state: test_mw_state.c libmw_host.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@

test_mw_draw: test_mw_draw.c libmw_host.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@

test_mw_texture: test_mw_texture.c libmw_host.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@
test_mw_transport_guard: test_mw_transport_guard.c libmw_host.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@

test_mw_host_api: test_mw_host_api.c libmw_host.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@
pool_%.o: $(MINIGL_MODULE_BACKEND)/%.c
	$(CC) $(CPPFLAGS) -DMINIGL_CARD_POOL_TEST -I../card_pool $(CFLAGS) -c $< -o $@
pool_card_pool.o: $(FIRMWARE)/card_pool.c
	$(CC) $(CPPFLAGS) -DMINIGL_CARD_POOL_TEST -I../card_pool $(CFLAGS) -c $< -o $@
pool_sdk_smp_lock.o: $(FIRMWARE)/sdk_smp_lock.c
	$(CC) $(CPPFLAGS) -DMINIGL_CARD_POOL_TEST -DSMP_LOCK_HOST_TEST -I../card_pool $(CFLAGS) -c $< -o $@
pool_host_card_pool.o: ../card_pool/host_card_pool.c
	$(CC) $(CPPFLAGS) -DMINIGL_CARD_POOL_TEST -I../card_pool $(CFLAGS) -c $< -o $@
test_mw_card_pool: test_mw_card_pool.c $(POOL_OBJS)
	$(CC) $(CPPFLAGS) -DMINIGL_CARD_POOL_TEST -I../card_pool $(CFLAGS) $^ -lm -o $@
test_mw_rasterix_mapper: test_mw_rasterix_mapper.c libmw_host.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@
rix_v9_capture: rix_v9_capture.c libmw_host.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@

clean:
	rm -f *.o libmw_host.a $(TESTS)
