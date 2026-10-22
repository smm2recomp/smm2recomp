#!/bin/bash
# usage: tools/fetch_python.sh <target>   (windows-x86_64 | linux-x86_64 | linux-aarch64 | macos-aarch64 | macos-x86_64)
# The Python the player packages bring along: the game code generator (tools/gen.py) and the setup scripts
# (games/smm2/setup.py, sysdata.py) run with it, so players install nothing. A relocatable CPython 3.12
# (python-build-standalone: PSF license, with its own components' licenses) plus the generator's three packages
# (tools/requirements-gen.txt: capstone BSD-3-Clause, unicorn GPL-2.0 (the library, unmodified), lz4 BSD-3-Clause),
# fetched by hash for the target - so every package can be made from one machine. Output: build/python-<target>/
# (shipped as python/), trimmed to what running the scripts needs.
set -euo pipefail
cd "$(dirname "$0")/.."
TARGET=$1
REL=20260924
VER=3.12.14
case "$TARGET" in
    macos-aarch64)  T=aarch64-apple-darwin      SHA=c2edb321cd32ec2b170df208db0446dccc4398db602ca27cf2079098fb1f7d9d PLAT=macosx_11_0_arm64 ;;
    macos-x86_64)   T=x86_64-apple-darwin       SHA=7ea9761b9069c10b9a20531d568645849d604c59e9c7f11f6659f1e1790c968e PLAT=macosx_11_0_x86_64 ;;
    linux-aarch64)  T=aarch64-unknown-linux-gnu SHA=c8499b61252c433280f134df954464d19811527b31cb920c35fc6967c1222e35 PLAT=manylinux_2_17_aarch64 ;;
    linux-x86_64)   T=x86_64-unknown-linux-gnu  SHA=269b2c99e4db15b242bf01832f4fea1e8f1a664f273cff519393f296e9820b41 PLAT=manylinux_2_17_x86_64 ;;
    windows-x86_64) T=x86_64-pc-windows-msvc    SHA=c5bf8edfe858c1df9891be498b5bbc8761d383df5b9790658b088fea4870433a PLAT=win_amd64 ;;
    *) echo "fetch_python.sh: no target $TARGET"; exit 1 ;;
esac
NAME=cpython-$VER+$REL-$T-install_only_stripped.tar.gz
DL=build/dl
OUT=build/python-$TARGET
mkdir -p "$DL"
[ -f "$DL/$NAME" ] || curl -fsSL --proto '=https' -o "$DL/$NAME.part" \
    "https://github.com/astral-sh/python-build-standalone/releases/download/$REL/${NAME//+/%2B}" && \
    { [ -f "$DL/$NAME" ] || mv "$DL/$NAME.part" "$DL/$NAME"; }
GOT=$( (shasum -a 256 2>/dev/null || sha256sum) < "$DL/$NAME" | cut -d' ' -f1)
[ "$GOT" = "$SHA" ] || { echo "$NAME: sha256 $GOT, expected $SHA"; exit 1; }
rm -rf "$OUT" && mkdir -p "$OUT"
tar -xzf "$DL/$NAME" -C "$OUT" --strip-components 1

# the generator's packages: the target's wheels (hash-checked), unpacked into its site-packages
if [ "$TARGET" = windows-x86_64 ]; then SITE=$OUT/Lib/site-packages LIB=$OUT/Lib; else SITE=$OUT/lib/python3.12/site-packages LIB=$OUT/lib/python3.12; fi
W=$DL/wheels-$PLAT
HOSTPY=${PYTHON:-$(ls tools/venv/bin/python 2>/dev/null || command -v python3)}
"$HOSTPY" -m pip download -q --disable-pip-version-check --only-binary=:all: --no-deps --platform "$PLAT" \
    --python-version 3.12 --implementation cp --require-hashes -r tools/requirements-gen.txt -d "$W"
mkdir -p "$SITE"
for w in "$W"/*.whl; do
    "$HOSTPY" -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$w" "$SITE"
done

# what running the scripts doesn't need: GUI toolkits, tests, the package installer, headers, docs
rm -rf "$LIB"/{test,idlelib,tkinter,turtledemo,ensurepip,lib2to3,pydoc_data} "$LIB"/site-packages/pip* \
       "$OUT/include" "$OUT/share" "$OUT"/lib/{tcl*,tk*,itcl*,thread*,Tix*} "$OUT"/lib/pkgconfig "$OUT/tcl" "$OUT/Scripts" \
       "$OUT/libs" 2>/dev/null || true
rm -f "$OUT"/lib/libtcl* "$OUT"/lib/libtk* "$OUT"/DLLs/tcl* "$OUT"/DLLs/tk* "$OUT"/DLLs/_tkinter* "$LIB"/lib-dynload/_tkinter*
find "$OUT" -name '__pycache__' -type d -prune -exec rm -rf {} + 2>/dev/null || true
find "$OUT" -name '*.a' -delete
rm -rf "$SITE"/capstone/include "$SITE"/unicorn/include
{
    echo "Python $VER (python-build-standalone $REL, $T): https://github.com/astral-sh/python-build-standalone"
    echo "Python: PSF License (LICENSE.txt in this folder or in the Python distribution); the components it includes"
    echo "keep their own licenses (python-build-standalone documents them)."
    echo "Packages for the game code generator: capstone 5.0.9 (BSD-3-Clause), unicorn 2.1.4 (GPL-2.0, unmodified"
    echo "library, run as a separate program's module), lz4 4.4.5 (BSD-3-Clause); their licenses are in site-packages/*.dist-info."
} > "$OUT/README-LICENSES.txt"
echo "$OUT: Python $VER for $TARGET with capstone, unicorn, lz4: $(du -sh "$OUT" | cut -f1)"
