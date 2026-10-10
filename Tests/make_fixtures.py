"""Synthetic audio fixtures for LAB X-0's tests (written to Tests/fixtures, ignored by git).

Every file is generated here: no sample from any library is copied. Usage: python Tests/make_fixtures.py
"""

import os
import pathlib
import shutil
import subprocess
import sys

import numpy as np
import soundfile as sf

ROOT = pathlib.Path(__file__).resolve().parent.parent
FIX = ROOT / "Tests" / "fixtures"
SR = 48000


def click(sr=SR, ms=3.0, seed=1):
    """A short, sharp burst: easy to time to the sample."""
    n = int(sr * ms / 1000)
    rng = np.random.default_rng(seed)
    x = np.zeros(int(sr * 0.05))
    env = np.exp(-np.arange(n) / (n / 5))
    x[:n] = rng.uniform(-1, 1, n) * env
    x[0] = 1.0
    return x * 0.9


def tone(freq, seconds, sr=SR, amp=0.5):
    t = np.arange(int(sr * seconds)) / sr
    return amp * np.sin(2 * np.pi * freq * t)


def click_loop(onsets, seconds, sr=SR):
    """A loop with clicks at the given times (seconds): the slicer must find exactly these."""
    x = np.zeros(int(sr * seconds))
    c = click(sr, ms=4.0)
    for k, t in enumerate(onsets):
        i = int(round(t * sr))
        seg = click(sr, ms=4.0, seed=k + 2)[:min(len(c), len(x) - i)]
        x[i:i + len(seg)] += seg * (0.6 + 0.4 * ((k * 7) % 3) / 2)
    return x


def write(name, data, sr=SR, subtype=None, fmt=None):
    path = FIX / name
    path.parent.mkdir(parents=True, exist_ok=True)
    sf.write(str(path), data, sr, subtype=subtype, format=fmt)
    return path


def main():
    if FIX.exists():
        shutil.rmtree(FIX)
    FIX.mkdir(parents=True)

    c = click()
    write("click.wav", c, subtype="PCM_24")
    write("tone_440.wav", tone(440.0, 1.0), subtype="FLOAT")
    write("tone_1000.wav", tone(1000.0, 2.0), subtype="FLOAT")

    # Formats: the same tone in every container
    t = tone(220.0, 0.5)
    stereo = np.stack([t, -t * 0.5], axis=1)
    write("formats/pcm16.wav", stereo, subtype="PCM_16")
    write("formats/pcm24.wav", stereo, subtype="PCM_24")
    write("formats/float32.wav", stereo, subtype="FLOAT")
    write("formats/aiff16.aiff", stereo, subtype="PCM_16", fmt="AIFF")
    write("formats/flac16.flac", stereo, subtype="PCM_16")
    write("formats/vorbis.ogg", stereo, subtype="VORBIS")
    write("formats/mono.wav", t, subtype="PCM_16")
    six = np.stack([t * (0.2 + 0.1 * k) for k in range(6)], axis=1)
    write("formats/six_channel.wav", six, subtype="PCM_16")
    try:
        write("formats/mpeg.mp3", stereo, fmt="MP3", subtype="MPEG_LAYER_III")
    except Exception:
        ffmpeg = shutil.which("ffmpeg")
        if ffmpeg:
            subprocess.run([ffmpeg, "-v", "error", "-y", "-i", str(FIX / "formats/pcm16.wav"), str(FIX / "formats/mpeg.mp3")], check=True)

    # Broken files
    bad = FIX / "bad"
    bad.mkdir()
    (bad / "zero.wav").write_bytes(b"")
    full = (FIX / "formats/pcm16.wav").read_bytes()
    (bad / "truncated.wav").write_bytes(full[:60])
    (bad / "text.wav").write_text("this is not audio\n" * 20)
    burst = tone(220.0, 1.0, amp=0.4)
    burst[1000:1100] = 1.0e4      # garbage far above +24 dBFS, as some game files decode
    write("bad/burst.wav", burst, subtype="FLOAT")
    deep = bad
    for k in range(9):
        deep = deep / ("a_very_long_folder_name_to_push_the_path_length_" + str(k))
    try:
        deep.mkdir(parents=True)
        sf.write("\\\\?\\" + str(deep / "long_path_click.wav"), c, SR)
    except Exception as e:
        print("long path not created:", e)

    # Slicing: clicks at known times inside a 2 s loop
    onsets = [0.0, 0.25, 0.5, 0.625, 0.75, 1.0, 1.25, 1.375, 1.5, 1.75]
    write("slicing/click_loop.wav", click_loop(onsets, 2.0), subtype="PCM_24")
    (FIX / "slicing/click_loop_onsets.txt").write_text(",".join(str(o) for o in onsets))

    # Tempo: a 4-bar loop at 172 BPM with a kick-snare-hat pattern, length exactly 16 beats
    bpm = 172.0
    beat = 60.0 / bpm
    loop = np.zeros(int(round(16 * beat * SR)))
    rng = np.random.default_rng(5)
    for step in range(64):
        tt = step * beat / 4
        i = int(round(tt * SR))
        if step % 16 in (0, 10):
            n = int(0.12 * SR)
            k = np.sin(2 * np.pi * 55 * np.arange(n) / SR * (1 + 2 * np.exp(-np.arange(n) / (0.01 * SR)))) * np.exp(-np.arange(n) / (0.05 * SR))
            loop[i:i + n] += 0.8 * k[:len(loop) - i]
        if step % 8 == 4:
            n = int(0.1 * SR)
            s = rng.uniform(-1, 1, n) * np.exp(-np.arange(n) / (0.025 * SR))
            loop[i:i + n] += 0.6 * s[:len(loop) - i]
        if step % 2 == 0:
            n = int(0.02 * SR)
            h = np.diff(rng.uniform(-1, 1, n + 1)) * np.exp(-np.arange(n) / (0.004 * SR))
            loop[i:i + n] += 0.25 * h[:len(loop) - i]
    write("tempo/loop_172_16beats.wav", loop / np.abs(loop).max() * 0.8, subtype="PCM_24")

    # Sub safe and slam: a 40 Hz sine with a 1 kHz sine on top
    lowhigh = tone(40.0, 2.0, amp=0.4) + tone(1000.0, 2.0, amp=0.4)
    write("lowhigh.wav", np.stack([lowhigh, lowhigh], axis=1), subtype="FLOAT")

    # Noise for the multiband's flatness test
    noise = np.random.default_rng(3).normal(0, 0.15, int(SR * 4))
    write("noise.wav", np.stack([noise, noise], axis=1), subtype="FLOAT")

    # Snare-like hit for the transient test: noise burst with a slow body
    n = int(0.3 * SR)
    sn = (np.random.default_rng(9).uniform(-1, 1, n) * (np.exp(-np.arange(n) / (0.004 * SR)) * 0.9 + np.exp(-np.arange(n) / (0.08 * SR)) * 0.5))
    write("snare.wav", sn / np.abs(sn).max() * 0.8, subtype="FLOAT")

    # Anomaly checks: a sweep (every moment has its own frequency) and a long steady tone
    secs = 10.0
    tt = np.arange(int(SR * secs)) / SR
    f0, f1 = 100.0, 2000.0
    phase = 2 * np.pi * f0 * secs / np.log(f1 / f0) * (np.exp(tt / secs * np.log(f1 / f0)) - 1)
    write("sweep.wav", 0.5 * np.sin(phase), subtype="FLOAT")
    write("tone_1000_long.wav", tone(1000.0, 8.0), subtype="FLOAT")

    # Break fitting: 16 beats at 172 BPM, a 1 kHz pip of 150 ms on every beat (downbeats louder)
    beat = 60.0 / 172.0
    pips = np.zeros(int(round(16 * beat * SR)))
    n = int(0.15 * SR)
    ramp = int(0.003 * SR)
    shape = np.ones(n)
    shape[:ramp] = 0.5 - 0.5 * np.cos(np.pi * np.arange(ramp) / ramp)
    shape[-ramp:] = shape[:ramp][::-1]
    pip = np.sin(2 * np.pi * 1000.0 * np.arange(n) / SR) * shape
    for k in range(16):
        i = int(round(k * beat * SR))
        pips[i:i + n] += pip * (0.8 if k % 4 == 0 else 0.5)
    write("tempo/pips_172bpm.wav", pips, subtype="PCM_24")

    print("fixtures written to", FIX)
    return 0


if __name__ == "__main__":
    sys.exit(main())
