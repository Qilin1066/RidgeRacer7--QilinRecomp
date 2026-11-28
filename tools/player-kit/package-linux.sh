#!/bin/sh
# Builds the Linux download for the processor it runs on (x86-64 or ARM64,
# where it builds), as Toy Commander's and Rhythm Heaven Fever's:
#   tools/player-kit/package-linux.sh <version> <out.tar.gz>
#
# Built with the zig it bundles (zig cc and zig c++ as CMake's compilers),
# so the kit's library has the libc++ the player's compiles link with, for
# the native glibc target: zig links against the glibc it finds here, which
# sets the oldest glibc the download runs on (CI builds on Ubuntu 22.04:
# glibc 2.35), and finds the system's X11, Wayland and sound headers for
# SDL3. Everything is compiled for the processor family's baseline
# (-mcpu=baseline), not this computer's. Then lays out:
#   RidgeRacer7--QilinRecomp/Play Ridge Racer 7, README.txt, LICENSES/
#   RidgeRacer7--QilinRecomp/kit/  qilin, lib/libridgeracer7_kit.a (the game's program),
#                                  lib/libridgeracer7_libs.a (its runtime and libraries),
#                                  include/, build-flags.txt, game.txt, files.txt, VERSION, icon.png,
#                                  toolchain/zig/ (the zig release, less other systems' C libraries)
# Nothing in it is made from the game: the player's computer lifts the code and translates the
# shaders of their own EBOOT.elf and compiles that code with the bundled zig (Play Ridge Racer 7,
# the launcher, tools/player-kit/launcher).
#
# Environment:
#   ZIG_DIR        the unpacked zig release for this processor (pinned by the workflow)
#   BUILD_DIR      where to build (default build-player-kit-linux)
#   CMAKE_EXTRA    more cmake arguments (e.g. -DFETCHCONTENT_SOURCE_DIR_IMGUI=...)
set -eu
version=$1
out=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
. "$here/games.sh"
. "$here/licenses.sh"
zig_dir=$(cd "${ZIG_DIR:?set ZIG_DIR to the unpacked zig release}" && pwd)
build=${BUILD_DIR:-$root/build-player-kit-linux}
stage=$(mktemp -d)
pkg="$stage/$PACKAGE"
exe=${EXE%.exe}   # ridgeracer7: no .exe on Linux

# zig as CMake's compilers and archiver, through wrappers (CMake wants one
# program per tool).
tools="$build/zig-tools"
mkdir -p "$tools"
wrap() {
    printf '#!/bin/sh\nexec %s %s "$@"\n' "'$zig_dir/zig'" "$2" > "$tools/$1"
    chmod 755 "$tools/$1"
}
wrap zig-cc "cc -mcpu=baseline"
wrap zig-c++ "c++ -mcpu=baseline"
wrap zig-ar ar
wrap zig-ranlib ranlib

# shellcheck disable=SC2086
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER="$tools/zig-cc" -DCMAKE_CXX_COMPILER="$tools/zig-c++" \
      -DCMAKE_AR="$tools/zig-ar" -DCMAKE_RANLIB="$tools/zig-ranlib" \
      -DQILIN_PLAYER_KIT=ON -DQILIN_WERROR=ON ${CMAKE_EXTRA:-}
cmake --build "$build" --target qilin qilin_launcher ridgeracer7_kit ridgeracer7_kit_link_check --parallel
cmake --install "$build" --component "$game" --prefix "$pkg"

cp "$build/qilin_launcher" "$pkg/Play $TITLE"
chmod 755 "$pkg/Play $TITLE" "$pkg/kit/qilin"

# zig, without the C libraries and headers of systems it won't compile for
# here (Windows, macOS, the BSDs, WebAssembly): about 130 MB of 300.
mkdir -p "$pkg/kit/toolchain"
cp -R "$zig_dir" "$pkg/kit/toolchain/zig"
z="$pkg/kit/toolchain/zig"
rm -rf "$z/doc" "$z/lib/libc/mingw" "$z/lib/libc/darwin" "$z/lib/libc/wasi" \
       "$z/lib/libc/freebsd" "$z/lib/libc/netbsd" "$z/lib/libc/openbsd"
for d in "$z"/lib/libc/include/*; do
    case $(basename "$d") in
    *-windows-*|*-darwin-*|*-freebsd*|*-netbsd*|*-openbsd*|generic-freebsd|generic-netbsd|generic-openbsd|wasm*) rm -rf "$d" ;;
    esac
done

# What the launcher needs to know about the game (games.sh), and the
# compiler: the bundled zig, run as `zig c++`.
write_game_txt "$pkg/kit/game.txt" "$exe" "toolchain/zig/zig" "c++"
# Which processor this download is for, in its README.
case $(uname -m) in
aarch64|arm64) arm_note=$(cat "$here/README-linux-arm64.txt") ;;
*) arm_note=$(cat "$here/README-linux-x86_64.txt") ;;
esac
fill "$here/README-linux.txt.in" | sed "s|@EXE_LINUX@|$exe|g" | awk -v note="$arm_note" '{ if ($0 == "@ARM_NOTE@") print note; else print }' > "$pkg/README.txt"
printf '%s\n' "$version" > "$pkg/kit/VERSION"
python3 "$here/launcher/make_icon.py" --png "$pkg/kit/icon.png"   # the menu entry's

# Every licence of what's included, by component: the project's and its
# libraries' (licenses.sh), and zig's.
lic="$pkg/LICENSES"
project_licenses "$lic"
mkdir -p "$lic/zig"
# zig's (MIT) and those of what it bundles, as zig ships them, at their
# places in its folder: libc++, libc++abi and libunwind (the LLVM
# licence), glibc's and musl's notices for their headers and stubs.
(cd "$z" && find . -type f \( -iname 'LICENSE*' -o -iname 'COPYING*' -o -iname 'COPYRIGHT*' -o -iname 'NOTICE*' \) | LC_ALL=C sort) |
while read -r f; do
    mkdir -p "$lic/zig/$(dirname "$f")"
    cp "$z/$f" "$lic/zig/$f"
done
[ -s "$lic/zig/LICENSE" ] && [ -s "$lic/zig/lib/libcxx/LICENSE.TXT" ] || { echo "zig's licences weren't found" >&2; exit 1; }
cat > "$lic/zig/README.txt" <<'EOF'
zig (kit/toolchain/zig) is under the MIT licence: LICENSE.
The zig program contains LLVM, Clang and LLD, under the Apache License 2.0
with LLVM Exceptions: the same text as lib/libcxx/LICENSE.TXT, which also
covers libc++, libc++abi (lib/libcxxabi) and libunwind (lib/libunwind).
lib/libc/glibc/LICENSES and lib/libc/musl/COPYRIGHT cover the C library
headers and sources zig bundles (the game links with this system's glibc).
The other files keep their places in zig's folder.
EOF

# Every file the release holds (itself too): the launcher removes anything
# else from kit/ and LICENSES/, which an older version unpacked over this
# one left behind (tools/player-kit/launcher/tidy.hpp).
(cd "$pkg" && { find . -type f | sed 's|^\./||'; echo kit/files.txt; } | LC_ALL=C sort -u > kit/files.txt)

# Readable by everyone, writable by the owner; programs stay runnable.
chmod -R u+rwX,go+rX,go-w "$pkg"
rm -f "$out"
(cd "$stage" && tar --owner=0 --group=0 --numeric-owner -cf - "$PACKAGE" | gzip -9 > "$out")
rm -rf "$stage"
echo "Packaged $out ($(du -h "$out" | cut -f1))"
