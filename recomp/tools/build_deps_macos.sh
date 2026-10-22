#!/bin/bash
# The libraries the shipped .app contains, built from pinned sources for the app's minimum macOS (MACOS_MIN, default
# 14.0) into build/deps (tools/bundle_macos.sh). Homebrew's bottles target the macOS they were built on, so they can't
# ship. Plain `make` (dev builds) keeps using Homebrew.
#   SDL3; with WITH_FFMPEG=1 also FFmpeg (LGPL only: what a game's movies need - WebM: VP9 + Vorbis, VP8 / Opus /
#   H.264 / AAC in WebM or MP4 - and swscale; SMM2 has no movies: GAME_MOVIES := 0).
# Needs: Xcode command line tools, cmake, ninja. Each step is skipped when its output exists (delete build/deps/<x> to
# redo).
set -euo pipefail
cd "$(dirname "$0")/.."
MIN=${MACOS_MIN:-14.0}
# the architecture to build for: this machine's (the app is built natively: Apple silicon or Intel)
case "$(uname -m)" in arm64) ARCH=arm64;; *) ARCH=x86_64;; esac
P=$PWD/build/deps
S=$P/src
mkdir -p "$S" "$P/bin" "$P/lib"
export MACOSX_DEPLOYMENT_TARGET=$MIN
JOBS=$(sysctl -n hw.ncpu)

fetch() {  # url sha256 file
    [ -f "$S/$3" ] || curl -fsSL -o "$S/$3.part" "$1" && { [ -f "$S/$3" ] || mv "$S/$3.part" "$S/$3"; }
    echo "$2  $S/$3" | shasum -a 256 -c --quiet
}
unpack() { rm -rf "$S/$2" && mkdir -p "$S/$2" && tar -xf "$S/$1" -C "$S/$2" --strip-components 1; }

fetch https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz \
      9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3 SDL3-3.4.18.tar.gz

if [ ! -f "$P/lib/libSDL3.0.dylib" ]; then
    unpack SDL3-3.4.18.tar.gz sdl
    cmake -S "$S/sdl" -B "$S/sdl/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$P" \
          -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN" -DCMAKE_OSX_ARCHITECTURES=$ARCH -DSDL_SHARED=ON -DSDL_STATIC=OFF \
          -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_DOCS=OFF
    cmake --build "$S/sdl/build" -j"$JOBS" && cmake --install "$S/sdl/build"
fi

if [ "${WITH_FFMPEG:-0}" = 1 ] && [ ! -f "$P/lib/libavcodec.dylib" ]; then
    fetch https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz \
          8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e ffmpeg-9.0.2.tar.xz
    unpack ffmpeg-9.0.2.tar.xz ffmpeg
    (cd "$S/ffmpeg" && ./configure --prefix="$P" --cc=clang --arch=$ARCH --enable-shared --disable-static \
        --extra-cflags="-mmacosx-version-min=$MIN" --extra-ldflags="-mmacosx-version-min=$MIN" \
        --disable-autodetect --disable-programs --disable-doc --disable-network --disable-avdevice --disable-avfilter \
        --disable-swresample --disable-everything \
        --enable-protocol=file --enable-demuxer=matroska,mov \
        --enable-decoder=vp9,vp8,vorbis,opus,h264,aac --enable-parser=vp9,vp8,vorbis,opus,h264,aac \
        --install-name-dir='@rpath' && make -j"$JOBS" && make install)
fi

echo "build/deps ready (macOS $MIN):"
for f in "$P"/lib/*.dylib; do
    [ -L "$f" ] && continue
    printf '  %-28s minos %s\n' "$(basename "$f")" "$(otool -l "$f" | awk '/LC_BUILD_VERSION/ {b=1} b && /minos/ {print $2; exit}')"
done
