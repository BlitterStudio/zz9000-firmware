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
# Prefer a library checkout with the descriptor-based transport: the zz9k
# worktree, then the historical e2e checkout, then the plain checkout.
AMIBERRY_DIR ?= $(patsubst %/src/amiberry_transport_zz9000.c,%,$(firstword $(wildcard \
  $(ROOT)/../amiberry-minigl-library-zz9k/src/amiberry_transport_zz9000.c \
  $(ROOT)/../amiberry-minigl-library-mw-e2e/src/amiberry_transport_zz9000.c \
  $(ROOT)/../amiberry-minigl-library/src/amiberry_transport_zz9000.c)))
ifeq ($(AMIBERRY_DIR),)
AMIBERRY_DIR := $(ROOT)/../amiberry-minigl-library
endif
MOCK_AMIGA := ./mock_amiga

# The E2E test compiles the real library transport; without a sibling
# library checkout it is skipped (full coverage runs where the library
# repo is available).
GUEST_MOCK := ./guest_mock/amiga
CLIENTS_CPPFLAGS := -I$(GUEST_MOCK) -I$(AMIBERRY_DIR)/include \
                    -I$(AMIBERRY_DIR)/src -I$(AMIBERRY_DIR)/vendor/minigl-framework/include \
                    -I$(AMIBERRY_DIR)/vendor/minigl-framework/library
# The guest harness builds 32-bit: the Amiga ABI it reproduces has 32-bit
# pointers AND 32-bit ULONG, and the clients walk the dispatch table and
# tag lists as ULONG arrays -- well-defined only when the two widths match.
# -m32 needs gcc-multilib/libc6-dev-i386 on the host.
CLIENTS_M32 := -m32
CLIENTS_OBJS := client_guest_mock.o client_harness.o \
                client_backend.o client_dispatch.o client_transport.o client_state.o \
                client_draw.o client_texture.o client_light.o client_light_get.o \
                client_list.o client_glu.o client_glu_quad.o client_glut.o \
                client_glut_shapes.o client_debug.o client_open.o \
                client_guest_ops.o client_guest_v29.o client_guest_reopen.o \
                client_fw_mw_parse.o client_fw_mw_texture.o client_fw_mw_draw.o \
                client_fw_mw_ctx.o client_fw_mw_service.o client_fw_sdk_palette.o client_fw_minigl_raster.o \
                client_fw_minigl_lighting.o client_fw_minigl_present.o \
                client_fw_minigl_rix_backend.o client_fw_rix_cmd.o \
                client_fw_rix_texture.o client_fw_rix_setup.o client_fw_rix_drv.o
# MiniGL V30 moved buffer objects into their own source file.
CLIENTS_OBJS += $(if $(wildcard $(AMIBERRY_DIR)/src/amiberry_gl_buffer.c),client_buffer.o)
ifneq ($(wildcard $(AMIBERRY_DIR)/src/amiberry_transport_zz9000.c),)
TESTS += test_mw_e2e
E2E_RUN = ./test_mw_e2e
else
$(warning amiberry-minigl-library not found at $(AMIBERRY_DIR); skipping test_mw_e2e)
E2E_RUN = @echo "test_mw_e2e skipped: amiberry-minigl-library not found"
endif
CLIENTS_AVAILABLE := $(wildcard $(AMIBERRY_DIR)/src/amiberry_transport_zz9000.c)
ifneq ($(CLIENTS_AVAILABLE),)
TESTS += test_mw_e2e_clients
CLIENTS_RUN = ./test_mw_e2e_clients
else
CLIENTS_RUN = @echo "test_mw_e2e_clients skipped: amiberry-minigl-library not found"
endif

.PHONY: all test asan clean check_clients_m32

all: libmw_host.a $(TESTS)

test: all
	./test_mw_validate
	./test_mw_state
	./test_mw_draw
	./test_mw_texture
	./test_mw_host_api
	./test_mw_card_pool
	$(E2E_RUN)
	./test_mw_transport_guard
	./test_mw_rasterix_mapper

	$(CLIENTS_RUN)

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


# --- Guest-client harness: the real library + real guest clients, driven
# through the mock AmigaOS in guest_mock/ against the real mw_service. ---
check_clients_m32:
	@printf 'int main(void) { return 0; }\n' | $(CC) $(CLIENTS_M32) -x c - -o .clients-m32-probe
	@rm -f .clients-m32-probe

test_mw_e2e_clients: check_clients_m32 $(CLIENTS_OBJS)
	$(CC) $(CLIENTS_M32) $(CFLAGS) $(filter-out check_clients_m32,$^) -lpthread -lm -o $@

client_guest_mock.o: guest_mock/guest_mock.c
	$(CC) $(CLIENTS_M32) $(CLIENTS_CPPFLAGS) $(CFLAGS) -c $< -o $@

client_harness.o: test_mw_e2e_clients.c
	$(CC) $(CLIENTS_M32) $(CPPFLAGS) $(CLIENTS_CPPFLAGS) -DMINIGL_DISPATCH_ABI_TYPES $(CFLAGS) -c $< -o $@

# Host-compat: -fno-strict-aliasing matches the library's own build (float
# bit-punning in the packed emitter). The relaxed diagnostics only cover
# host-compiler noise; behaviour matches the m68k build.
CLIENTS_LIB_CFLAGS := $(CLIENTS_CPPFLAGS) $(CFLAGS) -fno-builtin \
                      -fno-strict-aliasing -Wno-error=overflow -Wno-error=type-limits \
                      -Wno-error=implicit-fallthrough \
                      -DMINIGL_LIBRARY_BUILD -DMINIGL_BACKEND_AMIBERRY \
                      -DMINIGL_DISPATCH_ABI_TYPES -DMINIGL_GUEST_HOST_TEST -Dmemcpy=amiga_memcpy \
                      $(CLIENTS_M32)
client_backend.o: $(AMIBERRY_DIR)/src/amiberry_backend.c
	$(CC) $(CLIENTS_LIB_CFLAGS) -c $< -o $@
client_dispatch.o: $(AMIBERRY_DIR)/src/dispatch_table_amiberry.c
	$(CC) $(CLIENTS_LIB_CFLAGS) -c $< -o $@
client_transport.o: $(AMIBERRY_DIR)/src/amiberry_transport_zz9000.c
	$(CC) $(CLIENTS_LIB_CFLAGS) -c $< -o $@
client_state.o: $(AMIBERRY_DIR)/src/amiberry_gl_state.c
	$(CC) $(CLIENTS_LIB_CFLAGS) -c $< -o $@
client_draw.o: $(AMIBERRY_DIR)/src/amiberry_gl_draw.c
	$(CC) $(CLIENTS_LIB_CFLAGS) -c $< -o $@
client_texture.o: $(AMIBERRY_DIR)/src/amiberry_gl_texture.c
	$(CC) $(CLIENTS_LIB_CFLAGS) -c $< -o $@
client_buffer.o: $(AMIBERRY_DIR)/src/amiberry_gl_buffer.c
	$(CC) $(CLIENTS_LIB_CFLAGS) -c $< -o $@
client_light.o: $(AMIBERRY_DIR)/src/amiberry_gl_light.c
	$(CC) $(CLIENTS_LIB_CFLAGS) -c $< -o $@
client_light_get.o: $(AMIBERRY_DIR)/src/amiberry_gl_light_get.c
	$(CC) $(CLIENTS_LIB_CFLAGS) -c $< -o $@
client_list.o: $(AMIBERRY_DIR)/src/amiberry_gl_list.c
	$(CC) $(CLIENTS_LIB_CFLAGS) -c $< -o $@
client_glu.o: $(AMIBERRY_DIR)/src/amiberry_glu.c
	$(CC) $(CLIENTS_LIB_CFLAGS) -c $< -o $@
client_glu_quad.o: $(AMIBERRY_DIR)/src/amiberry_glu_quad.c
	$(CC) $(CLIENTS_LIB_CFLAGS) -c $< -o $@
client_glut.o: $(AMIBERRY_DIR)/src/amiberry_glut.c
	$(CC) $(CLIENTS_LIB_CFLAGS) -c $< -o $@
client_glut_shapes.o: $(AMIBERRY_DIR)/src/amiberry_glut_shapes.c
	$(CC) $(CLIENTS_LIB_CFLAGS) -c $< -o $@
client_debug.o: $(AMIBERRY_DIR)/vendor/minigl-framework/library/debug_output.c
	$(CC) $(CLIENTS_LIB_CFLAGS) -c $< -o $@

CLIENTS_GUEST_CFLAGS := $(CLIENTS_CPPFLAGS) $(CFLAGS) -fcommon \
                        -DMINIGL_DISPATCH_ABI_TYPES $(CLIENTS_M32)

client_open.o: $(AMIBERRY_DIR)/vendor/minigl-framework/client/minigl_open.c
	$(CC) $(CLIENTS_GUEST_CFLAGS) -c $< -o $@

client_guest_ops.o: $(AMIBERRY_DIR)/tests/amiberry_ops.c
	$(CC) $(CLIENTS_GUEST_CFLAGS) -Dmain=amiberry_ops_main -c $< -o $@
client_guest_v29.o: $(AMIBERRY_DIR)/tests/amiberry_v29.c
	$(CC) $(CLIENTS_GUEST_CFLAGS) -Dmain=amiberry_v29_main -c $< -o $@
client_guest_reopen.o: $(AMIBERRY_DIR)/tests/amiberry_reopen.c
	$(CC) $(CLIENTS_GUEST_CFLAGS) -Dmain=amiberry_reopen_main -c $< -o $@

# Firmware service objects, built 32-bit for the guest harness link.
client_fw_%.o: $(MINIGL_MODULE_BACKEND)/%.c
	$(CC) $(CLIENTS_M32) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
clean:
	rm -f *.o libmw_host.a $(TESTS)
