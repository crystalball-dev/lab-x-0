"""Spectral, level and pitch analysis for WAV files rendered by LabX0Render.

Usage:
    python Tests/analyze.py file.wav [file2.wav ...] [--pitch HZ] [--json]

Prints, per file: peak and RMS in dBFS, DC offset, dominant frequency, the share of energy in
the bands used for the reference analysis and the largest sample-to-sample jump. The functions are
also imported by run_tests.py for the detailed checks.
"""

import argparse
import json
import math
import sys

import numpy as np
import soundfile as sf

BANDS = [("sub<60", 20, 60), ("60-250", 60, 250), ("250-1k", 250, 1000),
         ("1k-4k", 1000, 4000), ("4k-10k", 4000, 10000), (">10k", 10000, 22000)]


def db(x, floor=-120.0):
    return 20.0 * math.log10(x) if x > 0 else floor


def load(path):
    data, sr = sf.read(path, always_2d=True)
    data = np.where(np.isfinite(data), data, 0.0)
    return data, sr


def mono(data):
    return data.mean(axis=1)


def rms_db(x):
    return db(math.sqrt(float(np.mean(np.asarray(x) ** 2)) + 1e-30))


def segment(x, sr, start, end):
    return x[int(start * sr):int(end * sr)]


def dominant_frequency(x, sr, fmin=20.0):
    n = 1 << int(math.floor(math.log2(min(len(x), 1 << 18))))
    seg = x[:n] * np.hanning(n)
    spec = np.abs(np.fft.rfft(seg))
    freqs = np.fft.rfftfreq(n, 1.0 / sr)
    lo = int(fmin * n / sr)
    k = lo + int(np.argmax(spec[lo:]))
    if 0 < k < len(spec) - 1:
        a, b, c = np.log(spec[k - 1] + 1e-12), np.log(spec[k] + 1e-12), np.log(spec[k + 1] + 1e-12)
        shift = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) != 0 else 0.0
        return (k + shift) * sr / n
    return freqs[k]


def power_spectrum(x, sr, n=8192):
    """Welch-averaged power spectrum with a Hann window."""
    if len(x) < n:
        n = 1 << int(math.floor(math.log2(max(len(x), 64))))
    win = np.hanning(n)
    acc = np.zeros(n // 2 + 1)
    count = 0
    for i in range(0, len(x) - n + 1, n // 2):
        acc += np.abs(np.fft.rfft(x[i:i + n] * win)) ** 2
        count += 1
    return np.fft.rfftfreq(n, 1.0 / sr), acc / max(count, 1)


def band_split(x, sr):
    freqs, p = power_spectrum(x, sr)
    total = p.sum() + 1e-20
    return {name: round(10 * math.log10(p[(freqs >= lo) & (freqs < hi)].sum() / total + 1e-20), 1)
            for name, lo, hi in BANDS}


def band_fraction(x, sr, lo, hi):
    freqs, p = power_spectrum(x, sr)
    return float(p[(freqs >= lo) & (freqs < hi)].sum() / (p.sum() + 1e-30))


def band_energy(x, sr, lo, hi):
    freqs, p = power_spectrum(x, sr)
    return float(p[(freqs >= lo) & (freqs < hi)].sum())


def harmonic_amplitudes(x, sr, f0, count=10):
    """Peak amplitude near each harmonic of f0 from one long Hann-windowed FFT."""
    n = 1 << int(math.ceil(math.log2(len(x))))
    spec = np.abs(np.fft.rfft(x * np.hanning(len(x)), n))
    res = sr / n
    amps = []
    for k in range(1, count + 1):
        centre = k * f0 / res
        lo, hi = int(centre - 3), int(centre + 4)
        if hi >= len(spec):
            break
        amps.append(float(spec[lo:hi].max()))
    return amps


def thd(x, sr, f0, count=10):
    a = harmonic_amplitudes(x, sr, f0, count)
    return math.sqrt(sum(v * v for v in a[1:])) / (a[0] + 1e-30)


def slope_db_per_octave(filtered, dry, sr, f_lo, f_hi):
    freqs, pf = power_spectrum(filtered, sr)
    _, pd = power_spectrum(dry, sr)
    sel = (freqs >= f_lo) & (freqs <= f_hi)
    ratio = 10 * np.log10(pf[sel] / (pd[sel] + 1e-30) + 1e-30)
    octaves = np.log2(freqs[sel])
    slope, _ = np.polyfit(octaves, ratio, 1)
    return float(slope)


def response_peak(filtered, dry, sr, f_lo, f_hi):
    freqs, pf = power_spectrum(filtered, sr)
    _, pd = power_spectrum(dry, sr)
    sel = (freqs >= f_lo) & (freqs <= f_hi)
    ratio = pf[sel] / (pd[sel] + 1e-30)
    return float(freqs[sel][int(np.argmax(ratio))])


def zero_crossing_track(x, sr):
    """Times and frequencies from successive positive-going zero crossings (one value per cycle)."""
    s = np.asarray(x)
    idx = np.where((s[:-1] <= 0) & (s[1:] > 0))[0]
    if len(idx) < 3:
        return np.array([]), np.array([])
    frac = s[idx] / (s[idx] - s[idx + 1])
    t = (idx + frac) / sr
    periods = np.diff(t)
    return t[1:], 1.0 / periods


def envelope(x, sr, frame_s=0.005):
    frame = max(1, int(sr * frame_s))
    usable = len(x) // frame * frame
    return np.sqrt((np.asarray(x[:usable]).reshape(-1, frame) ** 2).mean(axis=1))


def autocorr_period(env, frame_s, min_s, max_s):
    e = env - env.mean()
    ac = np.correlate(e, e, mode="full")[len(e) - 1:]
    lo, hi = int(min_s / frame_s), int(max_s / frame_s)
    k = lo + int(np.argmax(ac[lo:hi]))
    if 0 < k < len(ac) - 1:
        a, b, c = ac[k - 1], ac[k], ac[k + 1]
        d = a - 2 * b + c
        k = k + (0.5 * (a - c) / d if d != 0 else 0.0)
    return k * frame_s


def inharmonic_ratio(x, sr, f0, tolerance_hz=12.0):
    """Share of spectral energy that is not within tolerance of a harmonic of f0."""
    n = 1 << int(math.floor(math.log2(len(x))))
    spec = np.abs(np.fft.rfft(x[:n] * np.hanning(n))) ** 2
    freqs = np.fft.rfftfreq(n, 1.0 / sr)
    k = np.round(freqs / f0)
    near = np.abs(freqs - k * f0) <= tolerance_hz
    near &= k >= 1
    sel = freqs >= 20.0
    return float(spec[sel & ~near].sum() / (spec[sel].sum() + 1e-30))


def centroid_track(x, sr, frame=2048, hop=240):
    win = np.hanning(frame)
    freqs = np.fft.rfftfreq(frame, 1.0 / sr)
    out = []
    for i in range(0, len(x) - frame, hop):
        p = np.abs(np.fft.rfft(x[i:i + frame] * win)) ** 2
        out.append(float((freqs * p).sum() / (p.sum() + 1e-30)))
    return np.array(out), hop / sr


def modulation_frequency(track, step_s, fmin=0.5, fmax=20.0):
    t = track - track.mean()
    n = 1 << int(math.ceil(math.log2(len(t) * 4)))
    spec = np.abs(np.fft.rfft(t * np.hanning(len(t)), n))
    freqs = np.fft.rfftfreq(n, step_s)
    sel = (freqs >= fmin) & (freqs <= fmax)
    k = int(np.argmax(np.where(sel, spec, 0)))
    if 0 < k < len(spec) - 1:
        a, b, c = np.log(spec[k - 1] + 1e-12), np.log(spec[k] + 1e-12), np.log(spec[k + 1] + 1e-12)
        d = a - 2 * b + c
        return float((k + (0.5 * (a - c) / d if d != 0 else 0.0)) * (freqs[1] - freqs[0]))
    return float(freqs[k])


def peak_spread(x, sr, centre, half_width, rel_db=-20.0):
    """Frequency span of the spectral peaks within centre +- half_width that reach rel_db of the largest."""
    n = 1 << int(math.ceil(math.log2(len(x))))
    spec = np.abs(np.fft.rfft(x * np.hanning(len(x)), n))
    freqs = np.fft.rfftfreq(n, 1.0 / sr)
    sel = np.where((freqs >= centre - half_width) & (freqs <= centre + half_width))[0]
    s = spec[sel]
    peaks = [i for i in range(1, len(s) - 1) if s[i] >= s[i - 1] and s[i] >= s[i + 1]]
    if not peaks:
        return 0.0
    top = max(s[i] for i in peaks)
    keep = [freqs[sel[i]] for i in peaks if 20 * math.log10(s[i] / top + 1e-30) >= rel_db]
    return float(max(keep) - min(keep)) if keep else 0.0


def cross_correlation_lag(a, b, max_lag):
    """Lag (samples) that best aligns b to a: positive means b is later."""
    best, best_lag = -1e30, 0
    for lag in range(-max_lag, max_lag + 1):
        if lag >= 0:
            v = float(np.dot(a[:len(a) - lag], b[lag:]))
        else:
            v = float(np.dot(a[-lag:], b[:len(b) + lag]))
        if v > best:
            best, best_lag = v, lag
    return best_lag



def lufs(data, sr, momentary_max=False):
    """Integrated loudness per ITU-R BS.1770-4 (K-weighting, 400 ms blocks, absolute and relative gates).
    momentary_max: the loudest 400 ms block instead, which treats a short stab and a held pad alike."""
    from scipy.signal import bilinear, lfilter
    x = np.atleast_2d(np.asarray(data, dtype=float).T).T if np.asarray(data).ndim == 1 else np.asarray(data, dtype=float)
    # Stage 1: high shelf (+4 dB above ~1.5 kHz). Stage 2: RLB high-pass (~38 Hz). Analog prototypes, bilinear.
    f0, gain_db, q = 1681.974450955533, 3.999843853973347, 0.7071752369554196
    k = math.tan(math.pi * f0 / sr)
    vh, vb = 10 ** (gain_db / 20), 10 ** (gain_db / 40)
    a0 = 1 + k / q + k * k
    b_shelf = [(vh + vb * k / q + k * k) / a0, 2 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0]
    a_shelf = [1.0, 2 * (k * k - 1) / a0, (1 - k / q + k * k) / a0]
    f1, q1 = 38.13547087602444, 0.5003270373238773
    k1 = math.tan(math.pi * f1 / sr)
    a01 = 1 + k1 / q1 + k1 * k1
    b_hp = [1.0, -2.0, 1.0]
    a_hp = [1.0, 2 * (k1 * k1 - 1) / a01, (1 - k1 / q1 + k1 * k1) / a01]
    y = lfilter(b_hp, a_hp, lfilter(b_shelf, a_shelf, x, axis=0), axis=0)
    block, hop = int(0.4 * sr), int(0.1 * sr)
    powers = []
    for i in range(0, len(y) - block + 1, hop):
        powers.append(float(np.sum(np.mean(y[i:i + block] ** 2, axis=0))))
    powers = np.array(powers)
    if len(powers) == 0:
        return -120.0
    loud = -0.691 + 10 * np.log10(powers + 1e-30)
    if momentary_max:
        return float(loud.max())
    gated = powers[loud > -70]
    if len(gated) == 0:
        return -120.0
    rel = -0.691 + 10 * math.log10(gated.mean()) - 10
    gated2 = powers[(loud > -70) & (loud > rel)]
    return -0.691 + 10 * math.log10(gated2.mean() + 1e-30)

def analyse(path, args=None):
    data, sr = sf.read(path, always_2d=True)
    finite = np.isfinite(data)
    result = {"file": path, "sr": sr, "non_finite": int((~finite).sum())}
    data = np.where(finite, data, 0.0)
    m = data.mean(axis=1)

    result["peak_db"] = round(db(np.abs(data).max()), 2)
    result["rms_db"] = round(db(math.sqrt((data ** 2).mean())), 2)
    result["dc"] = round(float(m.mean()), 6)
    result["max_jump"] = round(float(np.abs(np.diff(data, axis=0)).max()), 4)
    result["max_side"] = round(float(np.abs(data[:, 0] - data[:, -1]).max()), 6) if data.shape[1] > 1 else 0.0
    result["dominant_hz"] = round(dominant_frequency(m, sr), 2)
    result["bands_db"] = band_split(m, sr)

    checks = [result["non_finite"] == 0]
    if args is not None and getattr(args, "pitch", None):
        cents = 1200 * math.log2(result["dominant_hz"] / args.pitch)
        result["pitch_error_cents"] = round(cents, 3)
        checks.append(abs(cents) <= 1.0)
    result["pass"] = all(checks)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("files", nargs="+")
    parser.add_argument("--pitch", type=float)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    results = [analyse(f, args) for f in args.files]
    if args.json:
        print(json.dumps(results, indent=2))
    else:
        for r in results:
            extras = f" cents={r['pitch_error_cents']}" if "pitch_error_cents" in r else ""
            bands = " ".join(f"{k}:{v}" for k, v in r["bands_db"].items())
            print(f"{'PASS' if r['pass'] else 'FAIL'} {r['file']} peak={r['peak_db']} rms={r['rms_db']} "
                  f"dc={r['dc']} jump={r['max_jump']} f0={r['dominant_hz']}{extras}\n      bands {bands}")
    sys.exit(0 if all(r["pass"] for r in results) else 1)


if __name__ == "__main__":
    main()
