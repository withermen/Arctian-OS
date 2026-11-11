# -*- coding: utf-8 -*-
"""
Arctian acilis animasyonu - veri ureteci.

Referans: "Arctian OS - Acilis Animasyonu" HTML sahnesi (1280x720).
Bu betik, sahnedeki 288 cokgeni (facet) ve zamanlama tablolarini
kernel_plus/system/bootanim_data.c icine, logo/baslik gorsellerini de
design/bootanim/*.argb icine yazar.

Calistirma:
    python tools/bootanim_gen.py

Gorseller referans sayfadan (tarayici cikisi) bir kez pisirilmistir;
yeniden uretmek icin elde edilen 1280x720 ekran goruntuleri gerekir.
"""

import math
import os
import re
import struct
from decimal import Decimal, ROUND_HALF_UP

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_C = os.path.join(ROOT, "kernel_plus", "system", "bootanim_data.c")
OUT_DIR = os.path.join(ROOT, "design", "bootanim")

W, H, C, R = 1280, 720, 16, 9
cw, ch = W / C, H / R

ACCENT = (0x38, 0xB6, 0xEE)
PAGE_BG = (0x05, 0x08, 0x0C)
BAR_BG = (0x16, 0x26, 0x36)


def h(a, b, k):
    v = math.sin(a * 127.1 + b * 311.7 + k * 74.7) * 43758.5453
    return v - math.floor(v)


def jf(v, n):
    """JS toFixed: yarim yukari, en kisa ondalik gosterim uzerinden."""
    d = Decimal(repr(float(v)))
    q = Decimal(1).scaleb(-n)
    s = str(d.quantize(q, rounding=ROUND_HALF_UP))
    if re.fullmatch(r"-0\.0+", s):
        s = s[1:]
    return s


def ms_of(v, n):
    return int(round(float(jf(v, n)) * 1000))


def hsl_to_rgb(hdeg, s, l):
    """CSS Color 3 hsl() -> 8-bit sRGB (tarayicinin yaptigi gibi)."""
    r, g, b = hsl_to_rgb_f(hdeg, s, l)

    def q(v):
        return max(0, min(255, int(round(v * 255.0))))

    return (q(r), q(g), q(b))


def hsl_to_rgb_f(hdeg, s, l):
    """CSS Color 3 hsl() -> 0..1 float sRGB (animasyon interpolasyonu icin)."""
    hh = (hdeg % 360.0) / 60.0
    ss = s / 100.0
    ll = l / 100.0
    c = (1.0 - abs(2.0 * ll - 1.0)) * ss
    x = c * (1.0 - abs((hh % 2.0) - 1.0))
    m = ll - c / 2.0
    if hh < 1:
        rp, gp, bp = c, x, 0.0
    elif hh < 2:
        rp, gp, bp = x, c, 0.0
    elif hh < 3:
        rp, gp, bp = 0.0, c, x
    elif hh < 4:
        rp, gp, bp = 0.0, x, c
    elif hh < 5:
        rp, gp, bp = x, 0.0, c
    else:
        rp, gp, bp = c, 0.0, x
    return (rp + m, gp + m, bp + m)


def rgb16(rgb):
    return tuple(max(0, min(65535, int(round(v * 65535.0)))) for v in rgb)


def rgb_u32(rgb):
    return (rgb[0] << 16) | (rgb[1] << 8) | rgb[2]


# ---------------------------------------------------------------- facets
def build_facets():
    pts = []
    for i in range(R + 1):
        row = []
        for j in range(C + 1):
            edge = i == 0 or j == 0 or i == R or j == C
            x = j * cw + (0 if edge else (h(i, j, 1) - 0.5) * cw * 0.55)
            y = i * ch + (0 if edge else (h(i, j, 2) - 0.5) * ch * 0.55)
            row.append((x, y))
        pts.append(row)

    out = []

    def add(tri, seed):
        xs = [p[0] for p in tri]
        ys = [p[1] for p in tri]
        x0, y0 = min(xs), min(ys)
        x1, y1 = max(xs), max(ys)
        cx = sum(xs) / 3.0
        cy = sum(ys) / 3.0
        d = math.hypot(cx - 640, cy - 340)
        f = min(1, max(0.12, (d - 110) / 280)) * (1 - 0.35 * max(0, (d - 520) / 300))

        lo_l = float(jf((4 + h(seed, 1, 3) * 15) * f + 3, 1))
        hi_h = float(jf(192 + h(seed, 2, 3) * 16, 0))
        hi_l = float(jf(58 + h(seed, 3, 3) * 28, 0))
        lo = hsl_to_rgb_f(212, 55, lo_l)
        hi = hsl_to_rgb_f(hi_h, 80, hi_l)
        lo16 = rgb16(lo)
        hi16 = rgb16(hi)

        delay = 0.2 + (cx / W * 0.75 + cy / H * 0.25) * 2.9 + h(seed, 4, 3) * 0.35
        delay_ms = ms_of(delay, 2)

        tw_dur = 0
        tw_delay = 0
        if h(seed, 5, 3) > 0.72:
            tw_dur = ms_of(3 + h(seed, 6, 3) * 3, 1)
            tw_delay = ms_of(delay + 1.5, 2)

        # 1/10 px tam sayi (CSS toFixed(1) ile ayni hassasiyet)
        tri10 = [(int(round(px * 10)), int(round(py * 10))) for px, py in tri]
        out.append({
            "tri": tri10,
            "lo": lo16, "hi": hi16,
            "delay": delay_ms,
            "tw_dur": tw_dur, "tw_delay": tw_delay,
        })

    for i in range(R):
        for j in range(C):
            a, b, c, e = pts[i][j], pts[i][j + 1], pts[i + 1][j + 1], pts[i + 1][j]
            s = i * 40 + j
            if h(i, j, 4) > 0.5:
                add([a, b, c], s)
                add([a, c, e], s + 500)
            else:
                add([a, b, e], s)
                add([b, c, e], s + 500)
    return out


# ---------------------------------------------------------------- easing
def bezier_y(x, x1, y1, x2, y2):
    """cubic-bezier(x1,y1,x2,y2) -> y for a given x (bisection)."""
    if x <= 0:
        return 0.0
    if x >= 1:
        return 1.0

    def bx(t):
        return 3 * (1 - t) ** 2 * t * x1 + 3 * (1 - t) * t * t * x2 + t ** 3

    def by(t):
        return 3 * (1 - t) ** 2 * t * y1 + 3 * (1 - t) * t * t * y2 + t ** 3

    lo, hi = 0.0, 1.0
    for _ in range(40):
        mid = (lo + hi) / 2
        if bx(mid) < x:
            lo = mid
        else:
            hi = mid
    return by((lo + hi) / 2)


def lut256(x1, y1, x2, y2):
    return [int(round(65535 * bezier_y(i / 256.0, x1, y1, x2, y2))) for i in range(257)]


# ---------------------------------------------------------------- art
def reconstruct(black, white, box):
    bp, wp = black.load(), white.load()
    x0, y0, x1, y1 = box
    out = Image.new("RGBA", (x1 - x0, y1 - y0), (0, 0, 0, 0))
    op = out.load()
    for y in range(y1 - y0):
        for x in range(x1 - x0):
            rb, gb, bb, _ = bp[x0 + x, y0 + y]
            rw, gw, bw, _ = wp[x0 + x, y0 + y]
            a = 255 - ((rw - rb) + (gw - gb) + (bw - bb)) // 3
            a = max(0, min(255, a))
            if a == 0:
                continue
            op[x, y] = (min(255, round(rb * 255 / a)),
                        min(255, round(gb * 255 / a)),
                        min(255, round(bb * 255 / a)), a)
    return out


def write_argb(img, path):
    """0xAARRGGBB, little-endian (B,G,R,A) bayt dizisi."""
    w, h = img.size
    data = bytearray(w * h * 4)
    p = img.load()
    i = 0
    for y in range(h):
        for x in range(w):
            r, g, b, a = p[x, y]
            data[i + 0] = b
            data[i + 1] = g
            data[i + 2] = r
            data[i + 3] = a
            i += 4
    with open(path, "wb") as f:
        f.write(data)


def main():
    facets = build_facets()
    print("facet sayisi:", len(facets))

    # Guvenlik: donusum sirasinda kose karisirsa ucgenler devlesir.
    for n, f in enumerate(facets):
        xs = [p[0] for p in f["tri"]]
        ys = [p[1] for p in f["tri"]]
        assert max(xs) - min(xs) <= 1300, ("genis ucgen", n, f["tri"])
        assert max(ys) - min(ys) <= 1300, ("uzun ucgen", n, f["tri"])

    ease_out = lut256(0.0, 0.0, 0.58, 1.0)
    ease_in_out = lut256(0.42, 0.0, 0.58, 1.0)
    ease_logo = lut256(0.2, 0.7, 0.2, 1.0)

    os.makedirs(OUT_DIR, exist_ok=True)

    # Gorseller: referans sayfanin siyah/beyaz zeminli karelerinden.
    tmp = os.environ.get("BOOTANIM_SHOTS", os.path.join(os.path.expanduser("~"), "AppData", "Local", "Temp", "opencode"))
    black_p = os.path.join(tmp, "bake_000000.png")
    white_p = os.path.join(tmp, "bake_ffffff.png")
    if os.path.exists(black_p) and os.path.exists(white_p):
        black = Image.open(black_p).convert("RGBA")
        white = Image.open(white_p).convert("RGBA")
        logo = reconstruct(black, white, (548, 252, 748, 452))
        title = reconstruct(black, white, (537, 459, 743, 474))
        logo.save(os.path.join(OUT_DIR, "logo.png"))
        title.save(os.path.join(OUT_DIR, "title.png"))
        write_argb(logo, os.path.join(OUT_DIR, "logo.argb"))
        write_argb(title, os.path.join(OUT_DIR, "title.argb"))
        print("gorseller yazildi:", logo.size, title.size)
    else:
        print("UYARI: referans kareler yok, gorseller atlandi")

    lines = []
    lines.append("/* Uretilen dosya - elle duzenlemeyin. */")
    lines.append("/* Kaynak: tools/bootanim_gen.py (referans acilis sahnesi). */")
    lines.append('#include "bootanim.h"')
    lines.append("")
    lines.append("const uint16_t banim_ease_out[257] = {")
    for i in range(0, 257, 12):
        lines.append("    " + ", ".join(str(v) for v in ease_out[i:i + 12]) + ",")
    lines.append("};")
    lines.append("")
    lines.append("const uint16_t banim_ease_in_out[257] = {")
    for i in range(0, 257, 12):
        lines.append("    " + ", ".join(str(v) for v in ease_in_out[i:i + 12]) + ",")
    lines.append("};")
    lines.append("")
    lines.append("const uint16_t banim_ease_logo[257] = {")
    for i in range(0, 257, 12):
        lines.append("    " + ", ".join(str(v) for v in ease_logo[i:i + 12]) + ",")
    lines.append("};")
    lines.append("")
    lines.append("const banim_facet_t banim_facets[] = {")
    for f in facets:
        (ax, ay), (bx, by), (cx, cy) = f["tri"]
        hi = f["hi"]
        lo = f["lo"]
        lines.append("    { { %d, %d, %d }, { %d, %d, %d }, { %d, %d, %d }, { %d, %d, %d }, %d, %d, %d },"
                     % (ax, bx, cx,
                        ay, by, cy,
                        hi[0], hi[1], hi[2], lo[0], lo[1], lo[2],
                        f["delay"], f["tw_dur"], f["tw_delay"]))
    lines.append("};")
    lines.append("")
    lines.append("const uint32_t banim_facet_count = %d;" % len(facets))
    lines.append("")

    with open(OUT_C, "w", encoding="utf-8") as fp:
        fp.write("\n".join(lines))
    print("yazildi:", OUT_C)


if __name__ == "__main__":
    main()
