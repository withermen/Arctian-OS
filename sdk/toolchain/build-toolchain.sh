#!/usr/bin/env bash
# Arctian OS C + C++20 Cross-Toolchain Kurucu (sudo gerektirmez)
# Alpine Linux v3.21 pure-musl sysroot (musl 1.2.5 + libstdc++ C++20 + libgcc_eh)
# + yerel GCC/G++ derleyici surucusu ile statik PIE x86-64 ELF64 uretir.
set -euo pipefail

TC="${ARCTIAN_TOOLCHAIN:-$HOME/arctian-toolchain}"
ALPINE_MIRROR="https://dl-cdn.alpinelinux.org/alpine/v3.21/main/x86_64"

mkdir -p "$TC/dl" "$TC/sysroot" "$TC/host"
cd "$TC/dl"

echo "[toolchain] 1/3 Alpine v3.21 x86_64 APKINDEX indiriliyor..."
curl -fsSL "$ALPINE_MIRROR/APKINDEX.tar.gz" -o APKINDEX.tar.gz
tar -xzf APKINDEX.tar.gz APKINDEX

find_apk() {
    local pkg="$1"
    awk -v p="$pkg" '
        /^P:/ { cur = substr($0, 3) }
        /^V:/ { if (cur == p) { printf "%s-%s.apk\n", p, substr($0, 3); exit } }
    ' APKINDEX
}

PKGS=(musl musl-dev libstdc++ libstdc++-dev libgcc gcc g++)
for p in "${PKGS[@]}"; do
    apk="$(find_apk "$p")"
    if [ -z "$apk" ]; then
        echo "[toolchain] HATA: Alpine APKINDEX icinde '$p' bulunamadi" >&2
        exit 1
    fi
    if [ ! -f "$apk" ]; then
        echo "[toolchain] Indiriliyor: $apk"
        curl -fsSL "$ALPINE_MIRROR/$apk" -o "$apk"
    fi
    echo "[toolchain] Aciliyor: $apk"
    tar -xzf "$apk" -C "$TC/sysroot" 2>/dev/null || true
done

echo "[toolchain] 2/3 Host C++20 derleyicisi (g++) denetleniyor..."
if command -v g++ >/dev/null 2>&1; then
    echo "[toolchain] Sistem g++ bulundu: $(g++ --version | head -n1)"
else
    echo "[toolchain] Sistemde g++ yok — apt-get download ile sudo'suz indiriliyor..."
    mkdir -p "$TC/dl/deb"
    cd "$TC/dl/deb"
    # Host gcc surumunu tespit et (or. 14)
    GCC_MAJ="$(gcc -dumpversion | cut -d. -f1)"
    apt-get download "g++-${GCC_MAJ}" "g++-${GCC_MAJ}-x86-64-linux-gnu" 2>/dev/null \
        || apt-get download "g++-${GCC_MAJ}"
    for deb in *.deb; do
        dpkg-deb -x "$deb" "$TC/host"
    done
    echo "[toolchain] Host g++-${GCC_MAJ} kuruldu: $TC/host/usr/bin/g++-${GCC_MAJ}"
fi

echo "[toolchain] 3/3 Sysroot dogrulamasi..."
CXX_INC_DIR="$(find "$TC/sysroot/usr/include/c++" -maxdepth 1 -mindepth 1 -type d | head -n1)"
GCC_LIB_DIR="$(find "$TC/sysroot/usr/lib/gcc/x86_64-alpine-linux-musl" -maxdepth 1 -mindepth 1 -type d | head -n1)"

for req in \
    "$TC/sysroot/usr/lib/libc.a" \
    "$TC/sysroot/usr/lib/rcrt1.o" \
    "$TC/sysroot/usr/lib/crti.o" \
    "$TC/sysroot/usr/lib/crtn.o" \
    "$TC/sysroot/usr/lib/libstdc++.a" \
    "$GCC_LIB_DIR/libgcc.a" \
    "$GCC_LIB_DIR/libgcc_eh.a" \
    "$GCC_LIB_DIR/crtbeginS.o" \
    "$GCC_LIB_DIR/crtendS.o"; do
    if [ ! -f "$req" ]; then
        echo "[toolchain] EKSIK DOSYA: $req" >&2
        exit 1
    fi
done

cat > "$TC/env.sh" <<EOF
export ARCTIAN_TOOLCHAIN="$TC"
export ARCTIAN_SYSROOT="$TC/sysroot"
export ARCTIAN_CXX_INC="$CXX_INC_DIR"
export ARCTIAN_GCC_LIB="$GCC_LIB_DIR"
EOF

echo "[toolchain] TAMAM!"
echo "  Sysroot : $TC/sysroot"
echo "  C++ Inc : $CXX_INC_DIR"
echo "  GCC Lib : $GCC_LIB_DIR"
