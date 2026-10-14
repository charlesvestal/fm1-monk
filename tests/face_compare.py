#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compares the firmware's frames (tests/host/face_test.c) with the iOS port's (assets/characters/ref):
the mean difference over the redrawn region, and the share of its pixels that differ visibly.
  tests/face_compare.py DIR [--sheet out.png]"""
import json
import sys
from pathlib import Path

from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parents[1]
REF = ROOT / "assets" / "characters" / "ref"


def load565(p):
    raw = Path(p).read_bytes()
    im = Image.new("RGB", (240, 240))
    px = im.load()
    for i in range(240 * 240):
        v = (raw[2 * i] << 8) | raw[2 * i + 1]
        r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
        px[i % 240, i // 240] = ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))
    return im


def main():
    d = Path(sys.argv[1])
    sheet_path = sys.argv[3] if len(sys.argv) > 3 and sys.argv[2] == "--sheet" else None
    fails, rows = 0, []
    for f in sorted(d.glob("*.565")):
        mine = load565(f)
        ref = Image.open(REF / (f.stem + ".png")).convert("RGB")
        diff = ImageChops.difference(mine, ref).convert("L")
        hist = diff.histogram()
        n = sum(hist)
        mean = sum(i * c for i, c in enumerate(hist)) / n
        bad = sum(hist[48:]) / n
        ok = mean < 3.0 and bad < 0.01
        fails += not ok
        print(f"  {'ok  ' if ok else 'FAIL'} {f.stem:22s} mean {mean:5.2f}  visibly different {bad * 100:5.2f}%")
        rows.append((mine, ref))
    if sheet_path:
        sh = Image.new("RGB", (480, 240 * len(rows)))
        for i, (m, r) in enumerate(rows):
            sh.paste(m, (0, 240 * i))
            sh.paste(r, (240, 240 * i))
        sh.save(sheet_path)
    print("face compare: ok" if not fails else f"face compare: {fails} frames differ")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
