# Host build of ZZ9000 apps. libzzapp-host provides main() and an SDL2
# API 0.2 table. Apps supply zz_main. Do not link the ARM crt0 or syscalls.
#
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)
project(zzapp-host LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

function(zzapp_host_find_sdl2 out_target)
  find_package(SDL2 QUIET)
  if(TARGET SDL2::SDL2)
    set(${out_target} SDL2::SDL2 PARENT_SCOPE)
    return()
  endif()
  if(TARGET SDL2::SDL2-static)
    set(${out_target} SDL2::SDL2-static PARENT_SCOPE)
    return()
  endif()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(PC_SDL2 QUIET sdl2)
  endif()
  if(PC_SDL2_FOUND)
    add_library(zzapp_sdl2 INTERFACE)
    target_include_directories(zzapp_sdl2 INTERFACE ${PC_SDL2_INCLUDE_DIRS})
    target_link_directories(zzapp_sdl2 INTERFACE ${PC_SDL2_LIBRARY_DIRS})
    target_link_libraries(zzapp_sdl2 INTERFACE ${PC_SDL2_LIBRARIES})
    set(${out_target} zzapp_sdl2 PARENT_SCOPE)
    return()
  endif()
  if(SDL2_LIBRARIES)
    add_library(zzapp_sdl2 INTERFACE)
    if(SDL2_INCLUDE_DIRS)
      target_include_directories(zzapp_sdl2 INTERFACE ${SDL2_INCLUDE_DIRS})
    endif()
    target_link_libraries(zzapp_sdl2 INTERFACE ${SDL2_LIBRARIES})
    set(${out_target} zzapp_sdl2 PARENT_SCOPE)
    return()
  endif()
  message(FATAL_ERROR
    "SDL2 not found. Install libsdl2-dev, or set SDL2_DIR / PKG_CONFIG_PATH.")
endfunction()

zzapp_host_find_sdl2(ZZAPP_SDL2)

add_library(zzapp-host STATIC
  ../src/zzapp.c
  src/main.c
  src/memory.c
  src/files.c
  src/display.c
  src/input.c
  src/audio.c
  src/console.c
  src/messages.c
  src/services.c
)
set_target_properties(zzapp-host PROPERTIES OUTPUT_NAME zzapp-host)
target_include_directories(zzapp-host
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(zzapp-host PUBLIC ${ZZAPP_SDL2})
if(WIN32)
  target_link_libraries(zzapp-host PUBLIC user32)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(zzapp-host PRIVATE -Wall -Wextra -Werror)
endif()
if(MSVC)
  target_compile_options(zzapp-host PRIVATE /W4)
  target_compile_definitions(zzapp-host PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# name SOURCES file...
# Links the app's zz_main against libzzapp-host. The library already contains
# src/zzapp.c and main(). Do not pass crt0, syscalls.c, or gl_wrap.c.
function(zzapp_host_executable name)
  cmake_parse_arguments(ZH "" "" "SOURCES" ${ARGN})
  if(NOT ZH_SOURCES)
    message(FATAL_ERROR "zzapp_host_executable(${name}) needs SOURCES")
  endif()
  add_executable(${name} ${ZH_SOURCES})
  target_link_libraries(${name} PRIVATE zzapp-host)
  if(MSVC)
    target_compile_definitions(${name} PRIVATE __builtin_memcpy=memcpy)
  endif()
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
  endif()
endfunction()

set(ZZAPP_TEMPLATE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../templates)
zzapp_host_executable(hello SOURCES ${ZZAPP_TEMPLATE_DIR}/hello.c)
zzapp_host_executable(console SOURCES ${ZZAPP_TEMPLATE_DIR}/console.c)
zzapp_host_executable(filter SOURCES ${ZZAPP_TEMPLATE_DIR}/filter.c)
zzapp_host_executable(echo SOURCES ${ZZAPP_TEMPLATE_DIR}/echo.c)
zzapp_host_executable(pixels SOURCES ${ZZAPP_TEMPLATE_DIR}/pixels.c)
zzapp_host_executable(tone SOURCES ${ZZAPP_TEMPLATE_DIR}/tone.c)
zzapp_host_executable(contract SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/contract.c)
zzapp_host_executable(msgflood SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/msgflood.c)

# Direct unit checks: they include the src file under test, so they do not
# link the SDL2 runner library.
add_executable(files_unit tests/files_unit.c)
target_include_directories(files_unit PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(files_unit PRIVATE -Wall -Wextra)
endif()

include(CTest)
if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  set(ZZAPP_CHECK ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_runner.py)
  function(zzapp_host_test name)
    add_test(
      NAME ${name}
      COMMAND ${Python3_EXECUTABLE} ${ZZAPP_CHECK} ${name}
              $<TARGET_FILE:${name}>
    )
    set_tests_properties(${name} PROPERTIES
      TIMEOUT 60
      ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy"
    )
  endfunction()
  zzapp_host_test(hello)
  zzapp_host_test(console)
  zzapp_host_test(filter)
  zzapp_host_test(echo)
  zzapp_host_test(pixels)
  zzapp_host_test(tone)
  zzapp_host_test(msgflood)
  add_test(NAME files_unit COMMAND files_unit)
  add_test(
    NAME keys
    COMMAND ${Python3_EXECUTABLE} ${ZZAPP_CHECK} keys $<TARGET_FILE:hello>
  )
  set_tests_properties(keys PROPERTIES
    TIMEOUT 60
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;ZZAPP_HOST_SELFTEST=1"
  )
  add_test(
    NAME contract
    COMMAND ${Python3_EXECUTABLE} ${ZZAPP_CHECK} contract $<TARGET_FILE:contract>
  )
  set_tests_properties(contract PROPERTIES
    TIMEOUT 60
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy"
  )
endif()
