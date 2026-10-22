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

## Publishing a release

A release is built on GitHub from its tag, so anyone can check that the package came from this source:
`.github/workflows/release.yml` calls the shared FM-1 builder,
[`zvenson/fm1-build` at `v1`](https://github.com/zvenson/fm1-build/tree/v1) (as LOOM-8, X0X and Melodee do). The
builder checks out the tagged commit, installs JieLi's Linux toolchain and the AC79 SDK at its fixed tag, runs
`./build.sh --release X.Y` and `tests/run_tests.sh` (a failing test stops the release; the voice against upstream
MonkSynth is skipped there), then signs build provenance for `monk-X.Y.fwsc` and `monk-X.Y-app.bin`. The Firmware
Hub shows such packages as built from source.

1. Write the notes first, as a draft: `gh release create vX.Y --draft --title "MONK-1 X.Y" --notes-file NOTES.md`
   (the workflow keeps a draft's notes; without one it makes a draft from the commits).
2. Push the tag: `git tag vX.Y && git push origin vX.Y`. The workflow builds, tests, attests, checks the SHA-256 sums
   and the signatures against the builder, the tag and the commit, then publishes the draft. A tag with a suffix
   (`v1.0-beta`) is a prerelease. A published release is never rebuilt: a new build needs a new tag. A manual run
   on a branch (Actions, Run workflow) builds and attests a development package, in the run's `packages` artifact.
3. The site from that package: `gh release download vX.Y --pattern '*.fwsc' --dir build`, then
   `tools/publish_pages.sh X.Y`: it builds the site (landing page, browser emulator, installer, manual, the words
   files, the firmware) with `tools/make_pages.py` and pushes it to the `gh-pages` branch, which GitHub Pages serves
   at <https://charlesvestal.github.io/fm1-monk/>.

To check a downloaded package yourself (GitHub CLI, signed in):

```sh
tag=vX.Y repo=charlesvestal/fm1-monk
gh release download "$tag" --repo "$repo" --dir build/verify --pattern '*.fwsc'
git fetch origin "refs/tags/$tag:refs/tags/$tag"
gh attestation verify "build/verify/monk-${tag#v}.fwsc" --repo "$repo" \
  --signer-workflow zvenson/fm1-build/.github/workflows/build.yml \
  --source-ref "refs/tags/$tag" --source-digest "$(git rev-parse "$tag^{commit}")"
```

The same checks the `-app.bin`. `SHA256SUMS` comes with the downloads; a checksum alone proves nothing about where a
package was built.

A local release build (`./build.sh --release X.Y`, package `build/monk-X.Y.fwsc`) is still how to try one on the
FM-1 before tagging. Its identity is FM-1_45 + the version, two digits each (0.10 is FM-1_45001000); the installer
checks it. Up to 0.9.1 it was FM-1_5 + the version, which MIN-1 also uses.
