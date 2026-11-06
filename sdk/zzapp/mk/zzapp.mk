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

ifeq ($(strip $(PIC_SYSROOT)),)
$(error Set ZZAPP_PIC_SYSROOT to the output of sdk/zzapp/build_pic_newlib.sh)
endif

# Capture the app's flags before this file appends the required ones.
ZZAPP_EXTRA_CFLAGS := $(CFLAGS)
ZZAPP_EXTRA_LDLIBS := $(LDLIBS)

CPUFLAGS := -mcpu=cortex-a9 -marm -mfpu=neon -mfloat-abi=hard
ZZAPP_APP_CFLAGS := $(CPUFLAGS) -fPIE -O2 -fno-strict-aliasing \
	-ffunction-sections -fdata-sections -fno-exceptions -fno-unwind-tables \
	-Wall -Wextra -Werror -std=c11 -I$(ZZAPP_SDK)/include \
	$(ZZAPP_EXTRA_CFLAGS)
ZZAPP_LIB_CFLAGS := $(ZZAPP_APP_CFLAGS) -I$(ZZAPP_SDK)/src
ZZAPP_DEPFLAGS := -MMD -MP
ZZAPP_LDFLAGS := $(CPUFLAGS) -nostdlib -fPIE -pie \
	-Wl,-T,$(ZZAPP_SDK)/zzapp.ld -Wl,--gc-sections \
	-Wl,--no-dynamic-linker -Wl,-z,text -Wl,-Bstatic -Wl,--no-undefined \
	-L$(PIC_SYSROOT)/arm-none-eabi/lib

APP_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(SRCS))
LIB_OBJS := $(BUILD)/libzzapp/zzapp.o $(BUILD)/libzzapp/gl_wrap.o \
	$(BUILD)/libzzapp/syscalls.o
DEPS := $(APP_OBJS:.o=.d) $(LIB_OBJS:.o=.d) $(BUILD)/crt0.d

.PHONY: all check clean
.DELETE_ON_ERROR:

all: $(ZZAPP_ELF)

check: $(ZZAPP_ELF)
	$(PYTHON) $(ZZAPP_SDK)/tools/zzapp-check $(ZZAPP_ELF)

$(ZZAPP_ELF): $(BUILD)/crt0.o $(APP_OBJS) $(BUILD)/libzzapp.a
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_LDFLAGS) -o $@ -Wl,--start-group $^ $(ZZAPP_EXTRA_LDLIBS) -lc -lgcc -lm -Wl,--end-group

$(BUILD)/libzzapp.a: $(LIB_OBJS)
	$(AR) rcs $@ $^

$(BUILD)/crt0.o: $(ZZAPP_SDK)/crt0.S
	@mkdir -p $(dir $@)
	$(CC) $(CPUFLAGS) -MMD -MP -MF $(BUILD)/crt0.d -c -o $@ $<

$(BUILD)/libzzapp/zzapp.o: $(ZZAPP_SDK)/src/zzapp.c
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_LIB_CFLAGS) $(ZZAPP_DEPFLAGS) -c -o $@ $<

$(BUILD)/libzzapp/gl_wrap.o: $(ZZAPP_SDK)/src/gl_wrap.c
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_LIB_CFLAGS) $(ZZAPP_DEPFLAGS) -c -o $@ $<

$(BUILD)/libzzapp/syscalls.o: $(ZZAPP_SDK)/src/syscalls.c
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_LIB_CFLAGS) $(ZZAPP_DEPFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(ZZAPP_APP_CFLAGS) $(ZZAPP_DEPFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD)
	@if [ "$(ZZAPP_ELF)" != "$(BUILD)/$(APP).elf" ]; then rm -f "$(ZZAPP_ELF)"; fi

-include $(DEPS)
