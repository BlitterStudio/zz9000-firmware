# Standalone build of section 9 test 6. E's harness includes tests.mk.
CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -O2 -g -m32
ROOT := ../../..
FW := $(ROOT)/ZZ9000_proto.sdk/ZZ9000OS/src
LIB ?= ../../../../zzapp-c-lib
SDK ?= $(ROOT)/sdk/zzapp
FWFLAGS := -I$(FW) -I$(SDK)/include -DMINIGL_HOST_TEST -DMGL_PLATFORM_HOST
LIBFLAGS := -I$(LIB)/src -I$(LIB)/include -DMGL_PLATFORM_HOST -DMINIGL_HOST_TEST \
	-fno-strict-aliasing -fno-builtin

MW_SRCS := mw_parse.c mw_texture.c mw_draw.c mw_ctx.c mw_service.c sdk_palette.c \
	minigl_raster.c minigl_lighting.c minigl_present.c \
	minigl_rix_backend.c rix_cmd.c rix_texture.c rix_setup.c rix_drv.c
MGL_SRCS := amiberry_gl_state.c amiberry_gl_draw.c amiberry_gl_texture.c \
	amiberry_gl_light.c amiberry_gl_light_get.c amiberry_gl_list.c \
	amiberry_gl_buffer.c amiberry_glu.c \
	amiberry_backend_inproc.c amiberry_transport_inproc.c

.PHONY: test clean
test: gl_present
	./gl_present

gl_present: gl_present.c zzapp_gl.o $(MW_SRCS:.c=.o) $(MGL_SRCS:.c=.o)
	$(CC) $(CFLAGS) $(FWFLAGS) -o $@ $^ -lm

zzapp_gl.o: $(FW)/zzapp_gl.c
	$(CC) $(CFLAGS) $(FWFLAGS) $(LIBFLAGS) -c $< -o $@

%.o: $(FW)/%.c
	$(CC) $(CFLAGS) $(FWFLAGS) -c $< -o $@

amiberry_%.o: $(LIB)/src/amiberry_%.c
	$(CC) $(CFLAGS) $(LIBFLAGS) $(FWFLAGS) -c $< -o $@

clean:
	rm -f *.o gl_present
