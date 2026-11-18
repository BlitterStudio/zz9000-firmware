# Builds templates/hello.c and templates/freestanding.c with the SDK toolchain
# file. -DZZAPP_FREESTANDING=ON builds both without newlib and needs no
# ZZAPP_PIC_SYSROOT.
#
#   cmake -S sdk/zzapp/cmake/example -B build/cmake-hello \
#     -DCMAKE_TOOLCHAIN_FILE=$ZZAPP_SDK/cmake/zzapp-toolchain.cmake
#   cmake --build build/cmake-hello
#
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)
project(zzapp_hello C ASM)

if(NOT ZZAPP_SDK)
  message(FATAL_ERROR "Configure with -DCMAKE_TOOLCHAIN_FILE=.../cmake/zzapp-toolchain.cmake")
endif()

include("${ZZAPP_SDK}/cmake/zzapp.cmake")
zzapp_add_executable(hello SOURCES "${ZZAPP_SDK}/templates/hello.c")
zzapp_add_executable(freestanding FREESTANDING
  SOURCES "${ZZAPP_SDK}/templates/freestanding.c")
