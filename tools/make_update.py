#!/usr/bin/env python3
"""Arctian OS guncelleme varliklari uretir: imzali update.txt + moduller.

Sozlesme (GitHub varlik kipi, bkz. include/arctian/update.h):
  - OS, en son surumden "update.txt" varligini ceker.
  - Manifestte package=<ad.wlf> ile anilan her modul yine ayni surumde
    varlik olarak durur; OS taban adrese ekleyip indirir.
  - Manifest govdesi (sig= satiri hariç) mağaza anahtarıyla
    RSA/SHA-256 imzalanir; OS imzasiz manifesti kabul etmez.

Kullanim (repo kokunden):
  python3 tools/make_update.py --version 0.8.1 --notes "duzeltmeler" \
      [--out build/update] murekkep=build/wlf/murekkep.wlf ...

  --version OS surumudur; kullanici update_check()'te kendi surumuyle
  karsilastirir. Moduller .wlf olmali (imzali) — kurulumda OS tarafinda
  da imza + CRC dogrulanir, bozuk/ruuzgarli paket kurulmaz.

Cikt: build/update/update.txt ve build/update/<ad>.wlf (oldugu gibi
kopyalanir; release'e hepsi birlikte yuklenir).
"""
import argparse
import os
import sys
import time
import zlib

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
sys.path.insert(0, os.path.join(_ROOT, "store", "common"))
import wlflib  # noqa: E402


def build_manifest(version, notes, modules):
    """modules: [(id, dosya_yolu)] -> (govde, icerik_listesi)."""
    lines = ["#arctian-update 1\n",
             "version=" + wlflib.enc(version) + "\n",
             "notes=" + wlflib.enc(notes) + "\n",
             "released=%d\n" % int(time.time())]
    staged = []
    for mid, path in modules:
        with open(path, "rb") as f:
            data = f.read()
        if not wlflib.verify_pkg(data):
            raise SystemExit("modul imzali/gecerli degil: %s" % path)
        crc = zlib.crc32(data) & 0xFFFFFFFF
        asset = mid + ".wlf"
        lines.append("@file\n")
        lines.append("id=" + wlflib.enc(mid) + "\n")
        lines.append("version=" + wlflib.enc(version) + "\n")
        lines.append("size=%d\n" % len(data))
        lines.append("crc=%08x\n" % crc)
        lines.append("package=" + asset + "\n")   # varlik adi: taban + ad
        staged.append((asset, data))
    body = "".join(lines)
    text = body + "sig=" + wlflib.sign_hex(body.encode("utf-8")) + "\n"
    return text, staged


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--version", required=True, help="surum, orn. 0.8.1")
    ap.add_argument("--notes", default="", help="kisa aciklama")
    ap.add_argument("--out", default=os.path.join(_ROOT, "build", "update"))
    ap.add_argument("modules", nargs="+", metavar="id=yol.wlf")
    a = ap.parse_args()

    mods = []
    for m in a.modules:
        if "=" not in m:
            raise SystemExit("modul bicimi id=yol.wlf: %s" % m)
        mid, path = m.split("=", 1)
        if not os.path.isfile(path):
            raise SystemExit("dosya yok: %s" % path)
        mods.append((mid, path))

    os.makedirs(a.out, exist_ok=True)
    text, staged = build_manifest(a.version, a.notes, mods)
    with open(os.path.join(a.out, "update.txt"), "w", newline="") as f:
        f.write(text)
    for asset, data in staged:
        with open(os.path.join(a.out, asset), "wb") as f:
            f.write(data)
    print("yazildi: %s (%d modul, %d bayt manifest)"
          % (os.path.join(a.out, "update.txt"), len(staged), len(text)))


if __name__ == "__main__":
    main()
