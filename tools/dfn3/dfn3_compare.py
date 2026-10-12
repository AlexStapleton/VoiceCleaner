#!/usr/bin/env python3
"""Compare our DFN3 offline output against the official deep-filter reference.

Loads two mono WAVs (reference, ours), finds the integer sample lag by
cross-correlation, aligns and trims to common length, then reports:
  - best lag (samples)
  - normalized cross-correlation / Pearson r at best lag (headline)
  - segmental RMS error and RMS ratio (ours/ref) in the noise-only lead-in
    region and the speech region
  - coarse magnitude-spectrum MSE

Usage: python tools/dfn3/dfn3_compare.py <ref.wav> <ours.wav> [--leadin-s 0.5]
"""
import sys
import wave
import numpy as np

SR = 48000
LEADIN_S = 0.5


def read_mono(path):
    with wave.open(path, "rb") as w:
        sr = w.getframerate()
        ch = w.getnchannels()
        sw = w.getsampwidth()
        n = w.getnframes()
        raw = w.readframes(n)
    if sw == 2:
        x = np.frombuffer(raw, dtype=np.int16).astype(np.float64) / 32768.0
    elif sw == 4:
        x = np.frombuffer(raw, dtype=np.int32).astype(np.float64) / 2147483648.0
    else:
        raise ValueError(f"unsupported sample width {sw}")
    if ch > 1:
        x = x.reshape(-1, ch)[:, 0]
    return x, sr


def best_lag(ref, ours, max_lag=4800):
    """Integer lag (samples) that best aligns ours to ref, via FFT xcorr.

    Positive lag => ours is delayed relative to ref by `lag` samples.
    """
    n = len(ref)
    a = ref - ref.mean()
    b = ours - ours.mean()
    size = 1 << int(np.ceil(np.log2(2 * n)))
    fa = np.fft.rfft(a, size)
    fb = np.fft.rfft(b, size)
    corr = np.fft.irfft(fa * np.conj(fb), size)
    lags = np.arange(-max_lag, max_lag + 1)
    vals = np.array([corr[l % size] for l in lags])
    return int(lags[np.argmax(vals)])


def pearson(a, b):
    a = a - a.mean()
    b = b - b.mean()
    denom = np.sqrt(np.sum(a * a) * np.sum(b * b))
    return float(np.sum(a * b) / denom) if denom > 0 else 0.0


def rms(x):
    return float(np.sqrt(np.mean(x * x))) if len(x) else 0.0


def mag_spectrum_mse(a, b, nfft=960, hop=480):
    def avg_mag(x):
        frames = []
        win = np.hanning(nfft)
        for i in range(0, len(x) - nfft, hop):
            frames.append(np.abs(np.fft.rfft(x[i:i + nfft] * win)))
        return np.mean(frames, axis=0) if frames else np.zeros(nfft // 2 + 1)
    ma, mb = avg_mag(a), avg_mag(b)
    return float(np.mean((ma - mb) ** 2))


def main():
    ref_path, ours_path = sys.argv[1], sys.argv[2]
    leadin_s = LEADIN_S
    if "--leadin-s" in sys.argv:
        leadin_s = float(sys.argv[sys.argv.index("--leadin-s") + 1])

    ref, sr1 = read_mono(ref_path)
    ours, sr2 = read_mono(ours_path)
    assert sr1 == sr2, f"sample-rate mismatch {sr1} vs {sr2}"
    sr = sr1

    lag = best_lag(ref, ours)
    # Align: shift ours by -lag so it lines up with ref.
    if lag >= 0:
        r = ref[lag:]
        o = ours[:len(ours) - lag] if lag > 0 else ours
    else:
        r = ref[:len(ref) + lag]
        o = ours[-lag:]
    m = min(len(r), len(o))
    r, o = r[:m], o[:m]

    r_al = pearson(r, o)

    lead = int(leadin_s * sr) - abs(lag)          # lead-in shrinks by the alignment shift
    lead = max(0, min(lead, m))
    noise_r, noise_o = r[:lead], o[:lead]
    sp_r, sp_o = r[lead:], o[lead:]

    print(f"# files")
    print(f"ref : {ref_path}  ({len(ref)} samp)")
    print(f"ours: {ours_path}  ({len(ours)} samp)")
    print(f"sr  : {sr} Hz")
    print()
    print(f"best_lag_samples        : {lag}  ({lag / sr * 1000:.2f} ms)")
    print(f"pearson_r_at_best_lag   : {r_al:.6f}   <-- HEADLINE")
    print(f"common_length_samples   : {m}")
    print()
    print("# noise-only lead-in region")
    print(f"  samples               : {lead}")
    print(f"  rms_ref               : {rms(noise_r):.6e}")
    print(f"  rms_ours              : {rms(noise_o):.6e}")
    print(f"  rms_error(ours-ref)   : {rms(noise_o - noise_r):.6e}")
    print(f"  rms_ratio(ours/ref)   : {rms(noise_o) / rms(noise_r) if rms(noise_r) > 0 else float('nan'):.4f}")
    print()
    print("# speech region")
    print(f"  samples               : {len(sp_r)}")
    print(f"  rms_ref               : {rms(sp_r):.6e}")
    print(f"  rms_ours              : {rms(sp_o):.6e}")
    print(f"  rms_error(ours-ref)   : {rms(sp_o - sp_r):.6e}")
    print(f"  rms_ratio(ours/ref)   : {rms(sp_o) / rms(sp_r) if rms(sp_r) > 0 else float('nan'):.4f}")
    print()
    print(f"mag_spectrum_mse        : {mag_spectrum_mse(r, o):.6e}")


if __name__ == "__main__":
    main()
