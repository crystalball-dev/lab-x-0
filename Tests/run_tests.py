"""Automated offline checks for LAB X-0.

Runs the LabX0Render harness through a battery of renders and checks the results with numpy.
Usage:  python Tests/run_tests.py [--only 1,2,5] [--skip-pluginval] [--fuzz-seconds 300] [--library <dir>]

Fixtures come from Tests/make_fixtures.py (generated on the first run). Checks that need the user's FL Studio legacy
packs (the plugin's default USER folder, or --library) or the STALKER SPECIMENS library report SKIP when those are
missing. Writes WAVs to Tests/out (ignored by git) and a JSON report to build/test-report.json.

Test numbers follow LAB_X0_HANDOFF.md section 5; docs/DESIGN.md says where a test was redefined. Tests 28 and 29 are
the LAB-X series regression checks: the runaway guard, and a long idle with the transport playing. Test 30 guards the
0.1.1 fix: queued pattern switches on bar lines that fall on block edges. Test 35 guards 0.2.1: RENDER's MIDI file
plays back into LAB X-0 as the render. Test 36 guards 0.2.2: stopping the transport releases the breaks.
"""

import argparse
import json
import math
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import time
import traceback

import numpy as np
import soundfile as sf

ROOT = pathlib.Path(__file__).resolve().parent.parent
HARNESS = ROOT / "build" / "LabX0Render_artefacts" / "Release" / "LabX0Render.exe"
BUNDLE = ROOT / "build" / "LabX0_artefacts" / "Release" / "VST3" / "LAB X-0.vst3"
OUT = ROOT / "Tests" / "out"
FIX = ROOT / "Tests" / "fixtures"
DEFAULT_LIBRARY = r"E:\Program Files\Image-Line\FL Studio 20\Data\Patches\Packs\Legacy"
sys.path.insert(0, str(ROOT / "Tests"))
import analyze as an  # noqa: E402

STAT = re.compile(r'(\w+)=("([^"]*)"|\S+)')
LIBRARY = None            # the USER library (pathlib.Path) when present
BPM = 174.0
BEAT = 60.0 / BPM
STEP = BEAT / 4.0         # a 1/16 step at 174 BPM
INIT = ["--init"]         # every parameter at its default, no kit, no patterns


# ----------------------------------------------------------------------------------------------------------------------
# Harness helpers

def parse_stats(text):
    stats = {}
    for line in text.splitlines():
        for key, value, quoted in STAT.findall(line):
            stats[key] = quoted if value.startswith('"') else value
    return stats


def run(args, timeout=900):
    """Runs the harness without rendering; returns (stdout, exit code)."""
    proc = subprocess.run([str(HARNESS)] + [str(a) for a in args], capture_output=True, text=True, timeout=timeout,
                          encoding="utf-8", errors="replace")
    return proc.stdout, proc.returncode


def render(name, extra, timeout=900):
    OUT.mkdir(parents=True, exist_ok=True)
    wav = OUT / f"{name}.wav"
    proc = subprocess.run([str(HARNESS), "--out", str(wav)] + [str(e) for e in extra], capture_output=True, text=True,
                          timeout=timeout, encoding="utf-8", errors="replace")
    stats = parse_stats(proc.stdout)
    stats["exit"] = proc.returncode
    if proc.returncode != 0:
        stats["stderr"] = (proc.stderr.strip() or proc.stdout.strip())[-400:]
    return wav, stats


def sets(values):
    out = []
    for v in values:
        out += ["--set", v]
    return out


def pat(rows, length=16, res="1/16"):
    return f"len={length} res={res};" + ";".join(rows)


def load(wav):
    data, sr = sf.read(str(wav), always_2d=True)
    return data, sr


def load_mono(wav):
    data, sr = load(wav)
    return data.mean(axis=1), sr


def clean(st):
    return st.get("exit") == 0 and int(st.get("non_finite", "1")) == 0 and int(st.get("faults", "1")) == 0


def db(x):
    return 20.0 * math.log10(max(float(x), 1e-12))


def rms_db(x):
    x = np.asarray(x, dtype=float)
    return db(math.sqrt(float(np.mean(x * x)))) if len(x) else -240.0


def seg(x, sr, a, b):
    return x[max(0, int(round(a * sr))):max(0, int(round(b * sr)))]


def first_above(x, sr, t, before=0.002, after=0.006, frac=0.5):
    """First sample near t whose level passes frac of the local peak (exact for the click fixture: its first sample
    is its peak)."""
    lo = max(0, int(round((t - before) * sr)))
    hi = min(len(x), int(round((t + after) * sr)))
    s = np.abs(x[lo:hi])
    if len(s) == 0 or s.max() <= 1e-9:
        return None
    return (lo + int(np.argmax(s > frac * s.max()))) / sr


def detect_onsets(x, sr, rel=0.25, min_gap=0.015):
    """Rising crossings of rel x the overall peak, at least min_gap apart."""
    a = np.abs(np.asarray(x))
    if a.max() <= 1e-9:
        return []
    above = a > rel * a.max()
    idx = list(np.flatnonzero(above[1:] & ~above[:-1]) + 1)
    if above[0]:
        idx.insert(0, 0)
    out = []
    for i in idx:
        if not out or (i - out[-1]) / sr >= min_gap:
            out.append(int(i))
    return [i / sr for i in out]


def match_times(measured, expected, tol):
    """Same count and each within tol seconds."""
    if len(measured) != len(expected):
        return False, None
    worst = max(abs(m - e) for m, e in zip(measured, expected)) if expected else 0.0
    return worst <= tol, worst


def tone_amp_db(x, sr, f, a, b):
    s = seg(x, sr, a, b)
    n = len(s)
    spec = np.abs(np.fft.rfft(s * np.hanning(n)))
    k = int(round(f * n / sr))
    return db(spec[max(0, k - 2):k + 3].max())


def third_octave_levels(x, sr, lo=40.0, hi=16000.0):
    n = len(x)
    spec = np.abs(np.fft.rfft(x * np.hanning(n))) ** 2
    f = np.fft.rfftfreq(n, 1.0 / sr)
    out, c = [], lo
    while c < hi:
        a, b = c / 2 ** (1 / 6), c * 2 ** (1 / 6)
        out.append(10 * math.log10(spec[(f >= a) & (f < b)].sum() + 1e-30))
        c *= 2 ** (1 / 3)
    return np.array(out)


def crest_db(x):
    x = np.asarray(x, dtype=float)
    return db(np.abs(x).max()) - rms_db(x)


def fx(rel):
    return str(FIX / rel)


# ----------------------------------------------------------------------------------------------------------------------
# Library helpers (USER library = FL Studio's legacy packs)

def find_ogg_in_wav(lib):
    """A WAV holding Ogg Vorbis (format tags 0x674F-0x6771), as FL's own kits store them. Cached in Tests/out."""
    cache = OUT / "ogg_in_wav.txt"
    if cache.exists():
        p = cache.read_text(encoding="utf-8").strip()
        if p and os.path.exists(p):
            return p
    for sub in ("Drums", "Kits", "FPC", "FX", "Vocals"):
        base = lib / sub
        if not base.is_dir():
            continue
        for dp, dn, fn in os.walk(base):
            for f in fn:
                if not f.lower().endswith(".wav"):
                    continue
                p = os.path.join(dp, f)
                try:
                    head = open(p, "rb").read(64)
                except OSError:
                    continue
                i = head.find(b"fmt ")
                if i >= 0 and i + 10 <= len(head):
                    tag = struct.unpack("<H", head[i + 8:i + 10])[0]
                    if 0x674F <= tag <= 0x6771:
                        cache.write_text(p, encoding="utf-8")
                        return p
    return None


def name_bpm(name):
    m = re.search(r'(\d{2,3}(?:\.\d+)?)\s*[-_ ]?\s*bpm', name, re.I) or re.search(r'bpm\s*[-_ ]?\s*(\d{2,3})', name, re.I)
    return float(m.group(1)) if m else None


def bare_bpm(name, dur):
    for m in re.finditer(r'(?<![\d.])(\d{2,3})(?![\d.])', name):
        v = float(m.group(1))
        if 60 <= v <= 200:
            beats = dur * v / 60
            if beats >= 2 and abs(beats - round(beats)) / round(beats) < 0.03:
                return v
    return None


def tempo_dataset(lib):
    """Tempo-tagged drum loops: D&B loops with "NNNbpm" names (Bass Explorer, Depth Charge Drums) and the rhythm-lab
    breaks, whose bare tempo suffix is checked against a whole number of beats."""
    tagged, bare = [], []
    for folder in ("Bass Explorer Presents 101 Drum & Bass Drum Loops",
                   os.path.join("FreeDrumKits.net - East Coast Drum Kit", "Soundtrack_Loops_Depth_Charge_DnB_WAV")):
        for dp, dn, fn in os.walk(lib / folder):
            for f in fn:
                if f.lower().endswith(".wav"):
                    b = name_bpm(f)
                    p = os.path.join(dp, f)
                    if b and ("Bass Explorer" in p or os.sep + "Drums" + os.sep in p):
                        tagged.append((p, b))
    for folder in ("rhythm-lab.com_amen_vol.1", "rhythm-lab.com_hiphop_kicking_breaks", "rhythm-lab.com_noskool_breaks"):
        for dp, dn, fn in os.walk(lib / folder):
            for f in fn:
                if f.lower().endswith(".wav"):
                    p = os.path.join(dp, f)
                    try:
                        dur = sf.info(p).duration
                    except Exception:
                        continue
                    b = bare_bpm(os.path.splitext(f)[0], dur)
                    if b:
                        bare.append((p, b))
    return tagged, bare


REX_SETS = ["rhythm-lab.com_amen_vol.1", "rhythm-lab.com_amen_vol.2", "rhythm-lab.com_amen_vol.3",
            "rhythm-lab.com_hiphop_kicking_breaks", "rhythm-lab.com_noskool_breaks"]


def rex_slices(path):
    """Sample rate and active slice starts of a ReCycle REX2 file (SINF and SLCE chunks; length-1 slices are ReCycle's
    inactive points below the author's sensitivity setting)."""
    d = open(path, "rb").read()
    j = d.find(b"SINF")
    sr = struct.unpack(">I", d[j + 10:j + 14])[0]
    out, i = [], 0
    while True:
        i = d.find(b"SLCE", i)
        if i < 0:
            break
        size = struct.unpack(">I", d[i + 4:i + 8])[0]
        p = d[i + 8:i + 8 + size]
        if struct.unpack(">I", p[4:8])[0] > 1:
            out.append(struct.unpack(">I", p[0:4])[0])
        i += 8
    return sr, out


def rex_pairs(lib):
    pairs = []
    for s in REX_SETS:
        base = lib / s / s
        rexdir = next((base / d for d in ("REX2", "REX") if (base / d).is_dir()), None)
        wavdir = base / "WAV"
        if rexdir is None or not wavdir.is_dir():
            continue
        wavs = {f.stem.lower(): f for f in wavdir.iterdir() if f.suffix.lower() == ".wav"}
        for f in sorted(rexdir.iterdir()):
            if f.stem.lower() in wavs:
                pairs.append((f, wavs[f.stem.lower()]))
    return pairs


def slice_batch(paths, sens):
    lst = OUT / "slice_list.txt"
    lst.write_text("\n".join(str(p) for p in paths), encoding="utf-8")
    out, _ = run(["--slice-batch", lst, "--sens", sens], timeout=1800)
    res = {}
    for line in out.splitlines():
        cols = line.split("\t")
        if len(cols) == 4 and cols[0] != "ERR":
            res[os.path.normcase(cols[3])] = (float(cols[0]), [int(v) for v in cols[2].split(",") if v])
    return res


# ----------------------------------------------------------------------------------------------------------------------
# The tests. Each records one or more checks through T.record(number, name, ok, detail); ok None = SKIP.

def t01_formats(T):
    files = ["formats/pcm16.wav", "formats/pcm24.wav", "formats/float32.wav", "formats/aiff16.aiff", "formats/flac16.flac",
             "formats/vorbis.ogg", "formats/mono.wav", "formats/six_channel.wav", "formats/mpeg.mp3"]
    failures, freqs = [], {}
    for rel in files:
        if not (FIX / rel).exists():
            failures.append((rel, "fixture missing"))
            continue
        wav, st = render("fmt_" + pathlib.Path(rel).stem, INIT + ["--pad", "1=" + fx(rel), "--set", "p01_route=2",
                                                                 "--events", "36@0.1-0.65:110", "--seconds", "0.7"])
        m, sr = load_mono(wav)
        f0 = an.dominant_frequency(seg(m, sr, 0.15, 0.5), sr, 60)
        freqs[rel] = round(f0, 2)
        if not clean(st) or st.get("slot0_status") != "2" or abs(f0 / 220.0 - 1) > 0.005:
            failures.append((rel, st.get("slot0"), round(f0, 1)))
    T.record(1, "formats: WAV 16/24/float, AIFF, FLAC, Ogg, MP3, mono, 6-channel load and play at pitch", not failures,
             {"failures": failures, "f0": freqs})
    ogg = find_ogg_in_wav(LIBRARY) if LIBRARY else None
    if ogg is None:
        T.record(1, "formats: FL Ogg-in-WAV kit sample", None, {"skip": "no USER library or no Ogg-in-WAV file found"})
        return
    wav, st = render("fmt_ogg_in_wav", INIT + ["--pad", "1=" + ogg, "--events", "36@0.05-0.9:110", "--seconds", "1.0"])
    m, sr = load_mono(wav)
    level = rms_db(seg(m, sr, 0.05, 0.4))
    T.record(1, "formats: FL Ogg-in-WAV kit sample", clean(st) and st.get("slot0_status") == "2" and level > -50,
             {"file": os.path.basename(ogg), "slot": st.get("slot0"), "rms_db": round(level, 1)})


def t02_bad_files(T):
    cases = [("bad/zero.wav", "UNREADABLE"), ("bad/text.wav", "UNREADABLE"), ("bad/truncated.wav", "EMPTY OR BROKEN"),
             ("bad/not_there.wav", "FILE NOT FOUND")]
    failures = []
    for rel, expect in cases:
        wav, st = render("bad_" + pathlib.Path(rel).stem, INIT + ["--pad", "1=" + fx(rel), "--events", "36@0.1-0.5", "--seconds", "0.6"])
        if not clean(st) or not st.get("slot0", "").startswith(expect) or float(st.get("peak_db", "0")) > -100:
            failures.append((rel, st.get("slot0"), st.get("faults"), st.get("peak_db")))
    missing_root = OUT / "no_such_library"
    wav, st = render("bad_missing_root", INIT + ["--library", missing_root, "--pad", "1=USER:Drums/kick.wav",
                                                 "--events", "36@0.1-0.5", "--seconds", "0.6"])
    if not clean(st) or "USER LIBRARY NOT FOUND" not in st.get("slot0", ""):
        failures.append(("missing USER root", st.get("slot0")))
    T.record(2, "bad files and paths give a status, never a fault", not failures, {"failures": failures})

    long_files = list((FIX / "bad").rglob("long_path_click.wav"))
    if not long_files:
        T.record(2, "a path longer than 260 characters loads", None, {"skip": "long-path fixture was not created"})
        return
    wav, st = render("long_path", INIT + ["--pad", "1=" + str(long_files[0]), "--set", "p01_route=2", "--events", "36@0.1-0.4",
                                          "--seconds", "0.5"])
    T.record(2, "a path longer than 260 characters loads", clean(st) and st.get("slot0_status") == "2" and float(st.get("peak_db", "-120")) > -30,
             {"length": len(str(long_files[0])), "slot": st.get("slot0"), "peak_db": st.get("peak_db")})


def t03_burst(T):
    out, _ = run(["--analyze", fx("bad/burst.wav")])
    gain = float(parse_stats(out).get("gain", "0"))
    wav, st = render("burst", INIT + ["--pad", "1=" + fx("bad/burst.wav"), "--set", "p01_route=2", "--events", "36@0.1-1.2:110",
                                      "--seconds", "1.3"])
    m, sr = load_mono(wav)
    d = np.diff(m)                                 # differenced: the master DC blocker's slow settling drops out
    hole = rms_db(seg(d, sr, 0.1115, 0.1322)) - rms_db(seg(d, sr, 0.15, 0.5))   # burst 0.1208-0.1229 s, +-10 ms
    body = rms_db(seg(m, sr, 0.15, 0.5))
    ok = clean(st) and 2.0 < gain < 2.5 and hole < -60 and body > -30 and float(st.get("peak_db", "9")) <= -0.2
    T.record(3, "burst guard: +80 dB garbage zeroed with 10 ms margins, normalised from the real peak", ok,
             {"normalise_gain": gain, "expected_gain": 2.228, "hole_vs_body_db": round(hole, 1), "body_rms_db": round(body, 1),
              "peak_db": st.get("peak_db")})


def t04_choke_mono_gate(T):
    base = INIT + ["--pad", "4=" + fx("tone_1000_long.wav"), "--pad", "3=" + fx("click.wav"),
                   "--events", "39@0.1-2.0,38@0.5-0.6", "--seconds", "1.0"] + sets(["p03_route=2", "p04_route=2"])
    res = {}
    for label, extra in (("choke", ["p03_choke=1", "p04_choke=1"]), ("open", [])):
        wav, st = render(f"choke_{label}", base + sets(extra))
        m, sr = load_mono(wav)
        res[label] = (rms_db(seg(m, sr, 0.2, 0.45)), rms_db(seg(m, sr, 0.56, 0.95)), clean(st))
    drop = res["choke"][0] - res["choke"][1]
    kept = res["open"][0] - res["open"][1]
    T.record(4, "choke group: a hit in the group silences the ringing pad", res["choke"][2] and drop > 60 and abs(kept) < 1.0,
             {"choked_drop_db": round(drop, 1), "unchoked_drop_db": round(kept, 2)})

    # The second retrigger lands 132 cycles after the first (in phase): poly stacks +6 dB, mono keeps one voice.
    out = {}
    for mode in (1, 0):
        wav, st = render(f"retrigger_mode{mode}", INIT + ["--pad", "1=" + fx("tone_440.wav"), "--events", "36@0.1-0.9,36@0.4-0.9",
                                                          "--seconds", "1.0"] + sets(["p01_route=2", f"p01_mode={mode}"]))
        m, sr = load_mono(wav)
        out[mode] = (rms_db(seg(m, sr, 0.45, 0.55)) - rms_db(seg(m, sr, 0.2, 0.3)), clean(st))
    # A retrigger at the old voice's peak (a quarter cycle later): a hard cut would step; the 3 ms fade must not. The
    # new hit rendered alone is subtracted, leaving the old voice's ending; its second difference shows any step (a
    # cut is about 300x the sine's own curvature).
    wav, st = render("retrigger_click", INIT + ["--pad", "1=" + fx("tone_440.wav"), "--events", "36@0.1-0.9,36@0.400568-0.9",
                                                "--seconds", "1.0"] + sets(["p01_route=2"]))
    alone, _ = render("retrigger_second_alone", INIT + ["--pad", "1=" + fx("tone_440.wav"), "--events", "36@0.400568-0.9",
                                                         "--seconds", "1.0"] + sets(["p01_route=2"]))
    m, sr = load_mono(wav)
    m = m - load_mono(alone)[0]
    d2 = np.abs(np.diff(m, 2))
    click = float(seg(d2, sr, 0.399, 0.406).max() / max(1e-12, seg(d2, sr, 0.2, 0.3).max()))
    ok = abs(out[1][0]) < 1.0 and 5.0 < out[0][0] < 7.0 and click < 10 and out[1][1] and out[0][1] and clean(st)
    T.record(4, "mono retrigger replaces the voice without a click; poly stacks (+6 dB in phase)", ok,
             {"mono_change_db": round(out[1][0], 2), "poly_change_db": round(out[0][0], 2), "retrigger_curvature_ratio": round(click, 2)})

    wav, st = render("gate_mode", INIT + ["--pad", "1=" + fx("tone_1000_long.wav"), "--events", "36@0.1-0.3", "--seconds", "0.7"]
                     + sets(["p01_route=2", "p01_mode=2"]))
    m, sr = load_mono(wav)
    held, after = rms_db(seg(m, sr, 0.15, 0.28)), rms_db(seg(m, sr, 0.33, 0.6))
    T.record(4, "gate mode stops the pad at note-off", clean(st) and held > -30 and after < -80,
             {"held_db": round(held, 1), "after_release_db": round(after, 1)})


def step_render(name, pattern, sr=48000, block=64, seconds=2.74, extra=()):
    return render(name, INIT + ["--sr", sr, "--block", block, "--bpm", BPM, "--pad", "1=" + fx("click.wav"), "--set", "p01_route=2",
                                "--pattern", pattern, "--seconds", seconds] + list(extra))


def timing_errors(wav, expected):
    m, sr = load_mono(wav)
    measured = [first_above(m, sr, t) for t in expected]
    if any(v is None for v in measured):
        return None, sr
    return [1000.0 * (a - b) for a, b in zip(measured, expected)], sr


def t05_step_timing(T):
    expected = [k * STEP for k in range(32)]
    for sr in (44100, 48000, 96000):
        wav, st = step_render(f"timing_{sr}", pat(["p1:xxxxxxxxxxxxxxxx"]), sr=sr)
        errors, _ = timing_errors(wav, expected)
        worst = max(abs(e) for e in errors) if errors else 99.0
        T.record(5, f"1/16 steps at 174 BPM on time at {sr} Hz, 64-sample blocks", clean(st) and worst <= 0.05 and st.get("pad_triggers") == "32",
                 {"worst_error_ms": round(worst, 4), "triggers": st.get("pad_triggers")})


def t06_swing(T):
    for swing in (0.5, 1.0):
        expected = [(k + (0.5 * swing if k % 2 else 0.0)) * STEP for k in range(32)]
        wav, st = step_render(f"swing_{swing}", pat(["p1:xxxxxxxxxxxxxxxx"]), extra=sets([f"swing={swing}"]))
        errors, _ = timing_errors(wav, expected)
        worst = max(abs(e) for e in errors) if errors else 99.0
        T.record(6, f"swing {swing}: odd steps {50 * swing:.0f} % of a step late", clean(st) and worst <= 0.05, {"worst_error_ms": round(worst, 4)})


def t07_ratchet_nudge(T):
    bar = [0.0, 0.25, 0.5, 0.75, 4.5, 7.7, 12.0]
    expected = [(s + 16 * b) * STEP for b in range(2) for s in bar]
    wav, st = step_render("ratchet_nudge", pat(["p1:4...x...x...x...", "p1.nudge:....5...c......."]))
    errors, _ = timing_errors(wav, expected)
    worst = max(abs(e) for e in errors) if errors else 99.0
    T.record(7, "ratchet x4 splits the step; nudge +50 % and -30 % move the hit", clean(st) and worst <= 0.05 and st.get("pad_triggers") == "14",
             {"worst_error_ms": round(worst, 4), "triggers": st.get("pad_triggers")})


def t08_probability(T):
    bars = 24
    for char, p in (("5", 0.5), ("2", 0.2)):
        wav, st = step_render(f"probability_{p}", pat(["p1:xxxxxxxxxxxxxxxx", "p1.prob:" + char * 16]), block=512,
                              seconds=(bars * 16 - 0.5) * STEP)
        m, sr = load_mono(wav)
        peak = np.abs(m).max()
        fired = np.zeros((bars, 16), dtype=int)
        for b in range(bars):
            for k in range(16):
                t = (16 * b + k) * STEP
                fired[b, k] = 1 if np.abs(seg(m, sr, t - 0.0005, t + 0.003)).max() > 0.2 * peak else 0
        n = bars * 16
        total = int(fired.sum())
        sd = math.sqrt(n * p * (1 - p))
        per_step = fired.sum(axis=0)
        varied = int(np.sum((per_step > 0) & (per_step < bars)))
        ok = clean(st) and abs(total - n * p) <= 3.5 * sd and varied >= (16 if p == 0.5 else 15) and int(st.get("pad_triggers", "-1")) == total
        T.record(8, f"probability {int(p * 100)} %: hit count in the binomial band, varies bar to bar", ok,
                 {"hits": total, "expected": n * p, "band": round(3.5 * sd, 1), "steps_that_vary": varied, "triggers": st.get("pad_triggers")})


def t09_host_jump(T):
    pattern = pat(["p1:x.......x......."])
    cases = (("forward", "2.0:2.5", 4.2, [0.0, 2 * BEAT, 4 * BEAT, 2.0 + 1.5 * BEAT, 2.0 + 3.5 * BEAT, 2.0 + 5.5 * BEAT]),
             ("loop back", "2.0:0", 4.3, [0.0, 2 * BEAT, 4 * BEAT, 2.0, 2.0 + 2 * BEAT, 2.0 + 4 * BEAT, 2.0 + 6 * BEAT]))
    for label, jump, seconds, expected in cases:
        wav, st = step_render("jump_" + label.replace(" ", "_"), pattern, seconds=seconds, extra=["--jump", jump])
        m, sr = load_mono(wav)
        found = detect_onsets(m, sr, rel=0.25, min_gap=0.05)
        ok, worst = match_times(found, expected, 0.0001)
        T.record(9, f"host jump ({label}): the sequencer follows the new position", clean(st) and ok,
                 {"onsets_s": [round(t, 5) for t in found], "expected_s": [round(t, 5) for t in expected]})


def t10_midi(T):
    wav, st = render("midi_pads", INIT + ["--pad", "5=" + fx("click.wav"), "--pad", "16=" + fx("click.wav"),
                                          "--events", "40@0.1-0.2:127,41@0.3-0.4:127,51@0.5-0.6:40", "--seconds", "0.7"]
                     + sets(["p05_route=2", "p16_route=2"]))
    m, sr = load_mono(wav)
    found = detect_onsets(m, sr, rel=0.02, min_gap=0.05)
    ok, _ = match_times(found, [0.1, 0.5], 0.0001)
    soft = db(np.abs(seg(m, sr, 0.49, 0.52)).max()) - db(np.abs(seg(m, sr, 0.09, 0.12)).max())
    T.record(10, "MIDI notes 36-51 play pads 1-16 (empty pad silent), velocity scales the hit", clean(st) and ok and soft < -6,
             {"onsets_s": [round(t, 5) for t in found], "velocity_40_vs_127_db": round(soft, 1)})

    wav, st = render("midi_pattern", INIT + ["--bpm", BPM, "--pad", "1=" + fx("click.wav"), "--set", "p01_route=2",
                                             "--pat", "0:" + pat(["p1:x..............."]), "--pat", "1:" + pat(["p1:x...x...x...x..."]),
                                             "--events", "97@0.5-0.6", "--seconds", "2.6"])
    m, sr = load_mono(wav)
    found = detect_onsets(m, sr, rel=0.25, min_gap=0.05)
    expected = [0.0, 4 * BEAT, 5 * BEAT, 6 * BEAT, 7 * BEAT]
    ok, _ = match_times(found, expected, 0.0001)
    T.record(10, "MIDI note 97 queues pattern 2, which starts at the next bar", clean(st) and ok, {"onsets_s": [round(t, 5) for t in found]})

    out, _ = run(["--analyze", fx("slicing/click_loop.wav")])
    pos = [int(v) for v in parse_stats(out).get("slice_pos", "").split(",") if v]
    wav, st = render("midi_slice", INIT + ["--break", "1=" + fx("slicing/click_loop.wav"), "--set", "b1_route=2",
                                           "--events", "62@0.2-0.6:127", "--seconds", "0.6"])
    m, sr = load_mono(wav)
    t = first_above(m, sr, 0.2 + 0.001, before=0.003, after=0.01)
    expect = 0.2 + (24000 - pos[2]) / 48000.0 if len(pos) > 2 else -1
    ok = clean(st) and t is not None and abs(t - expect) <= 0.0001
    T.record(10, "MIDI note 62 plays slice 3 of break A", ok, {"onset_s": t, "expected_s": round(expect, 5)})

    common = INIT + ["--bpm", BPM, "--pad", "1=" + fx("snare.wav"), "--pattern", pat(["p1:x...x...x...x..."]), "--seconds", "2.0"]
    _, a = render("midi_cc_off", common)
    _, b = render("midi_cc_on", common + ["--cc", "1:127@0"])
    change = float(b.get("rms_db", "-120")) - float(a.get("rms_db", "-120"))
    T.record(10, "CC1 drives EMISSION", clean(a) and clean(b) and abs(change) > 1.0, {"rms_change_db": round(change, 2)})


def make_index_folder(folder, count=50000):
    marker = folder / "count.txt"
    if marker.exists() and marker.read_text().strip() == str(count):
        return
    if folder.exists():
        shutil.rmtree(folder)
    words = ["kick", "snare", "hat", "openhat", "crash", "ride", "perc", "clap", "tom", "break", "loop", "fx", "vox", "riser", "shot"]
    data = np.zeros(32, dtype=np.int16)
    buf = OUT / "tiny.wav"
    sf.write(str(buf), data, 44100, subtype="PCM_16")
    blob = buf.read_bytes()
    for i in range(count):
        d = folder / f"Pack {i // 1000:02d}" / f"{words[(i // 100) % len(words)]} {i // 100 % 10}"
        if i % 100 == 0:
            d.mkdir(parents=True, exist_ok=True)
        (d / f"{words[i % len(words)]}_{i:05d}.wav").write_bytes(blob)
    marker.write_text(str(count))


def t11_index(T):
    folder = OUT / "index50k"
    make_index_folder(folder)
    out, code = run(["--index-test", folder, "--cache", OUT / "index50k.cache"], timeout=1800)
    st = parse_stats(out)
    ok = (code == 0 and st.get("entries") == "50000" and float(st.get("cache_load_ms", "9e9")) < 1000
          and st.get("cached_entries") == "50000" and float(st.get("rescan_with_cache_ms", "9e9")) < float(st.get("full_ms", "0")))
    T.record(11, "index of 50,000 files; cache reloads in under 1 s", ok,
             {k: st.get(k) for k in ("entries", "folders", "pass1_ms", "full_ms", "cache_load_ms", "rescan_with_cache_ms")})
    if not LIBRARY:
        T.record(11, "index of the real USER library", None, {"skip": "USER library not found"})
        return
    out, code = run(["--index-test", LIBRARY, "--cache", OUT / "user_index.cache"], timeout=3600)
    st = parse_stats(out)
    ok = (code == 0 and int(st.get("entries", "0")) >= 40000 and float(st.get("cache_load_ms", "9e9")) < 1000
          and st.get("cached_entries") == st.get("entries"))
    T.record(11, "index of the real USER library; cache reloads in under 1 s", ok,
             {k: st.get(k) for k in ("entries", "folders", "pass1_ms", "full_ms", "cache_load_ms", "rescan_with_cache_ms", "unsupported", "long_paths",
                                     "with_bpm", "hits")})


def t12_search(T):
    out, code = run(["--search-test", "300000"])
    st = parse_stats(out)
    worst = float(st.get("worst_ms", "9e9"))
    T.record(12, "search 300,000 entries in under 50 ms", code == 0 and worst < 50, {"worst_ms": worst})


def t13_categories(T):
    labels = ROOT / "Tests" / "category_labels.tsv"
    if not labels.exists():
        T.record(13, "categories >= 90 % on 200 hand-labelled names", False, {"error": "Tests/category_labels.tsv missing"})
        return
    out, code = run(["--categorise-file", labels])
    st = parse_stats(out)
    n, acc = int(st.get("labelled", "0")), float(st.get("accuracy", "0"))
    misses = [line.strip() for line in out.splitlines() if line.strip().startswith("miss")][:12]
    T.record(13, "categories >= 90 % on 200 hand-labelled names", code == 0 and n >= 200 and acc >= 90.0,
             {"labelled": n, "accuracy": acc, "misses": misses})


def t14_settings(T):
    out, code = run(["--settings-test"])
    st = parse_stats(out)
    T.record(14, "settings round trip (roots, favourites) and missing-root status", code == 0 and st.get("settings_test") == "PASS",
             {k: st.get(k) for k in ("settings_test", "missing_root_status", "slot_status")})


def t15_kit(T):
    out, code = run(["--check-kit"])
    st = parse_stats(out)
    rows = [line.split("\t") for line in out.splitlines() if line.startswith(("OK\t", "FAIL\t"))]
    if rows and all(r[0] == "FAIL" and "LIBRARY NOT FOUND" in r[2] for r in rows if len(r) > 2) and st.get("kit_ready") == "0":
        T.record(15, "every ZONE KIT entry loads", None, {"skip": "STALKER libraries not found"})
        return
    quiet = [(r[1], r[3]) for r in rows if r[0] == "OK" and len(r) > 3 and float(r[3]) < -40]
    failed = [(r[1], r[2]) for r in rows if r[0] == "FAIL"]
    T.record(15, f"every ZONE KIT entry loads ({st.get('kit_entries')}) and is loud enough", code == 0 and not failed and not quiet,
             {"ready": st.get("kit_ready"), "failed": failed[:6], "quieter_than_-40dBFS": quiet[:6]})


def t16_tempo(T):
    out, _ = run(["--analyze", fx("tempo/loop_172_16beats.wav"), "--host-bpm", "174"])
    st = parse_stats(out)
    T.record(16, "tempo from the file name: synthetic loop reads 172 BPM, 16 beats", st.get("bpm") == "172.000" and st.get("beats") == "16.000",
             {"bpm": st.get("bpm"), "beats": st.get("beats"), "source": st.get("source")})
    if not LIBRARY:
        T.record(16, "tempo estimate on >= 200 tagged drum loops", None, {"skip": "USER library not found"})
        return
    tagged, bare = tempo_dataset(LIBRARY)
    data = tagged + bare
    lst = OUT / "tempo_list.tsv"
    lst.write_text("\n".join(f"{p}\t{b}" for p, b in data), encoding="utf-8")
    out, _ = run(["--tempo-batch", lst], timeout=3600)
    lines = [line.split("\t") for line in out.splitlines() if line.count("\t") >= 2]
    modulo = host = 0
    misses = []
    for (path, truth), cols in zip(data, lines):
        if cols[0] == "ERR":
            misses.append((os.path.basename(path), truth, "ERR"))
            continue
        est, octave = float(cols[0]), float(cols[1])
        if est > 0 and min(abs(est * 2 ** k / truth - 1) for k in (-1, 0, 1)) <= 0.02:
            modulo += 1
        else:
            misses.append((os.path.basename(path)[:40], truth, round(est, 1)))
        if abs(octave / truth - 1) <= 0.02:
            host += 1
    n = len(lines)
    ok = n >= 200 and n == len(data) and modulo / n >= 0.9 and host / n >= 0.9
    T.record(16, "tempo estimate within 2 % (modulo octave, and with the host-octave rule) on >= 200 tagged drum loops", ok,
             {"loops": n, "dnb_loops": len(tagged), "rhythm_lab_breaks": len(bare), "modulo_octave_pct": round(100 * modulo / max(1, n), 1),
              "host_octave_pct": round(100 * host / max(1, n), 1), "misses": misses[:8]})


def t17_slicing(T):
    out, _ = run(["--analyze", fx("slicing/click_loop.wav")])
    st = parse_stats(out)
    pos = [int(v) for v in st.get("slice_pos", "").split(",") if v]
    onsets = [round(float(v) * 48000) for v in (FIX / "slicing/click_loop_onsets.txt").read_text().split(",")]
    exact = len(pos) == len(onsets) and all(-96 <= p - o <= 0 for p, o in zip(pos, onsets))
    T.record(17, "click loop: one slice per click, each within 2 ms before it", exact,
             {"slices": pos, "onsets": onsets})
    out, _ = run(["--analyze", fx("tempo/pips_172bpm.wav")])
    count = int(parse_stats(out).get("slices", "0"))
    T.record(17, "tone pips: one slice per pip, none at the pip ends", count == 16, {"slices": count})

    if not LIBRARY:
        T.record(17, "slices agree with the ReCycle slice points of the rhythm-lab REX2 files", None, {"skip": "USER library not found"})
        return
    pairs = rex_pairs(LIBRARY)
    if not pairs:
        T.record(17, "slices agree with the ReCycle slice points of the rhythm-lab REX2 files", None, {"skip": "no REX2/WAV pairs found"})
        return
    ours = slice_batch([w for _, w in pairs], 0.5)
    ref_total = det_total = matched = 0
    offsets = []
    for rx, wav in pairs:
        got = ours.get(os.path.normcase(str(wav)))
        if got is None:
            continue
        wsr, det = got
        rsr, ref = rex_slices(rx)
        ref = [r * wsr / rsr for r in ref]
        tol = 0.010 * wsr
        used = set()
        for d in det:
            best = None
            for k, r in enumerate(ref):
                if k not in used and abs(d - r) <= tol and (best is None or abs(d - r) < abs(d - ref[best])):
                    best = k
            if best is not None:
                used.add(best)
                matched += 1
                offsets.append((d - ref[best]) * 1000 / wsr)
        ref_total += len(ref)
        det_total += len(det)
    recall = matched / max(1, ref_total)
    precision = matched / max(1, det_total)
    tight = float(np.mean([(-3.0 <= o <= 2.0) for o in offsets])) if offsets else 0.0
    T.record(17, "default slices agree with the ReCycle slice points of the rhythm-lab REX2 files (within 10 ms)",
             recall >= 0.75 and precision >= 0.85 and tight >= 0.70,
             {"files": len(pairs), "rex_slices": ref_total, "our_slices": det_total, "recall": round(recall, 3),
              "precision": round(precision, 3), "within_-3_+2_ms": round(tight, 3), "median_offset_ms": round(float(np.median(offsets)), 2)})
    amen = next((w for _, w in pairs if w.name.lower() == "cw_amen01_175.wav"), None)
    if amen is not None:
        counts = [len(slice_batch([amen], s).get(os.path.normcase(str(amen)), (0, []))[1]) for s in (0.2, 0.5, 0.9)]
        T.record(17, "SENS: more sensitivity, more slices (cw_amen01_175)", counts[0] < counts[1] < counts[2], {"slices_at_0.2_0.5_0.9": counts})


def pip_track(wav, beat, count=15):
    m, sr = load_mono(wav)
    onsets, freqs = [], []
    for k in range(count):
        t = first_above(m, sr, k * beat, before=0.003, after=0.012)
        if t is None:
            return None, None
        onsets.append(t)
        freqs.append(an.dominant_frequency(seg(m, sr, t + 0.01, t + 0.12), sr, 200))
    return onsets, freqs


def t18_break_fit(T):
    pips = fx("tempo/pips_172bpm.wav")
    base = INIT + ["--bpm", BPM, "--break", "1=" + pips, "--set", "b1_route=2", "--seconds", "5.6"]
    # SLICE plays each slice from its start, 1 ms before the hit (the first slice starts at the file start): 1.2 ms allowed.
    cases = (("REPITCH", ["b1_fit=0"], pat(["b1:x"], 64), 1000.0 * BPM / 172.0, 0.0005, 2.0),
             ("SLICE", ["b1_fit=1"], pat(["b1:" + "x" * 64], 64), 1000.0, 0.0012, 2.0),
             ("STRETCH", ["b1_fit=2"], pat(["b1:x"], 64), 1000.0, 0.015, 3.0),
             ("SLICE +12 st", ["b1_fit=1", "b1_tune=12"], pat(["b1:" + "x" * 64], 64), 2000.0, 0.0012, 4.0))
    for label, extra, pattern, f_expect, t_tol, f_tol in cases:
        wav, st = render("fit_" + label.split()[0].lower() + ("_tune" if "+12" in label else ""), base + sets(extra) + ["--pattern", pattern])
        onsets, freqs = pip_track(wav, BEAT)
        if onsets is None:
            T.record(18, f"{label}: pips on the 174 BPM grid at {f_expect:.1f} Hz", False, {"error": "pips not found", "slot": st.get("slot16")})
            continue
        drift = max(abs((t - onsets[0]) - k * BEAT) for k, t in enumerate(onsets))
        f_err = max(abs(f - f_expect) for f in freqs)
        T.record(18, f"{label}: pips on the 174 BPM grid at {f_expect:.1f} Hz", clean(st) and drift <= t_tol and f_err <= f_tol,
                 {"worst_grid_error_ms": round(1000 * drift, 3), "freq_range_hz": [round(min(freqs), 2), round(max(freqs), 2)]})


ANOMALY_BPM = 120.0
SPQ = 60.0 / ANOMALY_BPM     # seconds per quarter at the anomaly tests' tempo


def anomaly_render(name, t_row=None, v_row=None):
    rows = ["p1:x"]
    if t_row:
        rows.append("t:" + t_row)
    if v_row:
        rows.append("v:" + v_row)
    wav, st = render(name, INIT + ["--bpm", ANOMALY_BPM, "--pad", "1=" + fx("sweep.wav"), "--pattern", pat(rows, 64),
                                   "--seconds", "4.0", "--set", "master_volume=-12"])
    m, sr = load_mono(wav)
    return m, sr, st


def warp_snr(out, control, sr, t0, positions, skip):
    """SNR of the engine's output against the control read at the model's source positions (seconds after t0)."""
    e = np.arange(len(positions)) / sr
    src = t0 + np.asarray(positions)
    expected = np.interp(src * sr, np.arange(len(control)), control)
    got = out[int(round(t0 * sr)):int(round(t0 * sr)) + len(positions)]
    keep = np.ones(len(positions), dtype=bool)
    for s in skip:
        keep &= ~((e >= s) & (e < s + 0.003))
    resid = got[keep] - expected[keep]
    return 10 * math.log10(float(np.sum(expected[keep] ** 2)) / max(1e-30, float(np.sum(resid ** 2))))


def t19_anomaly(T):
    control, sr, cst = anomaly_render("anomaly_control")
    t0 = 4 * SPQ                              # step 16 at 1/16
    l16, l4 = SPQ / 4, SPQ
    pad16 = "." * 16
    cases = [
        ("Springboard 1/16", pad16 + "ssss", 1 * SPQ, lambda e: np.fmod(e, l16), [k * l16 for k in range(4)]),
        ("Springboard 1/4", pad16 + "qqqqqqqq", 2 * SPQ, lambda e: np.fmod(e, l4), [0.0, l4]),
        ("Vortex reverse", pad16 + "vvvvvvvv", 2 * SPQ, lambda e: -np.fmod(e, 2 * SPQ), [0.0]),
        ("Jelly half speed", pad16 + "jjjjjjjj", 2 * SPQ, lambda e: 0.5 * e, [0.0]),
        # Compared while the tape still runs at least half speed: slowed further, the sweep falls so low that the
        # master DC blocker's longer low-frequency delay (about 70 samples at 20 Hz) separates engine and model.
        ("Comet tape stop", pad16 + "cccccccc", 1.0 * SPQ, lambda e: e - e * e / (2 * 2 * SPQ), [0.0]),
    ]
    for label, row, span, model, jumps in cases:
        out, sr, st = anomaly_render("anomaly_" + label.split()[0].lower() + "_" + label.split()[1].replace("/", ""), t_row=row)
        e = np.arange(int(span * sr)) / sr
        snr = warp_snr(out, control, sr, t0, model(e), jumps)
        live = rms_db(seg(out, sr, 0.5, t0 - 0.01) - seg(control, sr, 0.5, t0 - 0.01))
        T.record(19, f"TIME {label}: reads the past exactly as modelled", clean(st) and snr >= 30 and live < -80,
                 {"snr_db": round(snr, 1), "before_region_diff_db": round(live, 1)})

    for label, row in (("Whirligig 1/8", "wwwwwwww"), ("Burner roll", "bbbbbbbb"), ("Bubble stutter", "oooooooo")):
        out, sr, st = anomaly_render("anomaly_" + label.split()[0].lower(), t_row=pad16 + row)
        diff = rms_db(seg(out, sr, t0 + 0.05, t0 + 0.95) - seg(control, sr, t0 + 0.05, t0 + 0.95)) - rms_db(seg(control, sr, t0 + 0.05, t0 + 0.95))
        after = rms_db(seg(out, sr, t0 + 1.2, 3.9) - seg(control, sr, t0 + 1.2, 3.9))
        T.record(19, f"TIME {label}: changes its region, then back to live", clean(st) and diff > -20 and after < -60,
                 {"region_change_db": round(diff, 1), "after_region_diff_db": round(after, 1)})

    # Levels of the differenced signal: a gate or mute cuts the sweep mid-cycle, and the master DC blocker then settles
    # for tens of milliseconds at a few hertz, which is not signal.
    dc = np.diff(control)
    ref = rms_db(seg(dc, sr, t0, t0 + 2 * SPQ))
    out, sr, st = anomaly_render("anomaly_mute", v_row=pad16 + "mmmm")
    muted = rms_db(seg(np.diff(out), sr, t0 + 0.005, t0 + SPQ - 0.005))
    T.record(19, "VOLUME mute silences its region", clean(st) and muted < ref - 60, {"muted_db": round(muted, 1), "control_db": round(ref, 1)})
    out, sr, st = anomaly_render("anomaly_gate16", v_row=pad16 + "ssssssss")
    do = np.diff(out)
    on = [rms_db(seg(do, sr, t0 + k * l16 + 0.002, t0 + (k + 0.5) * l16 - 0.002)) for k in range(8)]
    off = [rms_db(seg(do, sr, t0 + (k + 0.5) * l16 + 0.002, t0 + (k + 1) * l16 - 0.002)) for k in range(8)]
    T.record(19, "VOLUME gate 1/16: open halves at full level, closed halves silent", clean(st) and min(on) > ref - 3 and max(off) < ref - 40,
             {"open_db": round(min(on), 1), "closed_db": round(max(off), 1), "control_db": round(ref, 1)})
    out, sr, st = anomaly_render("anomaly_fadeout", v_row=pad16 + "oooooooo")
    first, last = rms_db(seg(np.diff(out), sr, t0, t0 + 0.1)), rms_db(seg(np.diff(out), sr, t0 + 0.85, t0 + 0.98))
    T.record(19, "VOLUME fade out falls across its region", clean(st) and first - last > 20, {"start_db": round(first, 1), "end_db": round(last, 1)})


def t20_buffer(T):
    out, code = run(["--selftest-buffer"])
    T.record(20, "history buffer: exact reads across the wrap-around, nothing past the end", code == 0, {"output": out.strip()})


def t21_multiband(T):
    noise = INIT + ["--pad", "1=" + fx("noise.wav"), "--events", "36@0.0-4.0", "--seconds", "4.0", "--set", "master_volume=-12"]
    dry, sr = load_mono(render("mb_dry", noise + sets(["p01_route=2"]))[0])
    ref = third_octave_levels(seg(dry, sr, 0.2, 3.8), sr)
    for label, extra in (("120 Hz / 2.5 kHz", []), ("80 Hz / 6 kHz", ["mb_low_xover=80", "mb_high_xover=6000"])):
        wav, st = render("mb_flat_" + label.split()[0], noise + sets(["mb_depth=1", "mb_up=0", "mb_down=0"] + extra))
        x, sr = load_mono(wav)
        worst = float(np.abs(third_octave_levels(seg(x, sr, 0.2, 3.8), sr) - ref).max())
        T.record(21, f"multiband split {label}: bands sum flat within 0.2 dB", clean(st) and worst <= 0.2, {"worst_band_db": round(worst, 3)})
    tone = INIT + ["--pad", "1=" + fx("tone_1000.wav"), "--events", "36@0.0-2.0", "--seconds", "2.0", "--set", "master_volume=-12"]
    base, sr = load_mono(render("mb_loud_base", tone)[0])
    wav, st = render("mb_down", tone + sets(["mb_depth=1", "mb_up=0", "mb_down=1"]))
    down = rms_db(seg(load_mono(wav)[0], sr, 0.5, 1.8)) - rms_db(seg(base, sr, 0.5, 1.8))
    qbase, sr = load_mono(render("mb_quiet_base", tone + sets(["p01_level=-48"]))[0])
    wav2, st2 = render("mb_up", tone + sets(["p01_level=-48", "mb_depth=1", "mb_up=1", "mb_down=0"]))
    up = rms_db(seg(load_mono(wav2)[0], sr, 0.5, 1.8)) - rms_db(seg(qbase, sr, 0.5, 1.8))
    T.record(21, "multiband: DOWN pulls a loud band down, UP lifts a quiet one (>= 3 dB each)", clean(st) and clean(st2) and down <= -3 and up >= 3,
             {"downward_db": round(down, 2), "upward_db": round(up, 2)})


def t22_slam_clip(T):
    tone = INIT + ["--pad", "1=" + fx("tone_1000.wav"), "--events", "36@0.0-2.0", "--seconds", "2.0", "--set", "master_volume=-12"]
    base, sr = load_mono(render("slam_base", tone)[0])
    def harmonics_db(x):
        s = seg(x, sr, 0.3, 1.8)
        return 10 * math.log10(sum(10 ** (tone_amp_db(s, sr, 1000 * h, 0, len(s) / sr) / 10) for h in (2, 3, 4, 5))
                               / 10 ** (tone_amp_db(s, sr, 1000, 0, len(s) / sr) / 10))
    clean_h = harmonics_db(base)
    wav, st = render("slam_bark", tone + sets(["slam_bark=1"]))
    bark_h = harmonics_db(load_mono(wav)[0])
    T.record(22, "SLAM BARK adds harmonics (>= 20 dB more than clean)", clean(st) and bark_h - clean_h >= 20,
             {"clean_harmonics_db": round(clean_h, 1), "bark_harmonics_db": round(bark_h, 1)})

    # BITE is a 12:1 compressor from -30 dB with a 2 ms attack: 20 dB of input level becomes a couple of dB, while a
    # drum's first milliseconds pass (which is the punch). CLIP squashes the peaks themselves.
    levels = {}
    for lv in (0, -20):
        wav, st = render(f"slam_bite_{-lv}", tone + sets(["slam_bite=1", f"p01_level={lv}"]))
        levels[lv] = (rms_db(seg(load_mono(wav)[0], sr, 0.5, 1.8)), clean(st))
    spread = levels[0][0] - levels[-20][0]
    T.record(22, "SLAM BITE compresses: 20 dB of input level comes out within 6 dB", levels[0][1] and levels[-20][1] and spread <= 6,
             {"output_difference_db": round(spread, 2)})
    snare = INIT + ["--pad", "1=" + fx("snare.wav"), "--events", "36@0.1-0.5:127", "--seconds", "0.5", "--set", "master_volume=-12"]
    s0, sr = load_mono(render("slam_snare_base", snare)[0])
    wc, sc = render("slam_clip", snare + sets(["clip_drive=18"]))
    c0, cc = crest_db(seg(s0, sr, 0.1, 0.25)), crest_db(seg(load_mono(wc)[0], sr, 0.1, 0.25))
    T.record(22, "CLIP squashes the snare's peaks (crest factor down >= 6 dB)", clean(sc) and c0 - cc >= 6,
             {"crest_clean_db": round(c0, 1), "crest_clip_db": round(cc, 1)})

    loud = INIT + ["--pad", "1=" + fx("noise.wav"), "--events", "36@0.0-3.0:127", "--seconds", "3.0"]
    for ceiling in (-0.3, -6.0):
        wav, st = render(f"ceiling_{ceiling}", loud + sets(["p01_level=6", "clip_drive=18", "slam_bark=1", "master_volume=6", f"limit_ceiling={ceiling}"]))
        peak = float(st.get("peak_db", "9"))
        T.record(22, f"output never passes the ceiling ({ceiling} dBFS) when driven hard", clean(st) and peak <= ceiling + 0.05,
                 {"peak_db": peak})

    lh = INIT + ["--pad", "1=" + fx("lowhigh.wav"), "--events", "36@0.0-2.0", "--seconds", "2.0", "--set", "master_volume=-12"]
    plain, sr = load_mono(render("subsafe_clean", lh)[0])
    res = {}
    for ss, label in ((3, "80 Hz"), (0, "Off")):
        wav, st = render(f"subsafe_{ss}", lh + sets([f"sub_safe={ss}", "slam_bark=1", "slam_bite=1", "clip_drive=12"]))
        res[label] = (tone_amp_db(load_mono(wav)[0], sr, 40, 0.3, 1.8) - tone_amp_db(plain, sr, 40, 0.3, 1.8), clean(st))
    T.record(22, "SUB SAFE 80 Hz: a 40 Hz sub stays within 0.1 dB under full SLAM and CLIP", res["80 Hz"][1] and abs(res["80 Hz"][0]) <= 0.1
             and abs(res["Off"][0]) >= 1.0, {"sub_change_db_80Hz": round(res["80 Hz"][0], 3), "sub_change_db_off": round(res["Off"][0], 2)})

    for sr in (44100, 48000, 96000):
        wav, st = render(f"latency_{sr}", INIT + ["--sr", sr, "--pad", "1=" + fx("click.wav"), "--events", "36@0.1-0.2:127",
                                                   "--seconds", "0.3"] + sets(["slam_bark=0.5", "clip_drive=6"]))
        m, fs = load_mono(wav)
        t = first_above(m, fs, 0.1, before=0.002, after=0.004)
        err = (t - math.floor(0.1 * fs) / fs) * fs if t is not None else 99
        T.record(22, f"reported latency lines the punch chain up with the event at {sr} Hz", clean(st) and abs(err) <= 3,
                 {"latency": st.get("latency"), "error_samples": err})


def t23_transient(T):
    snare = INIT + ["--pad", "1=" + fx("snare.wav"), "--events", "36@0.1-0.5:110", "--seconds", "0.5", "--set", "master_volume=-12"]
    res = {}
    for label, extra in (("flat", []), ("attack+", ["tr_attack=1"]), ("attack-", ["tr_attack=-1"]), ("sustain+", ["tr_sustain=1"])):
        wav, st = render("transient_" + label.replace("+", "_up").replace("-", "_down"), snare + sets(extra))
        x, sr = load_mono(wav)
        res[label] = (crest_db(seg(x, sr, 0.1, 0.2)), rms_db(seg(x, sr, 0.16, 0.35)), clean(st))
    ok = (all(r[2] for r in res.values()) and res["attack+"][0] - res["flat"][0] >= 3 and res["attack-"][0] < res["flat"][0] - 1
          and res["sustain+"][1] - res["flat"][1] >= 2)
    T.record(23, "TRANSIENT: ATTACK raises the crest factor >= 3 dB (and lowers it), SUSTAIN lifts the tail >= 2 dB", ok,
             {k: {"crest_db": round(v[0], 1), "tail_db": round(v[1], 1)} for k, v in res.items()})


def t24_editor(T):
    png = OUT / "editor.png"
    if png.exists():
        png.unlink()
    _, st = render("editor", ["--preset", "2", "--bpm", BPM, "--seconds", "0.6", "--png", png])
    T.record(24, "editor renders at 1400 x 920", png.exists() and st.get("size") == "1400x920" and st["exit"] == 0,
             {"png": str(png), "size": st.get("size")})


def t25_presets(T, presets):
    failures, loud = [], {}
    for i, name in enumerate(presets):
        wav, st = render(f"preset_{i:02d}", ["--preset", i, "--bpm", "172", "--seconds", "8"])
        data, sr = load(wav)
        level = an.lufs(data, sr, momentary_max=True)
        loud[name] = round(level, 1)
        if not clean(st) or float(st.get("peak_db", "9")) > -0.2 or not (-14.0 <= level <= -8.0):
            failures.append((name, st.get("peak_db"), round(level, 1), st.get("faults")))
    T.record(25, f"all {len(presets)} presets render clean at -14..-8 LUFS momentary max", not failures,
             {"failures": failures, "momentary_lufs": loud})


def t26_state_fuzz_cpu(T, fuzz_seconds, presets):
    _, st = render("roundtrip", ["--preset", "5", "--seconds", "0.3", "--roundtrip", "--break", "2=" + fx("tempo/pips_172bpm.wav") + "@1.5",
                                 "--pat", "3:" + pat(["p2:x.x.", "t:..ss", "v:m..."], 32)] + sets(["swing=0.3", "p03_tune=5", "b2_fit=2", "emission=0.4"]))
    T.record(26, "state round trip: parameters, samples (with window offset) and patterns", st.get("roundtrip") == "PASS",
             {"roundtrip": st.get("roundtrip"), "state_bytes": st.get("state_bytes")})
    _, st = render("fuzz", ["--fuzz", "--seconds", fuzz_seconds, "--seed", "7", "--vary-blocks"], timeout=7200)
    ok = st["exit"] == 0 and int(st.get("non_finite", "1")) == 0 and int(st.get("growth_idle", "1")) == 0
    T.record(26, f"fuzz {int(fuzz_seconds)} s: notes, slices, presets, parameters, pattern edits, variable blocks", ok,
             {k: st.get(k) for k in ("events", "preset_changes", "param_changes", "pattern_edits", "non_finite", "growth_idle",
                                     "worst_growth_db", "peak_db", "faults", "block_max_ms")})
    heavy = presets.index("Garbage Run") if "Garbage Run" in presets else 0
    _, st = render("cpu", ["--preset", heavy, "--bpm", BPM, "--seconds", "20", "--block", "256"]
                   + sets(["emission=1", "b1_fit=2", "b2_fit=2", "b3_fit=2", "b4_fit=2", "mb_depth=1", "slam_bark=0.6", "slam_bite=0.6"]))
    rt = float(st.get("cpu_realtime_x", st.get("realtime_x", "0")))
    T.record(26, "CPU: heaviest kit, every break stretched, EMISSION full, >= 10x real time on one core", clean(st) and rt >= 10,
             {"cpu_realtime_x": rt, "block_max_ms": st.get("block_max_ms")})


def t27_pluginval(T):
    pluginval = shutil.which("pluginval") or r"F:\_DEV\tools\pluginval\pluginval.exe"
    if not BUNDLE.exists():
        T.record(27, "pluginval strictness 10", False, {"error": f"plugin not built: {BUNDLE}"})
        return
    proc = subprocess.run([pluginval, "--strictness-level", "10", "--validate-in-process", "--timeout-ms", "300000", "--validate", str(BUNDLE)],
                          capture_output=True, text=True, timeout=3600, encoding="utf-8", errors="replace")
    (ROOT / "build" / "pluginval-level10.log").write_text(proc.stdout + proc.stderr, encoding="utf-8")
    T.record(27, "pluginval strictness 10", proc.returncode == 0, {"exit": proc.returncode, "log": "build/pluginval-level10.log"})


def t28_guard(T):
    log = OUT / "guard_faults.log"
    if log.exists():
        log.unlink()
    bad = []
    for stage in (6, 0, 1, 2, 3, 4, 5):
        wav, st = render(f"guard_{stage}", ["--preset", "1", "--bpm", BPM, "--seconds", "3", "--inject", f"{stage}@1.5", "--fault-log", log])
        data, sr = load(wav)
        before = float(np.abs(data[:int(1.45 * sr)]).max())
        after = float(np.abs(data[int(1.45 * sr):]).max())
        if st["exit"] != 0 or int(st.get("faults", "0")) < 1 or int(st.get("non_finite", "1")) != 0 or after > max(before * 1.05, 0.97):
            bad.append((stage, st.get("faults"), round(db(after), 1)))
    text = log.read_text(encoding="utf-8") if log.exists() else ""
    stages = sorted(set(re.findall(r"FAULT\s+(\w+)", text)))
    T.record(28, "runaway guard: a blowout at any stage becomes silence, a FAULT and a log line", not bad and len(stages) >= 6,
             {"failures": bad, "logged_stages": stages})


def t29_idle(T, presets):
    log = OUT / "idle_faults.log"
    if log.exists():
        log.unlink()
    worst = []
    for name in ("Spatial Bubble", "Brain Scorcher Bus"):
        if name not in presets:
            continue
        _, st = render("idle_" + name.split()[0].lower(), ["--preset", presets.index(name), "--bpm", "130.1", "--block", "64",
                                                         "--seconds", "240", "--fault-log", log], timeout=3600)
        if not clean(st):
            worst.append((name, st.get("faults"), st.get("non_finite"), st.get("peak_db")))
    _, st = render("idle_stopped", ["--preset", "0", "--block", "64", "--seconds", "120", "--tail-from", "2", "--fault-log", log], timeout=3600)
    quiet = float(st.get("tail_peak_db", "0")) < -90
    T.record(29, "240 s with the transport playing at 64-sample blocks, and 120 s stopped: no faults, silence when stopped",
             not worst and clean(st) and quiet, {"failures": worst, "stopped_tail_peak_db": st.get("tail_peak_db")})


def switch_bar(name, bpm, sr, block, note_bars, bars=8, jump=None):
    """Pattern 1 hits once per bar, pattern 2 twice; MIDI note 97 (pattern 2) at note_bars bars from the start.
    Returns the first bar (1-based) with two hits, and the hits per bar."""
    bar = 240.0 / bpm
    t = note_bars * bar
    extra = ["--jump", jump] if jump else []
    wav, st = render(name, INIT + ["--pad", "1=" + fx("click.wav"), "--set", "p01_route=2",
                                   "--pat", "0:" + pat(["p1:x..............."]), "--pat", "1:" + pat(["p1:x.......x......."]),
                                   "--events", f"97@{t:.6f}-{t + 0.01:.6f}:100", "--bpm", bpm, "--sr", sr, "--block", block,
                                   "--seconds", f"{bars * bar:.6f}"] + extra)
    m, fs = load_mono(wav)
    hits = detect_onsets(m, fs, rel=0.25, min_gap=0.05)
    per_bar = [sum(1 for x in hits if k * bar - 0.002 <= x < (k + 1) * bar - 0.002) for k in range(bars)]
    first = next((k + 1 for k, n in enumerate(per_bar) if n == 2), None)
    return first, per_bar, clean(st)


def t30_pattern_switch(T):
    """A queued pattern starts at the first bar line at or after its note, at tempos whose bar lines fall exactly on
    block edges (at 48 kHz and 64-sample blocks: 90, 100, 120, 125, 144, 150, 160, 180 BPM) and at tempos where they
    do not. 0.1.0 switched late, or never, when the accumulated block position rounded low at such an edge."""
    positions = [(0.5, 2), (1.0, 2), (1.5, 3), (2.0, 3), (3.0, 4)]   # (note position in bars, expected bar, 1-based)
    cases = [(bpm, 48000, block) for bpm in (90, 100, 120, 125, 144, 150, 160, 170, 172, 174, 175, 180) for block in (64, 512)]
    cases += [(bpm, 44100, 64) for bpm in (105, 125, 147, 175)]
    wrong, total = [], 0
    for bpm, sr, block in cases:
        bar = 240.0 / bpm
        for pos, expected in positions + [(2.0 + 0.002 / bar, 4)]:   # last: a note 2 ms after a bar line waits for the next
            first, per_bar, ok = switch_bar("switch", bpm, sr, block, pos)
            total += 1
            if first != expected or not ok:
                wrong.append((bpm, sr, block, round(pos, 4), expected, first))
    T.record(30, f"queued pattern switches on the right bar ({total} cases: 16 tempos, 2 rates, 2 block sizes)", not wrong,
             {"wrong": wrong[:10], "cases": total})
    loops = []
    for bpm, block in ((120, 64), (120, 512), (174, 64)):
        bar = 240.0 / bpm
        first, per_bar, ok = switch_bar("switch_loop", bpm, 48000, block, 1.5, bars=6, jump=f"{2 * bar:.6f}:0")
        if first != 3 or not ok:
            loops.append((bpm, block, first, per_bar))
    T.record(30, "a pattern queued before the host loops back starts at the loop start", not loops, {"wrong": loops})


def t31_outputs(T):
    """Per-lane outputs (0.2.0): a lane set to output k leaves on that stereo bus, aligned with the main output to the
    sample, carrying its own level and pan, before ANOMALY and PUNCH; with only the main output it plays there."""
    clicks = [k * 4 * STEP for k in range(4)]
    snares = [(2 + 4 * k) * STEP for k in range(4)]
    common = INIT + ["--bpm", BPM, "--pad", "1=" + fx("click.wav"), "--pad", "2=" + fx("snare.wav"), "--set", "p01_route=2",
                     "--set", "p02_route=2", "--pattern", pat(["p1:x...x...x...x...", "p2:..x...x...x...x."]), "--seconds", "1.35"]
    prefix = OUT / "outputs"
    for old in OUT.glob("outputs_out*.wav"):
        old.unlink()
    wav, st = render("outputs_main", common + ["--set", "p01_out=1", "--multi-out", prefix])
    main, sr = load_mono(wav)
    aux_file = pathlib.Path(str(prefix) + "_out2.wav")
    aux, _ = load_mono(aux_file) if aux_file.exists() else (np.zeros(10), sr)
    on_aux = detect_onsets(aux, sr, rel=0.25, min_gap=0.05)
    on_main = detect_onsets(main, sr, rel=0.25, min_gap=0.05)
    ok_aux, worst_aux = match_times(on_aux, clicks, 1.01 / sr)
    ok_main, worst_main = match_times(on_main, snares, 1.01 / sr)
    others = sorted(OUT.glob("outputs_out*.wav"))
    T.record(31, "a pad on output 2 leaves there, on the sample; the main output keeps the rest", clean(st) and ok_aux and ok_main
             and st.get("outputs") == "21" and [f.name for f in others] == ["outputs_out2.wav"],
             {"outputs": st.get("outputs"), "aux_onsets_s": [round(t, 5) for t in on_aux], "main_onsets_s": [round(t, 5) for t in on_main],
              "files": [f.name for f in others]})

    wav, st = render("outputs_fallback", common + ["--set", "p01_out=1"])
    m, sr = load_mono(wav)
    both = detect_onsets(m, sr, rel=0.25, min_gap=0.05)
    ok, _ = match_times(both, sorted(clicks + snares), 1.01 / sr)
    T.record(31, "with only the main output enabled, a lane set to output 2 plays on the main output", clean(st) and ok,
             {"onsets_s": [round(t, 5) for t in both]})

    wav, st = render("outputs_level", common + ["--set", "p01_out=1", "--set", "p01_level=-12", "--multi-out", OUT / "outputs_level"])
    quiet_peak = float(st.get("out2_peak_db", "0"))
    _, st0 = render("outputs_level0", common + ["--set", "p01_out=1", "--multi-out", OUT / "outputs_level0"])
    drop = float(st0.get("out2_peak_db", "0")) - quiet_peak
    T.record(31, "the lane's own level applies on its output (-12 dB)", abs(drop - 12.0) < 0.1, {"drop_db": round(drop, 2)})

    # A VOLUME mute over the whole bar silences the main output's FULL bus, not a lane on its own output.
    muted = ["--set", "p02_route=0", "--pat", "0:" + pat(["p1:x...x...x...x...", "p2:..x...x...x...x.", "v:mmmmmmmmmmmmmmmm"])]
    wav, st = render("outputs_anomaly", INIT + ["--bpm", BPM, "--pad", "1=" + fx("click.wav"), "--pad", "2=" + fx("snare.wav"),
                                                "--set", "p01_route=2", "--set", "p01_out=1", "--seconds", "1.35",
                                                "--multi-out", OUT / "outputs_anomaly"] + muted)
    m, sr = load_mono(wav)
    a, _ = load_mono(pathlib.Path(str(OUT / "outputs_anomaly") + "_out2.wav"))
    main_level = rms_db(seg(m, sr, 0.05, 1.3))
    ok_clicks, _ = match_times(detect_onsets(a, sr, rel=0.25, min_gap=0.05), clicks, 1.01 / sr)
    T.record(31, "ANOMALY acts on the main output only: a muted bar leaves the lane on output 2 playing", clean(st) and ok_clicks
             and main_level < -80, {"main_rms_db": round(main_level, 1)})


def t32_slices_regions(T):
    """Hand-placed slices (0.2.0) replace the automatic ones everywhere slices are used, and survive the state; a sample
    slot can hold one region of a file (offset and length), as a slice exploded to a pad does."""
    loop = fx("slicing/click_loop.wav")
    base = INIT + ["--break", "1=" + loop, "--set", "b1_route=2", "--events", "61@0.2-0.9:127", "--seconds", "0.9"]
    _, st_a = render("slices_auto", base)
    wav, st = render("slices_custom", base + ["--slices", "1=0,23952,35952,59952,71952"])
    m, sr = load_mono(wav)
    found = detect_onsets(m, sr, rel=0.25, min_gap=0.05)
    expected = [0.2 + (24000 - 23952) / 48000, 0.2 + (30000 - 23952) / 48000, 0.2 + (36000 - 23952) / 48000, 0.2 + (48000 - 23952) / 48000]
    ok, _ = match_times(found, expected, 1.01 / sr)
    T.record(32, "MIDI note 61 plays the hand-placed slice 2 (sample 23952), not the automatic one", clean(st) and clean(st_a) and ok,
             {"onsets_s": [round(t, 4) for t in found], "expected_s": [round(t, 4) for t in expected]})

    wav, st = render("region_pad", INIT + ["--pad", "1=" + loop + "@0.5,0.1", "--set", "p01_route=2", "--events", "36@0.1-0.8:127",
                                            "--seconds", "0.8"])
    m, sr = load_mono(wav)
    found = detect_onsets(m, sr, rel=0.25, min_gap=0.05)
    after = rms_db(seg(np.diff(m), sr, 0.21, 0.79))
    T.record(32, "a pad holding a region (0.5 s + 0.1 s of the click loop) plays only that region", clean(st) and len(found) == 1
             and abs(found[0] - 0.1) <= 1.01 / sr and after < -90 and "0.10 S" in st.get("slot0", ""),
             {"onsets_s": [round(t, 4) for t in found], "after_region_db": round(after, 1), "slot": st.get("slot0")})

    _, st = render("slices_roundtrip", ["--preset", "3", "--seconds", "0.2", "--roundtrip", "--slices", "2=0,5000,9000",
                                        "--pad", "5=" + loop + "@0.5,0.1"])
    T.record(32, "state round trip keeps hand-placed slices and regions", st.get("roundtrip") == "PASS",
             {"roundtrip": st.get("roundtrip"), "state_bytes": st.get("state_bytes")})


def t33_explode(T):
    """EXPLODE (0.2.0): a break's components become pads holding regions of the break, and a new pattern plays the
    break back on them, on time (nudges) and at its levels (velocity, pad level)."""
    loop = fx("slicing/click_loop.wav")
    clicks = [float(v) for v in (FIX / "slicing/click_loop_onsets.txt").read_text().split(",")]
    wav, st = render("explode_clicks", INIT + ["--bpm", "120", "--break", "1=" + loop, "--set", "b1_route=2", "--explode", "1",
                                               "--seconds", "1.99"])
    m, sr = load_mono(wav)
    found = detect_onsets(m, sr, rel=0.2, min_gap=0.05)
    ok, worst = match_times(found, clicks, 0.0015)
    T.record(33, "the exploded click loop plays its 10 clicks back at their times (within 1.5 ms) from pattern 2",
             clean(st) and st.get("explode_ok") == "1" and st.get("explode_pattern") == "2" and ok,
             {"explode": st.get("explode"), "worst_ms": round(1000 * worst, 2) if worst is not None else None,
              "onsets": len(found)})

    _, st = render("explode_every", INIT + ["--bpm", "120", "--break", "1=" + loop, "--explode", "1", "--every-slice",
                                            "--seconds", "0.5"])
    T.record(33, "every-slice mode gives each of the 10 slices its own pad", st.get("explode", "").startswith("10 HITS -> 10 PADS"),
             {"explode": st.get("explode")})

    amen = LIBRARY / "rhythm-lab.com_amen_vol.1" / "rhythm-lab.com_amen_vol.1" / "WAV" / "cw_amen01_175.wav" if LIBRARY else None
    if amen is None or not amen.exists():
        T.record(33, "the exploded amen plays back like the break", None, {"skip": "USER library not found"})
        return
    common = INIT + ["--bpm", "175", "--break", "1=" + str(amen), "--set", "b1_route=2", "--seconds", "2.74"]
    orig, _ = render("explode_amen_orig", common + ["--pattern", pat(["b1:x"], 32)])
    wav, st = render("explode_amen", common + ["--explode", "1"])
    a, sr = load_mono(orig)
    b, _ = load_mono(wav)
    oa = detect_onsets(a, sr, rel=0.15, min_gap=0.04)
    ob = detect_onsets(b, sr, rel=0.15, min_gap=0.04)
    errors = [1000 * min(abs(x - y) for y in ob) for x in oa] if ob else [99.0]
    level = rms_db(b) - rms_db(a)
    ok = (clean(st) and st.get("explode_components") == "KICKx5,SNAREx4,SNAREx5,HATx6" and abs(len(oa) - len(ob)) <= 2
          and float(np.median(errors)) < 1.0 and abs(level) < 2.0)
    T.record(33, "the amen explodes into kick, snare, ghost snare and ride and plays back like the break at 175 BPM", ok,
             {"components": st.get("explode_components"), "onsets": [len(oa), len(ob)], "median_ms": round(float(np.median(errors)), 2),
              "worst_ms": round(max(errors), 2), "level_db": round(level, 2)})


def midi_note_ons(path):
    """(tick, note, velocity, channel 1-16) of every note-on in a standard MIDI file, and its ticks per quarter."""
    data = pathlib.Path(path).read_bytes()
    if data[:4] != b"MThd":
        return [], 0
    division = struct.unpack(">H", data[12:14])[0]
    pos, notes = 14, []
    while pos + 8 <= len(data):
        kind, size = data[pos:pos + 4], struct.unpack(">I", data[pos + 4:pos + 8])[0]
        body, pos = data[pos + 8:pos + 8 + size], pos + 8 + size
        if kind != b"MTrk":
            continue
        i, tick, status = 0, 0, 0
        while i < len(body):
            delta = 0
            while True:
                b = body[i]
                i += 1
                delta = (delta << 7) | (b & 0x7F)
                if b < 0x80:
                    break
            tick += delta
            if body[i] >= 0x80:
                status = body[i]
                i += 1
            if status == 0xFF:
                i += 1
                length = 0
                while True:
                    b = body[i]
                    i += 1
                    length = (length << 7) | (b & 0x7F)
                    if b < 0x80:
                        break
                i += length
            elif status in (0xF0, 0xF7):
                length = body[i]
                i += 1 + length
            else:
                kind_ = status & 0xF0
                if kind_ in (0xC0, 0xD0):
                    i += 1
                else:
                    a, b = body[i], body[i + 1]
                    i += 2
                    if kind_ == 0x90 and b > 0:
                        notes.append((tick, a, b, (status & 0x0F) + 1))
    return notes, division


def t34_render(T):
    """RENDER (0.2.0): a pattern rendered by a second engine is what the engine plays; stems sum to the mix and are as
    long as it; the MIDI file holds the hits as played."""
    common = INIT + ["--bpm", BPM, "--pad", "1=" + fx("click.wav"), "--pad", "2=" + fx("snare.wav"),
                     "--pattern", pat(["p1:x...x...x...x...", "p2:..x...x...x...x."])]
    folder = OUT / "render_stems"
    if folder.exists():
        shutil.rmtree(folder)
    _, st = render("render_cmd", common + ["--render", folder, "--render-what", "1", "--render-passes", "2"])
    files = sorted(f.name for f in folder.glob("*")) if folder.exists() else []
    expected = ["Pattern 01 mix.wav", "Pattern 01 pad 01 click.wav", "Pattern 01 pad 02 snare.wav", "Pattern 01.mid"]
    wav, _ = render("render_direct", common + ["--seconds", "2.75"])
    direct, sr = load(wav)
    if files != expected:
        T.record(34, "a mix-and-stems render writes the mix, one stem per lane and a MIDI file", False, {"files": files, "out": st.get("render")})
        return
    mix, _ = load(folder / "Pattern 01 mix.wav")
    a, _ = load(folder / "Pattern 01 pad 01 click.wav")
    b, _ = load(folder / "Pattern 01 pad 02 snare.wav")
    n = min(len(mix), len(direct))
    same = float(np.abs(mix[:n] - direct[:n]).max())
    stems = float(np.abs(a + b - mix).max())
    T.record(34, "the rendered mix is what the engine plays (direct render, 24-bit) and the stems sum to it", same < 3e-7 and stems < 3e-7
             and len(a) == len(b) == len(mix), {"mix_vs_direct": same, "stems_vs_mix": stems, "lengths": [len(mix), len(a), len(b)],
                                                "body_s": round(8 * BEAT, 4), "file_s": round(len(mix) / sr, 4)})

    notes, division = midi_note_ons(folder / "Pattern 01.mid")
    times = sorted((round(t / division * 60.0 / BPM, 4), note) for t, note, *_ in notes)
    want = sorted([(round(k * 4 * STEP, 4), 36) for k in range(8)] + [(round((2 + 4 * k) * STEP, 4), 37) for k in range(8)])
    worst = max((abs(x[0] - y[0]) for x, y in zip(times, want)), default=1.0)
    T.record(34, "the MIDI file holds every hit as played: pads on notes 36 and 37 at their steps", len(times) == len(want)
             and all(x[1] == y[1] for x, y in zip(times, want)) and worst < 0.0005,
             {"notes": len(times), "worst_ms": round(1000 * worst, 3), "division": division})

    one = OUT / "render_lane"
    if one.exists():
        shutil.rmtree(one)
    render("render_lane_cmd", common + ["--render", one, "--render-what", "2", "--render-lane", "2"])
    lane_files = sorted(f.name for f in one.glob("*.wav")) if one.exists() else []
    ok = lane_files == ["Pattern 01 pad 02 snare.wav"]
    if ok:
        x, sr = load_mono(one / lane_files[0])
        # The snare is a noise burst: its onset reads a few samples after the hit, and it can cross the threshold twice.
        found = detect_onsets(x, sr, rel=0.25, min_gap=0.1)
        ok, _ = match_times(found, [(2 + 4 * k) * STEP for k in range(4)], 0.00025)
    T.record(34, "a selected-lane render holds that lane only", ok, {"files": lane_files})


def midi_on_channel_one(src, dst):
    """A copy of a MIDI file with every note on channel 1, as LAB X-0 0.2.0 wrote them."""
    data = bytearray(pathlib.Path(src).read_bytes())
    pos = 14
    while pos + 8 <= len(data):
        kind, size = bytes(data[pos:pos + 4]), struct.unpack(">I", data[pos + 4:pos + 8])[0]
        start, end, pos = pos + 8, pos + 8 + size, pos + 8 + size
        if kind != b"MTrk":
            continue
        i, status = start, 0
        while i < end:
            while data[i] & 0x80:
                i += 1
            i += 1
            if data[i] >= 0x80:
                status = data[i]
                if status & 0xE0 == 0x80:
                    data[i] = status & 0xF0
                i += 1
            if status == 0xFF:
                i += 1
                length = 0
                while True:
                    b = data[i]
                    i += 1
                    length = (length << 7) | (b & 0x7F)
                    if b < 0x80:
                        break
                i += length
            elif status & 0xF0 in (0xC0, 0xD0):
                i += 1
            else:
                i += 2
    pathlib.Path(dst).write_bytes(bytes(data))


def t35_midi_roundtrip(T):
    """RENDER's MIDI file (0.2.1) plays back into LAB X-0 as the render: pads on channel 1 for their gate, breaks A-D
    on channels 2-5 as slice notes held until their next start, and what a note cannot hold is named."""
    kit = INIT + ["--bpm", "120", "--pad", "1=" + fx("click.wav"), "--pad", "2=" + fx("snare.wav"), "--set", "p02_mode=2",
                  "--break", "1=" + fx("slicing/click_loop.wav"), "--set", "b1_fit=1",
                  "--break", "2=" + fx("sweep.wav"), "--slices", "2=0,24000,48000,72000,96000,120000,144000,168000"]
    # Pad 2 gated with a ratchet; break A natural on its slices; break B's explicit slices (on a sweep, so a wrong
    # slice or break cannot pass). At 120 BPM and 48 kHz a tick is 25 samples, so the file can be exact.
    rows = ["p1:x...x...x...x...", "p2:..x...x...x..3..", "p2.vel:..5...9...7..9..", "b1:x.x.x..xx...x...",
            "b2:..3.1...5...2.0."]
    folder = OUT / "render_midi"
    if folder.exists():
        shutil.rmtree(folder)
    _, st = render("render_midi_cmd", kit + ["--pattern", pat(rows), "--render", folder, "--render-passes", "2"])
    mid = folder / "Pattern 01.mid"
    notes, division = midi_note_ons(mid) if mid.exists() else ([], 0)
    by_channel = {c: sorted(n for _, n, _, ch in notes if ch == c) for c in (1, 2, 3)}
    ok = (st.get("render_midi", "").startswith("MIDI: PADS ON CHANNEL 1") and division == 960
          and sorted({ch for *_, ch in notes}) == [1, 2, 3] and set(by_channel[3]) == {60, 61, 62, 63, 65})
    T.record(35, "the MIDI file puts pads on channel 1 and break B's slices on channel 3 (notes 60 and up)", ok,
             {"render_midi": st.get("render_midi"), "channels": {c: len(v) for c, v in by_channel.items()}, "b_notes": sorted(set(by_channel[3]))})
    if not mid.exists():
        return

    mix, _ = load(folder / "Pattern 01 mix.wav")
    wav, st2 = render("render_midi_replay", kit + ["--midi-file", mid, "--seconds", "6"])
    replay, _ = load(wav)
    # The whole file, tails included: the render's transport stops with the last pass, which releases the breaks as
    # the file's note-offs there do (0.2.2).
    n = len(mix)
    diff = float(np.abs(mix[:n] - replay[:n]).max())
    after = rms_db(replay[n:]) if len(replay) > n else -240.0
    T.record(35, "played into LAB X-0 with no pattern, the file gives the rendered mix to 24-bit precision, tails included",
             clean(st2) and diff < 3e-7 and after < -90, {"max_diff": diff, "samples": n, "after_mix_db": round(after, 1),
                                                        "events": st2.get("midi_file_events")})

    old = OUT / "render_midi_ch1.mid"
    midi_on_channel_one(mid, old)
    wav, _ = render("render_midi_replay_ch1", kit + ["--midi-file", old, "--seconds", "4.25"])
    replay_old, _ = load(wav)
    miss = float(np.abs(mix[:n] - replay_old[:n]).max())
    T.record(35, "control: the same notes all on channel 1 (0.2.0's file) play break B's slices from break A", miss > 0.1,
             {"max_diff": round(miss, 3)})

    approx = OUT / "render_midi_approx"
    if approx.exists():
        shutil.rmtree(approx)
    _, st3 = render("render_midi_approx_cmd", INIT + ["--bpm", "120", "--pad", "1=" + fx("click.wav"),
                                                     "--break", "1=" + fx("slicing/click_loop.wav"),
                                                     "--pattern", pat(["p1:x...x...", "p1.pitch: 0 . . . 7", "b1:2..x"]),
                                                     "--render", approx])
    notes3, _ = midi_note_ons(approx / "Pattern 01.mid") if (approx / "Pattern 01.mid").exists() else ([], 0)
    want = "MIDI APPROXIMATE: 1 BREAK HIT STARTS BETWEEN SLICES (MOVED TO THE NEAREST), 1 HIT WITH STEP PITCH PLAYS UNPITCHED"
    T.record(35, "a natural break start between slices and a pitched step are named on the status line, and still written",
             st3.get("render_midi") == want and sorted(t for t, _, _, ch in notes3 if ch == 2) == [0, 720],
             {"render_midi": st3.get("render_midi"), "break_ticks": sorted(t for t, _, _, ch in notes3 if ch == 2)})


def t36_stop_releases(T):
    """Stopping the transport (0.2.2) releases the Legato breaks the step sequencer holds, as FL's note-offs release the
    ones MIDI holds; pads ring out as before, and RUN on Free keeps playing. Found by the fuzz session: a long break
    played on for seconds after the stop."""
    common = INIT + ["--bpm", "120", "--break", "1=" + fx("sweep.wav"), "--pattern", pat(["b1:x"]), "--seconds", "3"]
    wav, st = render("stop_break", common + ["--stop-at", "1.0"])
    m, sr = load_mono(wav)
    playing = rms_db(m[int(0.5 * sr):int(0.9 * sr)])
    stopped = rms_db(m[int(1.1 * sr):])
    T.record(36, "a break the sequencer started stops within 0.1 s of the transport stopping (a 30 ms release)",
             clean(st) and playing > -30 and stopped < -90, {"playing_db": round(playing, 1), "after_stop_db": round(stopped, 1)})

    wav, st = render("stop_pad", INIT + ["--bpm", "120", "--pad", "1=" + fx("tone_1000_long.wav"), "--pattern", pat(["p1:x"]),
                                        "--seconds", "3", "--stop-at", "1.0"])
    m, sr = load_mono(wav)
    ringing = rms_db(m[int(1.5 * sr):int(2.5 * sr)])
    T.record(36, "a pad still rings out after the stop", clean(st) and ringing > -30, {"after_stop_db": round(ringing, 1)})

    wav, st = render("stop_free", INIT + ["--break", "1=" + fx("sweep.wav"), "--pattern", pat(["b1:x"]), "--free", "--seconds", "2"])
    m, sr = load_mono(wav)
    level = rms_db(m[int(1.0 * sr):])
    T.record(36, "with RUN on Free (no transport) the break plays on", clean(st) and level > -30, {"level_db": round(level, 1)})


# ----------------------------------------------------------------------------------------------------------------------

class Runner:
    def __init__(self):
        self.report = {"tests": [], "started": time.strftime("%Y-%m-%d %H:%M:%S")}

    def record(self, number, name, ok, detail):
        status = "SKIP" if ok is None else ("PASS" if ok else "FAIL")
        self.report["tests"].append({"number": number, "name": name, "status": status, **detail})
        print(f"{status}  {number:2d} {name}  {json.dumps(detail, default=str)[:420]}", flush=True)


def main():
    global LIBRARY
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", help="comma-separated test numbers")
    parser.add_argument("--skip-pluginval", action="store_true")
    parser.add_argument("--fuzz-seconds", type=float, default=300.0)
    parser.add_argument("--library", default=DEFAULT_LIBRARY, help="the USER library (FL Studio legacy packs)")
    args = parser.parse_args()

    if not HARNESS.exists():
        print(f"harness not built: {HARNESS}")
        return 2
    if not (FIX / "tempo" / "pips_172bpm.wav").exists():
        subprocess.run([sys.executable, "-I", str(ROOT / "Tests" / "make_fixtures.py")], check=True)
    OUT.mkdir(parents=True, exist_ok=True)
    LIBRARY = pathlib.Path(args.library) if args.library and os.path.isdir(args.library) else None

    only = {int(x) for x in args.only.split(",")} if args.only else None
    presets = [line.split("\t")[1] for line in run(["--list"])[0].splitlines() if "\t" in line]
    T = Runner()
    tests = [(1, t01_formats), (2, t02_bad_files), (3, t03_burst), (4, t04_choke_mono_gate), (5, t05_step_timing), (6, t06_swing),
             (7, t07_ratchet_nudge), (8, t08_probability), (9, t09_host_jump), (10, t10_midi), (11, t11_index), (12, t12_search),
             (13, t13_categories), (14, t14_settings), (15, t15_kit), (16, t16_tempo), (17, t17_slicing), (18, t18_break_fit),
             (19, t19_anomaly), (20, t20_buffer), (21, t21_multiband), (22, t22_slam_clip), (23, t23_transient), (24, t24_editor),
             (25, lambda t: t25_presets(t, presets)), (26, lambda t: t26_state_fuzz_cpu(t, args.fuzz_seconds, presets)),
             (27, t27_pluginval), (28, t28_guard), (29, lambda t: t29_idle(t, presets)), (30, t30_pattern_switch),
             (31, t31_outputs), (32, t32_slices_regions), (33, t33_explode), (34, t34_render), (35, t35_midi_roundtrip), (36, t36_stop_releases)]
    for number, fn in tests:
        if only is not None and number not in only:
            continue
        if number == 27 and args.skip_pluginval:
            continue
        try:
            fn(T)
        except Exception as e:   # one broken check must not hide the others
            T.record(number, "test raised an exception", False, {"error": repr(e), "trace": traceback.format_exc()[-600:]})

    results = T.report["tests"]
    passed = sum(r["status"] == "PASS" for r in results)
    failed = sum(r["status"] == "FAIL" for r in results)
    skipped = sum(r["status"] == "SKIP" for r in results)
    print(f"\n{passed} passed, {failed} failed, {skipped} skipped ({len(results)} checks)")
    (ROOT / "build").mkdir(exist_ok=True)
    (ROOT / "build" / "test-report.json").write_text(json.dumps(T.report, indent=2, default=str), encoding="utf-8")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
