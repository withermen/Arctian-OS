#!/bin/bash
# 0.8: sdk/office/* altindaki ofis uygulamalarini derler ve imzalar.
# wf install imza istiyor; WLF_KEY dev anahtariyla (konuk guveni) imzalanir.
#
#   WSL_BUILD=1 bash tools/build_office.sh
# Cikti: build/wlf/{safak,murekkep,abakus,sahne,mercek}.wlf
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/wlf"
mkdir -p "$OUT"
export WLF_KEY="$ROOT/tools/keys/dev_rsa2048.txt"

# wf install <ad> katalogu: GitHub release asset'lerini adresler.
CAT_BASE="https://github.com/withermen/Arctian-OS/releases/download/apps-v1"

: > "$OUT/catalog.txt"
echo "# Arctian .wlf katalogu - wf install <ad> bunu okur" >> "$OUT/catalog.txt"

for d in "$ROOT"/sdk/office/*/; do
    [ -f "$d/main.c" ] || continue
    [ -f "$d/wolf.json" ] || continue
    name=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['name'])" "$d/wolf.json")
    echo "== $name =="
    bash "$ROOT/sdk/build.sh" "$d" >/dev/null
    cp -f "$d/$name.wlf" "$OUT/$name.wlf"
    echo "$name=$CAT_BASE/$name.wlf" >> "$OUT/catalog.txt"
    python3 "$ROOT/sdk/wlfpack.py" --inspect "$OUT/$name.wlf" | grep -E "Ad|Pencere" || true
done

echo "Katalog: $OUT/catalog.txt"
echo "Yayin  : gh release upload apps-v1 $OUT/*.wlf $OUT/catalog.txt --clobber"
echo "Hazir: $OUT"
