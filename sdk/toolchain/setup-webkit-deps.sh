#!/usr/bin/env bash
# Arctian OS WebKit Port — Host Build Tools & Pure-Musl Static Sysroot Kurulumu (sudo gerektirmez)
set -euo pipefail

TC="${ARCTIAN_TOOLCHAIN:-$HOME/arctian-toolchain}"
[ -f "$TC/env.sh" ] || { echo "Once sdk/toolchain/build-toolchain.sh calistirin" >&2; exit 1; }
source "$TC/env.sh"

HOST="$TC/host"
SYS="$ARCTIAN_SYSROOT"
mkdir -p "$TC/dl/host_debs" "$TC/dl/alpine" "$HOST/bin"

echo "=== [1/3] Host derleme araclari (cmake, ninja, pkg-config, ruby, gperf, flex, bison, unifdef) ==="
cd "$TC/dl/host_debs"

WANT_HOST=(cmake cmake-data ninja-build pkg-config pkgconf pkgconf-bin libpkgconf3 ruby ruby3.3 libruby3.3 gperf flex bison unifdef libarchive13t64 libjsoncpp26 librhash1t64 libuv1t64 libyaml-0-2)
# Only download packages that exist in apt-cache
TO_DL=()
for p in "${WANT_HOST[@]}"; do
    if apt-cache show "$p" >/dev/null 2>&1; then
        TO_DL+=("$p")
    fi
done

# Collect missing direct library dependencies too
DEPS=$(apt-cache depends --no-recommends --no-suggests --no-conflicts --no-breaks --no-replaces --no-enhances "${TO_DL[@]}" 2>/dev/null \
    | awk '/Depends:/{gsub(/[<>]/,"",$2); print $2}' | sort -u)
for d in $DEPS; do
    case "$d" in
        libc6|libstdc++6|libgcc-s1|libm*|zlib1g|libssl*|libcrypto*|libcurl*|libexpat*|libzstd*|liblzma*|libbz2*|libtinfo*|libncurses*|libreadline*|libgmp*|libffi*|perl*|python*)
            ;;
        *)
            if ! dpkg -s "$d" >/dev/null 2>&1 && apt-cache show "$d" >/dev/null 2>&1; then
                TO_DL+=("$d")
            fi
            ;;
    esac
done

# Deduplicate and download
UNIQ_DL=($(printf "%s\n" "${TO_DL[@]}" | sort -u))
echo "[host] Indiriliyor (${#UNIQ_DL[@]} paket)..."
for p in "${UNIQ_DL[@]}"; do
    if ! ls "${p}_"*.deb >/dev/null 2>&1; then
        apt-get download "$p" >/dev/null 2>&1 || true
    fi
done

for deb in *.deb; do
    [ -f "$deb" ] || continue
    dpkg-deb -x "$deb" "$HOST"
done

# Create clean wrapper binaries in $HOST/bin with LD_LIBRARY_PATH + RUBYLIB
HLIB="$HOST/usr/lib/x86_64-linux-gnu:$HOST/lib/x86_64-linux-gnu"
for tool in cmake ctest ninja pkg-config pkgconf gperf flex bison unifdef; do
    BIN="$HOST/usr/bin/$tool"
    if [ -x "$BIN" ]; then
        cat > "$HOST/bin/$tool" <<EOF
#!/usr/bin/env bash
export LD_LIBRARY_PATH="$HLIB\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
export CMAKE_ROOT="$HOST/usr/share/cmake-\$(ls "$HOST/usr/share" | grep '^cmake-' | head -n1 | sed 's/cmake-//')"
exec "$BIN" "\$@"
EOF
        chmod +x "$HOST/bin/$tool"
    fi
done

# Ruby wrapper with RUBYLIB pointing into $HOST/usr/lib/ruby
RUBY_BIN="$HOST/usr/bin/ruby3.3"
[ -x "$RUBY_BIN" ] || RUBY_BIN="$(ls "$HOST"/usr/bin/ruby* 2>/dev/null | grep -E 'ruby[0-9]' | head -n1 || true)"
if [ -x "$RUBY_BIN" ]; then
    RVER="$(ls "$HOST/usr/lib/ruby" 2>/dev/null | grep -E '^[0-9]' | head -n1 || echo 3.3.0)"
    RLIB="$HOST/usr/lib/ruby/$RVER:$HOST/usr/lib/x86_64-linux-gnu/ruby/$RVER:$HOST/usr/lib/ruby/vendor_ruby:$HOST/usr/share/rubygems-integration/all/gems"
    cat > "$HOST/bin/ruby" <<EOF
#!/usr/bin/env bash
export LD_LIBRARY_PATH="$HLIB\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
export RUBYLIB="$RLIB\${RUBYLIB:+:\$RUBYLIB}"
exec "$RUBY_BIN" "\$@"
EOF
    chmod +x "$HOST/bin/ruby"
fi

echo "=== [2/3] Alpine v3.21 Pure-Musl Statik Kutuphaneler ($SYS) ==="
cd "$TC/dl/alpine"
MAIN_URL="https://dl-cdn.alpinelinux.org/alpine/v3.21/main/x86_64"
COMM_URL="https://dl-cdn.alpinelinux.org/alpine/v3.21/community/x86_64"
[ -f "$TC/dl/APKINDEX" ] || { curl -fsSL "$MAIN_URL/APKINDEX.tar.gz" -o "$TC/dl/APKINDEX.tar.gz"; tar -xzf "$TC/dl/APKINDEX.tar.gz" -C "$TC/dl" APKINDEX; }
[ -f "$TC/dl/APKINDEX.comm" ] || { curl -fsSL "$COMM_URL/APKINDEX.tar.gz" -o "$TC/dl/APKINDEX.comm.tar.gz"; tar -xzOf "$TC/dl/APKINDEX.comm.tar.gz" APKINDEX > "$TC/dl/APKINDEX.comm"; }

find_alpine_pkg() {
    local p="$1"
    local f
    f=$(awk -v p="$p" '/^P:/{c=substr($0,3)} /^V:/{if(c==p){printf "%s-%s.apk\n",p,substr($0,3); exit}}' "$TC/dl/APKINDEX")
    if [ -n "$f" ]; then echo "main|$f"; return; fi
    f=$(awk -v p="$p" '/^P:/{c=substr($0,3)} /^V:/{if(c==p){printf "%s-%s.apk\n",p,substr($0,3); exit}}' "$TC/dl/APKINDEX.comm")
    if [ -n "$f" ]; then echo "comm|$f"; return; fi
}

ALPINE_PKGS=(
    zlib-dev zlib-static
    bzip2-dev bzip2-static
    xz-dev xz-static
    zstd-dev zstd-static
    brotli-dev brotli-static
    expat-dev expat-static
    libpng-dev libpng-static
    libjpeg-turbo-dev libjpeg-turbo-static
    libwebp-dev libwebp-static
    pixman-dev pixman-static
    freetype-dev freetype-static
    harfbuzz-dev harfbuzz-static harfbuzz-icu
    fontconfig-dev fontconfig-static
    cairo-dev cairo-static
    icu-dev icu-static icu-data-en
    libxml2-dev libxml2-static
    sqlite-dev sqlite-static
    openssl-dev openssl-libs-static
    nghttp2-dev nghttp2-static
    libpsl-dev libpsl-static
    libidn2-dev libidn2-static
    libunistring-dev libunistring-static
    libgcrypt-dev libgcrypt-static
    libgpg-error-dev libgpg-error-static
    libtasn1-dev
    libffi-dev
    pcre2-dev
    util-linux-dev util-linux-static
    glib-dev glib-static
    libxkbcommon-dev libxkbcommon-static
    libepoxy-dev
    linux-headers
)

for p in "${ALPINE_PKGS[@]}"; do
    res="$(find_alpine_pkg "$p" || true)"
    if [ -z "$res" ]; then
        echo "[sysroot] UYARI: $p bulunamadi (atlaniyor)"
        continue
    fi
    repo="${res%%|*}"
    apk="${res#*|}"
    url="$MAIN_URL/$apk"
    [ "$repo" = "comm" ] && url="$COMM_URL/$apk"
    if [ ! -f "$apk" ]; then
        echo "[sysroot] Indiriliyor: $apk"
        curl -fsSL "$url" -o "$apk"
    fi
    tar -xzf "$apk" -C "$SYS" 2>/dev/null || true
done

echo "=== [3/3] Arac Dogrulamasi ==="
export PATH="$HOST/bin:$PATH"
for t in cmake ninja pkg-config ruby gperf flex bison unifdef; do
    echo -n "  $t: "
    case "$t" in
        cmake|ninja|ruby|gperf|flex|bison) "$t" --version | head -n1 ;;
        pkg-config) "$t" --version ;;
        unifdef) "$t" -V 2>&1 | head -n1 || true ;;
    esac
done
echo "=== MILESTONE 1 KURULUM TAMAM ==="
