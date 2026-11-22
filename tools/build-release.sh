#!/bin/bash
# build.bat bunu cagirir: build-release.sh <repo-wsl-yolu>
# /mnt/c (9P) uzerinde make D-state hang yapabildigi icin ext4 kopyada
# derler, ISO/IMG'yi repo build/ klasorune geri kopyalar.
set -e
SRC="$1"
if [ -z "$SRC" ] || [ ! -d "$SRC" ]; then
  echo "HATA: proje yolu bos/gecersiz: '$SRC'"
  exit 1
fi
DST="$HOME/arctian-build"
mkdir -p "$DST"
rsync -a --delete \
  --exclude='/build' --exclude='/.git' --exclude='/browser_gemini' \
  --exclude='/.qwen' --exclude='/release' --exclude='/.orch' \
  "$SRC/" "$DST/"
cd "$DST"
rm -f /tmp/GMfifo*
make -j4 pack
mkdir -p "$SRC/build"
cp -f build/arctian.iso build/arctian.img "$SRC/build/"
echo "BUILD_EXT4_OK ISO=$SRC/build/arctian.iso"
