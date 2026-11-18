# One ZZ9000 app. The app Makefile sets ZZAPP_SDK, APP and SRCS, then
# includes this file. Optional CFLAGS and LDLIBS are appended.
#
#   ZZAPP_SDK ?= /opt/zzapp-sdk
#   APP := hello
#   SRCS := hello.c
#   include $(ZZAPP_SDK)/mk/zzapp.mk
#
# One source must call ZZAPP_NOTE() from zzapp_note.h. `make`, `make check`
# and `make clean` build $(ZZAPP_ELF), run zzapp-check, and remove the
# objects. Set ZZAPP_PIC_SYSROOT to the PIC newlib sysroot.
#
# ZZAPP_FREESTANDING=1 builds without newlib. crt0 calls zz_exit, libzzapp
# leaves out syscalls.c and malloc, the link takes -lgcc but no -lc or -lm,
# and the build fails if the link map holds an archive other than libzzapp.a
# and libgcc.a. ZZAPP_PIC_SYSROOT is not needed in that mode. See
# sdk/docs/zzapp-developer-guide.md.
#
# SPDX-License-Identifier: MIT

ifeq ($(strip $(ZZAPP_SDK)),)
$(error Set ZZAPP_SDK to the sdk/zzapp directory)
endif
ifeq ($(strip $(APP)),)
$(error Set APP to the output name)
endif
ifeq ($(strip $(SRCS)),)
$(error Set SRCS to the app C sources)
endif

CROSS ?= arm-none-eabi-
CC := $(CROSS)gcc
AR := $(CROSS)ar
PYTHON ?= python3
PIC_SYSROOT ?= $(ZZAPP_PIC_SYSROOT)
BUILD ?= build
ZZAPP_ELF ?= $(BUILD)/$(APP).elf

override ZZAPP_FREESTANDING := $(strip $(or $(ZZAPP_FREESTANDING),0))
ifneq ($(filter-out 0 1,$(ZZAPP_FREESTANDING)),)
$(error ZZAPP_FREESTANDING must be 0 or 1)
endif

ifeq ($(ZZAPP_FREESTANDING),0)
ifeq ($(strip $(PIC_SYSROOT)),)
$(error Set ZZAPP_PIC_SYSROOT to the output of sdk/zzapp/build_pic_newlib.sh)
endif
endif

# Capture the app's flags before this file appends the required ones.
ZZAPP_EXTRA_CFLAGS := $(CFLAGS)
ZZAPP_EXTRA_LDLIBS := $(LDLIBS)

CPUFLAGS := -mcpu=cortex-a9 -marm -mfpu=neon -mfloat-abi=hard
ifeq ($(ZZAPP_FREESTANDING),1)
ZZAPP_MODE_DEFS := -DZZAPP_FREESTANDING=1
endif
ZZAPP_APP_CFLAGS := $(CPUFLAGS) -fPIE -O2 -fno-strict-aliasing \
	-ffunction-sections -fdata-sections -fno-exceptions -fno-unwind-tables \
	-Wall -Wextra -Werror -std=c11 -I$(ZZAPP_SDK)/include \
	$(ZZAPP_MODE_DEFS) $(ZZAPP_EXTRA_CFLAGS)
ZZAPP_LIB_CFLAGS := $(ZZAPP_APP_CFLAGS) -I$(ZZAPP_SDK)/src
ZZAPP_DEPFLAGS := -MMD -MP
ZZAPP_LDFLAGS := $(CPUFLAGS) -nostdlib -fPIE -pie \
	-Wl,-T,$(ZZAPP_SDK)/zzapp.ld -Wl,--gc-sections \
	-Wl,--no-dynamic-linker -Wl,-z,text -Wl,-Bstatic -Wl,--no-undefined
# Objects and the archive live in a per-mode directory, so switching
# ZZAPP_FREESTANDING in one BUILD never reuses the other mode's crt0.o,
# zzapp.o or archive members.
ifeq ($(ZZAPP_FREESTANDING),1)
OBJ := $(BUILD)/freestanding
ZZAPP_MAP := $(BUILD)/$(APP).map
ZZAPP_LDFLAGS += -Wl,-Map=$(ZZAPP_MAP)
ZZAPP_SYSLIBS := -lgcc
LIB_OBJS := $(OBJ)/libzzapp/zzapp.o $(OBJ)/libzzapp/gl_wrap.o \
	$(OBJ)/libzzapp/freestanding.o
else
OBJ := $(BUILD)/hosted
ZZAPP_LDFLAGS += -L$(PIC_SYSROOT)/arm-none-eabi/lib
ZZAPP_SYSLIBS := -lc -lgcc -lm
LIB_OBJS := $(OBJ)/libzzapp/zzapp.o $(OBJ)/libzzapp/gl_wrap.o \
	$(OBJ)/libzzapp/syscalls.o
endif

APP_OBJS := $(patsubst %.c,$(OBJ)/%.o,$(SRCS))
DEPS := $(APP_OBJS:.o=.d) $(LIB_OBJS:.o=.d) $(OBJ)/crt0.d

.PHONY: all check clean FORCE
.DELETE_ON_ERROR:

all: $(ZZAPP_ELF)

check: $(ZZAPP_ELF)
	$(PYTHON) $(ZZAPP_SDK)/tools/zzapp-check $(ZZAPP_ELF)

# The image is the same file in both modes. The stamp holds the mode of the
# last link and is rewritten only when that changes, so a mode change makes
# the image older than the stamp and forces the link.
ZZAPP_MODE_STAMP := $(BUILD)/$(APP).mode

$(ZZAPP_MODE_STAMP): FORCE
	@mkdir -p $(dir $@)
	@echo "$(ZZAPP_FREESTANDING)" | cmp -s - $@ || echo "$(ZZAPP_FREESTANDING)" > $@

FORCE:

$(ZZAPP_ELF): $(OBJ)/crt0.o $(APP_OBJS) $(OBJ)/libzzapp.a $(ZZAPP_MODE_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_LDFLAGS) -o $@ -Wl,--start-group $(filter-out $(ZZAPP_MODE_STAMP),$^) $(ZZAPP_EXTRA_LDLIBS) $(ZZAPP_SYSLIBS) -Wl,--end-group
ifeq ($(ZZAPP_FREESTANDING),1)
	$(PYTHON) $(ZZAPP_SDK)/tools/zzapp-check --no-libc $(ZZAPP_MAP)
endif

$(OBJ)/libzzapp.a: $(LIB_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

$(OBJ)/crt0.o: $(ZZAPP_SDK)/crt0.S
	@mkdir -p $(dir $@)
	$(CC) $(CPUFLAGS) $(ZZAPP_MODE_DEFS) -MMD -MP -MF $(OBJ)/crt0.d -c -o $@ $<

$(OBJ)/libzzapp/zzapp.o: $(ZZAPP_SDK)/src/zzapp.c
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_LIB_CFLAGS) $(ZZAPP_DEPFLAGS) -c -o $@ $<

$(OBJ)/libzzapp/gl_wrap.o: $(ZZAPP_SDK)/src/gl_wrap.c
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_LIB_CFLAGS) $(ZZAPP_DEPFLAGS) -c -o $@ $<

$(OBJ)/libzzapp/syscalls.o: $(ZZAPP_SDK)/src/syscalls.c
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_LIB_CFLAGS) $(ZZAPP_DEPFLAGS) -c -o $@ $<

# The mem* routines must not be rewritten into calls to themselves.
$(OBJ)/libzzapp/freestanding.o: $(ZZAPP_SDK)/src/freestanding.c
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_LIB_CFLAGS) -fno-builtin -fno-tree-loop-distribute-patterns $(ZZAPP_DEPFLAGS) -c -o $@ $<

$(OBJ)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_APP_CFLAGS) $(ZZAPP_DEPFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD)
	@if [ "$(ZZAPP_ELF)" != "$(BUILD)/$(APP).elf" ]; then rm -f "$(ZZAPP_ELF)"; fi

-include $(DEPS)
