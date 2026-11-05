#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# MONK-1 host tests: the maths, the platform pieces kept from Felucca / FoMni, Velo's sensing, the
# voice against upstream MonkSynth (MONKSYNTH_DSP: its dsp/ directory; default ../../monksynth-ios/dsp),
# the characters against the iOS port's frames, the emulator's build, and the update path against the package (needs ./build.sh).
#   tests/run_tests.sh
set -u
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
FAIL=0
OUT=build/host
mkdir -p "$OUT"
run() {
    name="$1"; shift
    if "$@" > "$OUT/$name.log" 2>&1; then
        echo "  ok   $name"
    else
        echo "  FAIL $name (see $OUT/$name.log)"
        tail -15 "$OUT/$name.log" | sed 's/^/       /'
        FAIL=1
    fi
}
run fastmath sh -c "$CC -O2 -ffp-contract=off -Wall -Wextra -Werror -o $OUT/fastmath_test tests/host/fastmath_test.c -lm && $OUT/fastmath_test"
run encoder sh -c "$CC -O2 -w -Ifirmware/hal -o $OUT/encoder_test tests/host/encoder_test.c && $OUT/encoder_test"
run uac sh -c "$CC -O2 -w -Ifirmware/src -Ifirmware/hal -o $OUT/uac_test tests/host/uac_test.c -lm && $OUT/uac_test"
run trs sh -c "$CC -O2 -w -Ifirmware/src -Ifirmware/hal -o $OUT/trs_test tests/host/trs_test.c && $OUT/trs_test"
run sense sh -c "$CC -O2 -ffp-contract=off -std=c99 -Wall -Wextra -Wno-unused-function -Wdouble-promotion -Werror \
    -o $OUT/sense_test tests/host/sense_test.c -lm && $OUT/sense_test"
UP="${MONKSYNTH_DSP:-../../monksynth-ios/dsp}"
if [ -f "$UP/synth.c" ]; then
    run monk sh -c "$CC -O2 -ffp-contract=off -std=c99 -Wall -Wextra -Wdouble-promotion -DOM_HOST -Ifirmware/src/dsp -I$UP \
        -o $OUT/monk_test tests/host/monk_test.c firmware/src/dsp/monk.c $UP/synth.c $UP/voice.c $UP/delay.c -lm && $OUT/monk_test"
else
    echo "  skip monk (no upstream MonkSynth DSP at $UP: set MONKSYNTH_DSP)"
fi
PY="${PYTHON:-python3}"
run face sh -c "$PY tools/gen_art.py $OUT/gen/art.h >/dev/null && mkdir -p $OUT/face && \
    $CC -O2 -std=c99 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -I$OUT/gen -o $OUT/face_test tests/host/face_test.c && \
    $OUT/face_test $OUT/face && $PY tests/face_compare.py $OUT/face"
mkdir -p build/gen
"$PY" tools/gen_words.py firmware/words.tsv web/words build/gen/ann_words.h >/dev/null   # the screen reader's words link
run ui sh -c "mkdir -p $OUT/ui && $CC -O1 -std=gnu99 -w -DOM_HOST -Ifirmware/src -Ifirmware/hal -I$OUT/gen -Ibuild/gen \
    -o $OUT/ui_sim tests/host/ui_sim.c firmware/src/dsp/monk.c -lm && $OUT/ui_sim $OUT/ui"
for f in tests/scenarios/*.monk; do            # scripted sessions: the screen reader's words (said checks)
    run "scenario-$(basename "$f" .monk)" $OUT/ui_sim $OUT/ui "$f"
done
if command -v emcc >/dev/null 2>&1; then
    run web sh web/emu/build.sh
else
    echo "  skip web (no Emscripten: the emulator, web/emu/build.sh)"
fi
run storage sh -c "$CC -O2 -o $OUT/storage_test tests/storage_test.c && $OUT/storage_test"
run flash-ok sh -c "$CC -std=c99 -Wall -Wextra -Werror -o $OUT/flash_ok_test tests/host/flash_ok_test.c && $OUT/flash_ok_test"   # firmware/hal/fm1_flash_ok.h: the flash parts the storage may use
# the M-UPGRADE handshake (firmware/src/ota.c): a release's identity cut to NAME_ and three digits for the plain
# query (M-UPGRADE 3.11), the whole one for the query with body 01; a dev build's; the update loader's, whole
for m in long short loader; do
    case $m in long) f="" ;; short) f="-DTEST_ID='\"FM-1_4500\"'" ;; loader) f="-DFELUCCA_LOADER=1" ;; esac
    run "ota-hs-$m" sh -c "$CC -std=c11 -O1 -Wall -Wextra -Werror -Wno-unused-function $f -o $OUT/ota_hs_$m tests/ota_hs_test.c && $OUT/ota_hs_$m"
done
# the web installer's handshake (web/fm1ota.js): a firmware that answers M-UPGRADE 3.11's query short and its whole
# identity to the one with 01, older builds, M-VAVE's firmware, the loader
if command -v node >/dev/null 2>&1; then
    run installer-hs node tests/host/installer_hs_test.mjs
fi
if [ -f build/monk.fwsc ]; then
    run ota-entry sh -c "$CC -o $OUT/ota_test tests/ota_test.c && $OUT/ota_test build/monk.fwsc"
    if [ -z "${AC79_SDK:-}" ]; then
        echo "  skip update-loader (needs AC79_SDK, as the build)"
    else
    run update-loader sh -c "head -c 100000 build/monk.bin > $OUT/old_app.bin && \
        python3 tools/fm1pkg_make.py $OUT/old_app.bin build/loader/ota.bin $OUT/old.fwsc >/dev/null && \
        $CC -o $OUT/ldr_test tests/ldr_test.c && $OUT/ldr_test $OUT/old.fwsc build/monk.fwsc"
    fi
    run installer python3 tests/install_test.py
else
    echo "  skip update-path tests (no build/monk.fwsc: run ./build.sh)"
fi
[ $FAIL -eq 0 ] && echo "all tests passed" || echo "TESTS FAILED"
exit $FAIL
