#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The manual's and the site's screenshots: tests/host/ui_sim.c's session (the real app on the computer),
# the chosen shots as docs/img/screen-*.png.
#   tools/gen_screens.sh
set -e
cd "$(dirname "$0")/.."
PY="${PYTHON:-python3}"
OUT=build/host
mkdir -p "$OUT/gen" "$OUT/ui" docs/img
"$PY" tools/gen_art.py "$OUT/gen/art.h" >/dev/null
[ -f build/gen/felucca_font.h ] || "$PY" tools/gen_font.py build/gen/felucca_font.h >/dev/null
${CC:-cc} -O1 -std=gnu99 -w -DOM_HOST -Ifirmware/src -Ifirmware/hal -I$OUT/gen -Ibuild/gen \
    -o $OUT/ui_sim tests/host/ui_sim.c firmware/src/dsp/monk.c -lm
$OUT/ui_sim $OUT/ui >/dev/null
"$PY" - "$OUT/ui" docs/img <<'PY'
import sys
from pathlib import Path
sys.path.insert(0, "tests")
from ui_shots import load565
src, out = Path(sys.argv[1]), Path(sys.argv[2])
for shot, name in [("01_boot", "sing"), ("03_singing", "singing"), ("04_pressure", "pressure"),
                   ("06_env", "envelope"), ("06b_echo", "echo"), ("07_touch", "touch"),
                   ("09_unicorn_singing", "unicorn"), ("17_punk", "punk"), ("111_cat", "cat"),
                   ("19_pizza", "pizza"), ("14_oldman", "oldman"), ("15_cow", "cow")]:
    load565(src / f"{shot}.565").save(out / f"screen-{name}.png", optimize=True)
    print(out / f"screen-{name}.png")
PY
