# LAB X-0 design notes

## Sonic identity

No laboratory X-0 exists in the *S.T.A.L.K.E.R.* games. LAB X-0 is a series conceit: Project X's unnumbered origin
site at the Generators behind the Chornobyl plant, the zero point every emission starts from. X-3 made
hallucinations, X-10 scorched minds, X-19 fused them. X-0 keeps time. Every built-in sound is a real game object
read from the user's own disk: the emission, ordnance and gunfire, detector ticks, anomalies, the Monolith's chant
and the stalkers' barks. `lab_x0_lore_summary.md` (in the repository root) has the canon facts and sources.

Guiding rules:

1. **The emission is the clock.** Everything locks to the host grid, sample accurate. The pattern position derives
   from the host's bar position, so it never drifts from FL Studio's playlist.
2. **Every hit is ordnance.** Transient first, then density. SUB SAFE keeps the band under its split mono and
   clean while the punch chain burns everything above it (LAB X-10's rule).
3. **Anomalies bend time.** Chop, repeat, scratch and reverse are a TIME lane and a VOLUME lane of the sequencer,
   named after anomalies.
4. **Breaks are artifacts**: found objects to cut, re-order and re-pitch.

## Signal flow

```
MIDI pads / step sequencer (control blocks of at most 16 samples, cut at every event)
  -> 16 PAD voices (stereo sample, tune, start, reverse, attack-hold-decay, TONE filter, drive, pan, choke, mode)
  -> 4 BREAK slots (REPITCH | SLICE | STRETCH; onset or grid slices; natural or re-sequenced slices)
  -> PREVIEW voice (browser)
  route per pad and slot: FULL | PUNCH | DRY
  FULL  -> ANOMALY (16 s history, TIME lane + VOLUME lane, two read heads) -+
  PUNCH --------------------------------------------------------------------+-> PUNCH chain
  DRY   -> latency pad -------------------------------------------------------------------------+
PUNCH: TRANSIENT -> MULTIBAND (3 LR4 bands, upward + downward) -> [2x oversampled: SUB SAFE split (LR8)
       -> SLAM (BARK tube stages, BITE compressor, WET) -> CLIP] -> + DRY -> width -> master -> DC blocker
       -> REGULATOR (LAB X-10 look-ahead limiter at CEILING) -> fault guard -> preset fade -> safety clip
```

Latency is the oversampler's plus the regulator's look-ahead: 69 samples at 44.1 kHz, 75 at 48 kHz, 147 at 96 kHz.
DRY waits the same time, so all three routes stay aligned.

## Modules

- **SampleBank** (`SampleBank.h`): one loader thread, slots 0-15 pads, 16-19 breaks, 20 preview. A buffer is
  published lock-free; an epoch counter that is odd while a block runs lets the loader free a retired buffer as
  soon as no block can still hold it. Voices never keep a sample pointer across blocks: they compare the slot's
  data id each block and stop (with a 4 ms click tail) when the sample changes under them. Decoding is stereo
  (mono duplicated, extra channels dropped), capped at 120 s; a break slot takes a WINDOW offset to reach later
  parts of long files (saved with the state). Burst guard: non-finite samples and anything above +24 dBFS are
  silenced with 10 ms either side, the whole burst found first so the margin after it is full. Peak normalised to
  -1 dBFS with the gain capped at +30 dB. Paths of 260 characters or more open through the `\\?\` prefix. A slot
  reports EMPTY, LOADING, its name and length, FILE NOT FOUND, UNREADABLE, EMPTY OR BROKEN, UNSUPPORTED FORMAT, or
  `<ROOT> LIBRARY NOT FOUND / SET FOLDER` when its root folder is gone.
- **Library** (`Library.h`, one per process through `SharedResourcePointer`): roots USER (FL Studio's legacy Packs
  folder by default, any folder by choice), ZONE (STALKER SPECIMENS) and PACK (FL's `stalker sounds`). A sample is
  stored as root + relative path, so a moved library only needs its root set again. Pass 1 lists paths (46,662 files
  in 0.5 s on the user's library), pass 2 reads headers with the plugin's own parsers (WAV, RF64, AIFF, FLAC and Ogg
  exact, including FL's Ogg-in-WAV; MP3 from its first frame or Xing header). The index is cached under
  `%APPDATA%\OPERATION FAIRWAY, LLC\LAB X-0\index\` and reloads in tens of milliseconds; a rescan reuses the cached
  headers. Searches AND their words over name and folder on a background job. Favourites and roots live in
  `LAB X-0.settings`. ZONE keeps the English and Russian voices and drops other languages, music, cutscenes and
  working folders (221,327 files).
- **Categories** (`categorise`): KICK SNARE HAT CYM PERC BREAK FX VOX from words, not substrings. Names are split at
  separators, letter-digit changes and capitals ("YChopHat3" has a hat, "Phat Phrog" has none, "Lex808" is an 808).
  The name decides first, then each folder from the deepest up. A loop says so (loop, break, bpm, fill, a tempo in
  the name that fits the length in whole beats) or sits in a loop folder; a loop with drum words is a BREAK, one
  with a key or instrument word is not a drum. Two-letter abbreviations (oh, sd, hh) are ignored in music folders,
  where they are words of song titles. Recordings of 30 s or more are music, breaks, speech or ambience, never
  one-shots. STALKER roots: voices are VOX, everything else FX.
- **ZONE KIT** (`KitCatalog.h`): 106 curated STALKER hits in six groups (ORDNANCE 17, FIRE 20, CLICK 17, PERC 16,
  VOX 18, FX 18), chosen by measurement (onset sharpness, length, level) from the trilogy and *S.T.A.L.K.E.R. 2*.
  Voice entries are named by faction, situation and language; their words are not transcribed.
- **Patterns** (`Pattern.h`): state, not parameters. 16 patterns x 22 lanes (16 pads, 4 breaks, TIME, VOLUME) x 64
  steps; per pattern a length and a resolution (1/8, 1/16, 1/32, 1/8T, 1/16T); per lane a length (polymeter) and a
  mute. A step holds on, velocity, probability, ratchet 1-4, nudge +-50 %, pitch +-24, slice (natural = -1) and a
  slot. Saved as a versioned gzip blob ("LX0P") in the state. The editor writes under a spin lock; the audio thread
  copies a changed pattern only when the lock is free, so it never waits. A text form (`len=16 res=1/16;p1:x...`)
  drives presets and tests.
- **Sequencer** (`DSP/Sequencer.h`): an event is a pure function of (step, lane), fired when its time falls in the
  block's window. Swing pushes odd steps late by 0.5 x swing x step; nudge moves a hit up to half a step either way;
  ratchets split the step evenly; probability uses a hash of (step, lane, pattern pass), so a render repeats
  exactly. A queued pattern starts at the first boundary of the current pattern at or after its note (half a sample
  of slack, so a note on the bar line takes that bar); the PATTERN parameter and buttons count from the block start.
  A block edge within a fifth of a sample of a boundary is moved onto it, so a bar line on a block edge belongs to
  exactly one block. Host jumps and loops resynchronise; small ppq wobbles are snapped to the previous block's end.
  RUN Free uses an internal clock at the last known tempo.
- **MIDI**: notes 36-51 play pads 1-16, 52-55 break slots A-D from their start, 60-91 slices 1-32 of the SLICE MIDI
  slot, 96-111 select a pattern at the next pattern boundary; CC1 adds to EMISSION.
- **Breaks**: tempo comes from an ACID chunk, then the file name (an explicit "bpm" token, or a bare 2-3 digit
  number that makes the file a whole number of beats within 3 %), then the two folders above it (explicit only),
  then an estimate (spectral-flux autocorrelation). An estimate is ambiguous by an octave, so it takes the octave
  nearest the host tempo, FL's slicer convention; explicit tempos are never folded. Beats follow from tempo and
  length, or the BEATS choice.
- **Slicing** (`AudioAnalysis.cpp`): onset candidates from log spectral flux above a moving median. The flux peaks
  as soon as a hit enters the analysis window, so the attack is searched from the window centre to one hop past
  its end: the sharpest rise of a 0.5 ms peak envelope, then back to where that rise starts (the first stroke of a
  flam), then 1 ms of pre-roll. A candidate that adds no energy (plain or high-passed) is the end of a sound, not
  an attack, and is dropped. SENS sets the strength threshold; slices are at least a 1/32 note apart. Grid slicing
  (1/8, 1/16, 1/32) is the alternative.
- **FIT**: REPITCH plays at host tempo / loop tempo (pitch follows). SLICE starts each slice on its grid step at the
  original speed. STRETCH keeps pitch: two Hann grains of 42 ms, half a grain apart, follow a timeline at the tempo
  ratio; each new grain starts within +-6 ms of the timeline wherever the source best continues the fading grain
  (WSOLA, normalised cross-correlation on a decimated mono copy), and the first grain after a trigger starts at full
  level to keep the attack. A natural step (`x`) plays the break where the pattern position says; a voice already
  playing through that point is not restarted, except by a ratchet repeat.
- **ANOMALY** (`DSP/Anomaly.h`): a region is a run of identical slots in the TIME or VOLUME lane, anchored at its
  first sample. The first pass of a repeat is live; reads only look into the past. TIME slots: Springboard 1/4 to
  1/64 (repeat), Whirligig 1/8 and 1/16 (baby scratch, depth from SCRATCH), Vortex (reverse), Comet (tape stop),
  Jelly (half speed), Burner (accelerating roll), Bubble (random stutter). VOLUME slots: electro gates 1/8 to 1/32,
  fade in, fade out, duck, tremolo 1/16, mute. Two read heads crossfade over 2.5 ms on every jump. The history buffer
  splits a delay into whole and fractional samples before wrapping and keeps a sentinel past the end (the LAB X-3
  and X-19 bug).
- **PUNCH** (`DSP/Punch.h`): TRANSIENT turns the gap between a fast and a slow envelope into up to +-18 dB.
  MULTIBAND is OTT-style: three LR4 bands, each pulled down above -18 dB (10:1 at DOWN 1) and lifted below -42 dB
  (4:1 at UP 1, not below -66 dB), DEPTH blending inside the split; at DEPTH 0 it crossfades over 20 ms to its
  untouched input, so an unused multiband adds no crossover phase. The 2x oversampled stage splits at SUB SAFE
  (Linkwitz-Riley 8th order, mono below), runs SLAM (BARK: three biased tanh stages with shelving; BITE: a 2 ms /
  90 ms compressor from -30 dB at 12:1; WET) and CLIP, and blends from its input to the processed path over 20 ms
  only while SLAM or CLIP works.
- **EMISSION** (macro, plus CC1): transient attack +0.5, multiband depth +0.35, bark +0.35, bite +0.3, clip
  +6 dB, anomaly mix +0.3, scratch depth +0.4.
- **Runaway guard**: every voice renders alone first and is killed if non-finite or above +36 dBFS. After the
  voice sums, ANOMALY, TRANSIENT, MULTIBAND and SLAM the block peak is checked: non-finite or above +42 dBFS
  silences the block and resets every stage (a FAULT); above +18 dBFS is logged as hot. Faults go to
  `%APPDATA%\OPERATION FAIRWAY, LLC\LAB X-0 faults.log` with tempo, pattern, voices, preset, every parameter and
  every slot's sample. SYSTEM shows the count.
- **Editor**: violet CRT, 1400 x 920. LIBRARY browser (USER, ZONE, PACK, KIT tabs; categories; search; sort by
  folder, name, length or tempo; preview, load, favourites, drag to any lane, drop files from Explorer), PATTERN
  grid with a step strip (velocity, probability, ratchet, nudge, pitch, slice), the selected break's waveform with
  its slices and beat grid, pattern tools (undo, copy, paste, clear, Euclid, fill, rotate, shuffle, reverse, roll,
  half and double time), the lane panel that follows the selected lane, and the punch modules.

## Presets

16 kits with patterns, named after places and events of the Zone. Every kit's master volume is set so its own
patterns play at about -11 LUFS momentary maximum at 172 BPM (test 25 holds them within -14..-8). Peaks stay
under the -0.3 dBFS ceiling. Samples come from the USER library (FL's legacy packs: rhythm-lab amens and breaks,
Bass Explorer loops, 808, FPC, RealDrumkits, Vintage), the ZONE KIT and the PACK.

## Changes from the plan, and why

1. **Tempo octave.** Snare-band backbeat features overlap at 174 and 87 BPM, so an estimate takes the octave nearest
   the host tempo. Explicit ACID and name tempos stay authoritative.
2. **Test 16 dataset.** The plan asked for 200 bpm-tagged loops; the user's library has 231 drum loops with tempos
   in their names (141 D&B loops from Bass Explorer and Depth Charge Drums, 90 rhythm-lab breaks whose bare tempo
   suffix is checked against their length). Synth and soundscape loops were left out: the estimator is for drums.
   The test checks accuracy modulo the octave and with the host-octave rule.
3. **Test 17 reference.** The plan's "amen 8-16 slices" was a guess. The rhythm-lab packs ship ReCycle REX2 copies
   of the same 151 breaks, sliced by the pack's author: 4,275 active slice points (ReCycle also stores inactive
   points below its sensitivity, as one-sample slices). The test compares default slices with those points
   within 10 ms. A two-bar amen gets about 20 slices, as the REX2 files do.
4. **Slicer.** The first version searched for attacks half a window too early (slices landed about 20 ms before
   each hit) and fired at the ends of truncated tones. Rewritten as described above.
5. **Test 13 sample.** A uniform random sample of the library is dominated by thousands of Library of Congress
   snippets. The labelled names are drawn at most three per top-level pack instead (240 from 142 packs), labelled
   from path and length alone before the categoriser was rewritten, and not changed after; one was later dropped from
   the published file for its wording.
6. **Multiband and punch bypass.** At default settings the LR4 split rotated the phase of the drums and cost about
   5 dB of peak against DRY. Both stages now hand back their input while unused and crossfade when engaged.
7. **SUB SAFE slope.** With an LR4 split, 6 % of the band an octave below the split leaked into the slammed band and
   moved by 0.5 dB. The split is now LR8 (0.4 % leak); test 22 measures the sub an octave below the split.
8. **STRETCH.** Plain overlap-add shifted the pitch of steady tones toward the tempo ratio (at 172 to 174 BPM it
   played the repitched frequency exactly). WSOLA alignment fixed it.
9. **EMISSION** also raises SCRATCH depth (+0.4), so the macro pushes Whirligig harder.
10. **WINDOW.** Files longer than 120 s (battle records, long breaks) are decoded from a WINDOW offset chosen in the
    BREAK panel, saved with the state.
11. **Long paths.** 424 files of the user's library have paths of 260 characters or more (all Library of Congress
    speech recordings). They are indexed and load.
12. **Missing root.** A slot whose root folder has disappeared names the library instead of the file.
13. **Series regression checks.** Tests 28 (runaway guard at every stage) and 29 (240 s idle with the transport
    playing at 64-sample blocks) carry over the lessons of LAB X-3 0.3.1 and X-19 0.1.2.

## Verification (0.1.0)

`python Tests/run_tests.py` runs 29 tests (71 checks, all passing). Results on the development machine:

| Test | Result |
|---|---|
| 1 formats | WAV 16/24/float, AIFF, FLAC, Ogg, MP3, mono and 6-channel all play at 220.02 Hz; FL's Ogg-in-WAV loads |
| 2 bad files | zero-byte, text, truncated, missing file and missing root give a status, no fault; a 515-character path loads |
| 3 burst guard | gain 2.2275 (expected 2.228, from the real peak); the burst's 20 ms hole sits 65 dB under the tone |
| 4 voices | choke silences the ringing pad by 82 dB; mono retrigger keeps one voice without a step; poly stacks +6.02 dB |
| 5 timing | worst error 0.022 ms (44.1 kHz), 0.020 ms (48 kHz), 0.010 ms (96 kHz), 64-sample blocks |
| 6-8 groove | swing, ratchet and nudge within 0.02 ms; probability 192 of 384 hits at 50 %, 74 at 20 %, varying by bar |
| 9-10 host and MIDI | jumps and loops resynchronise to 0.02 ms; pads, pattern select at the bar, slice notes and CC1 work |
| 11 index | 50,000 files: paths 155 ms, cache 33 ms; user library 46,662 files: paths 0.5 s, cache 37 ms |
| 12 search | 300,000 entries, slowest query 6.2 ms |
| 13 categories | 235 of 239 hand-labelled names (98.3 %) |
| 15 ZONE KIT | 106 of 106 load, all louder than -31 dBFS peak |
| 16 tempo | 231 loops: 99.1 % within 2 % modulo the octave and with the host-octave rule |
| 17 slicing | click loop exact (1 ms pre-roll); REX2: recall 0.82, precision 0.93, 75 % within -3..+2 ms |
| 18 fit | REPITCH grid error 0.02 ms at 1011.8 Hz; SLICE and STRETCH 1000.2 Hz; +12 st 2000 Hz |
| 19 anomaly | Springboard, Vortex, Jelly and Comet match their models at 33-49 dB SNR; gates, mute and fades as specified |
| 20 history buffer | 2,093,255 reads, worst error 7e-7 |
| 21 multiband | bands sum flat within 0.015 dB; DOWN -5.3 dB on a loud band, UP +6.7 dB on a quiet one |
| 22 slam and clip | BARK harmonics; BITE turns 20 dB of input into about 2 dB; CLIP -8 dB crest; ceiling holds; SUB SAFE 0.012 dB (3.5 dB without it); latency exact at 44.1/48/96 kHz |
| 23 transient | ATTACK +4.6 dB crest, -1.4 dB at -1; SUSTAIN +3 dB tail |
| 25 presets | all 16 at -11.0 to -11.3 LUFS momentary maximum, no faults |
| 26 state, fuzz, CPU | state round trip; 300 s fuzz with no fault or non-finite sample; 44x real time on one core with every break stretched and EMISSION full |
| 27 pluginval | strictness 10 passes |
| 28-29 regressions | every stage's runaway caught, silenced and logged; 2 x 240 s idle with the transport playing, no faults |

## Limitations

- REX2 files are read only by the test suite (as a reference); the plugin plays the WAV copies.
- MS-ADPCM WAVs (40 files in the user's library) are not supported and say so.
- No MIDI import, song mode or audio export.
- FL Studio cannot be scanned headlessly: after installing, the user scans plugins and does the listening pass.
- The presets and the ZONE KIT pin paths inside STALKER SPECIMENS and FL's `stalker sounds`; `docs/PINNED_PATHS.md`
  lists them for the library's maintenance tools.

## 0.1.1: queued pattern switches on block-edge bar lines

Reported by the session writing the user guide, with a reproduction through `LabX0Render`: a pattern queued by MIDI
note (or by the PATTERN parameter or buttons, which share the queue) could start one or more patterns late, or never.
It happened when bar lines fell exactly on audio block starts: at 48 kHz with 64-sample blocks (the user's FL setup)
whenever 180000 / BPM is a whole number (90, 100, 120, 125, 144, 150, 160, 180 BPM...), and at other tempos with other
block sizes. 0.1.0 rendered 21 of 60 such cases wrong, including "never" at 120 BPM.

`StepSequencer::collect` took the boundary strictly after the block start, with a 1e-9 tolerance, and used it only if
it came before the block end. Block positions are chained sums, so at an edge that is a bar line the position rounds a
hair above or below the boundary. When it rounded low, the block ending there saw the boundary as not yet reached and
the block starting there saw it as already passed; each bar line was a coin toss, so several could be missed in a
row. Separately, a note exactly on a bar line was always one pattern late, because the boundary at the block start
never counted.

The fix (a tolerance alone would have dropped the new pattern's downbeat when the position rounded the other way):

- A block edge within a fifth of a sample of a boundary of the current pattern is moved onto it, with the same
  arithmetic as the boundary itself, so the boundary belongs to exactly one block: the one it starts.
- The switch goes to the first boundary at or after the block start, compared exactly, as events are.
- A pattern note counts from its own position in the block, less half a sample: on the bar line it takes that bar,
  2 ms after it the next. The constraint applies only in the note's block, so a host loop that jumps back still
  starts the queued pattern at the loop start.

Test 30 renders the switch at 16 tempos (12 at 48 kHz with 64- and 512-sample blocks, 4 at 44.1 kHz with 64), with
notes mid-bar, on bar lines and 2 ms after them (168 cases), plus a queued pattern across a host loop. Run against
0.1.0's sequencer, both of its checks fail. With the fix the full suite passes: 30 tests, 73 checks, pluginval 10.

