#!/usr/bin/env python3
"""Generate a reproducible noisy validation WAV for DFN3 offline validation.

Output: 48 kHz mono PCM16, ~3.0 s. A speech-like signal (three
amplitude-modulated formant-ish tones at 200/900/2500 Hz with a slow AM
envelope) plus additive white noise at ~7 dB SNR, with a 0.5 s noise-only
lead-in so the noise floor can be measured on its own.

Deterministic: fixed RNG seed. Re-running reproduces the exact same file.

Usage: python docs/archive/dfn3-tools/dfn3_make_val_wav.py <out.wav>
"""
import sys
import wave
import numpy as np

SR = 48000
DUR_S = 3.0
LEADIN_S = 0.5          # noise-only lead-in
SNR_DB = 7.0            # speech-vs-noise SNR over the speech region
SEED = 20260814
FORMANTS = (200.0, 900.0, 2500.0)


def main(out_path):
    rng = np.random.default_rng(SEED)
    n = int(round(DUR_S * SR))
    t = np.arange(n) / SR
    lead = int(round(LEADIN_S * SR))

    # Speech-like signal: 3 formant tones, slow AM envelope, active only after lead-in.
    speech = np.zeros(n, dtype=np.float64)
    am = 0.5 * (1.0 + np.sin(2 * np.pi * 3.0 * t))          # 3 Hz amplitude modulation
    weights = (1.0, 0.6, 0.35)
    for f, w in zip(FORMANTS, weights):
        speech += w * np.sin(2 * np.pi * f * t)
    speech *= am
    # Gate: silent during the lead-in, smooth 20 ms ramp-in afterwards.
    gate = np.ones(n)
    gate[:lead] = 0.0
    ramp = int(0.02 * SR)
    gate[lead:lead + ramp] = np.linspace(0.0, 1.0, ramp)
    speech *= gate

    # White noise everywhere (including lead-in).
    noise = rng.standard_normal(n)

    # Scale noise for target SNR measured over the speech-active region.
    active = slice(lead + ramp, n)
    ps = np.mean(speech[active] ** 2)
    pn = np.mean(noise[active] ** 2)
    target_pn = ps / (10 ** (SNR_DB / 10.0))
    noise *= np.sqrt(target_pn / pn)

    mix = speech + noise
    peak = np.max(np.abs(mix))
    mix *= 0.89 / peak                                       # headroom, no clipping
    pcm = np.clip(np.round(mix * 32767.0), -32768, 32767).astype(np.int16)

    with wave.open(out_path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())

    print(f"wrote {out_path}: {n} samples, {DUR_S}s, {SR} Hz mono PCM16, "
          f"SNR~{SNR_DB} dB, lead-in {LEADIN_S}s, seed {SEED}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "val_in.wav")
