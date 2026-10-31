# MiniGL module build inventory shared with the external module build.
# The library repository includes this file from the pinned firmware revision.
ZZM_MINIGL_DIR := modules/minigl
ZZM_MINIGL_BUILD ?= $(BUILD_DIR)/modules/minigl
ZZM_MINIGL_ELF ?= $(ZZM_MINIGL_BUILD)/MINIGL.elf
ZZM_MINIGL_IMAGE ?= $(ZZM_MINIGL_BUILD)/MINIGL.ZZM
ZZM_MINIGL_LINKER ?= util/zzm_module.ld
ZZM_MINIGL_SLOT_ADDRESS := 0x09800000
ZZM_MINIGL_SLOT_SIZE := 0x00400000
ZZM_MINIGL_MODULE_ID := 4
ZZM_MINIGL_MODULE_NAME := MINIGL
ZZM_MINIGL_WIRE_VERSION := 9
ZZM_MINIGL_TRANSPORT_DESCRIPTOR_VERSION := 1

# Explicit ZZM3 manifest inputs for `zzm_pack --v3`.  The 64 KiB resident
# domain covers only module bookkeeping/descriptor state; each render context
# is separately bounded at 32 MiB by the runtime owner quota.
ZZM_MINIGL_V3_CPU_FLAGS := 7
ZZM_MINIGL_V3_RASTERIX_FLAGS := 31
ZZM_MINIGL_V3_OWNER_MASK := 15
ZZM_MINIGL_V3_HARDWARE := 1
ZZM_MINIGL_V3_WIRE := 9
ZZM_MINIGL_V3_DESCRIPTOR := 1
ZZM_MINIGL_V3_CPU_CSR_MAJOR := 0
ZZM_MINIGL_V3_CPU_CSR_MINOR := 0
ZZM_MINIGL_V3_RASTERIX_CSR_MAJOR := 2
ZZM_MINIGL_V3_RASTERIX_CSR_MINOR := 0
ZZM_MINIGL_V3_HOST_LAYOUT := 1
ZZM_MINIGL_V3_RESIDENT_QUOTA := 65536
ZZM_MINIGL_V3_CONTEXT_QUOTA := 33554432
ZZM_MINIGL_V3_WORKER_OPS := 0x00010000
ZZM_MINIGL_PACK_CPU_ARGS := $(ZZM_MINIGL_MODULE_ID) 1 \
	$(ZZM_MINIGL_V3_CPU_FLAGS) $(ZZM_MINIGL_V3_OWNER_MASK) \
	$(ZZM_MINIGL_V3_HARDWARE) $(ZZM_MINIGL_V3_WIRE) \
	$(ZZM_MINIGL_V3_DESCRIPTOR) $(ZZM_MINIGL_V3_CPU_CSR_MAJOR) \
	$(ZZM_MINIGL_V3_CPU_CSR_MINOR) $(ZZM_MINIGL_V3_HOST_LAYOUT) \
	$(ZZM_MINIGL_V3_RESIDENT_QUOTA) $(ZZM_MINIGL_V3_CONTEXT_QUOTA) \
	$(ZZM_MINIGL_V3_WORKER_OPS)
ZZM_MINIGL_PACK_RASTERIX_ARGS := $(ZZM_MINIGL_MODULE_ID) 1 \
	$(ZZM_MINIGL_V3_RASTERIX_FLAGS) $(ZZM_MINIGL_V3_OWNER_MASK) \
	$(ZZM_MINIGL_V3_HARDWARE) $(ZZM_MINIGL_V3_WIRE) \
	$(ZZM_MINIGL_V3_DESCRIPTOR) $(ZZM_MINIGL_V3_RASTERIX_CSR_MAJOR) \
	$(ZZM_MINIGL_V3_RASTERIX_CSR_MINOR) $(ZZM_MINIGL_V3_HOST_LAYOUT) \
	$(ZZM_MINIGL_V3_RESIDENT_QUOTA) $(ZZM_MINIGL_V3_CONTEXT_QUOTA) \
	$(ZZM_MINIGL_V3_WORKER_OPS)

# Firmware-owned module implementation sources.  These remain in the public
# firmware tree; the private library repository supplies only FRONTEND_SRCS.
ZZM_MINIGL_ENTRY_SRC := $(ZZM_MINIGL_DIR)/minigl_module.c

ZZM_MINIGL_BACKEND_SRCS := \
	$(SRC_DIR)/mw_service.c \
	$(SRC_DIR)/mw_parse.c \
	$(SRC_DIR)/mw_draw.c \
	$(SRC_DIR)/mw_texture.c \
	$(SRC_DIR)/mw_ctx.c \
	$(SRC_DIR)/minigl_transport.c \
	$(SRC_DIR)/minigl_present.c \
	$(SRC_DIR)/minigl_raster.c \
	$(SRC_DIR)/minigl_lighting.c \
	$(SRC_DIR)/minigl_profile.c \
	$(SRC_DIR)/minigl_rix_backend.c \
	$(SRC_DIR)/rix_cmd.c \
	$(SRC_DIR)/rix_drv.c \
	$(SRC_DIR)/rix_setup.c \
	$(SRC_DIR)/rix_texture.c \
	$(SRC_DIR)/zzapp_gl.c

ZZM_MINIGL_LINK_SRCS = $(ZZM_MINIGL_ENTRY_SRC) $(ZZM_MINIGL_BACKEND_SRCS) \
	$(ZZM_MINIGL_FRONTEND_SRCS)
ZZM_MINIGL_LINK_FLAGS := -nostdlib -Wl,-build-id=none -Wl,--gc-sections \
	-Wl,-T,$(ZZM_MINIGL_LINKER) \
	-Wl,--defsym,ZZM_SLOT=$(ZZM_MINIGL_SLOT_ADDRESS) \
	-Wl,--defsym,ZZM_SLOT_SIZE=$(ZZM_MINIGL_SLOT_SIZE)

# The library build resolves these beneath its pinned front-end checkout.
ZZM_MINIGL_FRONTEND_SRCS := \
	amiberry_gl_state.c amiberry_gl_draw.c amiberry_gl_texture.c \
	amiberry_gl_light.c amiberry_gl_light_get.c amiberry_gl_list.c \
	amiberry_gl_buffer.c amiberry_glu.c \
	amiberry_backend_inproc.c amiberry_transport_inproc.c

ZZM_MINIGL_CPU_FLAGS := -mcpu=cortex-a9 -marm -mfpu=neon -mfloat-abi=hard
ZZM_MINIGL_COMMON_CFLAGS := -ffunction-sections -fdata-sections \
	-fno-unwind-tables -fno-asynchronous-unwind-tables -fno-strict-aliasing
ZZM_MINIGL_CFLAGS := $(ZZM_MINIGL_CPU_FLAGS) $(ZZM_MINIGL_COMMON_CFLAGS) \
	-O2 -DMGL_PLATFORM_ZZARM -DMINIGL_MODULE
ZZM_MINIGL_RASTER_CFLAGS := $(ZZM_MINIGL_CFLAGS) -O3
ZZM_MINIGL_FRONTEND_CFLAGS := $(ZZM_MINIGL_CFLAGS)
