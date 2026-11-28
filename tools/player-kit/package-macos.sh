#!/bin/sh
# Builds the macOS download (what package.sh is for Windows), as Toy Commander's
# and Rhythm Heaven Fever's:
#   tools/player-kit/package-macos.sh <version> <out.dmg>
#
# On a Mac with Xcode 16 or newer, universal (arm64 and x86_64), into a disk
# image holding the app, a link to /Applications and README.txt:
#   Ridge Racer 7.app/Contents/Info.plist
#                           MacOS/Play Ridge Racer 7         the launcher
#                           Frameworks/libMoltenVK.dylib   Vulkan over Metal
#                           Resources/AppIcon.icns, LICENSES/
#                           Resources/kit/  qilin, lib/libridgeracer7_kit.a (the game's program),
#                                           lib/libridgeracer7_libs.a, include/,
#                                           build-flags.txt, game.txt, files.txt, VERSION
# No compiler is bundled: Apple's SDK may not be redistributed, so the
# launcher uses Apple's clang from the Command Line Tools (/usr/bin/clang++)
# and offers Apple's installer where it's missing. Nothing in it is made from
# the game: the player's Mac lifts the code and translates the shaders of
# their own EBOOT.elf.
# The app is signed ad hoc only (Apple silicon runs nothing unsigned): the
# player allows it once in System Settings, Privacy & Security. (No iOS kit:
# the launcher's "Play on iPad" isn't offered.)
#
# Environment:
#   MOLTENVK          MoltenVK's macOS release (MoltenVK-macos.tar), unpacked
#   MOLTENVK_VERSION  its version (v1.4.2), and MOLTENVK_SHA256 the tar's
#   MOLTENVK_NOTICES  the licences of what MoltenVK's library contains, as
#                     fetched by .github/workflows/player-kit-macos.yml:
#                     SPIRV-Cross-LICENSE, SPIRV-Tools-LICENSE,
#                     SPIRV-Headers-LICENSE, cereal-LICENSE
#   BUILD_DIR         where to build (default build-player-kit-macos)
#   CMAKE_EXTRA       more cmake arguments (e.g. -DFETCHCONTENT_SOURCE_DIR_IMGUI=...)
set -eu
version=$1
out=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
. "$here/games.sh"
. "$here/licenses.sh"
EXE=${EXE%.exe}  # no .exe on macOS
build=${BUILD_DIR:-$root/build-player-kit-macos}
moltenvk="${MOLTENVK:?set MOLTENVK to the unpacked MoltenVK-macos.tar}/MoltenVK"
notices=${MOLTENVK_NOTICES:?set MOLTENVK_NOTICES to the folder of MoltenVK contents licences}
stage=$(mktemp -d)
image="$stage/image"
app="$image/$TITLE.app"
contents="$app/Contents"
kit="$contents/Resources/kit"
launcher="Play $TITLE"

# shellcheck disable=SC2086
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" \
      -DQILIN_PLAYER_KIT=ON -DQILIN_WERROR=ON ${CMAKE_EXTRA:-}
cmake --build "$build" --target qilin qilin_launcher ridgeracer7_kit ridgeracer7_kit_link_check --parallel
mkdir -p "$contents/MacOS" "$contents/Frameworks" "$contents/Resources"
cmake --install "$build" --component "$game" --prefix "$contents/Resources"
cp "$build/qilin_launcher" "$contents/MacOS/$launcher"
cp "$moltenvk/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib" "$contents/Frameworks/"
python3 "$here/launcher/make_icns.py" "$contents/Resources/AppIcon.icns"

# The bundle's version: the release's X.Y.Z (an untagged build's describe
# text starts with the last tag's).
short=$(printf '%s\n' "$version" | sed -n 's/^\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*/\1/p')
[ -n "$short" ] || short=0.0.0
cat > "$contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key>
	<string>en</string>
	<key>CFBundleDisplayName</key>
	<string>$TITLE</string>
	<key>CFBundleExecutable</key>
	<string>$launcher</string>
	<key>CFBundleIconFile</key>
	<string>AppIcon</string>
	<key>CFBundleIdentifier</key>
	<string>io.github.qilin1066.$game-qilinrecomp</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>CFBundleName</key>
	<string>$TITLE</string>
	<key>CFBundlePackageType</key>
	<string>APPL</string>
	<key>CFBundleShortVersionString</key>
	<string>$short</string>
	<key>CFBundleVersion</key>
	<string>$short</string>
	<key>LSApplicationCategoryType</key>
	<string>public.app-category.games</string>
	<key>LSMinimumSystemVersion</key>
	<string>13.3</string>
	<key>NSHighResolutionCapable</key>
	<true/>
	<key>NSHumanReadableCopyright</key>
	<string>$TITLE QilinRecomp $version, GPL-3.0. Built from your own copy of the game; no game data included.</string>
</dict>
</plist>
EOF
plutil -lint "$contents/Info.plist"

# What the launcher needs to know about the game (games.sh), and the
# compiler: Apple's, from the Command Line Tools.
write_game_txt "$kit/game.txt" "$EXE" "/usr/bin/clang++"
fill "$here/README-macos.txt.in" > "$image/README.txt"
printf '%s\n' "$version" > "$kit/VERSION"

# Every licence of what's included, by component: the project's and its
# libraries' (licenses.sh), and MoltenVK's.
lic="$contents/Resources/LICENSES"
project_licenses "$lic"
mkdir -p "$lic/MoltenVK"
# MoltenVK (Apache-2.0) ships its own licence; its library also contains
# SPIRV-Cross and SPIRV-Tools (Apache-2.0), SPIRV-Headers' tables (MIT-style)
# and cereal (BSD-3-Clause), whose licences are taken at the revisions
# MoltenVK's release names (ExternalRevisions).
cp "$moltenvk/LICENSE" "$lic/MoltenVK/LICENSE.txt"
for f in SPIRV-Cross SPIRV-Tools SPIRV-Headers cereal; do cp "$notices/$f-LICENSE" "$lic/MoltenVK/$f-LICENSE.txt"; done
cat > "$lic/MoltenVK/ABOUT.txt" <<EOF
Contents/Frameworks/libMoltenVK.dylib is MoltenVK ${MOLTENVK_VERSION:?set MOLTENVK_VERSION}
(https://github.com/KhronosGroup/MoltenVK), Vulkan over Apple's Metal, from
its release's MoltenVK-macos.tar (SHA-256 ${MOLTENVK_SHA256:?set MOLTENVK_SHA256}),
unchanged but for its code signature: it is signed again, ad hoc, with the
rest of the app. Its licence is LICENSE.txt (Apache-2.0). The library also
contains SPIRV-Cross and SPIRV-Tools (Apache-2.0), tables from SPIRV-Headers
and cereal (BSD-3-Clause), at the revisions MoltenVK names in its
ExternalRevisions folder: their licences are the other files here.
EOF

# Every file the release holds (itself too), relative to the app: the
# launcher removes anything else from Resources/kit and Resources/LICENSES
# (tools/player-kit/launcher/tidy.hpp).
(cd "$app" && { find . -type f | sed 's|^\./||'; echo Contents/Resources/kit/files.txt; } | LC_ALL=C sort -u > Contents/Resources/kit/files.txt)

# Signed ad hoc, the parts inside first, then the whole app (which seals
# every file in it). Not notarised: that needs a paid Apple developer account.
codesign --force --sign - "$contents/Frameworks/libMoltenVK.dylib"
codesign --force --sign - "$kit/qilin"
codesign --force --sign - "$app"
codesign --verify --deep --strict --verbose=2 "$app"

# The disk image: the app, a link to /Applications to drag it onto, the readme.
ln -s /Applications "$image/Applications"
rm -f "$out"
made=no
for attempt in 1 2 3; do
    # hdiutil is known to fail now and then on a busy machine ("Resource busy").
    if hdiutil create -volname "$TITLE" -srcfolder "$image" -fs HFS+ -format UDZO -ov "$out"; then made=yes; break; fi
    echo "hdiutil create failed (attempt $attempt)" >&2
    sleep 5
done
[ "$made" = yes ] || { echo "can't make the disk image" >&2; exit 1; }
rm -rf "$stage"
echo "Packaged $out ($(du -h "$out" | cut -f1))"
