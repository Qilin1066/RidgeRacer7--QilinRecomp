cmake_minimum_required(VERSION 3.25)
# macOS: what players run (the runtime) is built for macOS 10.15, the oldest
# macOS a Vulkan 1.3 MoltenVK runs on; so runtime code avoids std::format,
# which Apple's libc++ only has from 13.3. The developer tools (qilin, the
# tests) need 13.3 and are built for it.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15 CACHE STRING "oldest macOS the runtime runs on")
endif()
# The single source of the version number (see CHANGELOG.md for the scheme).
project(RidgeRacer7QilinRecomp VERSION 0.8.0 LANGUAGES CXX)

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
)
target_include_directories(qilin_recomp PUBLIC recomp/src)
target_link_libraries(qilin_recomp PUBLIC qilin_game PRIVATE qilin_warnings)
qilin_tool(qilin_recomp)

# ---- The runtime: what lifted code compiles against (no std::format) ----
add_library(qilin_rt STATIC
    runtime/src/memory.cpp
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
