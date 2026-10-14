#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Make the MONK-1 site for GitHub Pages (https://charlesvestal.github.io/fm1-monk/):

  index.html                     what MONK-1 is, and the ways in: try, install, manual, download, source
  img/                           screenshots (docs/img, from tools/gen_screens.sh)
  emu/                           MONK-1 in the browser (build/emu, from web/emu/build.sh)
  install/index.html             the web installer (web/monk_installer.html, with fm1pkg.js, fm1ota.js and
                                 the package's metadata inlined; Chrome or Edge, Web MIDI)
  manual/index.html, img/        the manual (tools/manual_html.py, from docs/MANUAL.md)
  firmware/monk-VERSION.fwsc     the package the installer writes; also the download

  tools/make_pages.py build/monk-0.9.fwsc 0.9 OUT_DIR

The package must be a release build (./build.sh --release X.Y). Its identity, FM-1_5XXYYZZ, is what the
installer checks the download against and what the FM-1 reports after the install."""
import html
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SRC / "web"))
from make_site import product_of, strip_module  # noqa: E402  (Felucca's: the package format)

REPO = "https://github.com/charlesvestal/fm1-monk"

LANDING = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>MONK-1</title>
<meta name="description" content="Twelve singers for the M-VAVE FM-1: MonkSynth's formant voice with the characters of its iOS port, played with velocity and pressure from the FM-1's own keys.">
<meta name="theme-color" content="#faf4e8">
<style>
/* MONK-1's cream, brown ink and saffron, as its screen */
:root { --paper: #faf4e8; --sheet: #ece4d4; --rule: #d8cdba; --ink: #48392f; --muted: #706254;
        --hot: #b5600c; --hot-t: #f6dcb8; --cool: #f0a020; --cool-t: #fbe8c4; color-scheme: light; }
body { background: var(--paper); color: var(--ink); margin: 0; padding-inline: 16px; padding-block: 48px 64px;
       font: 400 17px/1.6 "Helvetica Neue", Helvetica, Arial, sans-serif; }
main { max-width: 42rem; margin: 0 auto; }
h1 { font-size: 3.2rem; font-weight: 700; line-height: 1; letter-spacing: .02em; margin: 0 0 .3rem; }
.sub { font-weight: 700; font-size: 1rem; margin: 0 0 1.2rem; padding-bottom: .8rem; border-bottom: 3px solid var(--cool); }
h2 { font-size: 1.15rem; font-weight: 700; margin: 2.4rem 0 .6rem; padding-top: .6rem; border-top: 2px solid var(--ink); }
.lede { font-size: 1.15rem; margin: 0 0 1.6rem; max-width: 36rem; }
.shots { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 12px; margin: 0 0 1.8rem; }
.shots img { width: 100%; height: auto; image-rendering: pixelated; border-radius: 4px; box-shadow: 0 0 0 1px var(--rule); }
.ways { display: grid; gap: 10px; margin: 0 0 1rem; }
.ways a { display: block; text-decoration: none; color: var(--ink); background: var(--sheet); border-radius: 8px; padding: 12px 16px; }
.ways a:first-child { background: var(--hot); color: #fff; }
.ways a:first-child span { color: #fbe8d4; }
.ways a:hover, .ways a:focus-visible { background: var(--cool-t); color: var(--ink); outline: none; }
.ways a:first-child:hover span, .ways a:first-child:focus-visible span { color: var(--muted); }
.ways strong { font-weight: 700; font-size: 1.15rem; display: block; }
.ways span { color: var(--muted); font-size: .95rem; }
.ways em.v { font-style: normal; font-weight: 400; }
ul.feat { padding-left: 1.2rem; margin: 0 0 1rem; }
ul.feat li { margin: 0 0 .4rem; }
ul.feat li::marker { color: var(--cool); }
p.small { color: var(--muted); font-size: .9rem; }
a { color: var(--ink); text-decoration-color: var(--cool); text-decoration-thickness: 2px; text-underline-offset: 3px; }
code { font-size: .9em; }
</style>
</head>
<body>
<main>
<h1>MONK-1</h1>
<p class="sub">Twelve singers for the M-VAVE FM-1</p>
<p class="lede">MonkSynth's singing voice on the FM-1, with the twelve characters of its iOS port, from a monk to a
slice of pizza, each on its own stage, its mouth following the vowel. The keys feel how they are played: how
hard a key lands sets the level, and leaning on a held key moves the vowel. Velocity and aftertouch, on an
FM-1, with no changes to the hardware.</p>
<div class="shots">
  <img src="img/screen-singing.png" width="240" height="240" alt="Jerry the monk singing, mouth wide open">
  <img src="img/screen-unicorn.png" width="240" height="240" alt="Philip the unicorn singing in front of a rainbow">
  <img src="img/screen-punk.png" width="240" height="240" alt="Snider the punk singing between two speakers">
</div>
<nav class="ways" aria-label="Get MONK-1">
  <a href="emu/"><strong>Try it in the browser</strong><span>The same code the FM-1 runs, with sound. Where you press a key is how hard it lands; drag down to lean on it.</span></a>
  <a href="install/"><strong>Install <em class="v">v__VERSION__</em></strong><span>From Chrome or Edge, with the FM-1 connected by USB. Nothing to install on the computer.</span></a>
  <a href="manual/"><strong>Manual</strong><span>Getting started in a few minutes, then everything else.</span></a>
  <a href="firmware/__PKG__"><strong>Download <em class="v">__PKG__</em></strong><span>For the command-line installer: <code>python3 tools/fm1_install.py __PKG__</code></span></a>
  <a href="__REPO__"><strong>Source</strong><span>GitHub, GPL-3.0. Built on Felucca by Hügelton Instruments.</span></a>
</nav>

<h2>What it does</h2>
<ul class="feat">
  <li><b>Twelve singers.</b> Jerry the monk, Guppo, Philip, Suze, Gerald, Bess, EeOo, Snider, Fipsy, Peppo, Marley and Razor, each with its own voice, each remembering your changes.</li>
  <li><b>Velocity.</b> How hard a key lands sets the note's level, read from the FM-1's own keys.</li>
  <li><b>Pressure.</b> Leaning on a held key moves the vowel, bends the pitch, or both, counted from where the key rests.</li>
  <li><b>The voice.</b> MonkSynth's formant synthesis: vowel, head size, breath, glide, a choir of up to ten, vibrato, echo and an envelope, four knobs a page.</li>
  <li><b>MIDI.</b> Notes with velocity and polyphonic aftertouch out over USB; notes, aftertouch and the vowel in.</li>
</ul>
<p>MonkSynth is by Jonathan Taylor (MIT), an homage to the Delay Lama plug-in. It is offered completely free of
charge; if you enjoy it, you are kindly requested to make a donation at <a href="https://www.savetibet.org">savetibet.org</a>.</p>
<p class="small">To go back to the official firmware, use the installer's "Back to the stock firmware", or
M-VAVE's updater, M-UPGRADE, from <a href="https://www.m-vave.com/download">m-vave.com/download</a>.
M-VAVE and FM-1 are trademarks of their owners. MONK-1 is not affiliated with M-VAVE.</p>
</main>
</body>
</html>
"""


def main(pkg, version, out):
    pkg, out = Path(pkg), Path(out)
    raw = pkg.read_bytes()
    product = product_of(raw)
    if not re.fullmatch(r"FM-1_5\d{6}", product):
        raise SystemExit(f"{pkg}: identity {product!r}: make a release build (./build.sh --release X.Y)")
    if b"FELUCCA-LOADER-1" not in raw:
        raise SystemExit(f"{pkg}: no update loader in it")
    name = f"monk-{re.sub(r'[^A-Za-z0-9.-]', '-', version)}.fwsc"
    if out.exists():
        shutil.rmtree(out)
    for d in ("install", "manual", "firmware"):
        (out / d).mkdir(parents=True)
    shutil.copy(pkg, out / "firmware" / name)
    # the installer
    page = (SRC / "web" / "monk_installer.html").read_text(encoding="utf-8")
    lib = strip_module((SRC / "web" / "fm1pkg.js").read_text(encoding="utf-8")) + "\n" + \
        strip_module((SRC / "web" / "fm1ota.js").read_text(encoding="utf-8"))
    meta = json.dumps({"version": version, "product": product, "pkg": "../firmware/" + name})
    for mark in ("/*LIB*/", "/*META*/"):
        if page.count(mark) != 1:
            raise SystemExit(f"monk_installer.html must contain {mark} once")
    (out / "install" / "index.html").write_text(page.replace("/*LIB*/", lib).replace("/*META*/", meta), encoding="utf-8")
    # the manual (a complete page; its images beside it)
    subprocess.run([sys.executable, str(SRC / "tools" / "manual_html.py"), str(SRC / "docs" / "MANUAL.md"),
                    str(out / "manual" / "index.html")], check=True, capture_output=True)
    shutil.copytree(SRC / "docs" / "img", out / "manual" / "img")
    # the landing page and its screenshots
    shutil.copytree(SRC / "docs" / "img", out / "img")
    (out / "index.html").write_text(LANDING.replace("__VERSION__", html.escape(version)).replace("__PKG__", name)
                                    .replace("__REPO__", REPO), encoding="utf-8")
    emu = SRC / "build" / "emu"
    if not (emu / "monk.wasm").exists():
        raise SystemExit("no build/emu/monk.wasm: run web/emu/build.sh (needs Emscripten)")
    shutil.copytree(emu, out / "emu")
    (out / ".nojekyll").write_text("")
    print(f"site: {out}: index.html, install/ ({product}), manual/, emu/, firmware/{name} ({len(raw)} B)")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:4])
