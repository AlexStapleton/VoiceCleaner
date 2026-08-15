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

Usage:
  python tools/dfn3_realtest.py
Assumes the input recordings and the processed *_dfn3 / *_rnnoise / *_pass wavs
live in the repo root (produced by offline-cli.exe).
"""
import os
import sys
import numpy as np

# reuse IO + alignment from the reference comparator
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dfn3_compare import read_mono, best_lag  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EPS = 1e-12


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


def stft_mag(x, nfft=1024, hop=256):
    win = np.hanning(nfft)
    frames = []
    for i in range(0, len(x) - nfft, hop):
        frames.append(np.abs(np.fft.rfft(x[i:i + nfft] * win)))
    if not frames:
        return np.zeros((1, nfft // 2 + 1)), np.fft.rfftfreq(nfft, 1.0)
    return np.array(frames), None


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
    inp, _ = read_mono(os.path.join(ROOT, "typing_only.wav"))
    rows = {}
    for name, fn in [("dfn3-stream", "typing_dfn3.wav"),
                     ("rnnoise", "typing_rnnoise.wav"),
                     ("passthrough", "typing_pass.wav")]:
        proc, _ = read_mono(os.path.join(ROOT, fn))
        i, p, lag = align_to_input(inp, proc)
        rms_red = 20.0 * np.log10((rms(p) + EPS) / (rms(i) + EPS))
        cf_in, cf_out = crest_factor(i), crest_factor(p)
        stl_in = short_time_levels(i, sr)
        stl_out = short_time_levels(p, sr)
        p99_in = np.percentile(stl_in, 99)
        p99_out = np.percentile(stl_out, 99)
        p99_red = 20.0 * np.log10((p99_out + EPS) / (p99_in + EPS))
        rows[name] = dict(
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
    inp, _ = read_mono(os.path.join(ROOT, "speech_only.wav"))
    rows = {}
    # voiced frames = loudest 40% of input short-time frames
    for name, fn in [("dfn3-stream", "speech_dfn3.wav"),
                     ("rnnoise", "speech_rnnoise.wav"),
                     ("passthrough", "speech_pass.wav")]:
        proc, _ = read_mono(os.path.join(ROOT, fn))
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
        rows[name] = dict(
            lag=lag,
            speechband_retention=retention,
            lsd_db=lsd,
            nrmse=nrmse,
            rms_in_db=dbfs(i), rms_out_db=dbfs(p),
        )
    return rows


# ------------------------------------------------------------------ Test C
def test_real_usecase(sr):
    inp, _ = read_mono(os.path.join(ROOT, "speech_typing.wav"))
    rows = {}
    nfft, hop = 1024, 256
    win = np.hanning(nfft)
    freqs = np.fft.rfftfreq(nfft, 1.0 / sr)
    hb = freqs > 3000            # high band (clicks live here)
    sb = (freqs >= 100) & (freqs <= 5000)
    for name, fn in [("dfn3-stream", "abtest_dfn3.wav"),
                     ("rnnoise", "abtest_rnnoise.wav"),
                     ("passthrough", "abtest_pass.wav")]:
        proc, _ = read_mono(os.path.join(ROOT, fn))
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
        # also crest factor of HF-filtered time domain as a transient proxy
        rows[name] = dict(
            lag=lag,
            hf_transient_reduction_db=hf_red_db,
            rms_in_db=dbfs(i), rms_out_db=dbfs(p),
        )
    return rows


def ptable(title, rows, fields):
    order = ["dfn3-stream", "rnnoise", "passthrough"]
    print(f"\n## {title}")
    label_w = max(len(f[0]) for f in fields)
    hdr = f"{'metric':<{label_w}} | " + " | ".join(f"{o:>14}" for o in order)
    print(hdr)
    print("-" * len(hdr))
    for key, fmt in fields:
        cells = []
        for o in order:
            v = rows[o].get(key)
            cells.append(fmt.format(v) if v is not None else "n/a".rjust(14))
        print(f"{key:<{label_w}} | " + " | ".join(f"{c:>14}" for c in cells))


def main():
    sr = 48000
    a = test_keyboard_suppression(sr)
    b = test_voice_preservation(sr)
    c = test_real_usecase(sr)

    print("=" * 78)
    print("VoiceCleaner REAL-recording engine comparison (K688 mechanical keyboard)")
    print("Each engine output aligned to its RAW INPUT by xcorr lag, then scored.")
    print("=" * 78)

    ptable("A. KEYBOARD SUPPRESSION  (typing_only.wav -> ideal = silence)", a, [
        ("lag", "{:14d}"),
        ("rms_in_db", "{:14.2f}"),
        ("rms_out_db", "{:14.2f}"),
        ("rms_reduction_db", "{:14.2f}"),
        ("crest_in", "{:14.2f}"),
        ("crest_out", "{:14.2f}"),
        ("crest_reduction", "{:14.2f}"),
        ("p99_reduction_db", "{:14.2f}"),
        ("residual_dbfs", "{:14.2f}"),
    ])
    print("\n  more-negative rms/p99 reduction = more suppression;"
          " larger crest_reduction = flatter clicks;"
          " residual_dbfs = noise LEFT (lower=quieter).")

    ptable("B. VOICE PRESERVATION  (speech_only.wav -> ideal = untouched)", b, [
        ("lag", "{:14d}"),
        ("speechband_retention", "{:14.4f}"),
        ("lsd_db", "{:14.3f}"),
        ("nrmse", "{:14.4f}"),
        ("rms_in_db", "{:14.2f}"),
        ("rms_out_db", "{:14.2f}"),
    ])
    print("\n  speechband_retention ~1.0 = voice preserved (<<1 = over-suppressed);"
          " lower lsd_db / nrmse = less damage.")

    ptable("C. REAL USE CASE  (speech_typing.wav -> click removal under speech)", c, [
        ("lag", "{:14d}"),
        ("hf_transient_reduction_db", "{:14.2f}"),
        ("rms_in_db", "{:14.2f}"),
        ("rms_out_db", "{:14.2f}"),
    ])
    print("\n  hf_transient_reduction_db over speech-active frames:"
          " more negative = more >3kHz click energy removed while speech present.")


if __name__ == "__main__":
    main()
