#!/bin/sh
# Builds hello in both modes into the SAME output file in one build
# directory, without deleting anything, through every mode transition:
# normal, freestanding, normal, freestanding, normal. After each build the
# image must equal a clean build of that mode, and no archive used by a
# normal link may hold freestanding.o. Needs ZZAPP_PIC_SYSROOT and the Arm
# toolchain on PATH.
#
# usage: mode_switch_test.sh <sdk/zzapp dir>
#
# SPDX-License-Identifier: MIT
set -eu

SDK=$(CDPATH= cd -- "${1:?usage: mode_switch_test.sh <sdk/zzapp dir>}" && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$SDK"

build() { # build-dir freestanding
  if ! make -s -f "$SDK/mk/zzapp.mk" ZZAPP_SDK="$SDK" APP=hello \
      SRCS=templates/hello.c BUILD="$1" ZZAPP_FREESTANDING="$2" \
      all >"$WORK/log" 2>&1; then
    cat "$WORK/log"
    exit 1
  fi
}

build "$WORK/clean0" 0
build "$WORK/clean1" 1

step=0
for mode in 0 1 0 1 0; do
  step=$((step + 1))
  build "$WORK/sw" "$mode"
  if ! cmp -s "$WORK/clean$mode/hello.elf" "$WORK/sw/hello.elf"; then
    echo "mode switch: step $step (ZZAPP_FREESTANDING=$mode) returned the other mode's image"
    exit 1
  fi
  for archive in $(find "$WORK/sw" -name libzzapp.a); do
    case "$archive" in
      *freestanding*) continue ;;
    esac
    if "${CROSS:-arm-none-eabi-}ar" t "$archive" | grep -q freestanding.o; then
      echo "mode switch: $archive holds freestanding.o"
      exit 1
    fi
  done
done
echo "mode switch: ok"
