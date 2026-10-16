cmake_minimum_required(VERSION 3.20)
project(zz9000_rasterix_harness LANGUAGES CXX)

set(RIX_CORE_TMU_COUNT "${TMU_COUNT}" CACHE STRING "RasterIX TMU count")
set(RIX_CORE_MAX_TEXTURE_SIZE 256 CACHE STRING "Maximum texture edge")
set(RIX_CORE_ENABLE_MIPMAPPING true CACHE STRING "Enable mipmapping")
set(RIX_CORE_MAX_DISPLAY_WIDTH 640 CACHE STRING "Display width")
set(RIX_CORE_MAX_DISPLAY_HEIGHT 480 CACHE STRING "Display height")
set(RIX_CORE_FRAMEBUFFER_SIZE_IN_PIXEL_LG 14 CACHE STRING "Internal framebuffer band pixels")
set(RIX_CORE_ENABLE_ATTRIBUTE_SCALING true CACHE STRING "Enable attribute scaling")
set(RIX_CORE_NUMBER_OF_TEXTURE_PAGES 256 CACHE STRING "Texture page count")
set(RIX_CORE_NUMBER_OF_TEXTURES 256 CACHE STRING "Texture count")
set(RIX_CORE_TEXTURE_PAGE_SIZE 4096 CACHE STRING "Texture page size")
set(RIX_CORE_GRAM_MEMORY_LOC 0 CACHE STRING "Simulation GRAM base")
set(RIX_CORE_COLOR_BUFFER_LOC_0 0 CACHE STRING "System framebuffer base")
set(RIX_CORE_COLOR_BUFFER_LOC_1 0 CACHE STRING "Color framebuffer base")
set(RIX_CORE_COLOR_BUFFER_LOC_2 0 CACHE STRING "Second color framebuffer base")
set(RIX_CORE_DEPTH_BUFFER_LOC 0 CACHE STRING "Depth framebuffer base")
set(RIX_CORE_STENCIL_BUFFER_LOC 0 CACHE STRING "Stencil framebuffer base")
set(RIX_CORE_THREADED_RASTERIZATION false CACHE STRING "Use synchronous frontend")
set(RIX_CORE_THREADED_RASTERIZATION_DISPLAY_LIST_SIZE "1024 * 1024 * 4" CACHE STRING "Display list size")
set(RIX_CORE_ENABLE_VSYNC false CACHE STRING "Disable vsync")
set(RIX_CORE_MAX_VBO_COUNT 256 CACHE STRING "Maximum VBOs")
set(RIX_CORE_AUTOLOAD_INTERNAL_FRAMEBUFFER false CACHE STRING "Keep framebuffer explicit")
set(RIX_CORE_SOFTWARE_RENDERING false CACHE STRING "Use RasterIX hardware protocol")

if(NOT DEFINED RASTERIX_ROOT OR NOT DEFINED VERILATOR_OBJDIR)
    message(FATAL_ERROR "RASTERIX_ROOT and VERILATOR_OBJDIR are required")
endif()

add_subdirectory("${RASTERIX_ROOT}/lib" rasterix-lib)

add_library(verilator_runtime STATIC
    /usr/share/verilator/include/verilated.cpp
    /usr/share/verilator/include/verilated_threads.cpp)
target_include_directories(verilator_runtime PUBLIC /usr/share/verilator/include)

add_executable(rasterix_harness harness.cpp)
target_compile_features(rasterix_harness PRIVATE cxx_std_17)
target_compile_options(rasterix_harness PRIVATE -O3 -Wall -Wextra -Wpedantic)
target_include_directories(rasterix_harness PRIVATE
    "${RASTERIX_ROOT}/lib"
    "${RASTERIX_ROOT}/lib/utils"
    "${RASTERIX_ROOT}/lib/3rdParty/span/include"
    "${VERILATOR_OBJDIR}")
target_link_libraries(rasterix_harness PRIVATE gl verilator_runtime "${VERILATOR_OBJDIR}/Vtop__ALL.a" pthread)
