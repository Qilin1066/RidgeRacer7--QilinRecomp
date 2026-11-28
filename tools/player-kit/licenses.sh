# The licences every download carries (sourced by package.sh, package-linux.sh and package-macos.sh, which add their
# compiler's or MoltenVK's), as Rhythm Heaven Fever's: project_licenses <LICENSES folder>, with $root (the source),
# $build (the build folder) and $PACKAGE set; crlf=1 gives the texts Windows line ends.
#   <PACKAGE>/LICENSE.txt     this project (GPL-3.0)
#   SDL3/, Dear ImGui/, Vulkan-Headers/, volk/
#                             what the launcher and the game's runtime are built with
#   FreeType/                 the console's font library is built on it (the FreeType Licence, the one of its two
#                             licences this project takes)
#   FFmpeg/                   libavcodec and libavutil, for the films and the music (LGPL-2.1-or-later), with how the
#                             LGPL's terms are met
# Each comes from the source CMake fetched (FETCHCONTENT_SOURCE_DIR_<NAME> if the build was given one).
project_licenses() {
    lic=$1
    to_text() { if [ "${crlf:-0}" = 1 ]; then sed 's/$/\r/' "$1"; else cat "$1"; fi; }
    source_of() {  # <FetchContent name, upper case> <its folder in _deps>
        dir=$(sed -n "s/^FETCHCONTENT_SOURCE_DIR_$1:PATH=//p" "$build/CMakeCache.txt")
        [ -n "$dir" ] || dir="$build/_deps/$2"
        printf '%s\n' "$dir"
    }
    mkdir -p "$lic/$PACKAGE" "$lic/SDL3" "$lic/Dear ImGui" "$lic/Vulkan-Headers" "$lic/volk" "$lic/FreeType" "$lic/FFmpeg"
    to_text "$root/LICENSE" > "$lic/$PACKAGE/LICENSE.txt"  # GPL-3.0
    to_text "$(source_of SDL3 sdl3-src)/LICENSE.txt" > "$lic/SDL3/LICENSE.txt"
    to_text "$(source_of IMGUI imgui-src)/LICENSE.txt" > "$lic/Dear ImGui/LICENSE.txt"
    to_text "$(source_of VULKANHEADERS vulkanheaders-src)/LICENSE.md" > "$lic/Vulkan-Headers/LICENSE.md"
    to_text "$(source_of VOLK volk-src)/LICENSE.md" > "$lic/volk/LICENSE.md"
    freetype=$(source_of FREETYPE freetype-src)
    to_text "$freetype/LICENSE.TXT" > "$lic/FreeType/LICENSE.TXT"
    to_text "$freetype/docs/FTL.TXT" > "$lic/FreeType/FTL.TXT"
    ffmpeg=$(source_of FFMPEG ffmpeg-src)
    to_text "$ffmpeg/COPYING.LGPLv2.1" > "$lic/FFmpeg/COPYING.LGPLv2.1"
    to_text "$ffmpeg/LICENSE.md" > "$lic/FFmpeg/LICENSE.md"
    cat > "$lic/FreeType/README.txt.tmp" <<'EOF'
Portions of this software are copyright (C) 2024 The FreeType Project
(https://freetype.org). All rights reserved.

FreeType 2.13.3 is built into kit/lib/libridgeracer7_libs.a, unchanged and
with none of its optional parts, for the console's font library. It is used
under the FreeType Licence (FTL.TXT; LICENSE.TXT says it may also be had
under the GPL, version 2).
EOF
    cat > "$lic/FFmpeg/README.txt.tmp" <<'EOF'
FFmpeg's libavcodec and libavutil (FFmpeg 7.1.1, https://ffmpeg.org) decode
the game's films (MPEG-2 video) and its music (ATRAC3plus). They are under
the GNU Lesser General Public License, version 2.1 or later: COPYING.LGPLv2.1
here. No part of FFmpeg under the GPL is used.

The source: FFmpeg's own release, https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz
(SHA-256 733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1),
unchanged. Which of its files are built, and with what settings, is
cmake/ffmpeg.cmake in this project's source (https://github.com/@REPO@),
which builds them with no changes of its own.

Relinking: FFmpeg's compiled files are kit/lib/libridgeracer7_libs.a's
members (libavcodec's and libavutil's objects, by their source names), and
the game is linked from them on your own computer every time it is built
(kit/build-flags.txt says how). To use another build of these libraries,
build it as a static library, put it before libridgeracer7_libs.a in
build-flags.txt's libs line (or replace those members of the archive), and
use "Build again from scratch": the game is linked with your libraries.
EOF
    sed "s|@REPO@|${REPO:?}|g" "$lic/FFmpeg/README.txt.tmp" > "$lic/FFmpeg/README.txt.tmp2"
    to_text "$lic/FreeType/README.txt.tmp" > "$lic/FreeType/README.txt"
    to_text "$lic/FFmpeg/README.txt.tmp2" > "$lic/FFmpeg/README.txt"
    rm "$lic/FreeType/README.txt.tmp" "$lic/FFmpeg/README.txt.tmp" "$lic/FFmpeg/README.txt.tmp2"
    for f in "$lic/$PACKAGE/LICENSE.txt" "$lic/SDL3/LICENSE.txt" "$lic/Dear ImGui/LICENSE.txt" "$lic/Vulkan-Headers/LICENSE.md" \
             "$lic/volk/LICENSE.md" "$lic/FreeType/FTL.TXT" "$lic/FFmpeg/COPYING.LGPLv2.1"; do
        [ -s "$f" ] || { echo "no licence text: $f" >&2; exit 1; }
    done
}
