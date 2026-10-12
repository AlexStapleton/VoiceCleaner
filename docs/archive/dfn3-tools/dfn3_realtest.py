#!/usr/bin/env python3
"""Real-recording A/B comparison of VoiceCleaner denoiser engines.

Unlike dfn3_compare.py (which scores against a ground-truth reference), these
real K688 recordings have NO clean/noise separation. So we score each engine's
output against the RAW INPUT, using metrics that make sense for real audio:

  A. Keyboard suppression   (from typing_only.wav)  -- the key objective test
  B. Voice preservation     (from speech_only.wav)  -- does it damage the voice?
  C. Real use case          (from speech_typing.wav) -- HF transient reduction

Each processed file is aligned to its input by integer xcorr lag first
(engines add algorithmic delay), then trimmed to common length.

The set of engines compared is the ENGINES table below: each entry names the
per-test output WAV (produced by offline-cli.exe). Missing files are skipped for
that test, so a partial set (e.g. only the aggression sweep) still runs.

Usage:
  python docs/archive/dfn3-tools/dfn3_realtest.py [suffix]
The optional suffix (e.g. "_n" for the +20 dB normalized re-test) is inserted
before ".wav" in every filename. All WAVs live in the repo root.
"""
import os
import sys
import numpy as np

# reuse IO + alignment from the reference comparator
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dfn3_compare import read_mono, best_lag  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EPS = 1e-12

# Optional filename suffix inserted before ".wav" (e.g. "_n" for the
# normalized +20 dB re-test). Set via argv: `python dfn3_realtest.py _n`.
SUFFIX = ""

# ---------------------------------------------------------------- engine table
# Each engine names its output WAV stem for each test (before SUFFIX / .wav).
# Set a stem to None to exclude the engine from that test. Files that are
# absent on disk are silently skipped, so this table can list more than exists.
ENGINES = [
    # label                 typing (test A)      speech (test B)         abtest (C)
    ("rnnoise",            "typing_rnnoise",    "speech_rnnoise",    "abtest_rnnoise"),
    ("dfn3-standard",      "typing_dfn3std",    "speech_dfn3std",    None),
    ("dfn3-ll(whole)",     "typing_dfn3ll",     "speech_dfn3ll",     None),
    ("dfn3-stream(_ll RT)","typing_dfn3stream", "speech_dfn3stream", None),
    ("dfn3 aggr=0.25",     "typing_dfn3a025",   "speech_dfn3a025",   None),
    ("dfn3 aggr=0.50",     "typing_dfn3a050",   "speech_dfn3a050",   None),
    ("dfn3 aggr=0.75",     "typing_dfn3a075",   "speech_dfn3a075",   None),
    ("dfn3 aggr=1.00",     "typing_dfn3a100",   "speech_dfn3a100",   None),
    ("passthrough",        "typing_pass",       "speech_pass",       "abtest_pass"),
]


def wav(name):
    """Insert SUFFIX before the .wav extension, e.g. typing_only -> typing_only_n."""
    base, ext = os.path.splitext(name)
    return os.path.join(ROOT, base + SUFFIX + ext)


def exists(stem):
    return stem is not None and os.path.isfile(wav(stem + ".wav"))


def rms(x):
    return float(np.sqrt(np.mean(x * x))) if len(x) else 0.0


def dbfs(x):
    r = rms(x)
    return 20.0 * np.log10(r + EPS)


def align_to_input(inp, proc):
    """Return (in_aligned, proc_aligned) trimmed to common length."""
    lag = best_lag(inp, proc)
    if lag >= 0:
        i = inp[lag:]
        p = proc[: len(proc) - lag] if lag > 0 else proc
    else:
        i = inp[: len(inp) + lag]
        p = proc[-lag:]
    m = min(len(i), len(p))
    return i[:m], p[:m], lag


def crest_factor(x):
    r = rms(x)
    return float(np.max(np.abs(x)) / (r + EPS)) if len(x) else 0.0


def short_time_levels(x, sr, win_ms=20.0):
    """RMS per non-overlapping short-time frame (linear)."""
    w = int(sr * win_ms / 1000.0)
    n = len(x) // w
    if n == 0:
        return np.array([rms(x)])
    fr = x[: n * w].reshape(n, w)
    return np.sqrt(np.mean(fr * fr, axis=1))


def band_energy(x, sr, lo, hi, nfft=1024, hop=256):
    """Total energy in [lo,hi] Hz summed over all frames."""
    win = np.hanning(nfft)
    freqs = np.fft.rfftfreq(nfft, 1.0 / sr)
    mask = (freqs >= lo) & (freqs <= hi)
    total = 0.0
    for i in range(0, len(x) - nfft, hop):
        sp = np.fft.rfft(x[i:i + nfft] * win)
        total += float(np.sum(np.abs(sp[mask]) ** 2))
    return total


# ------------------------------------------------------------------ Test A
def test_keyboard_suppression(sr):
    inp, _ = read_mono(wav("typing_only.wav"))
    rows = {}
    for label, typ, _sp, _ab in ENGINES:
        if not exists(typ):
            continue
        proc, _ = read_mono(wav(typ + ".wav"))
        i, p, lag = align_to_input(inp, proc)
        rms_red = 20.0 * np.log10((rms(p) + EPS) / (rms(i) + EPS))
        cf_in, cf_out = crest_factor(i), crest_factor(p)
        stl_in = short_time_levels(i, sr)
        stl_out = short_time_levels(p, sr)
        p99_in = np.percentile(stl_in, 99)
        p99_out = np.percentile(stl_out, 99)
        p99_red = 20.0 * np.log10((p99_out + EPS) / (p99_in + EPS))
        rows[label] = dict(
            lag=lag,
            rms_in_db=dbfs(i), rms_out_db=dbfs(p),
            rms_reduction_db=rms_red,
            crest_in=cf_in, crest_out=cf_out,
            crest_reduction=cf_in - cf_out,
            p99_reduction_db=p99_red,
            residual_dbfs=dbfs(p),
        )
    return rows


# ------------------------------------------------------------------ Test B
def test_voice_preservation(sr):
    inp, _ = read_mono(wav("speech_only.wav"))
    rows = {}
    for label, _typ, sp, _ab in ENGINES:
        if not exists(sp):
            continue
        proc, _ = read_mono(wav(sp + ".wav"))
        i, p, lag = align_to_input(inp, proc)
        # speech-band energy retention (100-5000 Hz)
        e_in = band_energy(i, sr, 100, 5000)
        e_out = band_energy(p, sr, 100, 5000)
        retention = e_out / (e_in + EPS)
        # voiced-frame selection from input
        nfft, hop = 1024, 256
        win = np.hanning(nfft)
        freqs = np.fft.rfftfreq(nfft, 1.0 / sr)
        vb = (freqs >= 100) & (freqs <= 5000)
        specs_in, specs_out, energies = [], [], []
        for k in range(0, len(i) - nfft, hop):
            si = np.abs(np.fft.rfft(i[k:k + nfft] * win))
            so = np.abs(np.fft.rfft(p[k:k + nfft] * win))
            specs_in.append(si)
            specs_out.append(so)
            energies.append(np.sum(si[vb] ** 2))
        specs_in = np.array(specs_in)
        specs_out = np.array(specs_out)
        energies = np.array(energies)
        thr = np.percentile(energies, 60)  # loudest 40% = voiced
        voiced = energies >= thr
        vi = specs_in[voiced]
        vo = specs_out[voiced]
        # log-spectral distance (dB) over speech band, voiced frames
        li = 10.0 * np.log10(vi[:, vb] ** 2 + EPS)
        lo = 10.0 * np.log10(vo[:, vb] ** 2 + EPS)
        lsd = float(np.mean(np.sqrt(np.mean((li - lo) ** 2, axis=1))))
        # normalized RMS spectral error over speech band, voiced frames
        num = np.sqrt(np.mean((vo[:, vb] - vi[:, vb]) ** 2))
        den = np.sqrt(np.mean(vi[:, vb] ** 2)) + EPS
        nrmse = float(num / den)
        rows[label] = dict(
            lag=lag,
            speechband_retention=retention,
            lsd_db=lsd,
            nrmse=nrmse,
            rms_in_db=dbfs(i), rms_out_db=dbfs(p),
        )
    return rows


# ------------------------------------------------------------------ Test C
def test_real_usecase(sr):
    inp, _ = read_mono(wav("speech_typing.wav"))
    rows = {}
    nfft, hop = 1024, 256
    win = np.hanning(nfft)
    freqs = np.fft.rfftfreq(nfft, 1.0 / sr)
    hb = freqs > 3000            # high band (clicks live here)
    sb = (freqs >= 100) & (freqs <= 5000)
    for label, _typ, _sp, ab in ENGINES:
        if not exists(ab):
            continue
        proc, _ = read_mono(wav(ab + ".wav"))
        i, p, lag = align_to_input(inp, proc)
        specs_in, specs_out, sp_energy, hf_in = [], [], [], []
        for k in range(0, len(i) - nfft, hop):
            si = np.fft.rfft(i[k:k + nfft] * win)
            so = np.fft.rfft(p[k:k + nfft] * win)
            specs_in.append(np.abs(si))
            specs_out.append(np.abs(so))
            sp_energy.append(np.sum(np.abs(si[sb]) ** 2))
            hf_in.append(np.sum(np.abs(si[hb]) ** 2))
        specs_in = np.array(specs_in)
        specs_out = np.array(specs_out)
        sp_energy = np.array(sp_energy)
        # speech-active frames = loudest 50% by speech-band energy
        thr = np.percentile(sp_energy, 50)
        active = sp_energy >= thr
        hf_e_in = np.sum(specs_in[active][:, hb] ** 2)
        hf_e_out = np.sum(specs_out[active][:, hb] ** 2)
        hf_red_db = 10.0 * np.log10((hf_e_out + EPS) / (hf_e_in + EPS))
        rows[label] = dict(
            lag=lag,
            hf_transient_reduction_db=hf_red_db,
            rms_in_db=dbfs(i), rms_out_db=dbfs(p),
        )
    return rows


def ptable(title, rows, fields):
    order = [label for (label, *_rest) in ENGINES if label in rows]
    print(f"\n## {title}")
    if not order:
        print("  (no output files present)")
        return
    label_w = max(len(f[0]) for f in fields)
    colw = max(14, max(len(o) for o in order))
    hdr = f"{'metric':<{label_w}} | " + " | ".join(f"{o:>{colw}}" for o in order)
    print(hdr)
    print("-" * len(hdr))
    for key, fmt in fields:
        cells = []
        for o in order:
            v = rows[o].get(key)
            cells.append(fmt.format(v) if v is not None else "n/a".rjust(colw))
        print(f"{key:<{label_w}} | " + " | ".join(f"{c:>{colw}}" for c in cells))


def main():
    global SUFFIX
    if len(sys.argv) > 1:
        SUFFIX = sys.argv[1]
        print(f"(using filename suffix '{SUFFIX}')")
    sr = 48000
    a = test_keyboard_suppression(sr)
    b = test_voice_preservation(sr)
    c = test_real_usecase(sr)

    print("=" * 78)
    print("VoiceCleaner REAL-recording engine comparison (K688 mechanical keyboard)")
    print("Each engine output aligned to its RAW INPUT by xcorr lag, then scored.")
    print("=" * 78)

    ptable("A. KEYBOARD SUPPRESSION  (typing_only.wav -> ideal = silence)", a, [
        ("lag", "{:d}"),
        ("rms_in_db", "{:.2f}"),
        ("rms_out_db", "{:.2f}"),
        ("rms_reduction_db", "{:.2f}"),
        ("crest_in", "{:.2f}"),
        ("crest_out", "{:.2f}"),
        ("crest_reduction", "{:.2f}"),
        ("p99_reduction_db", "{:.2f}"),
        ("residual_dbfs", "{:.2f}"),
    ])
    print("\n  more-negative rms/p99 reduction = more suppression;"
          " larger crest_reduction = flatter clicks;"
          " residual_dbfs = noise LEFT (lower=quieter).")

    ptable("B. VOICE PRESERVATION  (speech_only.wav -> ideal = untouched)", b, [
        ("lag", "{:d}"),
        ("speechband_retention", "{:.4f}"),
        ("lsd_db", "{:.3f}"),
        ("nrmse", "{:.4f}"),
        ("rms_in_db", "{:.2f}"),
        ("rms_out_db", "{:.2f}"),
    ])
    print("\n  speechband_retention ~1.0 = voice preserved (<<1 = over-suppressed);"
          " lower lsd_db / nrmse = less damage.")

    ptable("C. REAL USE CASE  (speech_typing.wav -> click removal under speech)", c, [
        ("lag", "{:d}"),
        ("hf_transient_reduction_db", "{:.2f}"),
        ("rms_in_db", "{:.2f}"),
        ("rms_out_db", "{:.2f}"),
    ])
    print("\n  hf_transient_reduction_db over speech-active frames:"
          " more negative = more >3kHz click energy removed while speech present.")


if __name__ == "__main__":
    main()
