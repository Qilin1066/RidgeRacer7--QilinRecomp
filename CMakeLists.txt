cmake_minimum_required(VERSION 3.25)
# macOS: what players run (the runtime) is built for macOS 10.15, the oldest
# macOS a Vulkan 1.3 MoltenVK runs on; so runtime code avoids std::format,
# which Apple's libc++ only has from 13.3. The developer tools (qilin, the
# tests) need 13.3 and are built for it.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15 CACHE STRING "oldest macOS the runtime runs on")
endif()
# The single source of the version number (see CHANGELOG.md for the scheme).
project(RidgeRacer7QilinRecomp VERSION 0.13.1 LANGUAGES C CXX)

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

# The compiler's settings for speed and exactness: floating point bit-exact with the guest (no contraction), the
# processor presets (QILIN_CPU: pi4, pi5, ...), the lifted code's level, LTO and PGO of the game. docs/toolchain.md.
include(cmake/toolchain.cmake)

# ---- Warnings for our own targets ----
add_library(qilin_warnings INTERFACE)
if(MSVC)
    target_compile_options(qilin_warnings INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    if(QILIN_REAL_MSVC)
        target_compile_options(qilin_warnings INTERFACE /Zc:preprocessor)  # (clang-cl's preprocessor is standard already)
    endif()
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
    recomp/src/rsx_program.cpp
    recomp/src/rsx_spirv.cpp
    recomp/src/spirv.cpp
)
target_include_directories(qilin_recomp PUBLIC recomp/src)
target_link_libraries(qilin_recomp PUBLIC qilin_game PRIVATE qilin_warnings)
qilin_tool(qilin_recomp)

# ---- The runtime: what lifted code compiles against (no std::format) ----
add_library(qilin_rt STATIC
    runtime/src/memory.cpp
    runtime/src/report.cpp
    runtime/src/unwind.cpp
)
target_include_directories(qilin_rt PUBLIC runtime/include)
target_link_libraries(qilin_rt PRIVATE qilin_warnings)

# ---- The qilin tool ----
add_executable(qilin recomp/src/main.cpp)
target_link_libraries(qilin PRIVATE qilin_game qilin_recomp qilin_warnings)
qilin_tool(qilin)
target_compile_definitions(qilin PRIVATE QILIN_VERSION="${PROJECT_VERSION}")

enable_testing()
find_package(Threads REQUIRED)
add_executable(ridgeracer7_runtime_io_test ridgeracer7/tests/runtime_io_test.cpp
    ridgeracer7/src/fs.cpp ridgeracer7/src/threads.cpp ridgeracer7/src/kernel_sched.cpp)
target_include_directories(ridgeracer7_runtime_io_test PRIVATE recomp/src runtime/include ridgeracer7/src)
target_link_libraries(ridgeracer7_runtime_io_test PRIVATE qilin_warnings Threads::Threads)
foreach(case open read-close threads)
    add_test(NAME ridgeracer7_runtime_io_${case} COMMAND ridgeracer7_runtime_io_test ${case})
    set_tests_properties(ridgeracer7_runtime_io_${case} PROPERTIES TIMEOUT 20)
endforeach()
# SPU threads and SPURS tasks (spu.cpp) with made-up programs, no game.
add_executable(ridgeracer7_spu_test ridgeracer7/tests/spu_test.cpp ridgeracer7/src/spu.cpp)
target_include_directories(ridgeracer7_spu_test PRIVATE recomp/src runtime/include ridgeracer7/src)
target_link_libraries(ridgeracer7_spu_test PRIVATE qilin_game qilin_host qilin_warnings Threads::Threads)  # (qilin_host: the sound core's thread priority)
foreach(case tasks group)
    add_test(NAME ridgeracer7_spu_${case} COMMAND ridgeracer7_spu_test ${case})
    set_tests_properties(ridgeracer7_spu_${case} PROPERTIES TIMEOUT 60)
endforeach()
# The scheduler and the synchronisation objects' waking orders, with guest memory stood in.
add_executable(ridgeracer7_sched_test ridgeracer7/tests/sched_test.cpp
    ridgeracer7/src/kernel_sched.cpp ridgeracer7/src/sync.cpp ridgeracer7/src/sync_calls.cpp ridgeracer7/src/events.cpp)
target_include_directories(ridgeracer7_sched_test PRIVATE recomp/src runtime/include ridgeracer7/src)
target_link_libraries(ridgeracer7_sched_test PRIVATE qilin_warnings Threads::Threads)
foreach(case fast priority two-tokens switch-point yield blocking sleep lend free mutex-fifo mutex-priority semaphore-fifo
        semaphore-priority lwmutex-fifo lwmutex-priority cond-priority cond-timeout events-fifo events-priority events-cancel stress)
    add_test(NAME ridgeracer7_sched_${case} COMMAND ridgeracer7_sched_test ${case})
    set_tests_properties(ridgeracer7_sched_${case} PROPERTIES TIMEOUT 30)
endforeach()
# The sound mixer's pacing against a fake sound card and a fake game, and its ports' blocks: no window, card or game.
add_executable(ridgeracer7_audio_test ridgeracer7/tests/audio_test.cpp)
target_include_directories(ridgeracer7_audio_test PRIVATE host/include ridgeracer7/src)
target_link_libraries(ridgeracer7_audio_test PRIVATE qilin_warnings)
foreach(case blocks restart jittery old coarse drift big-request stall)
    add_test(NAME ridgeracer7_audio_${case} COMMAND ridgeracer7_audio_test ${case})
    set_tests_properties(ridgeracer7_audio_${case} PROPERTIES TIMEOUT 60)
endforeach()
# The renderer's pure parts (textures' channel orders, surfaces' pixels, vertex attributes, primitives, rasterisation): no
# GPU, no game.
add_executable(ridgeracer7_render_test ridgeracer7/tests/render_test.cpp ridgeracer7/src/textures.cpp ridgeracer7/src/render_formats.cpp
    ridgeracer7/src/render_state.cpp)
target_include_directories(ridgeracer7_render_test PRIVATE ridgeracer7/src)
target_link_libraries(ridgeracer7_render_test PRIVATE qilin_warnings)
add_test(NAME ridgeracer7_render_test COMMAND ridgeracer7_render_test)
# The translated programs' other forms for draw state (rsx_spirv.hpp's Variant), on synthetic programs.
add_executable(ridgeracer7_shader_forms_test ridgeracer7/tests/shader_forms_test.cpp)
target_link_libraries(ridgeracer7_shader_forms_test PRIVATE qilin_recomp qilin_warnings)
qilin_tool(ridgeracer7_shader_forms_test)
add_test(NAME ridgeracer7_shader_forms_test COMMAND ridgeracer7_shader_forms_test)
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

    # Dear ImGui (MIT) for the settings menu, as the other Qilin recomps draw
    # theirs: the same commit (v1.93.0), its core and its SDL3 and Vulkan
    # backends, fetched and pinned. Its Vulkan backend loads its functions
    # through volk (no prototypes).
    FetchContent_Declare(imgui
        URL https://github.com/ocornut/imgui/archive/c1aaac65e896a45f81f47a4d68f9959e0f86c859.tar.gz
        URL_HASH SHA256=b3d8f99838bb3ff05e14185562345f85e00c032da4d8fc277608d7898d847114)
    FetchContent_MakeAvailable(imgui)
    add_library(qilin_imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
    target_include_directories(qilin_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_compile_definitions(qilin_imgui PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES VK_NO_PROTOTYPES IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    target_link_libraries(qilin_imgui PUBLIC SDL3::SDL3-static Vulkan::Headers)
    if(MSVC)
        target_compile_options(qilin_imgui PRIVATE /W0)  # third-party code: its warnings aren't ours
    else()
        target_compile_options(qilin_imgui PRIVATE -w)
    endif()

    add_library(qilin_host STATIC host/src/vulkan.cpp host/src/display.cpp host/src/audio.cpp host/src/paths.cpp)
    target_include_directories(qilin_host PUBLIC host/include)
    target_link_libraries(qilin_host PUBLIC qilin_volk PRIVATE SDL3::SDL3-static qilin_warnings)
    # Vulkan structures are written {sType} with the rest zeroed: that's the idiom, not a mistake.
    # (On the sources, so it comes after -Wextra on the command line.)
    if(NOT QILIN_REAL_MSVC)  # (clang-cl too)
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
include(cmake/ffmpeg.cmake)

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
    ridgeracer7/src/kernel_sched.cpp
    ridgeracer7/src/machine.cpp
    ridgeracer7/src/memory.cpp
    ridgeracer7/src/mempool.cpp
    ridgeracer7/src/net.cpp
    ridgeracer7/src/np.cpp
    ridgeracer7/src/pad.cpp
    ridgeracer7/src/pause.cpp
    ridgeracer7/src/renderer.cpp
    ridgeracer7/src/renderer_pipelines.cpp
    ridgeracer7/src/render_formats.cpp
    ridgeracer7/src/render_state.cpp
    ridgeracer7/src/textures.cpp
    ridgeracer7/src/rtc.cpp
    ridgeracer7/src/savedata.cpp
    ridgeracer7/src/settings.cpp
    ridgeracer7/src/film.cpp
    ridgeracer7/src/atrac.cpp
    ridgeracer7/src/rsx.cpp
    ridgeracer7/src/rsx_state.cpp
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
# Vulkan structures are written {sType} with the rest zeroed: the idiom, not a mistake (as host/'s sources).
if(NOT QILIN_REAL_MSVC)
    set_source_files_properties(ridgeracer7/src/renderer.cpp ridgeracer7/src/renderer_pipelines.cpp PROPERTIES COMPILE_OPTIONS -Wno-missing-field-initializers)
endif()
if(NOT TARGET qilin_host)
    message(FATAL_ERROR "the game's machine needs the host layer: configure with -DQILIN_HOST=ON")
endif()
target_link_libraries(ridgeracer7_machine PUBLIC qilin_game qilin_rt qilin_host PRIVATE qilin_warnings freetype qilin_ffmpeg)
# FreeType's headers are not ours to warn about.
target_include_directories(ridgeracer7_machine SYSTEM PRIVATE $<TARGET_PROPERTY:freetype,INTERFACE_INCLUDE_DIRECTORIES>)
# The settings menu (Dear ImGui over the window): built always, so that CI compiles it.
add_library(ridgeracer7_window STATIC ridgeracer7/src/menu.cpp)
target_link_libraries(ridgeracer7_window PUBLIC ridgeracer7_machine qilin_host qilin_imgui SDL3::SDL3-static PRIVATE qilin_warnings)
target_compile_definitions(ridgeracer7_window PRIVATE RIDGERACER7_VERSION="${PROJECT_VERSION}")
qilin_game_target(ridgeracer7_machine)
qilin_game_target(ridgeracer7_window)
if(NOT QILIN_REAL_MSVC)
    target_compile_options(ridgeracer7_window PRIVATE -Wno-missing-field-initializers)
endif()
# The RSX's side of a frame timed on the CPU: a synthetic frame of 2,000 draws through the command processing, the draws'
# gathering and the renderer, every Vulkan call a stub that digests what the GPU would be given. No GPU, no game.
add_executable(ridgeracer7_rsx_bench ridgeracer7/tests/rsx_bench.cpp ridgeracer7/src/rsx_state.cpp ridgeracer7/src/renderer.cpp
    ridgeracer7/src/renderer_pipelines.cpp ridgeracer7/src/render_formats.cpp ridgeracer7/src/render_state.cpp ridgeracer7/src/textures.cpp)
target_include_directories(ridgeracer7_rsx_bench PRIVATE recomp/src ridgeracer7/src)
target_link_libraries(ridgeracer7_rsx_bench PRIVATE qilin_host qilin_rt qilin_warnings)
if(NOT QILIN_REAL_MSVC)
    target_compile_options(ridgeracer7_rsx_bench PRIVATE -Wno-missing-field-initializers)
endif()
add_test(NAME ridgeracer7_rsx_bench COMMAND ridgeracer7_rsx_bench --frames 2)
# The settings file and the controller from its bindings, and the pause's holding and letting go: no game, no window.
add_executable(ridgeracer7_settings_test ridgeracer7/tests/settings_test.cpp ridgeracer7/src/settings.cpp ridgeracer7/src/pause.cpp)
target_include_directories(ridgeracer7_settings_test PRIVATE recomp/src runtime/include ridgeracer7/src host/include)
target_link_libraries(ridgeracer7_settings_test PRIVATE qilin_rt qilin_warnings Threads::Threads)
add_test(NAME ridgeracer7_settings_test COMMAND ridgeracer7_settings_test)
set_tests_properties(ridgeracer7_settings_test PROPERTIES TIMEOUT 20)
set(QILIN_GEN_DIR "" CACHE PATH "The folder `qilin lift <game> --out` wrote: the lifted code of your own copy")
if(QILIN_GEN_DIR)
    if(NOT EXISTS "${QILIN_GEN_DIR}/table.cpp" OR NOT EXISTS "${QILIN_GEN_DIR}/spu_table.cpp")
        message(FATAL_ERROR "QILIN_GEN_DIR (${QILIN_GEN_DIR}) holds no lifted code: run `qilin lift <game> --out ${QILIN_GEN_DIR}` first")
    endif()
    file(GLOB qilin_lifted CONFIGURE_DEPENDS "${QILIN_GEN_DIR}/*.cpp")
    # The translated shaders (`qilin shaders <game> --out`) are their own file, and have a check of their own.
    if(NOT EXISTS "${QILIN_GEN_DIR}/shaders.cpp")
        message(FATAL_ERROR "QILIN_GEN_DIR (${QILIN_GEN_DIR}) holds no translated shaders: run `qilin shaders <game> --out ${QILIN_GEN_DIR}` first")
    endif()
    add_executable(ridgeracer7_shader_check ridgeracer7/src/shader_check.cpp "${QILIN_GEN_DIR}/shaders.cpp")
    target_link_libraries(ridgeracer7_shader_check PRIVATE qilin_host qilin_rt qilin_warnings)
    if(MSVC)
        set_source_files_properties("${QILIN_GEN_DIR}/shaders.cpp" PROPERTIES COMPILE_OPTIONS "/w;/bigobj")
    else()
        set_source_files_properties("${QILIN_GEN_DIR}/shaders.cpp" PROPERTIES COMPILE_OPTIONS "-w")
    endif()
    if(NOT QILIN_REAL_MSVC)
        set_source_files_properties(ridgeracer7/src/shader_check.cpp PROPERTIES COMPILE_OPTIONS -Wno-missing-field-initializers)
    endif()
    add_executable(ridgeracer7 ridgeracer7/src/main.cpp ${qilin_lifted})
    target_include_directories(ridgeracer7 PRIVATE "${QILIN_GEN_DIR}")
    target_link_libraries(ridgeracer7 PRIVATE ridgeracer7_machine ridgeracer7_window qilin_host SDL3::SDL3-static qilin_warnings)
    # Lifted code is generated: its unused labels and the like are not warnings worth reading.
    if(MSVC)
        set(qilin_lifted_flags /w /bigobj ${QILIN_LIFTED_OPTIONS})
    else()
        set(qilin_lifted_flags -w ${QILIN_LIFTED_OPTIONS})
    endif()
    set_source_files_properties(${qilin_lifted} PROPERTIES COMPILE_OPTIONS "${qilin_lifted_flags}")
    if(QILIN_REAL_MSVC)
        target_compile_options(ridgeracer7 PRIVATE /MP)  # the lifted files side by side (Ninja runs clang-cl's in parallel itself)
    endif()
    qilin_game_target(ridgeracer7)
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

