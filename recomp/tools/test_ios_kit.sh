#!/bin/bash
# usage: tools/test_ios_kit.sh <kit dir> [<dir of iOS game objects>]
# Checks an iOS kit (make TARGET=ios-arm64 ios-kit; Contents/Resources/ios-kit in the Mac app): every file the Mac
# app's iPhone build reads (runtime/iosbuild.c), no game code in it, everything built for iOS devices. With a folder
# of iOS game objects (e.g. build/smm2-ios-arm64/obj), also links them with the kit into an executable, as the Mac
# app does. Exit 0 when all pass.
set -euo pipefail
K=${1:?usage: test_ios_kit.sh <kit dir> [<dir of iOS game objects>]}
OBJS=${2:-}
fail() { echo "test_ios_kit: FAIL: $*" >&2; exit 1; }

for f in lib/libsmm2rt.a lib/libSDL3.a link.txt game_cflags.txt Info.plist entitlements.plist kit.json; do
    [ -s "$K/$f" ] || fail "no $K/$f"
done
ls "$K/icons/"* >/dev/null 2>&1 || fail "no icons in $K/icons"
plutil -lint "$K/Info.plist" >/dev/null || fail "Info.plist doesn't parse"
for k in com.apple.developer.kernel.extended-virtual-addressing com.apple.developer.kernel.increased-memory-limit; do
    [ "$(/usr/libexec/PlistBuddy -c "Print :$k" "$K/entitlements.plist" 2>/dev/null)" = true ] || fail "entitlements.plist: $k isn't true"
done
id=$(plutil -extract gamelib_id raw "$K/kit.json" 2>/dev/null) || fail "kit.json has no gamelib_id"
[ ${#id} -eq 16 ] || fail "kit.json gamelib_id '$id' isn't 16 hex digits"
[ "$(plutil -extract ios_min raw "$K/kit.json")" = 17.0 ] || fail "kit.json ios_min isn't 17.0"
grep -q -- '-framework' "$K/link.txt" || fail "link.txt has no frameworks"
! grep -q -- '^-L\|^-I\|^-target' "$K/link.txt" "$K/game_cflags.txt" || fail "link.txt / game_cflags.txt carry paths or -target"

# no game code (grep without -q: with pipefail, nm cut off early would hide a match)
for a in "$K"/lib/*.a; do
    if nm "$a" 2>/dev/null | grep ' _f_7' >/dev/null; then fail "$a has game code in it"; fi
done
# every member of the runtime archive built for iOS devices (LC_BUILD_VERSION platform 2)
members=$(ar t "$K/lib/libsmm2rt.a" | grep -c '\.o$')
ios=$(otool -l "$K/lib/libsmm2rt.a" | awk '/cmd LC_BUILD_VERSION/ {b=1} b && /platform/ {if ($2 == 2) n++; b=0} END {print n+0}')
[ "$members" -gt 0 ] && [ "$ios" -eq "$members" ] || fail "libsmm2rt.a: $ios of $members members are built for iOS"

if [ -n "$OBJS" ]; then
    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT
    find "$OBJS" -name '*.o' > "$work/objs.txt"
    [ -s "$work/objs.txt" ] || fail "no objects in $OBJS"
    libs=(-Wl,-force_load,"$K/lib/libsmm2rt.a" "$K/lib/libSDL3.a")
    [ -f "$K/lib/libmmk_embed.a" ] && libs+=("$K/lib/libmmk_embed.a")
    xcrun --sdk iphoneos clang++ -target arm64-apple-ios17.0 -o "$work/exe" -Wl,-filelist,"$work/objs.txt" \
        "${libs[@]}" @"$K/link.txt" > "$work/link.log" 2>&1 || { tail -20 "$work/link.log" >&2; fail "the kit doesn't link with $OBJS"; }
    otool -l "$work/exe" | grep -q 'platform 2' || fail "the linked executable isn't for iOS"
    echo "test_ios_kit: linked $(wc -l < "$work/objs.txt" | tr -d ' ') objects with the kit"
fi
echo "test_ios_kit: PASS ($K, gamelib $id)"
