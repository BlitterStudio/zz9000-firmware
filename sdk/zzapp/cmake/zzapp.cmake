# zzapp_add_executable(name [FREESTANDING] SOURCES src...)
# Links crt0, libzzapp and the PIC sysroot the same way mk/zzapp.mk does,
# then runs zzapp-check. Include this after project(), with
# zzapp-toolchain.cmake as CMAKE_TOOLCHAIN_FILE.
#
# FREESTANDING links without newlib, like ZZAPP_FREESTANDING=1 in
# mk/zzapp.mk: crt0 calls zz_exit, syscalls.c and malloc are left out, the
# link takes -lgcc but no -lc or -lm, and a post-build step fails when the
# link map holds an archive other than libzzapp.a and libgcc.a. Configuring
# with -DZZAPP_FREESTANDING=ON makes every target freestanding. See
# sdk/docs/zzapp-developer-guide.md.
#
# SPDX-License-Identifier: MIT

if(NOT ZZAPP_SDK)
  if(DEFINED ENV{ZZAPP_SDK} AND NOT "$ENV{ZZAPP_SDK}" STREQUAL "")
    set(ZZAPP_SDK "$ENV{ZZAPP_SDK}")
  else()
    get_filename_component(ZZAPP_SDK "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
  endif()
endif()

if(NOT PYTHON)
  if(DEFINED ENV{PYTHON} AND NOT "$ENV{PYTHON}" STREQUAL "")
    set(PYTHON "$ENV{PYTHON}")
  else()
    find_program(PYTHON NAMES python3 python REQUIRED)
  endif()
endif()

set(CMAKE_EXECUTABLE_SUFFIX ".elf")

function(zzapp_add_executable name)
  cmake_parse_arguments(ZA "FREESTANDING" "" "SOURCES" ${ARGN})
  if(NOT ZA_SOURCES)
    message(FATAL_ERROR "zzapp_add_executable(${name}) needs SOURCES")
  endif()
  if(ZA_FREESTANDING OR ZZAPP_FREESTANDING)
    set(freestanding TRUE)
  else()
    set(freestanding FALSE)
  endif()

  if(freestanding)
    add_executable(${name}
      "${ZZAPP_SDK}/crt0.S"
      "${ZZAPP_SDK}/src/zzapp.c"
      "${ZZAPP_SDK}/src/gl_wrap.c"
      "${ZZAPP_SDK}/src/freestanding.c"
      ${ZA_SOURCES})
    # The mem* routines must not be rewritten into calls to themselves.
    set_source_files_properties("${ZZAPP_SDK}/src/freestanding.c" PROPERTIES
      COMPILE_OPTIONS "-fno-builtin;-fno-tree-loop-distribute-patterns")
    target_compile_definitions(${name} PRIVATE ZZAPP_FREESTANDING=1)
    target_link_libraries(${name} PRIVATE gcc)
    target_link_options(${name} PRIVATE "-Wl,-Map=$<TARGET_FILE:${name}>.map")
  else()
    add_executable(${name}
      "${ZZAPP_SDK}/crt0.S"
      "${ZZAPP_SDK}/src/zzapp.c"
      "${ZZAPP_SDK}/src/gl_wrap.c"
      "${ZZAPP_SDK}/src/syscalls.c"
      ${ZA_SOURCES})
    target_link_libraries(${name} PRIVATE
      -Wl,--start-group c gcc m -Wl,--end-group)
  endif()
  target_include_directories(${name} PRIVATE
    "${ZZAPP_SDK}/include"
    "${ZZAPP_SDK}/src")
  add_custom_command(TARGET ${name} POST_BUILD
    COMMAND ${PYTHON} "${ZZAPP_SDK}/tools/zzapp-check" "$<TARGET_FILE:${name}>"
    VERBATIM)
  if(freestanding)
    add_custom_command(TARGET ${name} POST_BUILD
      COMMAND ${PYTHON} "${ZZAPP_SDK}/tools/zzapp-check" --no-libc
              "$<TARGET_FILE:${name}>.map"
      VERBATIM)
  endif()
endfunction()
