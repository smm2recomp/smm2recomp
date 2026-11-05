#!/bin/bash
# usage: tools/make_ios_kit.sh <kit dir> <staged dir> <SDL3 for iOS> <GAMELIB_ID> [<libmmk_embed.a>]
#   (make TARGET=ios-arm64 GAME=smm2 DEPS=build/deps-ios ios-kit runs it)
# The iOS kit: everything the Mac app needs to build a player's iPhone / iPad app from their own dump
# (runtime/iosbuild.c), with no game code in it. The Mac app carries it as Contents/Resources/ios-kit.
#   lib/libsmm2rt.a        the runtime, setup screen, Metal backend, Dear ImGui, Mbed TLS (staged by make)
#   lib/libSDL3.a          static SDL3 for iOS (tools/build_deps_ios.sh)
#   lib/libmmk_embed.a     the course server inside the app, when it was built (make embed-server)
#   link.txt               the rest of the link: frameworks and system libraries, one argument per line
#   game_cflags.txt        how the generated game code is compiled, one argument per line (no -I, no -target)
#   Info.plist, icons/     the app's (tools/ios_bundle_lib.sh), with this release's version
#   entitlements.plist     the two the guest memory window needs (see tools/make_ios_app.sh)
#   kit.json               {"gamelib_id", "ios_min", "version"}: the game code must be built for this GAMELIB_ID
set -euo pipefail
cd "$(dirname "$0")/.."
. tools/ios_bundle_lib.sh
KIT=$1 STAGED=$2 SDL=$3 ID=$4 EMBED=${5:-}
VERSION=${RECOMP_VERSION:-$(git describe --tags --match 'v[0-9]*' 2>/dev/null || echo dev)}
SHORT=$(echo "$VERSION" | sed -n 's/^v\([0-9]*\.[0-9]*\.[0-9]*\).*/\1/p')  # (CFBundleShortVersionString: digits)
SHORT=${SHORT:-0.0.0}
BUILD=$(git rev-list --count HEAD 2>/dev/null || echo 1)

rm -rf "$KIT"
mkdir -p "$KIT/lib"
cp "$STAGED/libsmm2rt.a" "$SDL/lib/libSDL3.a" "$KIT/lib/"
[ -z "$EMBED" ] || cp "$EMBED" "$KIT/lib/libmmk_embed.a"
cp "$STAGED/link.txt" "$STAGED/game_cflags.txt" "$KIT/"
ios_info_plist "$KIT/Info.plist" org.smcraft.smm2recomp "$SHORT" "$BUILD" ios
ios_icons "$KIT/icons" "$KIT/Info.plist" ios
cat > "$KIT/entitlements.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>com.apple.developer.kernel.extended-virtual-addressing</key><true/>
    <key>com.apple.developer.kernel.increased-memory-limit</key><true/>
</dict>
</plist>
EOF
printf '{"gamelib_id": "%s", "ios_min": "17.0", "version": "%s"}\n' "$ID" "$VERSION" > "$KIT/kit.json"
echo "make_ios_kit: $KIT ($VERSION, gamelib $ID$([ -n "$EMBED" ] && echo ', with the course server'))"
