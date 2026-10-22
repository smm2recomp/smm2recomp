#!/bin/bash
# usage: tools/bundle_macos.sh <exe>   (make app-macos does this)
# The macOS player package: build/SMM2Recomp.app, zipped as dist/SMM2Recomp-macos-<arch>.zip (arm64 or x86_64: the
# app is built natively, for this machine's architecture).
#   Contents/MacOS/smm2recomp      the runtime with its setup screen (runtime/ui/launcher.mm)
#   Contents/Frameworks/           every non-system dylib it loads (SDL3, from build/deps: tools/build_deps_macos.sh),
#                                  load paths rewritten to @rpath
#   Contents/Resources/            python/ tools/ gamelib-src/ zig/ hactool/ licenses/ (tools/package_lib.sh),
#                                  README.txt, THIRD-PARTY.txt
# Nothing from the game: the app keeps the player's set-up game files, the game code built from them, the fonts,
# saves and settings in ~/Library/Application Support/<bundle id> (runtime/paths.h). Signed ad hoc, or with
# RECOMP_MACOS_IDENTITY (a Developer ID Application identity; then also notarizable). MACOS_MIN: LSMinimumSystemVersion,
# and every Mach-O inside must be built for it or older (else it fails and lists them).
set -euo pipefail
cd "$(dirname "$0")/.."
. tools/package_lib.sh
EXE=${1:-build/smm2-app/smm2recomp}
TITLE=$(sed -n 's/^GAME_TITLE *:= *//p' games/smm2/game.mk)
APP="build/$TITLE.app"
DEPS=$PWD/build/deps
ID=$(sed -n 's/^GAME_ID *:= *//p' games/smm2/game.mk)
BUNDLE_ID=$(sed -n 's/^GAME_BUNDLE_ID *:= *//p' games/smm2/game.mk)
VERSION=${RECOMP_VERSION:-$(git describe --tags --match 'v[0-9]*' 2>/dev/null || echo dev)}
SHORT=$(echo "$VERSION" | sed -n 's/^v\([0-9]*\.[0-9]*\.[0-9]*\).*/\1/p')  # (CFBundleShortVersionString: digits)
SHORT=${SHORT:-0.0.0}
IDENTITY=${RECOMP_MACOS_IDENTITY:--}
# the architecture: this machine's (the app is built natively: Apple silicon or Intel)
case "$(uname -m)" in
    arm64) ARCH=arm64; TARGET=macos-aarch64; ZT=aarch64-macos ;;
    *)    ARCH=x86_64; TARGET=macos-x86_64; ZT=x86_64-macos ;;
esac
[ -x "$EXE" ] || { echo "no $EXE (make app-macos)"; exit 1; }
# the app must hold no game code: that's built on the player's machine (grep without -q: with pipefail, nm cut off
# early would hide a match)
if nm "$EXE" | grep ' _f_7' >/dev/null; then echo "$EXE has game code linked in"; exit 1; fi

rm -rf "$APP"
C="$APP/Contents"
mkdir -p "$C/MacOS" "$C/Frameworks" "$C/Resources"
cp "$EXE" "$C/MacOS/$ID""recomp"
strip -S "$C/MacOS/${ID}recomp"  # (debug-map entries only: the symbols crash reports name functions by stay)
stage_common "$C/Resources" "$TARGET" "$ZT"
[ -f "$DEPS/share/licenses/SDL3/LICENSE.txt" ] && cp "$DEPS/share/licenses/SDL3/LICENSE.txt" "$C/Resources/licenses/LICENSE-SDL3.txt"
write_readme "$C/Resources/README.txt" "$TARGET" "$VERSION"

# ---- dylibs: copy every non-system dependency, recursively; point all references at @rpath/<name>
is_system() { case "$1" in /usr/lib/*|/System/*) return 0;; esac; return 1; }
find_dep() {  # dep as referenced by (the original of) $2 -> a real file
    case "$1" in
    @rpath/*|@loader_path/*)
        local n=${1#*/}
        for d in "$(dirname "$2")" "$DEPS/lib"; do [ -e "$d/$n" ] && { echo "$d/$n"; return; }; done
        echo "cannot resolve $1 (from $2)" >&2; exit 1;;
    *) echo "$1";;
    esac
}
deps_of() { otool -L "$1" | tail -n +2 | awk '{print $1}' | grep -v "^$(otool -D "$1" | tail -n +2 | head -1)\$" || true; }
origin() { local s; s="$(dirname "$1")/.$(basename "$1").src"; if [ -f "$s" ]; then cat "$s"; else echo "$1"; fi; }
queue="$C/MacOS/${ID}recomp"
copied=" "
while [ -n "$queue" ]; do
    f=${queue%%|*}; [ "$queue" = "$f" ] && queue= || queue=${queue#*|}
    for dep in $(deps_of "$f"); do
        is_system "$dep" && continue
        case "$dep" in /opt/homebrew/*|/usr/local/*) echo "$f links $dep (Homebrew's: build against build/deps)"; exit 1;; esac
        name=$(basename "$dep")
        if [[ "$copied" != *" $name "* ]]; then
            real=$(find_dep "$dep" "$(origin "$f")")
            cp -L "$real" "$C/Frameworks/$name"
            chmod u+w "$C/Frameworks/$name"
            install_name_tool -id "@rpath/$name" "$C/Frameworks/$name" 2>/dev/null
            echo "$real" > "$C/Frameworks/.$name.src"
            copied+="$name "
            queue=${queue:+$queue|}$C/Frameworks/$name
        fi
        [ "$dep" = "@rpath/$name" ] || install_name_tool -change "$dep" "@rpath/$name" "$f" 2>/dev/null
    done
done
rm -f "$C/Frameworks"/.*.src
rpaths() { otool -l "$1" | awk '/LC_RPATH/ {getline; getline; print $2}'; }
for r in $(rpaths "$C/MacOS/${ID}recomp"); do install_name_tool -delete_rpath "$r" "$C/MacOS/${ID}recomp"; done
install_name_tool -add_rpath "@executable_path/../Frameworks" "$C/MacOS/${ID}recomp"

# ---- every Mach-O inside, its minimum macOS
machos() { find "$APP" -type f \( -perm +111 -o -name '*.dylib' -o -name '*.so' \) | while read -r f; do
    file -b "$f" | grep -q Mach-O && echo "$f"; done | sort -u; }
minos() { otool -l "$1" 2>/dev/null | awk '/LC_BUILD_VERSION/ {b=1} b && /minos/ {print $2; exit}
                                          /LC_VERSION_MIN_MACOSX/ {v=1} v && /version/ {print $2; exit}'; }
newer() { [ "$1" != "$2" ] && [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" = "$1" ]; }  # $1 > $2
MIN=${MACOS_MIN:-}
TOO_NEW=""
NEWEST=11.0
MACHOS=$(machos)
while read -r f; do
    m=$(minos "$f"); [ -n "$m" ] || continue
    newer "$m" "$NEWEST" && NEWEST=$m
    [ -n "$MIN" ] && newer "$m" "$MIN" && TOO_NEW+="  ${f#$APP/} (macOS $m)"$'\n'
done <<< "$MACHOS"
if [ -n "$TOO_NEW" ]; then printf 'built for newer than macOS %s:\n%s' "$MIN" "$TOO_NEW"; exit 1; fi
MIN=${MIN:-$NEWEST}

cat > "$C/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>CFBundleExecutable</key><string>${ID}recomp</string>
    <key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
    <key>CFBundleName</key><string>$TITLE</string>
    <key>CFBundleDisplayName</key><string>$TITLE</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$SHORT</string>
    <key>CFBundleVersion</key><string>$SHORT</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>LSMinimumSystemVersion</key><string>$MIN</string>
    <key>LSArchitecturePriority</key><array><string>$ARCH</string></array>
    <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>GCSupportsControllerUserInteraction</key><true/>
    <key>CFBundleIconFile</key><string>AppIcon</string>
</dict>
</plist>
EOF
printf 'APPL????' > "$C/PkgInfo"
cp ../docs/branding/app-icon.icns "$C/Resources/AppIcon.icns"  # (the owner's app icon: tools/make_icons.sh)

# ---- signature (Apple silicon runs no unsigned code; the rewrites above invalidated the originals): every Mach-O
# inside first (the hardened runtime with a Developer ID: zig and Python run with it as they are), then the bundle
OPTS=()
[ "$IDENTITY" != - ] && OPTS=(--options runtime --timestamp)
while read -r f; do [ "$f" = "$C/MacOS/${ID}recomp" ] || codesign -f -s "$IDENTITY" ${OPTS[@]+"${OPTS[@]}"} "$f" 2>/dev/null; done <<< "$MACHOS"
codesign -f -s "$IDENTITY" ${OPTS[@]+"${OPTS[@]}"} "$APP"
codesign --verify --strict "$APP"

python3 ../scripts/check_package.py "$APP"
mkdir -p dist
ZIP=dist/SMM2Recomp-macos-$ARCH.zip
rm -f "$ZIP"
# no extended attributes: ditto would store them as ._ files, which any unzip but Finder's leaves inside the app,
# breaking its signature
ditto -c -k --norsrc --noextattr --noacl --keepParent "$APP" "$ZIP"
echo "$APP: $(du -sh "$APP" | cut -f1), $(ls "$C/Frameworks" | wc -l | tr -d ' ') dylibs, macOS $MIN+, signed $([ "$IDENTITY" = - ] && echo 'ad hoc' || echo "by $IDENTITY")"
echo "$ZIP: $(du -h "$ZIP" | cut -f1), $VERSION"
