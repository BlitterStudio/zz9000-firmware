# SPDX-License-Identifier: GPL-3.0-or-later
#
# Section 9 test 6: real front end and mw_service via the host transport.
MINIGL_LIB_DIR ?= ../../../amiberry-minigl-library-zz9k
C_GL_CPPFLAGS := -DMINIGL_HOST_TEST -DMGL_PLATFORM_HOST \
	-I$(MINIGL_LIB_DIR)/src -I$(MINIGL_LIB_DIR)/include \
	-fno-strict-aliasing -fno-builtin
C_GL_MW_SRCS := mw_parse.c mw_texture.c mw_draw.c mw_ctx.c mw_service.c sdk_palette.c \
	minigl_raster.c minigl_lighting.c minigl_present.c \
	minigl_rix_backend.c rix_cmd.c rix_texture.c rix_setup.c rix_drv.c
C_GL_FRONT_SRCS := amiberry_gl_state.c amiberry_gl_draw.c amiberry_gl_texture.c \
	amiberry_gl_light.c amiberry_gl_light_get.c amiberry_gl_list.c \
	amiberry_gl_buffer.c amiberry_glu.c amiberry_backend_inproc.c \
	amiberry_transport_inproc.c
C_GL_SRCS := $(addprefix $(FW)/,$(C_GL_MW_SRCS)) \
	$(addprefix $(MINIGL_LIB_DIR)/src/,$(C_GL_FRONT_SRCS))

ZZAPP_TESTS += $(BUILD)/c/gl_present

$(BUILD)/c/gl_present: c/gl_present.c $(FW)/zzapp_gl.c $(C_GL_SRCS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_HARNESS_CFLAGS) -m32 $(C_GL_CPPFLAGS) -o $@ $^ -lm
