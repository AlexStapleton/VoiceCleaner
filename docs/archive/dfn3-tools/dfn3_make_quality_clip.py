#!/usr/bin/env python3
"""Generate a deterministic 48 kHz quality-validation clip with GROUND TRUTH.

Unlike dfn3_make_val_wav.py (sine + WHITE noise), this builds KEYBOARD-STYLE
IMPULSIVE noise: trains of short, sharp, fast-decaying broadband transients
("key clicks") a few per second, over a faint stationary fan/hum floor. This is
the noise DFN3 is meant to beat RNNoise on.

The clean-speech proxy is a VOICED-SPEECH model, not sustained tones: a glottal
impulse train with a per-syllable F0 contour + vibrato, shaped by three formant
resonators, gated into syllables with silent gaps. The HARMONIC structure and
syllabic modulation are what speech-trained denoisers preserve -- a bag of
non-harmonic sustained sinusoids (as in dfn3_make_val_wav.py) is treated as
tonal NOISE and stripped by both DFN3 and RNNoise, which collapses any
ground-truth SNR metric. This proxy avoids that trap.

The clean speech and the noise are kept as SEPARATE signals and written out
alongside the mix, so downstream metrics have true references:
  - qual_mix.wav   : noisy input to denoise (clean + noise)
  - qual_clean.wav : clean speech ground truth (same scaling as in the mix)
  - qual_noise.wav : the noise alone        (same scaling as in the mix)
By construction, sample-for-sample, mix == clean + noise (both int16-rounded
from the identically-scaled float components).

Deterministic: fixed RNG seed. Re-running reproduces the exact same files.

Usage: python docs/archive/dfn3-tools/dfn3_make_quality_clip.py [out_prefix]   (default: qual)
"""
import sys
import wave
import numpy as np

SR = 48000
DUR_S = 4.0
LEADIN_S = 0.5            # noise-only lead-in (no speech)
SNR_DB = 5.0             # clean-vs-noise SNR over the speech-active region
SEED = 20260815
# Voiced-speech formants (freq Hz, bandwidth Hz, gain) -- roughly an /a/ vowel.
FORMANTS = ((700.0, 90.0, 1.0), (1220.0, 110.0, 0.6), (2600.0, 140.0, 0.35))

# Keyboard-click parameters.
CLICKS_PER_S = 6.0        # typing cadence (a few keystrokes per second)
CLICK_TAU_MS = 1.2        # fast exponential decay time constant
CLICK_LEN_MS = 5.0        # burst length
CLICK_BP_LO = 1500.0      # click energy emphasis band (broadband but mid/high)
CLICK_BP_HI = 8000.0
HUM_LEVEL = 0.06          # stationary fan/hum floor, relative to click trains


def write_wav(path, x_float):
    pcm = np.clip(np.round(x_float * 32767.0), -32768, 32767).astype(np.int16)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())


def rms(x):
    return float(np.sqrt(np.mean(x * x))) if len(x) else 0.0


def onepole_lowpass(x, fc):
    """Simple one-pole low-pass; used to shape the fan floor and clicks."""
    a = np.exp(-2.0 * np.pi * fc / SR)
    y = np.empty_like(x)
    acc = 0.0
    for i in range(len(x)):
        acc = a * acc + (1.0 - a) * x[i]
        y[i] = acc
    return y


def resonator(x, fc, bw):
    """Two-pole resonator (formant filter): y[n] = x[n] + a1 y[n-1] + a2 y[n-2]."""
    r = np.exp(-np.pi * bw / SR)
    a1 = 2.0 * r * np.cos(2.0 * np.pi * fc / SR)
    a2 = -(r * r)
    y = np.zeros_like(x)
    y1 = y2 = 0.0
    for i in range(len(x)):
        yi = x[i] + a1 * y1 + a2 * y2
        y[i] = yi
        y2, y1 = y1, yi
    return y


def make_voiced_speech(n, lead, rng):
    """Harmonic voiced-speech proxy: glottal impulse train (per-syllable F0
    contour + vibrato) through formant resonators, gated into syllables with
    silent gaps. Speech is silent through the lead-in."""
    src = np.zeros(n)                                     # glottal impulse train
    voiced = np.zeros(n)                                  # syllable gate (0/1)
    pos = lead + int(0.05 * SR)
    syllables = 0
    while pos < n - int(0.30 * SR):
        dur = int((0.15 + 0.15 * rng.random()) * SR)     # 150-300 ms syllable
        f0 = 100.0 + 40.0 * rng.random()                 # 100-140 Hz base pitch
        end = min(pos + dur, n)
        phase = 0.0
        for i in range(pos, end):
            frac = (i - pos) / dur
            f = f0 * (1.0 - 0.15 * frac) * (1.0 + 0.02 * np.sin(2 * np.pi * 5.0 * (i - pos) / SR))
            phase += f / SR
            if phase >= 1.0:
                phase -= 1.0
                src[i] = 1.0
        voiced[pos:end] = 1.0
        syllables += 1
        pos = end + int((0.08 + 0.12 * rng.random()) * SR)   # 80-200 ms gap

    voice = np.zeros(n)
    for fc, bw, g in FORMANTS:
        voice += g * resonator(src, fc, bw)
    # Smooth the syllable gate into a soft amplitude envelope (~ raised edges).
    env = onepole_lowpass(voiced, 60.0)
    env = onepole_lowpass(env[::-1], 60.0)[::-1]         # zero-phase-ish
    voice *= env
    return voice, syllables


def make_click(rng):
    """One short, sharp, fast-decaying broadband transient (a key click)."""
    n = int(round(CLICK_LEN_MS * 1e-3 * SR))
    t = np.arange(n) / SR
    burst = rng.standard_normal(n)
    # Band-emphasis by removing lows (difference = crude high-pass) and
    # rolling off the extreme highs, keeping it broadband but click-like.
    burst = np.diff(burst, prepend=burst[0])                 # crude high-pass -> sharp
    burst = burst - onepole_lowpass(burst, CLICK_BP_HI)      # tame the very top
    env = np.exp(-t / (CLICK_TAU_MS * 1e-3))                 # fast decay
    # A short attack so the very first sample isn't a full-scale step.
    att = min(8, n)
    env[:att] *= np.linspace(0.0, 1.0, att)
    return burst * env


def main(prefix="qual"):
    rng = np.random.default_rng(SEED)
    n = int(round(DUR_S * SR))
    t = np.arange(n) / SR
    lead = int(round(LEADIN_S * SR))

    # ---- Clean speech proxy: harmonic voiced speech, silent through lead-in.
    speech, n_syll = make_voiced_speech(n, lead, rng)
    ramp = int(0.02 * SR)

    # ---- Impulsive keyboard noise: randomized click train + faint fan floor.
    noise = np.zeros(n, dtype=np.float64)
    # Click onset times over the WHOLE clip (including the lead-in), Poisson-ish
    # spacing so cadence is irregular like real typing.
    pos = 0
    onsets = []
    mean_gap = SR / CLICKS_PER_S
    while pos < n:
        gap = int(rng.exponential(mean_gap))
        gap = max(gap, int(0.04 * SR))                       # >=40 ms between keys
        pos += gap
        if pos < n:
            onsets.append(pos)
    for on in onsets:
        click = make_click(rng) * (0.6 + 0.8 * rng.random())  # varied loudness
        end = min(on + len(click), n)
        noise[on:end] += click[:end - on]

    # Faint stationary fan/hum floor underneath (lowpassed noise + 120 Hz hum).
    fan = onepole_lowpass(rng.standard_normal(n), 400.0)
    fan /= (rms(fan) + 1e-12)
    hum = np.sin(2 * np.pi * 120.0 * t)
    floor = fan + 0.5 * hum
    noise += HUM_LEVEL * (rms(noise) / (rms(floor) + 1e-12)) * floor

    # ---- Scale noise for target SNR over the VOICED region (speech present,
    #      excluding inter-syllable silence so the SNR figure is meaningful). --
    sp_env = np.abs(speech)
    voiced_mask = sp_env > 0.10 * sp_env.max()
    ps = np.mean(speech[voiced_mask] ** 2)
    pn = np.mean(noise[lead:] ** 2)                       # noise power post lead-in
    noise *= np.sqrt((ps / (10 ** (SNR_DB / 10.0))) / pn)

    # ---- Mix, normalize; apply the SAME gain to the components so the written
    #      clean+noise reconstruct the mix sample-exactly. ----
    mix = speech + noise
    g = 0.89 / np.max(np.abs(mix))
    mix *= g
    speech *= g
    noise *= g

    write_wav(f"{prefix}_mix.wav", mix)
    write_wav(f"{prefix}_clean.wav", speech)
    write_wav(f"{prefix}_noise.wav", noise)

    # ---- Stats. ----
    in_snr = 10 * np.log10(np.mean(speech[voiced_mask] ** 2) / np.mean(noise[lead:] ** 2))
    print(f"wrote {prefix}_mix.wav / {prefix}_clean.wav / {prefix}_noise.wav")
    print(f"  {n} samples, {DUR_S}s, {SR} Hz mono PCM16, seed {SEED}")
    print(f"  lead-in (noise only) : {LEADIN_S}s ({lead} samp), rms {rms(noise[:lead]):.4e}")
    print(f"  voiced speech        : {n_syll} syllables (harmonic glottal+formant), "
          f"voiced {100*voiced_mask.mean():.0f}% of samples")
    print(f"  keyboard clicks      : {len(onsets)} over the clip "
          f"(~{len(onsets)/DUR_S:.1f}/s), tau {CLICK_TAU_MS} ms, len {CLICK_LEN_MS} ms")
    print(f"  fan/hum floor level  : {HUM_LEVEL:.2f} rel. to clicks")
    print(f"  input SNR (voiced reg): {in_snr:.2f} dB  (target {SNR_DB:.1f})")
    print(f"  peak-normalized to    : 0.89")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "qual")
