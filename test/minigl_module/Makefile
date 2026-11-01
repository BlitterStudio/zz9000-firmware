CC ?= cc
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
	-fno-strict-aliasing -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
	-Wno-overflow -Wno-type-limits
LDFLAGS ?= -Wl,--gc-sections
ROOT := ../..
FIRMWARE := $(ROOT)/ZZ9000_proto.sdk/ZZ9000OS
SRC := $(FIRMWARE)/src
LIB ?= $(ROOT)/amiberry-minigl-library-zz9k
BUILD := build
TARGET := $(BUILD)/minigl_module_test
PACKER := $(BUILD)/zzm_pack
PACKER_ELF := $(BUILD)/MINIGL.elf
PACKED_MODULE := $(BUILD)/MINIGL.ZZM

CPPFLAGS := -D_GNU_SOURCE -DMINIGL_MODULE -DMINIGL_HOST_TEST -DMINIGL_RASTERIX \
	-DTASKQ_HOST_TEST -DMGL_PLATFORM_ZZARM -I$(SRC) -I$(ROOT)/sdk/zzapp/include \
	-I../card_pool -I../zzm_resources/mock -I../modules/mock -I$(LIB)/include \
	-I$(LIB)/src -I$(LIB)/vendor/minigl-framework/include

BACKEND := $(SRC)/minigl_platform.c $(SRC)/mw_service.c $(SRC)/mw_parse.c \
	$(SRC)/mw_draw.c $(SRC)/mw_texture.c $(SRC)/mw_ctx.c \
	$(SRC)/minigl_transport.c $(SRC)/minigl_present.c $(SRC)/minigl_raster.c \
	$(SRC)/minigl_lighting.c $(SRC)/minigl_profile.c $(SRC)/minigl_rix_backend.c \
	$(SRC)/rix_cmd.c $(SRC)/rix_drv.c $(SRC)/rix_setup.c $(SRC)/rix_texture.c \
	$(SRC)/zzapp_gl.c
FRONTEND := $(addprefix $(LIB)/src/,amiberry_gl_state.c amiberry_gl_draw.c \
	amiberry_gl_texture.c amiberry_gl_light.c amiberry_gl_light_get.c \
	amiberry_gl_list.c amiberry_gl_buffer.c amiberry_glu.c \
	amiberry_backend_inproc.c amiberry_transport_inproc.c)
HOST := minigl_module_test.c module_arrays.c ../card_pool/host_card_pool.c \
	$(SRC)/zzm_loader.c $(SRC)/zzm_manifest.c $(SRC)/zzm_runtime.c \
	$(SRC)/zzm_minigl_host.c $(SRC)/card_pool.c $(SRC)/surface_allocator.c \
	$(SRC)/sdk_palette.c $(SRC)/sdk_smp_lock.c \
	$(FIRMWARE)/modules/minigl/minigl_module.c \
	$(FIRMWARE)/modules/libzzm_rt/zzm_rt.c

.PHONY: test clean

test: $(TARGET) $(PACKED_MODULE)
	$(TARGET)

$(BUILD):
	mkdir -p $@

$(TARGET): $(HOST) $(BACKEND) $(FRONTEND) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(HOST) $(BACKEND) $(FRONTEND) -lm

$(PACKER): ../../util/zzm_pack.c $(SRC)/zzm_manifest.c | $(BUILD)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -I$(SRC) -o $@ $^

$(PACKER_ELF): packer_input.c | $(BUILD)
	$(CC) -m32 -fno-pic -fno-pie -nostdlib -no-pie -Wl,-build-id=none \
		-Wl,-Ttext-segment=0x09800000 -Wl,-e,zzm_module_entry -o $@ $<

$(PACKED_MODULE): $(PACKER) $(PACKER_ELF)
	$(PACKER) --v3 4 1 7 15 1 9 1 0 0 1 65536 402653184 0x00010000 \
		$(PACKER_ELF) $@

clean:
	rm -rf $(BUILD)
