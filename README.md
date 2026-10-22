# MONK-1: twelve singers for the FM-1

A firmware for the M-VAVE FM-1 that turns it into
[MonkSynth](https://github.com/JonET/monksynth), Jonathan Taylor's formant vocal synthesizer (itself
an homage to the Delay Lama plug-in by AudioNerdz, 2002), with the twelve characters of
[its iOS port](https://github.com/charlesvestal/monksynth-ios). Each one sings in its own voice on
its own stage; the mouth follows the vowel, in frames, as the original's sprite sheet did.

Velocity and aftertouch from the FM-1's own keys, with no hardware changes, by way of
[Velo](https://github.com/charlesvestal/fm1-velocity)'s sensing: velocity sets each note's level,
aftertouch moves the vowel (or bends the pitch, or both), counted from where the key rests.

![Jerry the monk singing, mouth wide open, in front of mountains and prayer flags](docs/img/screen-singing.png) ![Philip the unicorn singing in front of a rainbow](docs/img/screen-unicorn.png) ![Snider the punk singing between two speakers](docs/img/screen-punk.png)

- **Try it in your browser** (no FM-1 needed): <https://charlesvestal.github.io/fm1-monk/emu/>
- **Install it** (Chrome or Edge, with the FM-1 plugged in): <https://charlesvestal.github.io/fm1-monk/install/>
- **Read the manual**: <https://charlesvestal.github.io/fm1-monk/manual/> (source: [docs/MANUAL.md](docs/MANUAL.md))
- **Download the firmware file**: [releases](https://github.com/charlesvestal/fm1-monk/releases)

MonkSynth is offered completely free of charge. If you enjoy it, you are kindly requested to make a
donation at [savetibet.org](https://www.savetibet.org).

## Playing

| | |
|---|---|
| keys | sing. One voice: the newest key held sounds, letting go falls back to the one under it, and moving between held keys glides (GLIDE) |
| velocity | the note's level (Touch: Velocity) |
| aftertouch | moves the vowel (Touch: Pressure, Depth, Sensitive) |
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
| Vibrato | LFO | Depth | Rate | Tune | |
| Echo | FX | Amount | Time | | |
| Envelope | ENV | Attack | Decay | Sustain | Release |
| Touch (all singers) | GLO | Pressure (vowel, pitch, both, both inverted) | Depth | Sensitive (how much aftertouch it takes) | Velocity (how much it sets the level) |

USB MIDI out: the notes on channel 1 with their velocity, and polyphonic aftertouch. In: notes (any
channel), CC 1 vowel, 5 glide, 7 level, 12 echo, 13 head size, aftertouch, CC 123 all notes off.

### Screen reader

MONK-1 tells the [FM-1 Screen Reader](https://charlesvestal.github.io/fm1-reader/) what its screen shows,
over USB: the singer, the page and its knobs, a knob's value as it turns, and what each button did. Open the
page in Chrome with the FM-1 plugged in; it speaks with its own voice, or hands the text to your computer's
screen reader.

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
flashes. Package identity `FM-1_45` + the version (dev builds `FM-1_4500`; 0.9.x
was `FM-1_5` + the version), USB name "MONK-1 FM-1".

## Credits

- [MonkSynth](https://github.com/JonET/monksynth) by Jonathan Taylor (MIT): the synthesizer. Its
  licence notice is in `firmware/src/dsp/monk.c`.
- The characters and their voices: [MonkSynth for iOS](https://github.com/charlesvestal/monksynth-ios).
- The pressure routings: [MonkSynth for Schwung](https://github.com/charlesvestal/schwung-monksynth).
- Built on the platform layer of [Felucca](https://github.com/hugelton/Felucca) (Hügelton
  Instruments, GPL-3.0-only), by way of [FoMni](https://github.com/charlesvestal/fm1-fomni) and Velo.

GPL-3.0-only.

## How the keys sense velocity and aftertouch

The FM-1's keys are a diode matrix of rubber domes, each with a carbon pill that closes the contact.
Stock firmware only asks whether a key is down. The pill's resistance falls as it is pressed harder,
and two of the matrix's row lines (PA7, PA8) are also ADC inputs, so the firmware can measure the
press (`firmware/src/app/sense.c`, from [Velo](https://github.com/charlesvestal/fm1-velocity); its
`notes/01-lab.md` has the measurements).

- **Reading the pill.** Each key is sampled every ~2.2 ms. Against the row's 10k pull-up, a seated
  key reads mostly the matrix diode (~0.6 V), and pressing harder moves it only 10–20 counts.
  So once a key has been touched, every other sample is taken with its row driven high by the pin's
  weak GPIO driver instead. That driver becomes the upper resistor of the divider, and the reading
  less the diode's share is the pill's resistance. The open, touch, seated and lifted states still
  run on the pulled-up samples.
- **Velocity.** The dome snaps, so every press lands within a sample or two whatever its speed.
  What tells a hard hit from a soft one is how deep the key sits just after landing. The velocity is
  the lowest driven reading in the first two driven samples after the touch (WAIT 2, 6.6 ms), on a
  log scale: conductive rubber's resistance falls as a power of the force, so its log is linear in
  the log of the force, placed between a floor (hardest) and a ceiling (lightest press that
  seats). Velo can also learn each key's own floor and ceiling from its landings; that is off here.
- **Aftertouch** is pressing harder than the key rests. A key keeps settling for a while after it
  lands, so the zero follows the reading down for 250 ms and then creeps after it, and comes back up
  at once when the key is eased (`ui.c` `pressure()`). The lean is the drop in the pill's
  resistance below that rest, in octaves; Touch: Sensitive sets how many make full aftertouch. Every
  held key's goes out as polyphonic aftertouch.
- **The top row** (the seven keys at the right end, C♯ to G) is on PA6, which has no ADC. Its
  velocity comes from a probe through the LED line PA10 with PA6 driven high, which saturates on
  firm presses; those keys have no aftertouch.
- **LEDs.** The button LEDs share the matrix with the keys, and a lit one shifts its column's
  readings, so while a key is down the LED at its matrix position stays dark when its column is
  sampled (`FM1_INPUT_LED_OK`).

`tests/host/sense_test.c` runs the sensing on synthetic press curves.
