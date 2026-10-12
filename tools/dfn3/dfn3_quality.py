#!/usr/bin/env python3
"""Measure denoise QUALITY against ground truth (clean speech + known noise).

Given the clean-speech reference, the noisy mix, and a processed output, report:
  - SNR improvement (headline): aligns proc to clean by xcorr lag, then
      input SNR  = 10log10(P_clean / P_(mix - clean))         over speech frames
      output SNR = 10log10(P_clean / P_(proc - clean))        over speech frames
      dSNR = output - input.
    Also a gain-invariant SI-SDR improvement (robust to a benign level change
    the raw formula would wrongly penalize) and a SEGMENTAL version (median of
    per-frame SNR over speech-active frames).
  - Noise suppression: dB RMS reduction in the noise-only lead-in (proc vs mix).
  - Speech distortion: in speech-active regions, gain-matched log-spectral
    distance and normalized RMS error between proc and clean -- so an engine
    that "denoises" by also killing speech is caught.
  - A one-line summary suitable for a table row.

Reuses read_mono / best_lag / rms from dfn3_compare.py; alignment handles the
processing latency via the xcorr lag.

Usage:
  python tools/dfn3/dfn3_quality.py --clean qual_clean.wav --mix qual_mix.wav \
      --proc out.wav [--noise qual_noise.wav] [--leadin-s 0.5] [--label NAME]
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dfn3_compare import read_mono, best_lag, rms  # noqa: E402

FRAME = 1024              # ~21 ms segmental-SNR / VAD frame
HOP = 512
NFFT = 1024


def align(clean, mix, proc):
    """Shift proc by the xcorr lag so it lines up with clean; trim clean & mix
    to the same region. mix shares clean's timebase (mix == clean + noise)."""
    lag = best_lag(clean, proc)                  # +lag => proc delayed vs clean
    if lag >= 0:
        c = clean[lag:]
        mx = mix[lag:]
        p = proc[:len(proc) - lag] if lag > 0 else proc
    else:
        c = clean[:len(clean) + lag]
        mx = mix[:len(mix) + lag]
        p = proc[-lag:]
    m = min(len(c), len(p), len(mx))
    return c[:m], mx[:m], p[:m], lag


def snr_db(target, estimate):
    """Raw SNR: 10log10(||target||^2 / ||estimate - target||^2)."""
    e = estimate - target
    pt = np.sum(target * target)
    pe = np.sum(e * e)
    return 10.0 * np.log10(pt / pe) if pe > 0 else float("inf")


def si_sdr_db(target, estimate):
    """Scale-invariant SDR: project estimate onto target (removes a benign
    global gain), then signal-to-residual in dB."""
    a = np.dot(estimate, target) / (np.dot(target, target) + 1e-20)
    proj = a * target
    e = estimate - proj
    pp = np.sum(proj * proj)
    pe = np.sum(e * e)
    return 10.0 * np.log10(pp / pe) if pe > 0 else float("inf")


def speech_mask(clean):
    """Per-frame boolean: frame is speech-active if its RMS exceeds 10% of the
    loudest frame. Derived from the CLEAN reference, so it tracks the AM gating
    and the lead-in silence robustly."""
    idx = range(0, len(clean) - FRAME, HOP)
    fr = np.array([rms(clean[i:i + FRAME]) for i in idx])
    thr = 0.10 * fr.max() if len(fr) else 0.0
    return list(idx), fr > thr


def seg_snr(target, estimate, idx, mask):
    """Median per-frame SNR over speech-active frames."""
    vals = []
    for i, active in zip(idx, mask):
        if not active:
            continue
        t = target[i:i + FRAME]
        e = estimate[i:i + FRAME] - t
        pt, pe = np.sum(t * t), np.sum(e * e)
        if pe > 0 and pt > 0:
            vals.append(10.0 * np.log10(pt / pe))
    return float(np.median(vals)) if vals else float("nan")


def log_spectral_distance(clean, proc, idx, mask):
    """Gain-matched log-spectral distance (dB) over speech-active frames."""
    a = np.dot(proc, clean) / (np.dot(clean, clean) + 1e-20)     # global gain match
    proc = proc * a
    win = np.hanning(NFFT)
    dists = []
    eps = 1e-10
    for i, active in zip(idx, mask):
        if not active:
            continue
        C = np.abs(np.fft.rfft(clean[i:i + FRAME] * win, NFFT)) ** 2
        P = np.abs(np.fft.rfft(proc[i:i + FRAME] * win, NFFT)) ** 2
        d = 10 * np.log10(C + eps) - 10 * np.log10(P + eps)
        dists.append(np.sqrt(np.mean(d ** 2)))
    return float(np.mean(dists)) if dists else float("nan"), a


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clean", required=True)
    ap.add_argument("--mix", required=True)
    ap.add_argument("--proc", required=True)
    ap.add_argument("--noise", default=None)
    ap.add_argument("--leadin-s", type=float, default=0.5)
    ap.add_argument("--label", default=None)
    args = ap.parse_args()

    clean, sr = read_mono(args.clean)
    mix, _ = read_mono(args.mix)
    proc, _ = read_mono(args.proc)
    label = args.label or os.path.basename(args.proc)

    c, mx, p, lag = align(clean, mix, proc)
    idx, mask = speech_mask(c)

    # Speech-active-only concatenations for the whole-region SNR figures.
    sel = np.concatenate([np.arange(i, i + FRAME) for i, a in zip(idx, mask) if a]) \
        if any(mask) else np.arange(len(c))
    c_sp, mx_sp, p_sp = c[sel], mx[sel], p[sel]

    in_snr = snr_db(c_sp, mx_sp)
    out_snr = snr_db(c_sp, p_sp)
    in_sisdr = si_sdr_db(c_sp, mx_sp)
    out_sisdr = si_sdr_db(c_sp, p_sp)
    seg_in = seg_snr(c, mx, idx, mask)
    seg_out = seg_snr(c, p, idx, mask)

    # Noise-only lead-in region (shrinks by the alignment shift).
    lead = max(0, min(int(args.leadin_s * sr) - abs(lag), len(c)))
    supp_db = 20 * np.log10(rms(mx[:lead]) / (rms(p[:lead]) + 1e-20)) if lead > 0 else float("nan")

    lsd, gain = log_spectral_distance(c, p, idx, mask)
    # Normalized RMS error over speech-active samples, gain-matched.
    nrmse = rms(p_sp * gain - c_sp) / (rms(c_sp) + 1e-20)

    print(f"# {label}")
    print(f"  align lag            : {lag} samp ({lag / sr * 1000:.2f} ms)")
    print(f"  input  SNR  (speech) : {in_snr:7.2f} dB    SI-SDR {in_sisdr:7.2f} dB")
    print(f"  output SNR  (speech) : {out_snr:7.2f} dB    SI-SDR {out_sisdr:7.2f} dB")
    print(f"  dSNR (out-in)        : {out_snr - in_snr:+7.2f} dB    "
          f"dSI-SDR {out_sisdr - in_sisdr:+7.2f} dB   <-- HEADLINE")
    print(f"  segmental SNR in/out : {seg_in:6.2f} / {seg_out:6.2f} dB   "
          f"(median, dSegSNR {seg_out - seg_in:+.2f} dB)")
    print(f"  noise suppression    : {supp_db:7.2f} dB   (lead-in RMS drop, proc vs mix)")
    print(f"  speech distortion    : LSD {lsd:6.2f} dB   NRMSE {nrmse:6.3f}   (gain-matched, x{gain:.3f})")
    print()
    print(f"  ROW  {label:<18} dSNR {out_snr - in_snr:+6.2f}  "
          f"dSI-SDR {out_sisdr - in_sisdr:+6.2f}  "
          f"suppress {supp_db:+6.2f}  LSD {lsd:5.2f}  NRMSE {nrmse:.3f}")


if __name__ == "__main__":
    main()
