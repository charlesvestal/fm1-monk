# MONK-1: twelve singers for the FM-1

A firmware for the M-VAVE FM-1 that turns it into
[MonkSynth](https://github.com/JonET/monksynth), Jonathan Taylor's formant vocal synthesizer (itself
an homage to the Delay Lama plug-in by AudioNerdz, 2002), with the twelve characters of
[its iOS port](https://github.com/charlesvestal/monksynth-ios). Each one sings in its own voice on
its own stage; the mouth follows the vowel, in frames, as the original's sprite sheet did.

The keys feel how hard they are held, without hardware changes, by way of
[Velo](https://github.com/charlesvestal/fm1-velocity)'s sensing: leaning on a held key moves the
vowel (or bends the pitch, or both), counted from where the key rests.

MonkSynth is offered completely free of charge. If you enjoy it, you are kindly requested to make a
donation at [savetibet.org](https://www.savetibet.org).

## Playing

| | |
|---|---|
| keys | sing. One voice: the newest key held sounds, letting go falls back to the one under it, and moving between held keys glides (GLIDE) |
| pressing harder | moves the vowel (TOUCH: PRESSURE, DEPTH) |
| PRESETS | the singer: Jerry the monk, Guppo, Philip, Suze, Gerald, Bess, EeOo, Snider, Fipsy, Peppo, Marley, Razor. Each loads its voice and remembers your changes |
| ALGORITHM | the vowel, on any page |
| page buttons | a set of knobs each: HOME Sing, EDIT Choir, LFO Vibrato, FX Echo, ENV Envelope, GLO Touch. The menu (the button, the page, whose the knobs are, the knobs, the encoders) comes up over the singer when a page button, a knob or PRESETS is touched, and goes after a few seconds |
| KNOB 1–4 | the page's knobs |
| OCT− / OCT+ | octave |
| PLAY | HOLD: the note stays after the key; another key takes over |
| SAVE | saves now (it saves by itself a few seconds after the last change); held 1.5 s: this singer's voice back to how it came |

The pages:

| | button | 1 | 2 | 3 | 4 |
|---|---|---|---|---|---|
| Sing | HOME | Vowel | Head (size of the vocal tract) | Breath | Glide |
| Choir | EDIT | Voices (unison, 1–10) | Detune | Spread (the voices' head sizes) | Level |
| Vibrato | LFO | Depth | Rate | | |
| Echo | FX | Amount | Time | | |
| Envelope | ENV | Attack | Decay | Sustain | Release |
| Touch (all singers) | GLO | Pressure (vowel, pitch, both, both inverted) | Depth | Sensitive (how far a lean goes) | Tune |

USB MIDI out: the notes on channel 1 (with the keys' velocity), and polyphonic aftertouch. In: notes (any
channel), CC 1 vowel, 5 glide, 7 level, 12 echo, 13 head size, aftertouch, CC 123 all notes off.

## How it is made

- **The voice** (`firmware/src/dsp/monk.c`) is MonkSynth's DSP (MIT) ported: FOF synthesis, a grain
  of three damped formant sines and breath noise added at every pitch period, the five vowels
  interpolated on Catmull-Rom splines, upstream's stepped 32-steps-a-semitone pitch, vibrato with its
  jitter, the two glides, ADSR, unison with detune and spread, and the stereo echo. Memory came down
  from upstream's 168 KB a voice to 12 KB (shared tables, a shorter overlap buffer, the echo at half
  rate in int16), and the grain is rebuilt only when the vowel, head size or breath change.
  `tests/host/monk_test.c` runs it next to upstream's own code: dry, the two agree sample for sample
  within rounding.
- **The characters** are the iOS port's drawings. The scenes and the faces (eyes open or blinking,
  brows lifted by loudness) are rendered by the iOS code itself (`tools/ExportFM1Art.swift`) and
  packed as 256-colour images in deflated bands (`tools/gen_art.py`, 177 KB for all twelve). The
  mouth is drawn live (`firmware/src/app/face.c`): the iOS ToonMouth's five shapes, lips, tongue,
  teeth and outline, through a small anti-aliased path rasterizer, with the moustaches and whiskers
  on top. `tests/face_compare.py` checks the result against the iOS port's own frames.
- **The keys**: Velo's sensing (`firmware/src/app/sense.c`), with its measured best settings (DRIVE
  2, WAIT 2, the log-scale map, the top row through the PA10 probe) and its per-key calibration off.

## Build, test, flash

As the other firmwares (see `BUILDING.md`): `./build.sh` makes `build/monk.fwsc`,
`sh tests/run_tests.sh` runs the host tests, `python3 tools/fm1_install.py build/monk.fwsc --yes`
flashes. Package identity `FM-1_5` (dev builds `FM-1_500`), USB name "MONK-1 FM-1".

## Credits

- [MonkSynth](https://github.com/JonET/monksynth) by Jonathan Taylor (MIT): the synthesizer. Its
  licence notice is in `firmware/src/dsp/monk.c`.
- The characters and their voices: [MonkSynth for iOS](https://github.com/charlesvestal/monksynth-ios).
- The pressure routings: [MonkSynth for Schwung](https://github.com/charlesvestal/schwung-monksynth).
- Built on the platform layer of [Felucca](https://github.com/hugelton/Felucca) (Hügelton
  Instruments, GPL-3.0-only), by way of [FoMni](https://github.com/charlesvestal/fm1-fomni) and Velo.

GPL-3.0-only.
