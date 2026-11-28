# The macOS download's CMake side (tools/player-kit/package-macos.sh lays out the app), as Toy Commander's and Rhythm
# Heaven Fever's: included from CMakeLists.txt on Apple systems.

# (The launcher's macOS part, apple_tools.cpp, is added by tools/player-kit/launcher/launcher.cmake.)

# The macOS player kit (called from CMakeLists.txt's player kit section, which sets kit_whole, kit_libraries,
# kit_runtime, kit_archive and lifted_text): everything but the lifted code in two universal archives (arm64 and x86_64),
# the game's program (linked whole: its main) and its libraries', the recompiler, and build-flags.txt, which the launcher
# compiles and links the player's lifted code with, using Apple's clang from the Command Line Tools (/usr/bin/clang++,
# game.txt's compiler).
function(qilin_player_kit_macos)
    if(NOT "arm64" IN_LIST CMAKE_OSX_ARCHITECTURES OR NOT "x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES)
        message(FATAL_ERROR "the macOS player kit is universal: configure it with -DCMAKE_OSX_ARCHITECTURES=\"arm64;x86_64\"")
    endif()
    # Apple's libtool merges archives, universal ones too (Apple's ar has no MRI scripts).
    find_program(QILIN_APPLE_LIBTOOL NAMES libtool PATHS /usr/bin NO_DEFAULT_PATH REQUIRED)
    set(inputs)
    foreach(library IN LISTS kit_libraries)
        list(APPEND inputs "$<TARGET_FILE:${library}>")
    endforeach()
    add_custom_command(OUTPUT "${kit_archive}" "${kit_runtime}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/kit"
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${kit_archive}"
        COMMAND "${QILIN_APPLE_LIBTOOL}" -static -no_warning_for_no_symbols -o "${kit_archive}" ${inputs}
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:${kit_whole}>" "${kit_runtime}"
        DEPENDS ${kit_libraries} ${kit_whole}
        COMMENT "Merging the player kit's libraries" VERBATIM)
    add_custom_target(ridgeracer7_kit ALL DEPENDS "${kit_archive}" "${kit_runtime}")
    # What SDL3 links on macOS (its CMakeLists.txt, sdl_link_dependency), and iconv (SDL_iconv). The rest is the C++
    # library, which clang++ adds.
    set(kit_system_libraries
        -Wl,-framework,Cocoa -Wl,-framework,IOKit -Wl,-framework,ForceFeedback -Wl,-framework,Carbon
        -Wl,-framework,CoreAudio -Wl,-framework,AudioToolbox -Wl,-framework,AVFoundation -Wl,-framework,CoreMedia
        -Wl,-framework,CoreVideo -Wl,-framework,CoreBluetooth -Wl,-framework,CoreGraphics -Wl,-framework,Foundation
        -Wl,-framework,GameController -Wl,-framework,Metal -Wl,-framework,QuartzCore
        -Wl,-weak_framework,UniformTypeIdentifiers -Wl,-weak_framework,CoreHaptics -liconv)
    string(JOIN " " kit_system_text ${kit_system_libraries})
    # Read by the launcher (tools/player-kit/launcher/build.cpp): `name = value` lines, $kit the kit's folder (each flag
    # one argument, so -force_load's path may hold spaces); `<name>.<arch>` follows `<name>` for the Mac's own processor.
    # The game is compiled for the player's Mac only: its processor's architecture and its CPU (Apple clang's
    # -mcpu=native on Apple silicon; on Intel -march=native, with the SSSE3 the game's x86-64 build always has), and for
    # the macOS it runs (clang's default, the SDK's).
    file(WRITE "${CMAKE_BINARY_DIR}/kit/build-flags.txt"
        "# Written by CMake for the player kit (QILIN_PLAYER_KIT), macOS: universal.\n"
        "cxxflags = -O2 -DNDEBUG -std=c++23 -w ${lifted_text} -I$kit/include\n"
        "native.arm64 = -arch arm64 -mcpu=native\n"
        "native.x86_64 = -arch x86_64 -mssse3 -march=native\n"
        "ldflags = -O2\n"
        "ldflags.arm64 = -arch arm64\n"
        "ldflags.x86_64 = -arch x86_64\n"
        "libs = -Wl,-force_load,$kit/lib/libridgeracer7_kit.a $kit/lib/libridgeracer7_libs.a ${kit_system_text}\n")
    # The kit's archives linked with stand-ins for the lifted code, for both processors (CMAKE_OSX_ARCHITECTURES): they're
    # complete.
    add_executable(ridgeracer7_kit_link_check ${CMAKE_SOURCE_DIR}/tools/player-kit/link_check.cpp)
    target_include_directories(ridgeracer7_kit_link_check PRIVATE ${CMAKE_SOURCE_DIR}/runtime/include)
    target_link_libraries(ridgeracer7_kit_link_check PRIVATE "-Wl,-force_load,${kit_runtime}" "${kit_archive}" ${kit_system_libraries})
    add_dependencies(ridgeracer7_kit_link_check ridgeracer7_kit)
    install(TARGETS qilin RUNTIME DESTINATION kit COMPONENT ridgeracer7)
    install(FILES "${kit_runtime}" "${kit_archive}" DESTINATION kit/lib COMPONENT ridgeracer7)
    install(FILES "${CMAKE_BINARY_DIR}/kit/build-flags.txt" DESTINATION kit COMPONENT ridgeracer7)
endfunction()
