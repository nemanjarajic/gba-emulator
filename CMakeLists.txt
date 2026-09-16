cmake_minimum_required(VERSION 3.24)
project(gba_gpu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# Homebrew supplies the whole Vulkan stack here (loader, headers, MoltenVK,
# glslc, validation layers). See env.sh for the runtime environment.
execute_process(COMMAND brew --prefix
                OUTPUT_VARIABLE HOMEBREW_PREFIX
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
if(HOMEBREW_PREFIX)
  list(APPEND CMAKE_PREFIX_PATH "${HOMEBREW_PREFIX}")
endif()

find_package(Vulkan REQUIRED)
find_program(GLSLC glslc HINTS "${HOMEBREW_PREFIX}/bin" REQUIRED)

# --- shader compilation ------------------------------------------------------
# Shaders are compiled to SPIR-V at build time. -g keeps the GLSL source line
# info in the SPIR-V, which is what makes validation-layer messages and
# debugPrintfEXT output point at real line numbers.
set(SHADER_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_DIR}")
set(SHADER_OUTPUTS "")

function(add_compute_shader SRC)
  get_filename_component(NAME "${SRC}" NAME_WE)
  set(OUT "${SHADER_DIR}/${NAME}.spv")
  # Glob the shared core headers so editing the emulator core triggers a
  # shader rebuild -- the core is #included into the shader, so CMake cannot
  # infer this dependency on its own.
  file(GLOB CORE_DEPS "${CMAKE_SOURCE_DIR}/src/core/*.h" "${CMAKE_SOURCE_DIR}/src/core/*.inc")
  add_custom_command(
    OUTPUT "${OUT}"
    COMMAND "${GLSLC}" --target-env=vulkan1.2 -O -g -DGBA_GLSL=1
            -I "${CMAKE_SOURCE_DIR}/src/core"
            -o "${OUT}" "${CMAKE_SOURCE_DIR}/${SRC}"
    DEPENDS "${CMAKE_SOURCE_DIR}/${SRC}" ${CORE_DEPS}
    COMMENT "glslc ${SRC}"
    VERBATIM)
  list(APPEND SHADER_OUTPUTS "${OUT}")
  set(SHADER_OUTPUTS "${SHADER_OUTPUTS}" PARENT_SCOPE)
endfunction()

add_compute_shader(src/shader/square.comp)
add_compute_shader(src/shader/core_compile_test.comp)
add_compute_shader(src/shader/gba.comp)
add_compute_shader(src/shader/gba_soa.comp)

add_custom_target(shaders ALL DEPENDS ${SHADER_OUTPUTS})

# --- library -----------------------------------------------------------------
# Host-side backing store for the dual-compiled core (C++ build of src/core/).
add_library(gba_host STATIC src/host/memory.cpp src/host/png.cpp)
target_include_directories(gba_host PUBLIC src)
target_compile_options(gba_host PRIVATE -Wall -Wextra)

add_library(gba_vk STATIC src/gpu/vk_context.cpp src/gpu/instance_pool.cpp)
target_include_directories(gba_vk PUBLIC src)
target_link_libraries(gba_vk PUBLIC Vulkan::Vulkan)
target_compile_definitions(gba_vk PUBLIC SHADER_DIR="${SHADER_DIR}")
target_compile_options(gba_vk PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

# --- executables -------------------------------------------------------------
add_executable(m0_square src/app/m0_square.cpp)
target_link_libraries(m0_square PRIVATE gba_vk)
add_dependencies(m0_square shaders)

add_executable(m1_membus src/app/m1_membus.cpp)
target_link_libraries(m1_membus PRIVATE gba_host)
target_compile_options(m1_membus PRIVATE -Wall -Wextra)

# Runs any of the jsmolka CPU test ROMs against the reference core.
add_executable(cpu_test src/app/cpu_test.cpp)
target_link_libraries(cpu_test PRIVATE gba_host)
target_compile_options(cpu_test PRIVATE -Wall -Wextra)

# Runs a full test ROM inside the compute shader and diffs against the CPU.
add_executable(m4_gpu_test src/app/m4_gpu_test.cpp)
target_link_libraries(m4_gpu_test PRIVATE gba_host gba_vk)
add_dependencies(m4_gpu_test shaders)
target_compile_options(m4_gpu_test PRIVATE -Wall -Wextra)

add_executable(m5_ppu src/app/m5_ppu.cpp)
target_link_libraries(m5_ppu PRIVATE gba_host gba_vk)
add_dependencies(m5_ppu shaders)
target_compile_options(m5_ppu PRIVATE -Wall -Wextra)

add_executable(m6_sprites src/app/m6_sprites.cpp)
target_link_libraries(m6_sprites PRIVATE gba_host gba_vk)
add_dependencies(m6_sprites shaders)
target_compile_options(m6_sprites PRIVATE -Wall -Wextra)

add_executable(m6_coverage src/app/m6_coverage.cpp)
target_link_libraries(m6_coverage PRIVATE gba_host gba_vk)
add_dependencies(m6_coverage shaders)
target_compile_options(m6_coverage PRIVATE -Wall -Wextra)

add_executable(m6_effects src/app/m6_effects.cpp)
target_link_libraries(m6_effects PRIVATE gba_host gba_vk)
add_dependencies(m6_effects shaders)
target_compile_options(m6_effects PRIVATE -Wall -Wextra)

add_executable(m7_io src/app/m7_io.cpp)
target_link_libraries(m7_io PRIVATE gba_host)
target_compile_options(m7_io PRIVATE -Wall -Wextra)

add_executable(m8_bench src/app/m8_bench.cpp)
target_link_libraries(m8_bench PRIVATE gba_host gba_vk)
add_dependencies(m8_bench shaders)
target_compile_options(m8_bench PRIVATE -Wall -Wextra)

add_executable(m9_harness src/app/m9_harness.cpp)
target_link_libraries(m9_harness PRIVATE gba_host gba_vk)
add_dependencies(m9_harness shaders)
target_compile_options(m9_harness PRIVATE -Wall -Wextra)

# Renders any ROM on both targets, compares, and writes a PNG.
add_executable(render_rom src/app/render_rom.cpp)
target_link_libraries(render_rom PRIVATE gba_host gba_vk)
add_dependencies(render_rom shaders)
target_compile_options(render_rom PRIVATE -Wall -Wextra)

# Runs the same core source on both targets and diffs the results.
add_executable(m1_parity src/app/m1_parity.cpp)
target_link_libraries(m1_parity PRIVATE gba_host gba_vk)
add_dependencies(m1_parity shaders)
target_compile_options(m1_parity PRIVATE -Wall -Wextra)
