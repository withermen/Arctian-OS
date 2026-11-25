#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLCHAIN_DIR="$(cd "$SCRIPT_DIR/../toolchain" && pwd)"
CXX="$TOOLCHAIN_DIR/arctian-cxx"
SYS="$HOME/arctian-toolchain/sysroot"
OUT="${1:-/tmp/arctian_cairo_smoke.elf}"

export PATH="$HOME/arctian-toolchain/host/bin:$PATH"

EXTRA_LIBS=()
if [ -f "$SYS/usr/lib/libsharpyuv.a" ]; then
    EXTRA_LIBS+=("-lsharpyuv")
fi
if [ -f "$SYS/usr/lib/libgraphite2.a" ]; then
    EXTRA_LIBS+=("-lgraphite2")
fi
if [ -f "$SYS/usr/lib/libintl.a" ]; then
    EXTRA_LIBS+=("-lintl")
fi

"$CXX" -O2 -Wall -Wextra -fno-use-linker-plugin \
    -I"$SYS/usr/include" \
    -I"$SYS/usr/include/cairo" \
    -I"$SYS/usr/include/pixman-1" \
    -I"$SYS/usr/include/freetype2" \
    -I"$SYS/usr/include/libpng16" \
    -I"$SYS/usr/include/libxml2" \
    "$SCRIPT_DIR/sysroot_smoke.cpp" \
    -Wl,--start-group \
    -lcairo -lpixman-1 -lfontconfig -lexpat \
    -lfreetype -lpng16 -ljpeg -lwebp "${EXTRA_LIBS[@]}" \
    -lbz2 -lbrotlidec -lbrotlicommon \
    -licui18n -licuuc -licudata \
    -lsqlite3 -lxml2 -llzma -lz \
    -lssl -lcrypto \
    -Wl,--end-group \
    -o "$OUT"
