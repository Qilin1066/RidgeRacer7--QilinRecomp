cmake_minimum_required(VERSION 3.25)
# macOS: what players run (the runtime) is built for macOS 10.15, the oldest
# macOS a Vulkan 1.3 MoltenVK runs on; so runtime code avoids std::format,
# which Apple's libc++ only has from 13.3. The developer tools (qilin, the
# tests) need 13.3 and are built for it.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15 CACHE STRING "oldest macOS the runtime runs on")
endif()
# The single source of the version number (see CHANGELOG.md for the scheme).
project(RidgeRacer7QilinRecomp VERSION 0.13.0 LANGUAGES C CXX)

# ---- Language: C++23 everywhere (recompiler, runtime, generated code) ----
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Minimum compilers for the C++23 language and library features we use:
# GCC 14, Clang 19, Xcode 16 (AppleClang 16), Visual Studio 2022 17.10
# (std::expected, std::print, std::ranges, std::string::contains).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
    message(FATAL_ERROR "GCC ${CMAKE_CXX_COMPILER_VERSION} is too old: C++23 <print> needs GCC 14 or newer")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19)
    # Clang 18 lacks the C++20 concepts level libstdc++ requires for std::expected.
    message(FATAL_ERROR "Clang ${CMAKE_CXX_COMPILER_VERSION} is too old: needs Clang 19 or newer")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
    message(FATAL_ERROR "AppleClang ${CMAKE_CXX_COMPILER_VERSION} is too old: needs Xcode 16 or newer")
elseif(MSVC AND MSVC_VERSION LESS 1940)
    message(FATAL_ERROR "MSVC ${MSVC_VERSION} is too old: needs Visual Studio 2022 17.10 or newer")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "64-bit hosts only (see docs/roadmap-and-design-targets.md)")
endif()

option(QILIN_WERROR "Treat warnings in our own code as errors" OFF)

# Floating point is bit-exact with the guest: the host compiler must not fuse
# a multiply and an add on its own (the PowerPC fmadd family is fused, and
# will be written as std::fma; nothing else is).
if(NOT MSVC)
    add_compile_options(-ffp-contract=off)
endif()

# ---- Warnings for our own targets ----
add_library(qilin_warnings INTERFACE)
if(MSVC)
    target_compile_options(qilin_warnings INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor)
    # fopen/strerror etc. are standard C++; MSVC's "use the _s variants" advice is not.
    target_compile_definitions(qilin_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
    if(QILIN_WERROR)
        target_compile_options(qilin_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(qilin_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
    if(QILIN_WERROR)
        target_compile_options(qilin_warnings INTERFACE -Werror)
    endif()
endif()

# Developer tools on macOS: macOS 13.3 or later (std::format).
function(qilin_tool target)
    if(APPLE)
        target_compile_options(${target} PRIVATE -mmacosx-version-min=13.3)
        target_link_options(${target} PRIVATE -mmacosx-version-min=13.3)
    endif()
endfunction()

# ---- The game's files: runtime code too (no std::format) ----
# The program (ELF), the disc's encrypted copy of it (SELF, its plain header
# only), PARAM.SFO, and finding them in the player's copy.
add_library(qilin_game STATIC
    recomp/src/elf.cpp
    recomp/src/import_names.cpp
    recomp/src/media.cpp
    recomp/src/self.cpp
    recomp/src/sfo.cpp
    recomp/src/sha256.cpp
    recomp/src/spu_image.cpp
    recomp/src/syscall_names.cpp
    recomp/src/title.cpp
)
target_include_directories(qilin_game PUBLIC recomp/src)
target_link_libraries(qilin_game PRIVATE qilin_warnings)

# ---- The recompiler's own parts: tools only (std::format is fine) ----
add_library(qilin_recomp STATIC
    recomp/src/archive.cpp
    recomp/src/cfg.cpp
    recomp/src/lift.cpp
    recomp/src/ppu.cpp
    recomp/src/shaders.cpp
    recomp/src/spu.cpp
    recomp/src/spu_cfg.cpp
    recomp/src/spu_lift.cpp
)
target_include_directories(qilin_recomp PUBLIC recomp/src)
target_link_libraries(qilin_recomp PUBLIC qilin_game PRIVATE qilin_warnings)
qilin_tool(qilin_recomp)

# ---- The runtime: what lifted code compiles against (no std::format) ----
add_library(qilin_rt STATIC
    runtime/src/memory.cpp
    runtime/src/report.cpp
)
target_include_directories(qilin_rt PUBLIC runtime/include)
target_link_libraries(qilin_rt PRIVATE qilin_warnings)

# ---- The qilin tool ----
add_executable(qilin recomp/src/main.cpp)
target_link_libraries(qilin PRIVATE qilin_game qilin_recomp qilin_warnings)
qilin_tool(qilin)
target_compile_definitions(qilin PRIVATE QILIN_VERSION="${PROJECT_VERSION}")

enable_testing()
add_executable(qilin_tests recomp/tests/tests.cpp)
target_link_libraries(qilin_tests PRIVATE qilin_game qilin_recomp qilin_warnings)
qilin_tool(qilin_tests)
add_test(NAME qilin_tests COMMAND qilin_tests)
# qilin's exit codes: a usage error is 2, a missing file 1, the version 0.
add_test(NAME qilin_version COMMAND qilin --version)
add_test(NAME qilin_missing_file COMMAND qilin info no-such-folder)
set_tests_properties(qilin_missing_file PROPERTIES WILL_FAIL TRUE)
add_test(NAME qilin_usage COMMAND qilin)
set_tests_properties(qilin_usage PROPERTIES WILL_FAIL TRUE)

# ---- The game ----
# Its machine (memory, the first thread, the boundary with the system) is
# built always, so that CI compiles it. The executable itself needs the lifted
# code, which is made from the player's own copy and is never in the
# repository: `qilin lift <game> --out <folder>`, then configure with
# -DQILIN_GEN_DIR=<folder> (tools/build-ridgeracer7.ps1 does both).
# ---- The host platform: window, input and audio (SDL3), Vulkan ----
# The other Qilin recomps' host layer (host/), with this game's names. Fetched at
# configure time, pinned, built statically: SDL3, the Vulkan headers and volk
# (which loads Vulkan at run time, so no SDK is needed to build or run).
option(QILIN_HOST "Build the host platform layer (window, Vulkan, input, audio)" ON)
if(QILIN_HOST)
    include(FetchContent)
    set(FETCHCONTENT_QUIET ON)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)  # synthetic input: not needed
    FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL GIT_TAG release-3.4.20 GIT_SHALLOW ON)
    FetchContent_Declare(VulkanHeaders GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers
                         GIT_TAG vulkan-sdk-1.4.363.0 GIT_SHALLOW ON)
    # volk is one C file: fetched without its own CMake project (SOURCE_SUBDIR points nowhere).
    FetchContent_Declare(volk GIT_REPOSITORY https://github.com/zeux/volk GIT_TAG vulkan-sdk-1.4.363.0 GIT_SHALLOW ON
                         SOURCE_SUBDIR no-cmake)
    FetchContent_MakeAvailable(SDL3 VulkanHeaders volk)
    add_library(qilin_volk STATIC ${volk_SOURCE_DIR}/volk.c)
    target_include_directories(qilin_volk PUBLIC ${volk_SOURCE_DIR})
    target_link_libraries(qilin_volk PUBLIC Vulkan::Headers ${CMAKE_DL_LIBS})
    target_compile_definitions(qilin_volk PUBLIC VK_NO_PROTOTYPES)

    add_library(qilin_host STATIC host/src/vulkan.cpp host/src/display.cpp host/src/audio.cpp host/src/paths.cpp)
    target_include_directories(qilin_host PUBLIC host/include)
    target_link_libraries(qilin_host PUBLIC qilin_volk PRIVATE SDL3::SDL3-static qilin_warnings)
    # Vulkan structures are written {sType} with the rest zeroed: that's the idiom, not a mistake.
    # (On the sources, so it comes after -Wextra on the command line.)
    if(NOT MSVC)
        set_source_files_properties(host/src/vulkan.cpp host/src/display.cpp host/src/audio.cpp host/src/paths.cpp host/tests/vktest.cpp
                                    PROPERTIES COMPILE_OPTIONS -Wno-missing-field-initializers)
    endif()

    # The Vulkan layer's own test: a device, an offscreen frame, read back. Skips
    # (exit 77) where there's no Vulkan 1.3 device, as on CI runners.
    add_executable(qilin_vktest host/tests/vktest.cpp)
    target_link_libraries(qilin_vktest PRIVATE qilin_host qilin_warnings)
    add_test(NAME qilin_vktest COMMAND qilin_vktest)
    set_tests_properties(qilin_vktest PROPERTIES SKIP_RETURN_CODE 77)

    # A controller reaches the game (SDL's virtual gamepad). Needs a window:
    # skips where there's no display, as on CI runners.
    add_executable(qilin_padtest host/tests/padtest.cpp)
    target_link_libraries(qilin_padtest PRIVATE qilin_host SDL3::SDL3-static qilin_warnings)
    add_test(NAME qilin_padtest COMMAND qilin_padtest)
    set_tests_properties(qilin_padtest PROPERTIES SKIP_RETURN_CODE 77)
endif()
add_executable(qilin_layout_test host/tests/layout_test.cpp)
target_include_directories(qilin_layout_test PRIVATE host/include)
target_link_libraries(qilin_layout_test PRIVATE qilin_warnings)
add_test(NAME qilin_layout_test COMMAND qilin_layout_test)
add_executable(qilin_frame_times_test host/tests/frame_times_test.cpp)
target_include_directories(qilin_frame_times_test PRIVATE host/include)
target_link_libraries(qilin_frame_times_test PRIVATE qilin_warnings)
add_test(NAME qilin_frame_times_test COMMAND qilin_frame_times_test)

# FreeType (the FreeType Licence), for the system's font library, which is built on it on the console
# too: fetched, pinned, built statically, with none of its optional parts.
include(FetchContent)
set(FETCHCONTENT_QUIET ON)
foreach(part ZLIB BZIP2 PNG HARFBUZZ BROTLI)
    set(FT_DISABLE_${part} ON CACHE BOOL "" FORCE)
endforeach()
FetchContent_Declare(freetype
    URL https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz
    URL_HASH SHA256=0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289)
set(qilin_saved_shared ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(freetype)
set(BUILD_SHARED_LIBS ${qilin_saved_shared})

add_library(ridgeracer7_machine STATIC
    ridgeracer7/src/events.cpp
    ridgeracer7/src/audio.cpp
    ridgeracer7/src/fault.cpp
    ridgeracer7/src/font.cpp
    ridgeracer7/src/fs.cpp
    ridgeracer7/src/gamedata.cpp
    ridgeracer7/src/gcm.cpp
    ridgeracer7/src/imports.cpp
    ridgeracer7/src/kernel.cpp
    ridgeracer7/src/machine.cpp
    ridgeracer7/src/memory.cpp
    ridgeracer7/src/mempool.cpp
    ridgeracer7/src/net.cpp
    ridgeracer7/src/np.cpp
    ridgeracer7/src/pad.cpp
    ridgeracer7/src/rsx.cpp
    ridgeracer7/src/spu.cpp
    ridgeracer7/src/spurs.cpp
    ridgeracer7/src/sync.cpp
    ridgeracer7/src/sync_calls.cpp
    ridgeracer7/src/syscalls.cpp
    ridgeracer7/src/sysutil.cpp
    ridgeracer7/src/threads.cpp
    ridgeracer7/src/timers.cpp
    ridgeracer7/src/tty.cpp
    ridgeracer7/src/usbd.cpp
    ridgeracer7/src/video.cpp
)
target_include_directories(ridgeracer7_machine PUBLIC ridgeracer7/src)
target_link_libraries(ridgeracer7_machine PUBLIC qilin_game qilin_rt PRIVATE qilin_warnings freetype)
# FreeType's headers are not ours to warn about.
target_include_directories(ridgeracer7_machine SYSTEM PRIVATE $<TARGET_PROPERTY:freetype,INTERFACE_INCLUDE_DIRECTORIES>)
set(QILIN_GEN_DIR "" CACHE PATH "The folder `qilin lift <game> --out` wrote: the lifted code of your own copy")
if(QILIN_GEN_DIR)
    if(NOT EXISTS "${QILIN_GEN_DIR}/table.cpp")
        message(FATAL_ERROR "QILIN_GEN_DIR (${QILIN_GEN_DIR}) holds no lifted code: run `qilin lift <game> --out ${QILIN_GEN_DIR}` first")
    endif()
    file(GLOB qilin_lifted CONFIGURE_DEPENDS "${QILIN_GEN_DIR}/*.cpp")
    add_executable(ridgeracer7 ridgeracer7/src/main.cpp ${qilin_lifted})
    target_include_directories(ridgeracer7 PRIVATE "${QILIN_GEN_DIR}")
    target_link_libraries(ridgeracer7 PRIVATE ridgeracer7_machine qilin_host qilin_warnings)
    # Lifted code is generated: its unused labels and the like are not warnings worth reading.
    if(MSVC)
        set_source_files_properties(${qilin_lifted} PROPERTIES COMPILE_OPTIONS "/w;/bigobj")
        target_compile_options(ridgeracer7 PRIVATE /MP)  # the lifted files side by side
    else()
        set_source_files_properties(${qilin_lifted} PROPERTIES COMPILE_OPTIONS "-w")
    endif()
endif()

# ---- The differential test of the lifter ----
# qilin_difftest_gen lifts random encodings of every instruction in the
# lifter's slice; its output is compiled into qilin_difftest, which runs each
# against a separately written reference interpreter (recomp/tests/difftest).
add_executable(qilin_difftest_gen recomp/tests/difftest/gen.cpp)
target_include_directories(qilin_difftest_gen PRIVATE recomp/tests/difftest)
target_link_libraries(qilin_difftest_gen PRIVATE qilin_recomp qilin_rt qilin_warnings)
qilin_tool(qilin_difftest_gen)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/difftest_cases.cpp
    COMMAND qilin_difftest_gen ${CMAKE_CURRENT_BINARY_DIR}/difftest_cases.cpp
    DEPENDS qilin_difftest_gen
    COMMENT "Lifting the differential test's cases")
add_executable(qilin_difftest
    recomp/tests/difftest/run.cpp
    recomp/tests/difftest/ref.cpp
    recomp/tests/difftest/ref_fp.cpp
    recomp/tests/difftest/ref_vmx.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/difftest_cases.cpp)
target_include_directories(qilin_difftest PRIVATE recomp/tests/difftest)
target_link_libraries(qilin_difftest PRIVATE qilin_recomp qilin_rt qilin_warnings)
qilin_tool(qilin_difftest)
# Lifted code is generated: its unused labels and the like are not warnings worth reading.
if(MSVC)
    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/difftest_cases.cpp PROPERTIES COMPILE_OPTIONS "/w")
else()
    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/difftest_cases.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()
add_test(NAME qilin_difftest COMMAND qilin_difftest)

# The same for the SPU lifter (recomp/tests/spu_difftest): single instructions against its own reference.
add_executable(qilin_spu_difftest_gen recomp/tests/spu_difftest/gen.cpp)
target_link_libraries(qilin_spu_difftest_gen PRIVATE qilin_recomp qilin_rt qilin_warnings)
qilin_tool(qilin_spu_difftest_gen)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/spu_difftest_cases.cpp
    COMMAND qilin_spu_difftest_gen ${CMAKE_CURRENT_BINARY_DIR}/spu_difftest_cases.cpp
    DEPENDS qilin_spu_difftest_gen
    VERBATIM)
add_executable(qilin_spu_difftest
    recomp/tests/spu_difftest/run.cpp
    recomp/tests/spu_difftest/ref.cpp
    recomp/tests/spu_difftest/ref_fp.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/spu_difftest_cases.cpp)
target_include_directories(qilin_spu_difftest PRIVATE recomp/tests/spu_difftest)
target_link_libraries(qilin_spu_difftest PRIVATE qilin_recomp qilin_rt qilin_warnings)
qilin_tool(qilin_spu_difftest)
if(MSVC)
    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/spu_difftest_cases.cpp PROPERTIES COMPILE_OPTIONS "/w")
else()
    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/spu_difftest_cases.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()
add_test(NAME qilin_spu_difftest COMMAND qilin_spu_difftest)
