#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# MONK-1 in the browser: the app compiled to WebAssembly (Emscripten), plus its page.
#   [OM_VERSION=0.1] [FM1_READER=../fm1-reader] web/emu/build.sh
#       ->  build/emu/{index.html, worklet.js, monk.wasm, reader/ (fm1-reader's words.js, tts.js, espeak/)}
# -ffp-contract=off, like the device.
set -e
cd "$(dirname "$0")/../.."
PY="${PYTHON:-python3}"
mkdir -p build/gen build/emu
[ -f build/gen/felucca_font.h ] || "$PY" tools/gen_font.py build/gen/felucca_font.h >/dev/null
[ -f build/gen/art.h ] || "$PY" tools/gen_art.py build/gen/art.h >/dev/null
"$PY" tools/gen_words.py firmware/words.tsv web/words build/gen/ann_words.h >/dev/null
emcc -O2 -ffp-contract=off -std=gnu99 -Wall -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable \
    -DOM_HOST -DOM_WEB "-DOM_VERSION=\"$(printf %s "${OM_VERSION:-DEV}" | tr a-z A-Z)\"" -Ifirmware/src -Ifirmware/src/dsp -Ibuild/gen \
    --no-entry -sSTANDALONE_WASM -sSTACK_SIZE=1048576 -sINITIAL_MEMORY=33554432 -sFILESYSTEM=0 \
    -o build/emu/monk.wasm web/emu/monk_web.c firmware/src/dsp/monk.c
cp web/emu/index.html web/emu/worklet.js build/emu/
# the screen reader's words and voice (fm1-reader's words.js, tts.js and espeak-ng), from its checkout beside this one
R="${FM1_READER:-../fm1-reader}/web"
rm -rf build/emu/reader
if [ -f "$R/words.js" ] && [ -f "$R/espeak/espeak.wasm" ]; then
    mkdir -p build/emu/reader/espeak
    cp "$R/words.js" "$R/tts.js" "$R/tts_worker.js" build/emu/reader/
    cp "$R/espeak/espeak.js" "$R/espeak/espeak.wasm" "$R/espeak/LICENSE" "$R/espeak/README.md" build/emu/reader/espeak/
else
    echo "emu: no fm1-reader at $R (FM1_READER): the screen is read as its own text, and without this page's voice"
fi
echo "emu: build/emu ($(wc -c < build/emu/monk.wasm) B wasm)"
