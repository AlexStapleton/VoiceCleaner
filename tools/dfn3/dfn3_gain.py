#!/usr/bin/env python3
"""Apply a uniform gain (in dB) to a mono/stereo PCM16 WAV.

Multiplies every sample by 10**(dB/20), clamps to [-1, 1] (reporting the
clipped-sample count), and writes the result as PCM16 at the original rate/
channel count.

Usage:
  python tools/dfn3/dfn3_gain.py <in.wav> <out.wav> <dB>

Example (+20 dB):
  python tools/dfn3/dfn3_gain.py typing_only.wav typing_only_n.wav 20
"""
import sys
import wave
import numpy as np


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    in_path, out_path, db = sys.argv[1], sys.argv[2], float(sys.argv[3])

    with wave.open(in_path, "rb") as w:
        sr = w.getframerate()
        ch = w.getnchannels()
        sw = w.getsampwidth()
        n = w.getnframes()
        raw = w.readframes(n)
    if sw != 2:
        raise ValueError(f"expected PCM16, got sample width {sw}")

    x = np.frombuffer(raw, dtype=np.int16).astype(np.float64) / 32768.0
    factor = 10.0 ** (db / 20.0)
    y = x * factor

    clipped = int(np.sum((y > 1.0) | (y < -1.0)))
    y = np.clip(y, -1.0, 1.0)

    out = np.round(y * 32767.0).astype(np.int16)
    with wave.open(out_path, "wb") as w:
        w.setnchannels(ch)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(out.tobytes())

    peak_in = 20.0 * np.log10(np.max(np.abs(x)) + 1e-12)
    peak_out = 20.0 * np.log10(np.max(np.abs(y)) + 1e-12)
    print(f"{in_path} -> {out_path}: {db:+.1f} dB (x{factor:.4f})")
    print(f"  samples={len(x)} ch={ch} sr={sr}")
    print(f"  peak_in={peak_in:.2f} dBFS  peak_out={peak_out:.2f} dBFS")
    print(f"  clipped_samples={clipped} ({100.0*clipped/max(1,len(x)):.4f}%)")


if __name__ == "__main__":
    main()
