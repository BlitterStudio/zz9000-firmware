# zzapp_add_executable(name SOURCES src...)
# Links crt0, libzzapp and the PIC sysroot the same way mk/zzapp.mk does,
# then runs zzapp-check. Include this after project(), with
# zzapp-toolchain.cmake as CMAKE_TOOLCHAIN_FILE.
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
  cmake_parse_arguments(ZZAPP "" "" "SOURCES" ${ARGN})
  if(NOT ZZAPP_SOURCES)
    message(FATAL_ERROR "zzapp_add_executable(${name}) needs SOURCES")
  endif()
  add_executable(${name}
    "${ZZAPP_SDK}/crt0.S"
    "${ZZAPP_SDK}/src/zzapp.c"
    "${ZZAPP_SDK}/src/gl_wrap.c"
    "${ZZAPP_SDK}/src/syscalls.c"
    ${ZZAPP_SOURCES})
  target_include_directories(${name} PRIVATE
    "${ZZAPP_SDK}/include"
    "${ZZAPP_SDK}/src")
  target_link_libraries(${name} PRIVATE
    -Wl,--start-group c gcc m -Wl,--end-group)
  add_custom_command(TARGET ${name} POST_BUILD
    COMMAND ${PYTHON} "${ZZAPP_SDK}/tools/zzapp-check" "$<TARGET_FILE:${name}>"
    VERBATIM)
endfunction()
