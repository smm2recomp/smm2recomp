#!/bin/bash
# usage: tools/build_hactool.sh <target>   (windows-x86_64 | linux-x86_64 | linux-aarch64 | macos-aarch64 | macos-x86_64)
# hactool (SciresM, ISC license; its bundled mbedtls: Apache-2.0) for the player packages: the setup screen
# (runtime/ui/launcher.mm) runs it on the player's own NSP / XCI and firmware with the player's own prod.keys
# (games/smm2/setup.py, sysdata.py). Built from the pinned release's source with zig cc for the target, so every
# package can be made from one machine. Output: build/hactool-<target>/hactool[.exe] + LICENSE.
set -euo pipefail
cd "$(dirname "$0")/.."
TARGET=$1
VER=1.4.0
SHA=1373c98ef8c58da42cb1e81b361283955caad0e1b8b5ad389b0ee49b7a21da43
ZIG=${ZIG:-$(ls build/zig/zig 2>/dev/null || command -v zig)}
case "$TARGET" in
    windows-x86_64) ZT=x86_64-windows-gnu EXE=.exe ;;
    linux-x86_64)   ZT=x86_64-linux-gnu.2.17 EXE= ;;
    linux-aarch64)  ZT=aarch64-linux-gnu.2.17 EXE= ;;
    macos-aarch64)  ZT=aarch64-macos.13.0-none EXE= ;;
    macos-x86_64)   ZT=x86_64-macos.13.0-none EXE= ;;
    *) echo "build_hactool.sh: no target $TARGET"; exit 1 ;;
esac
DL=build/dl
SRC=build/hactool-src-$VER
OUT=build/hactool-$TARGET
mkdir -p "$DL"
[ -f "$DL/hactool-$VER.tar.gz" ] || curl -fsSL --proto '=https' -o "$DL/hactool-$VER.tar.gz" \
    "https://github.com/SciresM/hactool/archive/refs/tags/$VER.tar.gz"
GOT=$( (shasum -a 256 2>/dev/null || sha256sum) < "$DL/hactool-$VER.tar.gz" | cut -d' ' -f1)
[ "$GOT" = "$SHA" ] || { echo "hactool-$VER.tar.gz: sha256 $GOT, expected $SHA"; exit 1; }
rm -rf "$SRC" && mkdir -p "$SRC" && tar -xzf "$DL/hactool-$VER.tar.gz" -C "$SRC" --strip-components 1
# Fixes to 1.4.0 (nca.c):
# - an update's (BKTR) relocation and subsection tables are spread out in place with memcpy over overlapping memory:
#   Apple's and glibc's memcpy happen to cope, Windows' copies forwards and scrambles the tables, so applying an
#   update crashed there (0xC0000005 in bktr_get_relocation). memmove.
# - reads of fewer than 16 bytes inside one AES block: in a patched RomFS the bytes came from the stale sector_ofs
#   instead of this read's block offset; in plain CTR sections the next seek dropped the bytes just read (upstream's
#   fix #98, after 1.4.0)
python3 - "$SRC/nca.c" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
s = s.replace('memcpy(bktr_get_relocation_bucket(', 'memmove(bktr_get_relocation_bucket(')
s = s.replace('memcpy(bktr_get_subsection_bucket(', 'memmove(bktr_get_subsection_bucket(')
assert s.count('memmove(bktr_get_') == 2
fixes = [
    ('                memcpy(buffer, block_buf + ctx->sector_ofs, count);\n'
     '                nca_section_fseek(ctx, ctx->bktr_ctx.virtual_seek + count);',
     '                memcpy(buffer, block_buf + block_ofs, count);\n'
     '                nca_section_fseek(ctx, ctx->bktr_ctx.virtual_seek + count);'),
    ('                    nca_section_fseek(ctx, ctx->cur_seek - ctx->offset);\n                    return count;',
     '                    nca_section_fseek(ctx, ctx->cur_seek - ctx->offset + ctx->sector_ofs);\n                    return count;'),
]
for a, b in fixes:
    assert s.count(a) == 1, a
    s = s.replace(a, b)
open(p, 'w').write(s)
EOF
rm -rf "$OUT" && mkdir -p "$OUT/obj"
export ZIG_GLOBAL_CACHE_DIR=$PWD/build/zig-cache ZIG_LOCAL_CACHE_DIR=$PWD/build/zig-cache
CF=(-target "$ZT" -O2 -std=gnu11 -w -I"$SRC/mbedtls/include"
    -D_BSD_SOURCE -D_POSIX_SOURCE -D_POSIX_C_SOURCE=200112L -D_DEFAULT_SOURCE -D__USE_MINGW_ANSI_STDIO=1 -D_FILE_OFFSET_BITS=64)
# (one compiler per file, a few at a time: zig cc compiles one file per invocation fastest)
objs=()
pids=()
for c in "$SRC"/mbedtls/library/*.c "$SRC"/*.c; do
    case $(basename "$c") in net_sockets.c) continue ;; esac  # (no networking: Windows would need ws2_32)
    o="$OUT/obj/$(basename "$(dirname "$c")")_$(basename "${c%.c}").o"
    objs+=("$o")
    "$ZIG" cc "${CF[@]}" -c "$c" -o "$o" &
    pids+=($!)
    if [ ${#pids[@]} -ge 8 ]; then wait "${pids[0]}"; pids=("${pids[@]:1}"); fi
done
for p in "${pids[@]}"; do wait "$p"; done
"$ZIG" cc -target "$ZT" -o "$OUT/hactool$EXE" "${objs[@]}" $([[ "$TARGET" == macos-* ]] || echo -s) \
    $([ "$TARGET" = windows-x86_64 ] && echo -ladvapi32)
rm -rf "$OUT/obj" "$OUT"/*.lib "$OUT"/*.pdb  # (zig leaves an import library next to a Windows exe)
{ cat "$SRC/LICENSE"; echo; echo "hactool $VER: https://github.com/SciresM/hactool (built from the release's source)."
  echo "Its bundled mbedtls is Apache-2.0: see mbedtls-LICENSE.txt."; } > "$OUT/LICENSE.txt"
cp "$SRC/mbedtls/apache-2.0.txt" "$OUT/mbedtls-LICENSE.txt"
echo "$OUT/hactool$EXE: $(du -h "$OUT/hactool$EXE" | cut -f1)"
