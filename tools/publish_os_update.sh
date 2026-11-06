#!/usr/bin/env bash
# Arctian OS guncellemesi yayinla: imzali update.txt + modulleri GitHub
# surumune (tag os-<surum>) yukler. OS, releases/latest/download/update.txt
# üzerinden çeker; her yeni sürüm "latest"i kendisi yapar.
#
# Kullanim (repo kokunden):
#   tools/publish_os_update.sh 0.8.1 "notlar" murekkep=build/wlf/murekkep.wlf ...
#
# Token ekrana yazilmaz; git credential helper'dan alinir.
set -euo pipefail
REPO="withermen/Arctian-OS"
[ $# -ge 2 ] || { echo "kullanim: $0 <surum> <notlar> [id=yol.wlf ...]" >&2; exit 1; }
VER="$1"; NOTES="$2"; shift 2
TAG="os-${VER}"
OUT="build/update"

python3 tools/make_update.py --version "$VER" --notes "$NOTES" --out "$OUT" "$@"

# Token: GH_TOKEN varsa o, yoksa git credential helper (ekrana yazilmaz).
tok="${GH_TOKEN:-}"
if [ -z "$tok" ]; then
  tok=$(printf 'protocol=https\nhost=github.com\n\n' | git credential fill \
        | sed -n 's/^password=//p')
fi
[ -n "$tok" ] || { echo "token alinamadi" >&2; exit 1; }
H1="Authorization: Bearer $tok"
H2="Accept: application/vnd.github+json"
H3="User-Agent: arctian-upd"

# release var mi, yoksa olustur
rel=$(curl -sf -H "$H1" -H "$H2" -H "$H3" \
      "https://api.github.com/repos/$REPO/releases/tags/$TAG") || rel=""
if [ -z "$rel" ]; then
  body=$(python3 -c 'import json,sys; print(json.dumps({
      "tag_name": sys.argv[1], "name": "Arctian OS " + sys.argv[1],
      "body": "OS guncellemesi " + sys.argv[1] + ": " + sys.argv[2]
              + ". update.txt imzali manifest; moduller ayni surumde varlik.",
      "draft": False, "prerelease": False}))' "$TAG" "$NOTES")
  rel=$(curl -sf -X POST -H "$H1" -H "$H2" -H "$H3" \
        -H "Content-Type: application/json" -d "$body" \
        "https://api.github.com/repos/$REPO/releases")
  echo "surum olusturuldu: $TAG"
else
  echo "surum mevcut: $TAG (varliklar yenilenecek)"
fi
upl=$(echo "$rel" | python3 -c 'import json,sys; print(json.load(sys.stdin)["upload_url"].split("{")[0])')
reln=$(echo "$rel" | python3 -c 'import json,sys; print(json.load(sys.stdin)["id"])')

# ayni adli eski varliklari kaldir (clobber)
curl -sf -H "$H1" -H "$H2" -H "$H3" \
  "https://api.github.com/repos/$REPO/releases/$reln/assets" |
python3 -c 'import json,sys
for a in json.load(sys.stdin): print(a["id"])' |
while read -r aid; do
  curl -sf -X DELETE -H "$H1" -H "$H2" -H "$H3" \
    "https://api.github.com/repos/$REPO/releases/assets/$aid" >/dev/null
done

for f in "$OUT"/*; do
  name=$(basename "$f")
  curl -sf -X POST -H "$H1" -H "$H2" -H "$H3" \
    -H "Content-Type: application/octet-stream" \
    --data-binary "@$f" "$upl?name=$name" >/dev/null
  echo "  yuklendi: $name ($(wc -c <"$f") bayt)"
done
echo "TAMAM: https://github.com/$REPO/releases/tag/$TAG"
