#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLCHAIN_DIR="$(cd "$SCRIPT_DIR/../toolchain" && pwd)"
CXX="$TOOLCHAIN_DIR/arctian-cxx"
SYS="${ARCTIAN_SYSROOT:-$HOME/arctian-toolchain/sysroot}"
WPE_BUILD="${WPE_BUILD_DIR:-$HOME/webkit-build/build-wpe}"
WPE_INC="${WPE_INC_DIR:-$HOME/webkit-build/tmp/wpe_inc}"
OUT_DIR="${1:-$HOME/webkit-build/tmp}"
OUT_ELF="$OUT_DIR/arctian_webkit.elf"
OUT_STRIPPED="$OUT_DIR/arctian_webkit.stripped.elf"
OBJ_DIR="$OUT_DIR/browser_obj"

export PATH="$HOME/arctian-toolchain/host/bin:$PATH"

mkdir -p "$OUT_DIR" "$OBJ_DIR"

INCLUDES=(
    -I"$SCRIPT_DIR"
    -I"$WPE_INC"
    -I"$WPE_BUILD/JavaScriptCoreGLib/DerivedSources"
    -I"$WPE_BUILD/JavaScriptCoreGLib/Headers"
    -I"$WPE_BUILD/DerivedSources/ForwardingHeaders"
    -I"$SYS/usr/include/wpe-1.0"
    -I"$SYS/usr/include/glib-2.0"
    -I"$SYS/usr/lib/glib-2.0/include"
    -I"$SYS/usr/include/libsoup-3.0"
    -I"$SYS/usr/include/cairo"
    -I"$SYS/usr/include/pixman-1"
    -I"$SYS/usr/include/freetype2"
    -I"$SYS/usr/include/libpng16"
    -I"$SYS/usr/include/libxml2"
)

CFLAGS=(
    -O2 -Wall -Wextra
    -ffunction-sections -fdata-sections
)

echo "[BUILD] Compiling arctian_wpe_backend.cpp..."
"$CXX" "${CFLAGS[@]}" "${INCLUDES[@]}" -c "$SCRIPT_DIR/arctian_wpe_backend.cpp" -o "$OBJ_DIR/arctian_wpe_backend.o"

echo "[BUILD] Compiling arctian_webkit_browser.cpp..."
"$CXX" "${CFLAGS[@]}" "${INCLUDES[@]}" -c "$SCRIPT_DIR/arctian_webkit_browser.cpp" -o "$OBJ_DIR/arctian_webkit_browser.o"

echo "[BUILD] Linking multi-call static-PIE $OUT_ELF..."
"$CXX" \
    -O2 -ffunction-sections -fdata-sections \
    -Wl,--gc-sections -Wl,--disable-new-dtags \
    "$OBJ_DIR/arctian_webkit_browser.o" \
    "$OBJ_DIR/arctian_wpe_backend.o" \
    "$WPE_BUILD/lib/libWPEWebKit-2.0.a" \
    -lgioopenssl -lgioenvironmentproxy -lssl -lcrypto \
    "$WPE_BUILD/lib/libWebCore.a" \
    "$WPE_BUILD/lib/libJavaScriptCore.a" \
    "$WPE_BUILD/lib/libPAL.a" \
    "$WPE_BUILD/lib/libWTF.a" \
    "$WPE_BUILD/lib/libxdgmime.a" \
    "$WPE_BUILD/lib/libbmalloc.a" \
    -Wl,--start-group \
    -lcairo -lpixman-1 -lfontconfig -lexpat -lfreetype -lbz2 -lpng -lz \
    -lharfbuzz -lharfbuzz-icu -lgraphite2 \
    -lsoup-3.0 -lnghttp2 -lpsl -lidn2 -lunistring -lbrotlidec -lbrotlicommon -lsqlite3 \
    -lgio-2.0 -lgobject-2.0 -lgmodule-2.0 -lglib-2.0 -lffi -lpcre2-8 -lintl -lmount -lblkid -leconf \
    -licui18n -licuuc -licudata \
    -lxslt -lxml2 -llzma \
    -lwebpdemux -lwebp -lsharpyuv -ljpeg \
    -lwpe-1.0 -lxkbcommon \
    -lgcrypt -lgpg-error -ltasn1 \
    -lssl -lcrypto \
    -Wl,--end-group \
    -o "$OUT_ELF"

echo "[BUILD] Stripping -> $OUT_STRIPPED..."
cp -f "$OUT_ELF" "$OUT_STRIPPED"
strip --strip-all "$OUT_STRIPPED"

echo "[VERIFY] Sizes:"
ls -lh "$OUT_ELF" "$OUT_STRIPPED"

echo "[VERIFY] ELF headers / segments / dynamic:"
readelf -h "$OUT_STRIPPED" | grep -E 'Class:|Type:|Machine:'
echo "PT_INTERP count: $(readelf -l "$OUT_STRIPPED" | grep -c 'INTERP' || true)"
echo "NEEDED count:    $(readelf -d "$OUT_STRIPPED" | grep -c 'NEEDED' || true)"
echo "TEXTREL count:   $(readelf -d "$OUT_STRIPPED" | grep -c 'TEXTREL' || true)"
