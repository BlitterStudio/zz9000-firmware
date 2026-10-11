# Build libwebp's decoder and demuxer without compiling its encoder, muxer,
# examples, or command-line tools. The upstream project remains the source of
# truth for decoder source selection and generated configuration.
cmake_minimum_required(VERSION 3.16)
project(ZZ9000WebPDecoder C)

if(NOT DEFINED LIBWEBP_SOURCE_DIR OR NOT IS_DIRECTORY "${LIBWEBP_SOURCE_DIR}")
  message(FATAL_ERROR "LIBWEBP_SOURCE_DIR must name an extracted libwebp source tree")
endif()
if(NOT DEFINED SDK_WEBP_ALLOC_HEADER OR NOT EXISTS "${SDK_WEBP_ALLOC_HEADER}")
  message(FATAL_ERROR "SDK_WEBP_ALLOC_HEADER must name sdk_webp_alloc.h")
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WEBP_LINK_STATIC ON CACHE BOOL "" FORCE)
set(WEBP_USE_THREAD OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_CWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_DWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_GIF2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_IMG2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_VWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPINFO OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_LIBWEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_FUZZTEST OFF CACHE BOOL "" FORCE)

# Upstream creates webpdecoder from decoder, decoder DSP, and decoder utility
# objects. Adding its source tree preserves its source-set and config logic.
add_subdirectory("${LIBWEBP_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/libwebp")

# Upstream's webpdemux links against its combined encoder/decoder target.
# Build the same demux source set against the decoder-only archive instead.
get_target_property(WEBP_DEMUX_SRCS webpdemux SOURCES)
add_library(zz_webpdemux STATIC ${WEBP_DEMUX_SRCS})
set_target_properties(zz_webpdemux PROPERTIES OUTPUT_NAME webpdemux)
target_include_directories(zz_webpdemux PRIVATE
  "${LIBWEBP_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/libwebp")
target_link_libraries(zz_webpdemux PUBLIC webpdecoder)

# These options are intentionally private to libwebp compilation. Firmware
# sources retain the ordinary allocator API and call the wrappers explicitly.
foreach(target webpdecode webpdspdecode webputilsdecode zz_webpdemux)
  target_compile_definitions(${target} PRIVATE SDK_WEBP_REPLACE_ALLOCATORS)
  target_compile_options(${target} PRIVATE "-include${SDK_WEBP_ALLOC_HEADER}")
endforeach()

add_custom_target(zz_webp_decoder_demux ALL DEPENDS webpdecoder zz_webpdemux)
