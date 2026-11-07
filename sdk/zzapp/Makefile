# Builds every template through mk/zzapp.mk, the include a third party uses.
# Set ZZAPP_PIC_SYSROOT to the output of build_pic_newlib.sh.
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
mgl-conformance_SRCS := templates/mgl_conformance.c
filter_SRCS := templates/filter.c
sbrk_smoke_SRCS := sbrk_smoke.c

TEMPLATES := hello pixels fault tone echo console zzmembench mgl-conformance filter
CHECK_APPS := $(TEMPLATES) sbrk_smoke

.PHONY: all check clean check-tool $(TEMPLATES) $(CHECK_APPS)

all: $(TEMPLATES)

NOTE_TEST := $(ZZAPP_SDK)/../tests/zzapp_check_test.py
check: $(if $(wildcard $(NOTE_TEST)),check-tool,) $(addprefix check-,$(CHECK_APPS))
	$(PYTHON) $(ZZAPP_SDK)/tools/zzapp-check --expect-note $(ZZAPP_SDK)/build/hello.elf 0 0
	$(PYTHON) $(ZZAPP_SDK)/tools/zzapp-check --expect-note $(ZZAPP_SDK)/build/pixels.elf 1 0
	$(PYTHON) $(ZZAPP_SDK)/tools/zzapp-check --expect-note $(ZZAPP_SDK)/build/tone.elf 2 0
	$(PYTHON) $(ZZAPP_SDK)/tools/zzapp-check --expect-note $(ZZAPP_SDK)/build/mgl-conformance.elf 5 0

check-tool:
	$(PYTHON) $(ZZAPP_SDK)/../tests/zzapp_check_test.py

$(CHECK_APPS):
	$(MAKE) -f $(ZZAPP_SDK)/mk/zzapp.mk \
		ZZAPP_SDK=$(ZZAPP_SDK) \
		APP=$@ \
		SRCS="$($@_SRCS)" \
		BUILD=$(ZZAPP_SDK)/build/$@ \
		ZZAPP_ELF=$(ZZAPP_SDK)/build/$@.elf \
		all

check-%:
	$(MAKE) -f $(ZZAPP_SDK)/mk/zzapp.mk \
		ZZAPP_SDK=$(ZZAPP_SDK) \
		APP=$* \
		SRCS="$($*_SRCS)" \
		BUILD=$(ZZAPP_SDK)/build/$* \
		ZZAPP_ELF=$(ZZAPP_SDK)/build/$*.elf \
		check

# Path cited by the template notes: sdk/zzapp/build/<app>.elf
build/%.elf: %
	@test -f $@

clean:
	rm -rf $(ZZAPP_SDK)/build
