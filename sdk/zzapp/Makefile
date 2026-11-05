# App link. The app uses the official Arm GNU 13.2 compiler/libgcc plus the
# PIC newlib sysroot built by build_pic_newlib.sh. Stock newlib is non-PIC
# and cannot satisfy the R_ARM_RELATIVE-only PIE loader contract.
#
# SPDX-License-Identifier: MIT

CROSS ?= arm-none-eabi-
CC := $(CROSS)gcc
AR := $(CROSS)ar
PYTHON ?= python3
PIC_SYSROOT ?= $(ZZAPP_PIC_SYSROOT)

ifeq ($(strip $(PIC_SYSROOT)),)
$(error Set ZZAPP_PIC_SYSROOT to the output of sdk/zzapp/build_pic_newlib.sh)
endif

CPUFLAGS := -mcpu=cortex-a9 -marm -mfpu=neon -mfloat-abi=hard
CFLAGS := $(CPUFLAGS) -fPIE -O2 -fno-strict-aliasing -ffunction-sections -fdata-sections \
	-fno-exceptions -fno-unwind-tables -Wall -Wextra -Werror -std=c11 -Iinclude -Isrc
ASMFLAGS := $(CPUFLAGS)
LDFLAGS := $(CPUFLAGS) -nostdlib -fPIE -pie \
	-Wl,-T,$(CURDIR)/zzapp.ld -Wl,--gc-sections \
	-Wl,--no-dynamic-linker -Wl,-z,text -Wl,-Bstatic -Wl,--no-undefined \
	-L$(PIC_SYSROOT)/arm-none-eabi/lib

BUILD := build
LIB_OBJS := $(BUILD)/zzapp.o $(BUILD)/gl_wrap.o $(BUILD)/syscalls.o

.PHONY: all check-relocs clean

all: $(BUILD)/hello.elf $(BUILD)/pixels.elf $(BUILD)/fault.elf $(BUILD)/tone.elf $(BUILD)/echo.elf $(BUILD)/zzmembench.elf $(BUILD)/mgl-conformance.elf

check-relocs: $(BUILD)/hello.elf $(BUILD)/pixels.elf $(BUILD)/fault.elf $(BUILD)/tone.elf $(BUILD)/echo.elf $(BUILD)/sbrk_smoke.elf $(BUILD)/zzmembench.elf $(BUILD)/mgl-conformance.elf
	$(PYTHON) check_relocs.py $(BUILD)/hello.elf $(BUILD)/pixels.elf $(BUILD)/fault.elf $(BUILD)/tone.elf $(BUILD)/echo.elf $(BUILD)/sbrk_smoke.elf $(BUILD)/zzmembench.elf $(BUILD)/mgl-conformance.elf

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/crt0.o: crt0.S | $(BUILD)
	$(CC) $(ASMFLAGS) -c -o $@ $<

$(BUILD)/note_hello.o: zzapp_note.S | $(BUILD)
	$(CC) $(ASMFLAGS) -DZZAPP_NOTE_FLAGS=0 -c -o $@ $<

$(BUILD)/note_pixels.o: zzapp_note.S | $(BUILD)
	$(CC) $(ASMFLAGS) -DZZAPP_NOTE_FLAGS=1 -c -o $@ $<

$(BUILD)/note_smoke.o: zzapp_note.S | $(BUILD)
	$(CC) $(ASMFLAGS) -DZZAPP_NOTE_FLAGS=0 -c -o $@ $<

$(BUILD)/hello.o: templates/hello.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/pixels.o: templates/pixels.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/note_fault.o: zzapp_note.S | $(BUILD)
	$(CC) $(ASMFLAGS) -DZZAPP_NOTE_FLAGS=0 -c -o $@ $<

$(BUILD)/note_tone.o: zzapp_note.S | $(BUILD)
	$(CC) $(ASMFLAGS) -DZZAPP_NOTE_FLAGS=2 -c -o $@ $<

$(BUILD)/tone.o: templates/tone.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/note_echo.o: zzapp_note.S | $(BUILD)
	$(CC) $(ASMFLAGS) -DZZAPP_NOTE_FLAGS=0 -c -o $@ $<

$(BUILD)/echo.o: templates/echo.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/fault.o: templates/fault.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/sbrk_smoke.o: sbrk_smoke.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/note_zzmembench.o: zzapp_note.S | $(BUILD)
	$(CC) $(ASMFLAGS) -DZZAPP_NOTE_FLAGS=1 -c -o $@ $<

$(BUILD)/zzmembench.o: templates/zzmembench.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/note_mgl-conformance.o: zzapp_note.S | $(BUILD)
	$(CC) $(ASMFLAGS) -DZZAPP_NOTE_FLAGS=5 -c -o $@ $<

$(BUILD)/mgl-conformance.o: templates/mgl_conformance.c templates/mgl_conformance_matrix.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/libzzapp.a: $(LIB_OBJS)
	$(AR) rcs $@ $^

$(BUILD)/hello.elf: $(BUILD)/crt0.o $(BUILD)/note_hello.o $(BUILD)/hello.o $(BUILD)/libzzapp.a
	$(CC) $(LDFLAGS) -o $@ -Wl,--start-group $^ -lc -lgcc -lm -Wl,--end-group
	$(PYTHON) rela_dyn.py $@

$(BUILD)/pixels.elf: $(BUILD)/crt0.o $(BUILD)/note_pixels.o $(BUILD)/pixels.o $(BUILD)/libzzapp.a
	$(CC) $(LDFLAGS) -o $@ -Wl,--start-group $^ -lc -lgcc -lm -Wl,--end-group
	$(PYTHON) rela_dyn.py $@

$(BUILD)/fault.elf: $(BUILD)/crt0.o $(BUILD)/note_fault.o $(BUILD)/fault.o $(BUILD)/libzzapp.a
	$(CC) $(LDFLAGS) -o $@ -Wl,--start-group $^ -lc -lgcc -lm -Wl,--end-group
	$(PYTHON) rela_dyn.py $@

$(BUILD)/sbrk_smoke.elf: $(BUILD)/crt0.o $(BUILD)/note_smoke.o $(BUILD)/sbrk_smoke.o $(BUILD)/libzzapp.a
	$(CC) $(LDFLAGS) -o $@ -Wl,--start-group $^ -lc -lgcc -lm -Wl,--end-group
	$(PYTHON) rela_dyn.py $@

$(BUILD)/tone.elf: $(BUILD)/crt0.o $(BUILD)/note_tone.o $(BUILD)/tone.o $(BUILD)/libzzapp.a
	$(CC) $(LDFLAGS) -o $@ -Wl,--start-group $^ -lc -lgcc -lm -Wl,--end-group
	$(PYTHON) rela_dyn.py $@

$(BUILD)/echo.elf: $(BUILD)/crt0.o $(BUILD)/note_echo.o $(BUILD)/echo.o $(BUILD)/libzzapp.a
	$(CC) $(LDFLAGS) -o $@ -Wl,--start-group $^ -lc -lgcc -lm -Wl,--end-group
	$(PYTHON) rela_dyn.py $@

$(BUILD)/zzmembench.elf: $(BUILD)/crt0.o $(BUILD)/note_zzmembench.o $(BUILD)/zzmembench.o $(BUILD)/libzzapp.a
	$(CC) $(LDFLAGS) -o $@ -Wl,--start-group $^ -lc -lgcc -lm -Wl,--end-group
	$(PYTHON) rela_dyn.py $@

$(BUILD)/mgl-conformance.elf: $(BUILD)/crt0.o $(BUILD)/note_mgl-conformance.o $(BUILD)/mgl-conformance.o $(BUILD)/libzzapp.a
	$(CC) $(LDFLAGS) -o $@ -Wl,--start-group $^ -lc -lgcc -lm -Wl,--end-group
	$(PYTHON) rela_dyn.py $@

clean:
	rm -rf $(BUILD)
