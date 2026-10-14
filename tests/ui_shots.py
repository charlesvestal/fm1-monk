#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tests/host/ui_sim.c's screenshots (raw RGB565) as one contact sheet:  tests/ui_shots.py DIR OUT.png"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw


def load565(p):
    raw = Path(p).read_bytes()
    im = Image.new("RGB", (240, 240))
    px = im.load()
    for i in range(240 * 240):
        v = (raw[2 * i] << 8) | raw[2 * i + 1]
        r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
        px[i % 240, i // 240] = ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))
    return im


shots = sorted(Path(sys.argv[1]).glob("*.565"))
cols = 4
sheet = Image.new("RGB", (cols * 250, ((len(shots) + cols - 1) // cols) * 262), (30, 30, 30))
d = ImageDraw.Draw(sheet)
for i, s in enumerate(shots):
    x, y = (i % cols) * 250 + 5, (i // cols) * 262 + 18
    sheet.paste(load565(s), (x, y))
    d.text((x, y - 14), s.stem, fill=(220, 220, 220))
sheet.save(sys.argv[2])
