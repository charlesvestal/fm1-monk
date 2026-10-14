# Building MONK-1

The build makes three files in `build/`:

| File | What |
| --- | --- |
| `monk.bin` | the firmware app |
| `loader/ota.bin` | the update loader |
| `monk.fwsc` | the installable package (app + loader) |

## Prerequisites (macOS)

- Python 3 with Pillow built with libraqm (the font generator uses its text features; Homebrew's
  Python has it, macOS's own `/usr/bin/python3` does not): `PYTHON=/opt/homebrew/bin/python3.14
  ./build.sh` picks one.
- Docker Desktop. The JieLi toolchain is Linux x86-64 only; the build runs each tool in a
  `linux/amd64` `debian:bookworm-slim` container (Rosetta on Apple silicon). Keep the source
  tree in a folder Docker can share, e.g. under `/Users`.
- The JieLi Linux toolchain (clang 4.0.1 for pi32v2, from JieLi's package server):

  ```
  tools/get_toolchain.sh            # installs to ~/.jieli/toolchain
  ```

- The JieLi AC79 SDK (Apache-2.0). The package uses three of its files
  (`cpu/wl82/tools/uboot.boot`, `cfg_tool.bin`, `cfg/eq_cfg_hw.bin`); they are not part of this tree.

  ```
  git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
      https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
  ```

On Linux x86-64 the toolchain runs natively and Docker is not needed.

## Build

```
./build.sh
```

`JIELI_TOOLCHAIN` and `AC79_SDK` override the default locations
(`~/.jieli/toolchain`, `~/fw-AC79_AIoT_SDK`).

`./build.sh --release 0.1` makes a release build; the package is `build/monk-0.1.fwsc`.

On macOS with podman instead of Docker, put a `docker` script that runs `exec podman "$@"` first
on your PATH. `OM_JOBS` (default 4) limits parallel compiles: a podman machine drops
connections when many containers start at once.

Build option: `OM_CDC=1` adds Felucca's USB serial function (off by default: one plain
MIDI interface).

The build also generates `build/gen/`: the font, and the characters (`tools/gen_art.py`: `assets/characters/` packed as palette images in deflated bands). It fails if a soft-double routine is linked
(a `double` crept in) and checks the image, RAM and pool sizes.

## Tests

```
sh tests/run_tests.sh
```

Runs, on the build machine: the maths library against libm; the key sensing (Velo's
`tests/host/sense_test.c`); the voice against upstream MonkSynth's own DSP (`tests/host/monk_test.c`:
dry, the port follows upstream sample for sample within rounding; with the half-rate echo, the level;
it needs a MonkSynth checkout, `MONKSYNTH_DSP=path/to/monksynth/dsp`, default `../../monksynth-ios/dsp`);
the characters as the firmware draws them against the iOS port's own frames (`tests/host/face_test.c`,
`tests/face_compare.py`); the whole app on the computer (`tests/host/ui_sim.c`: a scripted session,
and screenshots: `python3 tests/ui_shots.py build/host/ui sheet.png`); the encoder decoder, USB
audio and TRS MIDI pieces; flash storage; and Felucca's update-path tests against `build/monk.fwsc`.

## The characters

`assets/characters/` comes from the iOS port's drawing code. To render them again (after a change
to the characters there): copy `tools/ExportFM1Art.swift` into monksynth-ios's
`Tests/MonkSynthTests/`, run `scripts/test.sh MonkSynthTests/ExportFM1Art` there (it writes
`/tmp/fm1-monk-art`), then `python3 tools/gen_art.py import /tmp/fm1-monk-art`.

## Install

From the command line (needs `pip3 install mido python-rtmidi`):

```
python3 tools/fm1_install.py build/monk.fwsc
python3 tools/fm1_install.py --info          # identity of the connected FM-1
```

Installing firmware is at your own risk. Hold OCT− and OCT+ for 5 seconds for Felucca's update
mode. If the FM-1 no longer starts but reaches the chip's update mode (4C4A:8057 on USB),
`tools/fm1_rescue.sh` puts stock firmware back from a Mac; otherwise recovery needs
[FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter).
