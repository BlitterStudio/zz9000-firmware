# Cross file for a ZZ9000 app. Pass it as CMAKE_TOOLCHAIN_FILE.
# ZZAPP_SDK and ZZAPP_PIC_SYSROOT come from -D or the environment.
# The compiler is arm-none-eabi-gcc (Arm GNU Toolchain 13.2.rel1).
#
# SPDX-License-Identifier: MIT

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT ZZAPP_SDK)
  if(DEFINED ENV{ZZAPP_SDK} AND NOT "$ENV{ZZAPP_SDK}" STREQUAL "")
    set(ZZAPP_SDK "$ENV{ZZAPP_SDK}")
  else()
    get_filename_component(ZZAPP_SDK "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
  endif()
endif()
set(ZZAPP_SDK "${ZZAPP_SDK}" CACHE PATH "ZZApp SDK root" FORCE)

if(NOT ZZAPP_PIC_SYSROOT)
  if(DEFINED ENV{ZZAPP_PIC_SYSROOT})
    set(ZZAPP_PIC_SYSROOT "$ENV{ZZAPP_PIC_SYSROOT}")
  endif()
endif()
if(NOT ZZAPP_PIC_SYSROOT)
  message(FATAL_ERROR "Set ZZAPP_PIC_SYSROOT to the PIC newlib sysroot")
endif()
set(ZZAPP_PIC_SYSROOT "${ZZAPP_PIC_SYSROOT}" CACHE PATH "PIC newlib sysroot" FORCE)

if(NOT ZZAPP_CROSS)
  set(ZZAPP_CROSS "arm-none-eabi-")
endif()

set(CMAKE_C_COMPILER "${ZZAPP_CROSS}gcc" CACHE FILEPATH "ZZApp C compiler" FORCE)
set(CMAKE_ASM_COMPILER "${ZZAPP_CROSS}gcc" CACHE FILEPATH "ZZApp assembler" FORCE)
set(CMAKE_AR "${ZZAPP_CROSS}ar" CACHE FILEPATH "ZZApp ar" FORCE)
set(CMAKE_RANLIB "${ZZAPP_CROSS}ranlib" CACHE FILEPATH "ZZApp ranlib" FORCE)
set(CMAKE_LINKER "${ZZAPP_CROSS}gcc" CACHE FILEPATH "ZZApp linker" FORCE)

set(ZZAPP_CPU_FLAGS "-mcpu=cortex-a9 -marm -mfpu=neon -mfloat-abi=hard")
set(CMAKE_C_FLAGS_INIT "${ZZAPP_CPU_FLAGS} -fPIE -O2 -fno-strict-aliasing -ffunction-sections -fdata-sections -fno-exceptions -fno-unwind-tables -Wall -Wextra -Werror -std=c11")
set(CMAKE_ASM_FLAGS_INIT "${ZZAPP_CPU_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${ZZAPP_CPU_FLAGS} -nostdlib -fPIE -pie -Wl,-T,${ZZAPP_SDK}/zzapp.ld -Wl,--gc-sections -Wl,--no-dynamic-linker -Wl,-z,text -Wl,-Bstatic -Wl,--no-undefined -L${ZZAPP_PIC_SYSROOT}/arm-none-eabi/lib")

set(CMAKE_EXECUTABLE_SUFFIX ".elf")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_FIND_ROOT_PATH "${ZZAPP_PIC_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_C_STANDARD_LIBRARIES "")
