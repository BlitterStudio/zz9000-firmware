# Builds every template through mk/zzapp.mk, the include a third party uses.
# Set ZZAPP_PIC_SYSROOT to the output of build_pic_newlib.sh. The freestanding
# template builds with ZZAPP_FREESTANDING=1 and needs no sysroot.
#
# SPDX-License-Identifier: MIT

ZZAPP_SDK := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PYTHON ?= python3

hello_SRCS := templates/hello.c
pixels_SRCS := templates/pixels.c
fault_SRCS := templates/fault.c
tone_SRCS := templates/tone.c
echo_SRCS := templates/echo.c
console_SRCS := templates/console.c
zzmembench_SRCS := templates/zzmembench.c
isolate_SRCS := templates/isolate.c
spin_SRCS := templates/spin.c
svcbench_SRCS := templates/svcbench.c
memhog_SRCS := templates/memhog.c
svcprobe_SRCS := templates/svcprobe.c
mgl-conformance_SRCS := templates/mgl_conformance.c
filter_SRCS := templates/filter.c
sbrk_smoke_SRCS := sbrk_smoke.c
freestanding_SRCS := templates/freestanding.c
freestanding_FREESTANDING := 1
fpenv_SRCS := templates/fpenv.c
ringflood_SRCS := templates/ringflood.c

TEMPLATES := hello pixels fault tone echo console zzmembench isolate spin svcbench memhog svcprobe mgl-conformance filter freestanding fpenv ringflood
CHECK_APPS := $(TEMPLATES) sbrk_smoke

.PHONY: all check clean check-tool check-map-test check-fp-codegen check-mode-switch check-freestanding-guard $(TEMPLATES) $(CHECK_APPS)

all: $(TEMPLATES)

NOTE_TEST := $(ZZAPP_SDK)/../tests/zzapp_check_test.py
check: $(if $(wildcard $(NOTE_TEST)),check-tool,) check-map-test check-fp-codegen $(addprefix check-,$(CHECK_APPS)) check-mode-switch check-freestanding-guard
	$(PYTHON) $(ZZAPP_SDK)/tools/zzapp-check --expect-note $(ZZAPP_SDK)/build/hello.elf 0 0
	$(PYTHON) $(ZZAPP_SDK)/tools/zzapp-check --expect-note $(ZZAPP_SDK)/build/pixels.elf 1 0
	$(PYTHON) $(ZZAPP_SDK)/tools/zzapp-check --expect-note $(ZZAPP_SDK)/build/tone.elf 2 0
	$(PYTHON) $(ZZAPP_SDK)/tools/zzapp-check --expect-note $(ZZAPP_SDK)/build/mgl-conformance.elf 5 0
	$(PYTHON) $(ZZAPP_SDK)/tools/zzapp-check --expect-note $(ZZAPP_SDK)/build/freestanding.elf 0 0

check-tool:
	$(PYTHON) $(ZZAPP_SDK)/../tests/zzapp_check_test.py

check-map-test:
	$(PYTHON) $(ZZAPP_SDK)/tests/check_map_test.py

check-fp-codegen:
	CROSS=$(or $(CROSS),arm-none-eabi-) $(PYTHON) $(ZZAPP_SDK)/tests/fp_codegen_test.py

# normal, freestanding, normal again in one build directory.
check-mode-switch:
	CROSS=$(or $(CROSS),arm-none-eabi-) sh $(ZZAPP_SDK)/tests/mode_switch_test.sh $(ZZAPP_SDK)

# A freestanding link that pulls a libc member must fail the map check. The
# leak source calls strlen, which needs no syscall stubs, and the link gets
# -lc on purpose.
check-freestanding-guard:
	@log=$(ZZAPP_SDK)/build/freestanding-leak.log; mkdir -p $(ZZAPP_SDK)/build; \
	if $(MAKE) -f $(ZZAPP_SDK)/mk/zzapp.mk \
		ZZAPP_SDK=$(ZZAPP_SDK) \
		APP=freestanding-leak \
		SRCS="templates/freestanding.c tests/freestanding_leak.c" \
		BUILD=$(ZZAPP_SDK)/build/freestanding-leak \
		ZZAPP_ELF=$(ZZAPP_SDK)/build/freestanding-leak.elf \
		ZZAPP_FREESTANDING=1 \
		LDLIBS="-Wl,-u,zz_leak -L$(ZZAPP_PIC_SYSROOT)/arm-none-eabi/lib -lc" \
		all >$$log 2>&1; then \
		cat $$log; echo "freestanding guard: a libc link was accepted"; exit 1; \
	fi; \
	grep -q "freestanding link pulled libc.a" $$log || { cat $$log; echo "freestanding guard: the link failed for another reason"; exit 1; }; \
	echo "freestanding guard: libc link rejected"

$(CHECK_APPS):
	$(MAKE) -f $(ZZAPP_SDK)/mk/zzapp.mk \
		ZZAPP_SDK=$(ZZAPP_SDK) \
		APP=$@ \
		SRCS="$($@_SRCS)" \
		BUILD=$(ZZAPP_SDK)/build/$@ \
		ZZAPP_ELF=$(ZZAPP_SDK)/build/$@.elf \
		ZZAPP_FREESTANDING=$($@_FREESTANDING) \
		all

check-%:
	$(MAKE) -f $(ZZAPP_SDK)/mk/zzapp.mk \
		ZZAPP_SDK=$(ZZAPP_SDK) \
		APP=$* \
		SRCS="$($*_SRCS)" \
		BUILD=$(ZZAPP_SDK)/build/$* \
		ZZAPP_ELF=$(ZZAPP_SDK)/build/$*.elf \
		ZZAPP_FREESTANDING=$($*_FREESTANDING) \
		check

# Path cited by the template notes: sdk/zzapp/build/<app>.elf
build/%.elf: %
	@test -f $@

clean:
	rm -rf $(ZZAPP_SDK)/build
