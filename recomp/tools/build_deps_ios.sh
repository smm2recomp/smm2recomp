#!/bin/bash
# Static SDL3 for iPhone / iPad (device, arm64, iOS IOS_MIN, default 17.0) into build/deps-ios, for
# `make TARGET=ios-arm64 DEPS=build/deps-ios` (the app, and the iOS kit the Mac app carries: make ios-kit).
# Same pinned SDL as tools/build_deps_macos.sh. Needs Xcode with the iOS platform, cmake and ninja.
# Skipped when build/deps-ios/lib/libSDL3.a exists (delete build/deps-ios to redo).
set -euo pipefail
cd "$(dirname "$0")/.."
MIN=${IOS_MIN:-17.0}
P=$PWD/build/deps-ios
S=$P/src
mkdir -p "$S"
JOBS=$(sysctl -n hw.ncpu)

fetch() {  # url sha256 file
    [ -f "$S/$3" ] || curl -fsSL -o "$S/$3.part" "$1" && { [ -f "$S/$3" ] || mv "$S/$3.part" "$S/$3"; }
    echo "$2  $S/$3" | shasum -a 256 -c --quiet
}
unpack() { rm -rf "$S/$2" && mkdir -p "$S/$2" && tar -xf "$S/$1" -C "$S/$2" --strip-components 1; }

if [ ! -f "$P/lib/libSDL3.a" ]; then
    xcrun --sdk iphoneos --show-sdk-path >/dev/null || { echo "build_deps_ios: no iOS SDK (Xcode with iOS)" >&2; exit 1; }
    fetch https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz \
          9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3 SDL3-3.4.18.tar.gz
    unpack SDL3-3.4.18.tar.gz sdl
    cmake -S "$S/sdl" -B "$S/sdl/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$P" \
          -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
          -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN" -DSDL_SHARED=OFF -DSDL_STATIC=ON \
          -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_DOCS=OFF
    cmake --build "$S/sdl/build" -j"$JOBS" && cmake --install "$S/sdl/build"
    rm -rf "$S/sdl"  # (the source tarball stays, for a rebuild)
fi

echo "build/deps-ios ready (iOS $MIN): $(lipo -info "$P/lib/libSDL3.a" | sed 's/.*: //')"
