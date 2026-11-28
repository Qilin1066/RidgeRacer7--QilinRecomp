# How the game is compiled for speed: the host compiler's floating-point and vector settings, the processor presets,
# link-time optimisation and profile-guided optimisation. docs/toolchain.md has the measurements behind each choice.
#
#   QILIN_CPU        ""                 the compiler's own baseline (plus SSSE3 on x86-64, below)
#                    pi4                ARM64: -mcpu=cortex-a72 -moutline-atomics (Raspberry Pi 4; runs on a Pi 5 too)
#                    pi5                ARM64: -mcpu=cortex-a76 (Raspberry Pi 5; not for a Pi 4)
#                    x86-64-v2|v3       x86-64 levels (v2: SSE4.2, 2009 on; v3: AVX2, 2013 on)
#                    native             this machine's processor (a build that may not run elsewhere)
#   QILIN_LIFTED_OPT O2 (default) | O3  the optimisation level of the lifted code
#   QILIN_LTO        OFF | ON           link-time optimisation of the game: ThinLTO (Clang), LTO (GCC), /GL and /LTCG (MSVC)
#   QILIN_LTO_JOBS   2                  how many code generators the link runs at once (each takes about what a compile takes)
#   QILIN_PGO        OFF | GENERATE | USE    profile-guided optimisation of the game, with QILIN_PGO_PROFILE
#
# Runtime code: nothing here changes what the code means. Floating point stays exact (no -ffast-math, no contraction):
# the relaxed mode for play is the runtime's own (a switch in its headers), not the compiler's.

# Which compiler this is, whatever its command line looks like: clang-cl is Clang with MSVC's options (CMake's MSVC is
# true for it too).
set(QILIN_CLANG FALSE)
set(QILIN_GCC FALSE)
set(QILIN_REAL_MSVC FALSE)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(QILIN_CLANG TRUE)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(QILIN_GCC TRUE)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(QILIN_REAL_MSVC TRUE)
endif()
set(QILIN_CLANG_CL FALSE)
if(QILIN_CLANG AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(QILIN_CLANG_CL TRUE)
endif()

# The target processor's family (the build's, not this machine's when cross-compiling).
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" qilin_processor)
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    set(qilin_processor "${CMAKE_OSX_ARCHITECTURES}")
endif()
set(QILIN_ARM64 FALSE)
set(QILIN_X86_64 FALSE)
if(qilin_processor MATCHES "^(aarch64|arm64)$")
    set(QILIN_ARM64 TRUE)
elseif(qilin_processor MATCHES "^(x86_64|amd64)$")
    set(QILIN_X86_64 TRUE)
endif()

# A helper: an option given to Clang's or GCC's driver, written the way this compiler's command line takes it
# (clang-cl passes a GCC-style option through /clang:).
function(qilin_gnu_option out)
    set(options ${ARGN})
    if(QILIN_CLANG_CL)
        list(TRANSFORM options PREPEND "/clang:")
    endif()
    set(${out} ${options} PARENT_SCOPE)
endfunction()

# ---- Floating point: exact ----
# The host compiler must not fuse a multiply and an add on its own (the PowerPC fmadd family is fused, and is written
# as std::fma; nothing else is). GCC and Clang (clang-cl too) are told so; MSVC doesn't contract under /fp:precise.
if(QILIN_CLANG OR QILIN_GCC)
    qilin_gnu_option(qilin_fp -ffp-contract=off)
    add_compile_options(${qilin_fp})
endif()

# No C++ modules here: Ninja would otherwise scan every file for them first (a second pass over each lifted file, and
# with clang-cl a clang-scan-deps that not every install has).
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# ---- The processor ----
# x86-64: SSSE3 always, as MSVC's build assumes (spu.hpp's byte shuffles; every x86-64 processor the game could run on
# has it, and spu.cpp checks it before the first SPU program runs). Without it GCC and Clang fell back to the plain C++
# shuffles, and a lifted SPU program ran 2.5 times slower with Clang than with MSVC (docs/toolchain.md).
set(QILIN_CPU "" CACHE STRING "Processor preset: empty (baseline), pi4, pi5, x86-64-v2, x86-64-v3 or native")
set_property(CACHE QILIN_CPU PROPERTY STRINGS "" pi4 pi5 x86-64-v2 x86-64-v3 native)
set(qilin_cpu_options "")
if(QILIN_CPU STREQUAL "pi4" OR QILIN_CPU STREQUAL "pi5")
    if(NOT QILIN_ARM64 OR NOT (QILIN_CLANG OR QILIN_GCC))
        message(FATAL_ERROR "QILIN_CPU=${QILIN_CPU} is for an ARM64 build with GCC or Clang (this is ${CMAKE_CXX_COMPILER_ID} for '${qilin_processor}')")
    endif()
    if(QILIN_CPU STREQUAL "pi4")
        # The Pi 4's Cortex-A72 is ARMv8.0: no LSE atomics. Outlined atomics pick LL/SC there and LSE on a Pi 5.
        set(qilin_cpu_options -mcpu=cortex-a72 -moutline-atomics)
    else()
        set(qilin_cpu_options -mcpu=cortex-a76)
    endif()
elseif(QILIN_CPU MATCHES "^x86-64-v[23]$")
    if(NOT QILIN_X86_64)
        message(FATAL_ERROR "QILIN_CPU=${QILIN_CPU} is for an x86-64 build (this is for '${qilin_processor}')")
    endif()
    if(QILIN_REAL_MSVC)
        # MSVC has no SSE4 switch: v2 is its default code with the SSSE3 shuffles, v3 /arch:AVX2.
        if(QILIN_CPU STREQUAL "x86-64-v3")
            set(qilin_cpu_options /arch:AVX2)
        endif()
    else()
        qilin_gnu_option(qilin_cpu_options -march=${QILIN_CPU})
    endif()
elseif(QILIN_CPU STREQUAL "native")
    if(QILIN_REAL_MSVC)
        message(FATAL_ERROR "QILIN_CPU=native needs GCC or Clang")
    elseif(QILIN_ARM64 AND NOT APPLE)
        set(qilin_cpu_options -mcpu=native)
    else()
        qilin_gnu_option(qilin_cpu_options -march=native)
    endif()
elseif(NOT QILIN_CPU STREQUAL "")
    message(FATAL_ERROR "QILIN_CPU=${QILIN_CPU}: not a preset (empty, pi4, pi5, x86-64-v2, x86-64-v3 or native)")
endif()
if(QILIN_X86_64 AND (QILIN_CLANG OR QILIN_GCC) AND QILIN_CPU STREQUAL "")
    qilin_gnu_option(qilin_cpu_options -mssse3)
endif()
add_compile_options(${qilin_cpu_options})

# ---- The lifted code's optimisation level ----
# -O2 measured as fast as -O3 on the lifted SPU programs and builds faster (docs/toolchain.md). CMake's Release is -O3
# for GCC and Clang and /O2 for MSVC and clang-cl; this sets the lifted files' own.
set(QILIN_LIFTED_OPT "O2" CACHE STRING "Optimisation level of the lifted code: O2 or O3")
set_property(CACHE QILIN_LIFTED_OPT PROPERTY STRINGS O2 O3)
if(NOT QILIN_LIFTED_OPT MATCHES "^O[23]$")
    message(FATAL_ERROR "QILIN_LIFTED_OPT=${QILIN_LIFTED_OPT}: O2 or O3")
endif()
set(QILIN_LIFTED_OPTIONS "")
if(QILIN_CLANG OR QILIN_GCC)
    qilin_gnu_option(QILIN_LIFTED_OPTIONS -${QILIN_LIFTED_OPT})
elseif(QILIN_LIFTED_OPT STREQUAL "O3")
    message(FATAL_ERROR "QILIN_LIFTED_OPT=O3 needs GCC or Clang (MSVC's highest is /O2)")
endif()

# ---- Link-time optimisation of the game ----
# The game's own targets only (qilin_game_target, below), not the tools and tests, whose static libraries GNU ld must read.
option(QILIN_LTO "Link-time optimisation of the game: ThinLTO with Clang, LTO with GCC, /GL and /LTCG with MSVC" OFF)
set(QILIN_LTO_JOBS 2 CACHE STRING "Code generators the game's link-time optimisation runs at once")
if(NOT QILIN_LTO_JOBS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "QILIN_LTO_JOBS=${QILIN_LTO_JOBS}: a whole number from 1")
endif()
set(qilin_lto_compile "")
set(qilin_lto_link "")
if(QILIN_LTO)
    set(qilin_cache "${CMAKE_BINARY_DIR}/lto-cache")
    if(QILIN_CLANG_CL)
        # lld-link reads the bitcode; Microsoft's link.exe can't.
        if(NOT CMAKE_LINKER MATCHES "lld-link")
            message(FATAL_ERROR "QILIN_LTO with clang-cl needs LLVM's linker: configure with -DCMAKE_LINKER=lld-link (beside clang-cl)")
        endif()
        if(NOT CMAKE_AR MATCHES "llvm-lib")
            message(FATAL_ERROR "QILIN_LTO with clang-cl needs LLVM's llvm-lib for the game's libraries (CMake found '${CMAKE_AR}')")
        endif()
        set(qilin_lto_compile -flto=thin)
        set(qilin_lto_link /opt:lldltojobs=${QILIN_LTO_JOBS} "/lldltocache:${qilin_cache}")
    elseif(QILIN_CLANG)
        set(qilin_lto_compile -flto=thin)
        if(APPLE)
            set(qilin_lto_link -flto=thin "-Wl,-cache_path_lto,${qilin_cache}")  # (Apple's linker: its own number of jobs)
        else()
            # GNU ld can't read the bitcode without LLVM's plugin: lld can.
            set(qilin_lto_link -flto=thin -fuse-ld=lld -Wl,--thinlto-jobs=${QILIN_LTO_JOBS} "-Wl,--thinlto-cache-dir=${qilin_cache}")
        endif()
    elseif(QILIN_GCC)
        # Slim objects need gcc-ar's index in the static libraries; without it, fat ones (code and IR both).
        if(CMAKE_CXX_COMPILER_AR AND CMAKE_CXX_COMPILER_RANLIB)
            set(CMAKE_AR "${CMAKE_CXX_COMPILER_AR}")
            set(CMAKE_RANLIB "${CMAKE_CXX_COMPILER_RANLIB}")
            set(qilin_lto_compile -flto=${QILIN_LTO_JOBS} -fno-fat-lto-objects)
        else()
            set(qilin_lto_compile -flto=${QILIN_LTO_JOBS} -ffat-lto-objects)
        endif()
        set(qilin_lto_link -flto=${QILIN_LTO_JOBS})
    elseif(QILIN_REAL_MSVC)
        set(qilin_lto_compile /GL)
        set(qilin_lto_link /LTCG /CGTHREADS:${QILIN_LTO_JOBS})
    endif()
endif()

# ---- Profile-guided optimisation of the game ----
# GENERATE builds the game counting what runs; a run of it writes the counts (docs/toolchain.md: a scripted race);
# USE builds it again laid out and inlined by them. Clang's counts are merged with llvm-profdata into one .profdata,
# MSVC's .pgc files with pgomgr into the .pgd, GCC's .gcda files are read from their folder.
set(QILIN_PGO "OFF" CACHE STRING "Profile-guided optimisation of the game: OFF, GENERATE or USE")
set_property(CACHE QILIN_PGO PROPERTY STRINGS OFF GENERATE USE)
set(QILIN_PGO_PROFILE "" CACHE FILEPATH
    "The profile: Clang's merged .profdata (USE), MSVC's .pgd (GENERATE and USE), GCC's folder of .gcda files (both)")
set(qilin_pgo_compile "")
set(qilin_pgo_link "")
set(qilin_pgo_definitions "")
if(NOT QILIN_PGO MATCHES "^(OFF|GENERATE|USE)$")
    message(FATAL_ERROR "QILIN_PGO=${QILIN_PGO}: OFF, GENERATE or USE")
endif()
if(NOT QILIN_PGO STREQUAL "OFF")
    if(QILIN_PGO STREQUAL "USE" AND NOT EXISTS "${QILIN_PGO_PROFILE}")
        message(FATAL_ERROR "QILIN_PGO=USE needs QILIN_PGO_PROFILE, the profile a run of the GENERATE build made ('${QILIN_PGO_PROFILE}' isn't there)")
    endif()
    if(QILIN_CLANG)
        if(QILIN_PGO STREQUAL "GENERATE")
            # (Atomic counters: the game runs on many threads.)
            set(qilin_pgo_compile -fprofile-instr-generate -fprofile-update=atomic)
            set(qilin_pgo_definitions QILIN_PGO_GENERATE=1)
            if(QILIN_CLANG_CL)
                # CMake links clang-cl's objects with the linker itself, which doesn't add the profile's runtime: found
                # beside clang-cl's other runtime libraries.
                execute_process(COMMAND "${CMAKE_CXX_COMPILER}" /clang:-print-runtime-dir OUTPUT_VARIABLE qilin_rt_dir
                                OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE qilin_rt_result)
                set(qilin_profile_lib "${qilin_rt_dir}/clang_rt.profile.lib")
                if(NOT EXISTS "${qilin_profile_lib}")
                    set(qilin_profile_lib "${qilin_rt_dir}/clang_rt.profile-x86_64.lib")
                endif()
                if(NOT qilin_rt_result EQUAL 0 OR NOT EXISTS "${qilin_profile_lib}")
                    message(FATAL_ERROR "QILIN_PGO=GENERATE: clang-cl's profile runtime (clang_rt.profile) isn't in '${qilin_rt_dir}'")
                endif()
                set(qilin_pgo_link "${qilin_profile_lib}")
            else()
                set(qilin_pgo_link -fprofile-instr-generate)
            endif()
        else()
            set(qilin_pgo_compile "-fprofile-instr-use=${QILIN_PGO_PROFILE}" -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date)
        endif()
        if(QILIN_CLANG_CL)
            list(TRANSFORM qilin_pgo_compile PREPEND "/clang:")
        endif()
    elseif(QILIN_GCC)
        if(NOT QILIN_PGO_PROFILE)
            set(QILIN_PGO_PROFILE "${CMAKE_BINARY_DIR}/pgo")
        endif()
        if(QILIN_PGO STREQUAL "GENERATE")
            set(qilin_pgo_compile "-fprofile-generate=${QILIN_PGO_PROFILE}" -fprofile-update=atomic)
            set(qilin_pgo_link "-fprofile-generate=${QILIN_PGO_PROFILE}")
            set(qilin_pgo_definitions QILIN_PGO_GENERATE=1)
        else()
            set(qilin_pgo_compile "-fprofile-use=${QILIN_PGO_PROFILE}" -fprofile-partial-training -Wno-missing-profile)
        endif()
    elseif(QILIN_REAL_MSVC)
        # MSVC's profiles go through whole-program code generation.
        if(NOT QILIN_PGO_PROFILE)
            set(QILIN_PGO_PROFILE "${CMAKE_BINARY_DIR}/ridgeracer7.pgd")
        endif()
        set(qilin_pgo_compile /GL)
        if(QILIN_PGO STREQUAL "GENERATE")
            set(qilin_pgo_link /LTCG "/GENPROFILE:PGD=${QILIN_PGO_PROFILE}")
        else()
            set(qilin_pgo_link /LTCG "/USEPROFILE:PGD=${QILIN_PGO_PROFILE}")
        endif()
    else()
        message(FATAL_ERROR "QILIN_PGO: not known for ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

# The options above for one of the game's targets (the executable, the machine, the window): what is compiled into
# the game and nothing else.
function(qilin_game_target target)
    target_compile_options(${target} PRIVATE ${qilin_lto_compile} ${qilin_pgo_compile})
    target_compile_definitions(${target} PRIVATE ${qilin_pgo_definitions})
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "EXECUTABLE")
        target_link_options(${target} PRIVATE ${qilin_lto_link})
        if(QILIN_CLANG_CL AND QILIN_PGO STREQUAL "GENERATE")
            target_link_libraries(${target} PRIVATE ${qilin_pgo_link})
        else()
            target_link_options(${target} PRIVATE ${qilin_pgo_link})
        endif()
    endif()
endfunction()

set(qilin_driver "")
if(QILIN_CLANG_CL)
    set(qilin_driver " (clang-cl)")
endif()
message(STATUS "Qilin toolchain: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}${qilin_driver}; processor '${qilin_processor}', "
               "preset '${QILIN_CPU}' (${qilin_cpu_options}); lifted code -${QILIN_LIFTED_OPT}; LTO ${QILIN_LTO}; PGO ${QILIN_PGO}")
