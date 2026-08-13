# System Architecture Blueprint v5 — Final
## Real-Time Microphone Noise Suppression for Windows 11

**Supersedes:** v4.1. Final pass, structured around a comparison with how comparable programs are
actually built (§10). That comparison exposed one serious omission — **clock drift between the
capture and render domains (§5.8)** — plus a set of smaller structural and operational gaps now
folded in: a DSP core library with thin front-ends (§2.1), an offline processing mode and
regression harness (§11), delay-matched bypass and dry/wet mix (§7.2), VAD timing (§7.3), and
operational concerns (§12).
**Use case:** competitive gaming voice chat, no sidetone, no hotkeys
**Capture hardware:** FIFINE AmpliTank K688 (USB mode) on a boom arm
**Target system:** Ryzen 9 9950X3D + RTX 5070 Ti, Windows 11 25H2
**Stack:** C++20 / MSVC, WASAPI, ONNX Runtime (CPU EP), VB-Cable

---

## 1. Objective and constraints

**Primary goal:** remove background noise — chiefly mechanical keyboard and mouse clicks — from
the voice path, with zero dropouts and no measurable effect on the rest of the system.

**Latency is a tunable, not a target.** v3 discarded latency entirely on the grounds that you
don't monitor your own voice and Discord's pipeline already carries ~100–150 ms. That reasoning
still holds, but "don't optimise it" is different from "don't expose it." v4 makes latency a
configurable profile with honest floors, so you can find the setting you actually like rather
than inheriting mine. See §5.

**The system must stay invisible.** No kernel components, no test signing, no hooks, no injection.
With push-to-talk now dropped entirely (§7), the application touches exactly one class of API:
audio. See §3.

### 1.1 What the hardware fixes for us

The K688 over USB tops out at **16-bit / 48 kHz** with a capsule response of **70 Hz – 15 kHz**.
Three consequences that simplify the design:

- **48 kHz is the mic's ceiling, not a compromise.** Both models are 48 kHz-native, so the whole
  chain runs at the hardware's maximum with no resampling anywhere. The resampling fallback in v3
  §5.6 becomes a guard rail that should never fire.
- **The high-pass filter is nearly redundant.** The capsule already rolls off below 70 Hz.
  Default the filter to 80 Hz and treat it as insurance against boom-arm thumps, not as a
  workhorse.
- **DeepFilterNet3's weak region doesn't apply.** DFN3 applies true deep filtering below ~5 kHz
  and only envelope enhancement above; that upper band is where its "thinned air" artifact lives.
  The mic stops at 15 kHz, so there's little up there to smear.

16-bit is the nominal quality limit, but it isn't the binding one: 16-bit carries ~96 dB of
dynamic range against a mic SNR in the 75–80 dB range. The analog stage is the floor, and Opus at
64–96 kbps is well below both.

### 1.2 Targets

| Metric | Target |
|---|---|
| Dropouts | **Zero over an 8-hour session** — the actual quality bar |
| End-to-end latency | Per selected profile (§5.1); default ~60 ms Studio, ~30 ms Fast |
| CPU | < 1 core sustained |
| Added DPC latency | Indistinguishable from baseline, measured not assumed |
| Frame-time impact in game | Within run-to-run variance (PresentMon) |
| Anti-cheat incidents | Zero. Secure Boot on, HVCI on, test signing off, always |

---

## 2. Architecture

```
┌──────────────────────────────────────────────────────────────────────┐
│  FIFINE K688 (USB)  —  16-bit / 48 kHz / cardioid dynamic            │
└─────────────────────────────┬────────────────────────────────────────┘
                              │  WASAPI shared-mode capture, event-driven
                              ▼
                      ┌───────────────┐
                      │  Ring A (SPSC)│──────────┐  optional dry tap (§9)
                      └───────┬───────┘          ▼
                              │           raw .wav writer
                              ▼             (non-RT thread)
          ┌────────────────────────────────────────┐
          │  DSP thread  (MMCSS "Pro Audio")       │
          │                                        │
          │  DC block ─► ENGINE ─► HPF ─► AGC ─► lim│
          │              │                         │
          │              ├─ DeepFilterNet3 (ORT)   │  ◄── default
          │              └─ RNNoise (C lib)        │  ◄── fallback
          └───────────────────┬────────────────────┘
                              ▼
                      ┌───────────────┐
                      │  Ring B (SPSC)│
                      └───────┬───────┘
                              │  WASAPI shared-mode render
                              ▼
                ┌──────────────────────────┐
                │  VB-Cable "CABLE Input"  │   (signed third-party driver)
                │            ║             │
                │  VB-Cable "CABLE Output" │
                └────────────┬─────────────┘
                             ▼
                Discord selects "CABLE Output" as its microphone


  ┌──────────────────┐   shared memory (telemetry) ┌──────────────────┐
  │  Tray UI process │ ◄─────────────────────────► │  Audio process   │
  │  (Win32)         │   named pipe (commands)     │                  │
  └──────────────────┘                             └──────────────────┘
```

### 2.1 Layering: a DSP core library with thin front-ends *(new in v5)*

Every mature project in this space separates the algorithm from the delivery mechanism. werman's
noise-suppression-for-voice wraps a single `RnNoiseCommonPlugin` core in six thin adapters
(LADSPA, LV2, VST2, VST3, AU, AUv3); DeepFilterNet ships a core crate plus a CLI binary plus a
LADSPA plugin. We are building exactly one front-end, and that's fine — but the boundary should
exist from day one, because retrofitting it later means untangling WASAPI assumptions out of DSP
code.

```
  ┌──────────────────────────────────────────────────────┐
  │  denoise-core   (static lib, zero Windows deps)      │
  │  INoiseFilter, engines, DSP chain, parameter block   │
  │  Input: float frames.  Output: float frames.         │
  └──────────────────────────────────────────────────────┘
        ▲                  ▲                    ▲
        │                  │                    │
  ┌─────┴──────┐   ┌───────┴────────┐   ┌───────┴────────┐
  │ Standalone │   │ Offline CLI    │   │ VST3 (later,   │
  │ (WASAPI)   │   │ (§11)          │   │  optional)     │
  └────────────┘   └────────────────┘   └────────────────┘
```

The rule: **`denoise-core` never includes `<windows.h>`, never knows what WASAPI is, never
allocates after `Init()`, and never logs.** It takes frames and returns frames. This costs almost
nothing now and buys the offline harness in §11 immediately — which is itself the reason to do it,
independent of any future plugin.

A VST3 front-end is explicitly *not* planned, but if it ever happens it unlocks Equalizer APO,
VoiceMeeter and OBS as hosts with no core changes.

---

Two processes. Threads in the audio process: WASAPI capture, DSP, WASAPI render, a low-priority
writer for the optional dry tap, and a non-RT command handler for the UI pipe (§8.5). Optionally a
second DSP thread so the two engines run concurrently under warm standby (§9.3). One thread in the
UI process (§8.2). No kernel components. No hooks. Nothing installed but your own executables plus
VB-Cable.

---

## 3. Anti-cheat hygiene

A hard requirement, not a footnote. Kernel anti-cheat makes launch-time decisions about system
state and run-time decisions about process behaviour.

**System state — never violate:**

- **Secure Boot on. HVCI on. Never `bcdedit /set testsigning on`.** This is the entire reason
  VB-Cable was chosen over the open-source virtual audio drivers, which currently require test
  signing.
- **No kernel driver of our own.** Ever.

**Process behaviour — the application does none of these:**

- `OpenProcess` / `ReadProcessMemory` / `WriteProcessMemory` against anything
- DLL injection, `SetWindowsHookEx`, IAT or inline hooking
- **Global keyboard or mouse hooks.** v3 flagged `WH_KEYBOARD_LL` as the likeliest thing here to
  get flagged. **v4 removes the need entirely — there is no hotkey feature** (§7). Do not
  reintroduce one.
- Overlay rendering or any contact with the game's present path
- Running elevated (nothing needs it after VB-Cable is installed)

**Build hygiene:** plain, unobfuscated binaries. No packer, no anti-debug, no process hiding.
Authenticode-sign your user-mode executables if convenient — not required, but it looks like
normal software. Hiding is itself the suspicious act.

---

## 4. Routing: VB-Cable

**Settled.** Closed-source but production-signed, which is the property that matters. Not the
open-source alternatives (test signing), not our own driver (EV certificate, verified legal
entity, 2–4 months for a nicer device name), not an APO (requires owning a driver package).

### 4.1 First-run wizard — setup steps

1. Install VB-Cable as admin, reboot. Adds **CABLE Input** (playback — your app writes here) and
   **CABLE Output** (recording — Discord reads here).
2. Set **both** cable endpoints to **48000 Hz** in Sound Settings → Properties → Advanced. A
   mismatch causes silent resampling and is the most common cause of "it sounds wrong."
3. Leave both cable endpoints as **non-default** devices. The app addresses them by ID; making
   CABLE Input the default output would route your game audio into it.
4. Set the **physical mic** endpoint to **16-bit, 48000 Hz** — the K688's native maximum.
5. Lower VB-Cable's own **Max Latency** in its control panel if you're chasing a low profile.
   It's a buffer your app doesn't control and at default it can dominate everything else.

### 4.2 Gain staging — do this before touching any app setting

**This will improve your output more than any parameter in the software.** The K688 is a dynamic
mic with low output (~−58 dB sensitivity). Under-driving the on-mic gain knob and compensating
with Windows' digital boost amplifies the preamp noise floor and hands the model a harder problem
than it needs.

- Set the **on-mic gain knob** so normal speaking lands around −18 to −12 dBFS at the hardware.
- Leave the Windows mic level at 100%, **microphone boost off**.
- Verify with the app's own input meter before enabling any processing.

### 4.3 Downstream configuration — as important as the DSP

**In Discord, disable its own processing.** It ships Krisp noise suppression, echo cancellation,
noise reduction and AGC, all on by default. Stacking those on an already-cleaned stream sounds
worse than either alone — double-suppression artifacts, gain fighting your AGC, Krisp modelling
noise that's no longer there. Set Noise Suppression to None; disable Echo Cancellation, Noise
Reduction, Automatic Gain Control. Voice Activity detection is fine.

**In Windows, uncheck "Audio Enhancements"** on the K688's endpoint, so Windows' capture effects
don't process the signal before you see it.

Both belong in the wizard as checklist items with deep links, not buried in a README.

### 4.4 Licensing

VB-Cable is donationware. If this ever ships to anyone else, verify redistribution terms; if
unclear, have the wizard link to the vendor's page and walk the user through installing it. Not a
concern while the app stays personal.

---

## 5. Latency: configurable with enforced floors *(new in v4)*

### 5.1 Why the floor is where it is

Understand this before picking numbers, because it makes half the tuning knobs pointless.

**The 480-sample frame is irreducible.** Both models require exactly 10 ms of audio. If you set
the capture period to 2.67 ms you receive 128 samples per event — and still wait ~10 ms to
accumulate a full frame before any processing can begin. **Shrinking the capture period below the
frame size does not reduce accumulation delay.** It only reduces buffer padding and gives finer
scheduling granularity.

Total latency decomposes as:

```
  10 ms   frame accumulation          FIXED — cannot be reduced
+ 0/30 ms model look-ahead            RNNoise 0, DFN3 30 (2 frames)   [§6.3 experiment]
+  ~2 ms  inference (DFN3 mean)       measured, not assumed
+ 0-3 ms  limiter look-ahead          tunable
+  X ms   render period + ring depth  tunable
+  Y ms   VB-Cable internal buffer    tunable in its control panel, outside our process
```

### 5.2 Profiles

| Profile | Period | Ring depth | Limiter LA | Fast ≈ | Studio ≈ |
|---|---|---|---|---|---|
| **Minimum** (experimental) | 2.67 ms | 3 frames | 0 (soft clip) | ~16 ms | ~46 ms |
| **Low** | 5.33 ms | 4 frames | 1.5 ms | ~20 ms | ~50 ms |
| **Balanced** *(default)* | 10 ms | 16 frames | 3 ms | ~30 ms | ~60 ms |
| **Safe** | 20 ms | 24 frames | 3 ms | ~50 ms | ~80 ms |

Figures are estimates excluding the VB-Cable buffer. The app reports its own measured contribution
in telemetry; treat the table as a starting point, not a specification.

Minimum and Low use `IAudioClient3::InitializeSharedAudioStream` with a requested periodicity;
Balanced and Safe use ordinary `IAudioClient::Initialize` at or above the default engine period.
Not every driver honours small periods, and some report a small `minPeriod` then glitch at it —
which is what §5.3 exists to catch.

### 5.3 Enforced minimums and the startup guard

Configurability without guard rails just relocates the bug. Enforce in code:

- **Ring depth ≥ 3 frames.** One filling, one being processed, one draining. Below this you are
  guaranteed to underrun on any scheduling hiccup.
- **Period ≥ measured p99.9 inference time.** If a frame takes longer than the period, you
  underrun by construction, not by bad luck.
- **Startup calibration.** On launch, run the selected engine over ~30 seconds of live input,
  collect the inference-time distribution, and compare against the selected profile. If the
  profile can't be sustained, **refuse it and fall back one step**, with a tray notification
  explaining why. Never let the user select a config that will glitch.
- **Runtime demotion.** If overruns exceed a threshold over a rolling window, step down a profile
  automatically and notify (§5.6).

### 5.4 Real-time discipline

Non-negotiable inside the DSP loop: no `malloc`/`free`, no file or network I/O, no mutex
acquisition, no logging, no COM calls, no exceptions, no paged memory. All buffers allocated and
touched at init. Telemetry leaves via relaxed atomic stores into shared memory only.

```cpp
DWORD taskIndex = 0;
HANDLE hAv = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
AvSetMmThreadPriority(hAv, AVRT_PRIORITY_CRITICAL);
// ... AvRevertMmThreadCharacteristics(hAv) on teardown — leaking this degrades the whole system.
```

That is real MMCSS registration. `SetThreadPriority(THREAD_PRIORITY_TIME_CRITICAL)` alone, as the
original blueprint used, does not engage MMCSS.

### 5.5 CCD affinity — off by default

Ship disabled. Implement behind a config flag purely so Phase 0 can measure it. The workload is
~2% of one core, and AMD's 3D V-Cache Optimizer parks the non-V-Cache CCD during gaming — exactly
the die the original plan reserved. If Phase 0 shows no difference, which is the expected outcome,
delete the code rather than carrying dead complexity. If kept, discover topology at runtime via
`GetLogicalProcessorInformationEx(RelationCache, ...)`, grouping by L3 size. Never hard-code
`0xFFFF0000`.

### 5.6 Overrun policy

- **Pass through dry audio** for the missed frame. Never silence, never drop — both click; 10 ms
  of unprocessed audio is imperceptible.
- Increment an atomic overrun counter surfaced in telemetry.
- On sustained overruns: step down the latency profile, or fall back Studio → Fast, and raise a
  tray notification. **Degrade visibly, never silently.**

### 5.7 Device lifecycle and format policy

Implement `IMMNotificationClient`. Handle default-device change, disconnect, format change,
`AUDCLNT_E_DEVICE_INVALIDATED`, sleep/resume, and USB unplug mid-match. On any of these, output
silence from a clean state and rebuild both streams from a non-RT thread. This is the most common
cause of "my mic just stopped working" in real WASAPI apps.

**Format guard:** call `GetMixFormat()` at startup. If `nSamplesPerSec != 48000`, do not silently
proceed — warn clearly and point at the endpoint setting. With the K688 this should never fire,
but it will the day you plug in an XLR interface. Also check channel count: the mix format is
often 2 channels even for a mono mic. Take channel 0 by default, with a channel-select setting.

---

### 5.8 Clock drift between capture and render — the omission v5 fixes *(new in v5)*

**This is the most serious gap the comparison in §10 exposed, and it targets exactly the stated
quality bar: zero dropouts over an eight-hour session.**

The K688 clocks its ADC from its own crystal. VB-Cable runs on a separate software clock. These
are **independent clock domains**, and no two clocks run at the same rate. A 20 ppm mismatch —
entirely ordinary — is about 3.5 samples per minute, roughly 1.7 seconds of accumulated offset over
an eight-hour session.

The failure mode is precise and nasty: your rings drift monotonically toward full or toward empty.
Everything sounds perfect for twenty minutes. Then Ring B underruns, you get a click, it recovers,
and it clicks again a few minutes later, with increasing frequency. This is the single most common
bug in capture→render bridging applications, and it presents as "random crackling" that correlates
with nothing you can reproduce on demand. Every serious audio router implements a fix; VAC and
Sonarworks both expose "safety buffer" settings that exist because of it.

Nothing in v1 through v4.1 addressed this. The deep rings in §5.2 delay the symptom and guarantee
it will eventually happen anyway.

**Do not fix it by dropping or inserting frames.** That's the naive approach and it produces
audible clicks and gaps — a jolt every few minutes instead of a click every few minutes.

**The correct fix is adaptive resampling driven by a control loop:**

1. **Measure.** Track Ring B occupancy, heavily low-pass filtered — you want the slow trend, not
   the per-frame jitter. A simple leaky integrator over many seconds.
2. **Control.** Run a slow PI controller whose setpoint is the ring's target occupancy (half full)
   and whose output is a resampling ratio near 1.0, typically within ±100 ppm. Fons Adriaensen's
   *Controlling Adaptive Resampling* is the canonical reference for tuning this loop; Firefox's
   audio backend cites it for exactly this problem.
3. **Correct.** Apply that ratio with a variable-rate resampler on the **output** side, after all
   DSP. Ratio changes must be applied smoothly — a pitch shift of 100 ppm is far below the
   threshold of audibility, but a *step change* in ratio is a click.
4. **Never let the loop fight transients.** Time constants in the seconds-to-tens-of-seconds range.
   You are correcting a drift measured in samples per minute; reacting fast buys nothing and
   introduces instability.

**Where it sits in the pipeline:** after the limiter, before Ring B's consumer. The resampler
operates on already-final audio and is the last thing in the chain.

**Telemetry must expose it:** current resampling ratio in ppm, ring occupancy trend, and cumulative
correction applied. If the ratio pins at a rail, something is structurally wrong (usually a format
mismatch, §5.7) and you want to see that rather than hear it.

**Phase 0 must measure it.** The existing 8-hour pass-through test becomes the drift test: log Ring
B occupancy once per second for the full session and plot it. A monotonic trend is drift, and its
slope tells you the ppm mismatch of your specific hardware pair. **Run this before writing the
compensator, so you know what you're compensating.**

---

## 6. Model approach *(specified in v4)*

### 6.1 No training. Pretrained weights only.

We are not training, fine-tuning, or distilling anything. Both models ship with permissively
licensed pretrained weights that outperform anything achievable without a serious dataset and
weeks of GPU time.

| | Studio Mode — **default** | Fast Mode — fallback |
|---|---|---|
| Model | DeepFilterNet3, pretrained | RNNoise, pretrained |
| Runtime | ONNX Runtime, CPU EP | Reference C library, compiled in |
| License | Apache-2.0 / MIT | BSD-3-Clause |
| Algorithmic delay | 40 ms (20 ms window, 10 ms hop, 2-frame look-ahead) | 10 ms |
| Cost | ~1.7 ms/frame mean, ~5 ms max | < 0.5 ms/frame |
| Strength | Keyboard, clicks, non-stationary noise | Fans, AC, hum, coil whine |

**Studio Mode is the default** because your dominant noise is a mechanical keyboard — impulsive
and non-stationary, precisely RNNoise's weakness and DFN3's strength. The original blueprint had
this backwards.

Fast Mode is retained as the overrun fallback, as an A/B reference when judging output quality,
and because it's the easier engine to bring up first in Phase 2. If you never select it manually
after a month, deleting it in v5 is a legitimate simplification.

### 6.2 DeepFilterNet3 integration — where the real work is

Take the upstream pretrained weights exported to ONNX and run them on ONNX Runtime's CPU
execution provider with `intra_op_num_threads = 1`. More threads raise throughput and raise
jitter; you want the opposite trade. Upstream is Rust/tract; ORT is the natural C++ binding.

Prefer the **combined single-graph export** over the split encoder / ERB-decoder / DF-decoder
form — fewer dispatch boundaries per frame. Keep the split form available for debugging, since it
lets you inspect intermediate tensors.

**The part that isn't obvious: the STFT, inverse STFT, and the 32-band ERB filterbank are not
inside the ONNX graph.** You implement those yourself, and getting them bit-correct against the
reference implementation is the bulk of Phase 2 — more effort than wiring up the inference call.
Budget accordingly. Validate by processing a fixed test file through your implementation and the
reference `deep-filter` binary and comparing sample-for-sample.

**Do not quantize.** fp32 is comfortably fast enough here; int8 risks audible quality loss to save
compute you don't need.

### 6.3 Experimental: reduced look-ahead

30 of DFN3's 40 ms is its 2-frame look-ahead. Some ONNX exports allow running at 1 or 0 look-ahead
frames, taking Studio Mode from ~50 ms to ~30 ms end-to-end. The model was trained with that
context, so expect a quality cost. Build it as a toggle, A/B it by ear, and default it **off**.

### 6.4 RNNoise integration

Use the reference C library directly. It's self-contained and includes its own feature extraction,
so none of the STFT/filterbank work above applies. Compile it into the audio process.

**On AVX-512 — a deliberate deflation.** The original blueprint made hand-written AVX-512 kernels
its centerpiece. RNNoise costs under 0.5 ms per 10 ms frame: it already fits with roughly 20×
margin. Compile with `/arch:AVX512`, let the autovectorizer work, measure, and move on. Write
intrinsics only if a profile demands it, which it will not. Zen 5's full-width 512-bit datapaths
are real and impressive, and irrelevant to a workload this small.

There is no DirectML, no iGPU, no GPU path of any kind. The Ryzen 9000 iGPU is RDNA 2 with 2 CUs
(the original blueprint said RDNA 3), DirectML was deprecated in 2025, per-dispatch overhead
dominates at a 100 Hz cadence, and CPU inference already fits with margin.

### 6.5 Engine interface

```cpp
class INoiseFilter {
public:
    virtual ~INoiseFilter() = default;
    // RT-safe: no allocation, no locks, no I/O. Exactly 480 samples in and out.
    virtual void     ProcessFrame(const float* in, float* out) noexcept = 0;
    virtual float    LastVadProbability() const noexcept = 0;
    virtual void     SetAggression(float zeroToOne) noexcept = 0;
    virtual uint32_t AlgorithmicDelaySamples() const noexcept = 0;  // 480 vs 1920
};
```

`AlgorithmicDelaySamples()` exists because the engines have different path lengths. Without it you
cannot delay-align the crossfade or report honest latency.

### 6.6 Hot-swap: warm standby

Both engines run continuously; the inactive one's output is discarded. Swaps are then instant and
clean.

Starting an engine on demand doesn't work: DFN3 carries recurrent state and needs ~80–100 ms of
context before its output stabilises, so a cold swap gives ~10 frames of degraded audio that no
crossfade can hide. The original blueprint's "run both for a single transition frame" was based
on a model of the engines that doesn't match how they work.

Warm standby costs a permanent ~1.7 ms/frame. You have sixteen cores. Pay it.

Crossfade on **delay-aligned** streams — pad the shorter-delay engine's output by the 1440-sample
difference before mixing, or you'll hear a comb-filter swoosh:

```
Y(t) = (1 − α(t))·Y_old(t) + α(t)·Y_new(t),   α: 0 → 1 over 5 ms (240 samples)
```

Use an equal-power (cos/sin) ramp — the outputs are correlated but not identical, and a linear
crossfade between correlated signals dips in the middle.

---

## 7. DSP chain

### 7.1 Chain

```
mic ─► DC blocker ─► [ ENGINE ] ─► high-pass ─► VAD-gated AGC ─► limiter ─► cable
```

Only the DC blocker precedes the model, because a DC offset is a genuine hardware defect that
degrades inference. Everything else sits behind it: both networks were trained on unfiltered noisy
speech, and heavy pre-filtering shifts your input off the training distribution.

**DC blocker.** `y[n] = x[n] − x[n−1] + R·y[n−1]`, corner at `fc ≈ fs·(1−R)/2π`. The original
blueprint's R = 0.995 gives **38 Hz** at 48 kHz, not the ~15 Hz it claimed. Use **R = 0.998**.

**High-pass.** 2nd-order Butterworth biquad, Q = 0.7071, **default 80 Hz**, exposed as a setting.
The original's "2nd-order Linkwitz-Riley" was a category error — LR is a crossover topology whose
defining property only means anything when you sum high and low bands, and there is no low band
here. Per §1.1 this filter is nearly redundant against the K688's own 70 Hz rolloff; keep it as
insurance against boom-arm thumps, don't make it steep.

**AGC, VAD-gated.** Target −18 to −12 dBFS RMS, **gain frozen when VAD reports non-speech.** An
ungated AGC hunts upward during pauses and amplifies whatever residual the model left — a
breathing hiss that appears and disappears, and the most common reason a DIY suppressor sounds
worse than raw audio. Hysteresis: open at 0.6, close at 0.3, 200 ms hold. Attack 50 ms, release
500 ms.

**Limiter.** Look-ahead brickwall at −1.0 dBFS, look-ahead per latency profile (§5.2). At the
Minimum profile, substitute a soft clipper.

**Aggression.** Neither engine has a native aggression parameter — the original specified a slider
with no mechanism. Shape the band gains before application:

```
g' = max(g_min, g^γ)
```

γ from the slider (0.4 at 0%, 1.0 at 50%, 2.5 at 100%), gain floor `g_min` ≈ −20 dB. **The floor
matters more than the exponent** — hard-zeroing bands is what produces the "underwater" artifact.
Never let `g_min` reach zero. For DFN3, apply the same shaping to the 32 ERB gains before the
deep-filter stage.

**Optional compressor.** EasyEffects, OBS and every broadcast voice chain put a gentle compressor
between the AGC and the limiter. AGC corrects slow level drift; a compressor controls
sentence-to-sentence dynamics. 2:1 ratio, −18 dBFS threshold, 10 ms attack, 100 ms release is a
sane starting point, off by default. Worth adding once everything else works — it does more for
perceived consistency on voice comms than another 5% of noise suppression.

**Out of scope:** acoustic echo cancellation. Needs a loopback reference and a different algorithm
class; headphones assumed. Say so in the UI so it isn't filed as a bug.

### 7.2 Bypass and dry/wet — get the delay right *(new in v5)*

v4.1 specified a bypass toggle for A/B comparison. As specified it would be **broken**, and subtly
so.

Studio Mode has 40 ms of algorithmic delay; the dry signal has none. A naive bypass therefore jumps
the output 40 ms forward in time — a discontinuity you'll hear as a click, and worse, an A/B
comparison where the two sides don't line up. You'd be comparing processed-and-delayed against
raw-and-early and forming opinions about the difference.

**Delay-match the dry path.** Keep a delay line on the raw signal matched to
`AlgorithmicDelaySamples()` of whichever engine is active, so bypass is sample-aligned and
click-free. The engine interface already exposes the number; this is what it's for.

Once the dry path is delay-matched, **a dry/wet mix falls out for free** — and it's more useful
than a binary toggle:

```
out = (1 − mix)·dry_delayed + mix·wet
```

Partial suppression frequently sounds more natural than full. A mix around 0.8 keeps a little room
tone, which reads as "good microphone" rather than "noise-gated". Expose it as a slider with
default 1.0; you may well settle lower.

### 7.3 VAD timing: hangover and pre-roll *(new in v5)*

werman's plugin exposes three VAD parameters, and the third is one most implementations miss:

- **VAD threshold** — the probability above which audio counts as speech.
- **Grace period (hangover)** — keep treating audio as speech for N ms after the last detection.
  Without it, the tails of words get chopped. ~200 ms.
- **Retroactive grace (pre-roll)** — keep the audio from N ms *before* detection fired. Requires a
  small lookahead buffer, and prevents the clipped consonant at the start of a word — the "…ello,
  can you hear me" effect.

We are not gating output on VAD (both models attenuate rather than mute, which is better), so this
applies to **AGC gating** (§7.1) and to the dry/wet automation if you add it. The hangover
hysteresis in §7.1 covers the tail case; **pre-roll is the genuinely new item.** Since we already
tolerate 40 ms of delay, a 50 ms pre-roll buffer costs nothing we care about.

---

## 8. UI architecture *(new in v4.1)*

v4 specified a tray UI in one line and left everything behind it unstated. This section is that
everything.

### 8.1 Separate process, not a separate thread

**Decision: the UI is its own process.** The reasoning is not performance — a tray icon costs
nothing. It is that **UI work blocks in places you cannot predict**, and in the same address space
that becomes your problem:

- `Shell_NotifyIcon` can hang for **seconds** if Explorer is busy, restarting, or wedged. This is
  not exotic; it happens on ordinary desktops.
- Common dialogs (file, settings, device pickers) pump their own modal message loop, so your
  message handling stops being cooperative for the duration.
- DWM composition can stall a redraw arbitrarily.
- A COM STA apartment for shell integration introduces re-entrancy you don't control.

None of that matters in a process that only draws pixels. All of it matters if that process also
owns your real-time buffers. A separate process gives hard isolation: the UI can hang, crash, leak,
or be killed from Task Manager, and your voice keeps flowing. The audio process should keep working
with **no UI running at all** — that is the correctness test.

**The alternative is defensible.** Single process, UI on its own normal-priority thread, audio on
MMCSS, sharing nothing but a parameter block. It mostly works, and it ships one binary with no IPC.
But "mostly" is carrying real weight in that sentence, and the failure mode is your mic dying
mid-match.

**Why the choice is cheap either way:** the abstraction is identical. A parameter snapshot in one
direction, a telemetry block in the other. Build two processes; if the plumbing annoys you,
collapsing into one later touches only the transport, not the design.

### 8.2 Threads inside the UI process: one

A single thread running a standard Win32 message pump, with a `WM_TIMER` at ~20 Hz that reads the
telemetry block and invalidates the relevant regions. No worker thread, no polling loop, no
async anything. The UI has no work that justifies a second thread, and adding one only creates
opportunities to draw from the wrong thread.

The level meter is the only element wanting faster updates; 20 Hz is enough for a meter with a
sensible peak-hold and decay, and it costs nothing.

### 8.3 UI → audio: the parameter snapshot

**This is the mechanism v4 never specified, and getting it wrong is the classic way to put a
priority inversion into a real-time path.** Do not use a mutex. Do not use a message queue the RT
thread has to drain.

Use a **double-buffered parameter snapshot with an atomic index flip**:

```cpp
struct Params {                 // POD only — no pointers, no allocation, no strings
    float  aggression;          // 0..1
    float  hpfCutoffHz;
    float  agcTargetDbfs;
    float  limiterCeilingDbfs;
    uint32_t engine;            // 0 = Fast, 1 = Studio
    uint32_t bypass;            // 0 / 1
    // ... all RT-relevant settings, fixed size
};

alignas(128) static Params        g_params[2];
alignas(128) static std::atomic<uint32_t> g_active{0};

// Non-RT thread (IPC handler) — writes the inactive slot, then publishes:
void PublishParams(const Params& p) {
    const uint32_t next = 1u - g_active.load(std::memory_order_relaxed);
    g_params[next] = p;                                   // plain stores
    g_active.store(next, std::memory_order_release);      // publish
}

// RT thread — one acquire load per frame, wait-free:
const Params& snap = g_params[g_active.load(std::memory_order_acquire)];
```

Properties that matter: the RT thread never blocks, never allocates, and never sees a half-written
struct. The writer never waits. There is no lock to invert.

**Smoothing is the RT thread's job, not the UI's.** A gain or cutoff that jumps discontinuously
between frames will click. Ramp changed parameters over 10–20 ms inside the DSP thread after
reading the snapshot. The UI publishes targets; the RT thread reaches them.

**Structural changes are not parameters.** Switching input device, changing latency profile, or
enabling the dry tap require tearing down and rebuilding streams. Those go through the command
channel (§8.5) and are executed by a non-RT thread that stops the pipeline, rebuilds, and restarts
it — never by mutating state under a running RT thread.

### 8.4 Audio → UI: the telemetry block

A shared-memory region written **only** by the DSP thread, using relaxed atomic stores, once every
~100 frames (~1 s). The UI reads it whenever it likes; a torn read of a stale counter is harmless
and the UI must be written to tolerate one.

Contents:

- Inference time p50 / p99 / p100 (`QueryPerformanceCounter`)
- Ring A and Ring B occupancy, current and high-water
- Overrun count since start, and since last reset
- VAD state and current input RMS (for the meter)
- Measured end-to-end delay estimate
- Active engine, active latency profile, and whether an automatic demotion has occurred

Never a queue, never a log, never a string the RT thread has to format.

### 8.5 Command channel

A local named pipe, UI → audio, for discrete commands: select device, change profile, start/stop
dry tap, reset counters, shut down. Handled on a **dedicated non-RT thread** in the audio process
that translates commands into either a `PublishParams` call (§8.3) or a pipeline rebuild (§8.4).
The RT thread never touches the pipe.

Low rate, tiny payloads, no back-pressure concerns. Do not be tempted to push telemetry back over
the pipe — shared memory exists precisely so the RT thread never participates in IPC.

### 8.6 Controls

- **Input device selector** — the primary interaction. Run the app, point it at the mic.
- Engine: Studio (default) / Fast / Auto
- Latency profile: Minimum / Low / Balanced (default) / Safe
- Aggression slider
- High-pass cutoff
- Output gain target
- **Input level meter** — needed for the §4.2 gain-staging step, so build it in Phase 1
- **Bypass toggle** — build this early; you will use it constantly for A/B
- Dry tap on/off (§11)

**No hotkeys, no push-to-talk, no global input capture of any kind.** Removed by request, and it
happens to eliminate the one anti-cheat-adjacent element of the design (§3).

Removed relative to the original blueprint: the "Hardware Control Toggle" (CPU vs iGPU), since
there is no iGPU path. CCD pinning stays a hidden config-file flag, not a UI element.

### 8.7 Lifecycle

- The audio process is the service; the UI is a viewer. Starting the UI attaches to a running
  audio process, or launches one if absent.
- Closing the UI does **not** stop audio. Only an explicit Exit does.
- The audio process must run correctly headless — this is worth an explicit test, because it's the
  cheapest possible proof that no UI dependency has leaked into the RT path.
- Settings persist in a plain config file written by the audio process, not the UI, so a UI crash
  cannot corrupt them mid-write.

---

## 9. Concurrency and performance *(new in v4.1)*

### 9.1 The framing: this is a deadline problem, not a throughput problem

You have ~2 ms of work arriving every 10 ms — roughly 17% of one core. Threads make more work
happen per unit time. That is not the problem. The problem is that **each individual frame must
finish before its deadline, every time, for eight hours**.

**Optimise p99.9 frame time, not mean frame time.** Most parallelism trades the wrong way on that
axis: every synchronisation point adds variance, and variance is what produces the dropout at hour
six. Several things in this section make the mean *worse* and are still correct.

### 9.2 Where the design is already parallel — and why

Capture / DSP / render / dry-tap-writer are four threads joined by lock-free rings. This is
**pipeline parallelism, and its purpose is isolation, not speed.** The WASAPI threads can block on
their events and the writer can block on disk without any of it reaching the RT path. Keep this
shape. Do not add threads to it casually.

### 9.3 Genuine opportunities, ranked

**1. Run the two engines concurrently.** Warm standby (§6.6) processes every frame through both
DFN3 and RNNoise. Sequentially that's ~2.2 ms; they share no state, so two MMCSS threads with a
single join fence gets ~1.7 ms. Saves ~0.5 ms of a 10 ms budget — marginal, but it is the only
genuinely independent task pair in the whole design and the join is trivial. Worth doing.

**2. SIMD inside the frame, not more threads.** The STFT/ISTFT is two 960-point real FFTs per
frame and the ERB filterbank is a small matrix multiply. That is where vectorisation pays. **Use a
real FFT library** (pffft, or IPP if you want dispatch across SIMD levels) rather than writing one.
Data parallelism, no synchronisation, no jitter — strictly better than thread parallelism here.

**3. Test `intra_op_num_threads = 2`, and judge on the tail.** §6.2 specifies 1, which remains the
default. But run the experiment once. ORT's thread pool spin-waits, and for graphs this small the
distribution cost can exceed the parallel gain: expect mean to fall and p99.9 to rise. **Decide on
p99.9.** If it rises, revert and never revisit.

### 9.4 What will actively hurt

- **Parallelising across frames.** DFN3's GRU carries hidden state — frame *N+1* mathematically
  depends on *N*. This is not a missed optimisation, it's a correctness violation.
- **Splitting a 480-sample frame across threads.** Synchronisation cost exceeds the work.
- **Any thread pool in the RT path.** Allocation, lock contention, and priority inversion the
  moment your MMCSS-critical thread waits on a normal-priority worker.
- **Parallelising the post chain.** IIR filters have feedback: sample *n* needs *n−1*. Structurally
  sequential. The DC blocker, high-pass, AGC and limiter are all in this category.

### 9.5 The things that actually move p99.9

**Denormals — do this first.** During suppression the signal spends long stretches at near-zero
amplitude, which is exactly where IIR filter state and neural activations drift into denormal
floats. Denormal handling can cost 50–100× on a single operation, and it strikes precisely when the
suppressor is working hardest. Set once per audio thread, at thread start:

```cpp
_MM_SET_FLUSH_TO_ZERO_MODE(_MM_FLUSH_ZERO_ON);
_MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
```

**This is the single highest-value change in this section.** Set it on every thread that touches
float DSP, including the engine threads from §9.3.

**Cache footprint — and the first real argument for CCD choice.** DFN3 is ~2.3M fp32 parameters,
roughly 9 MB of weights. That blows past the 1 MB per-core L2 and lives in L3. Warm standby means
both engines' working sets are resident.

This is the first mechanism in the entire project that would genuinely benefit from the 3D V-Cache
die — **and it argues for the V-Cache CCD, the opposite of the original blueprint's isolation
scheme.** Add this as a fourth Phase 0 configuration to measure (§12). If it shows a real p99.9
improvement, that is a reason to pin; the original cache-hygiene rationale never was.

**Cache-line-pad the ring indices.** Producer and consumer atomics sharing a cache line means every
write invalidates the other core's copy — false sharing on the hottest structure you have. Align
each index to 128 bytes on Zen 5, and keep them in separate structs rather than trusting a compiler
to pad for you.

**`VirtualLock` the RT buffers** so nothing in the audio path can page-fault. This is also
Microsoft's stated requirement for real-time audio code paths: all buffers and all code in the
process path must be non-pageable.

**Pre-touch every allocation at init.** Allocating at startup is not enough — first-touch page
faults are still page faults. Write a byte to every page before the RT thread starts.

### 9.6 Measurement discipline

Every claim in this section is testable and several are likely to be wrong on your specific machine.
Instrument first: histogram frame times with `QueryPerformanceCounter` into pre-allocated buckets,
report p50 / p99 / p99.9 / p100 in telemetry, and change one thing at a time. A change that improves
the mean and worsens p99.9 is a regression, however good the benchmark looks.

---

## 10. Comparison with existing implementations *(new in v5)*

A final pass against how comparable programs are actually structured. Each row is what they do and
what, if anything, we take from it.

| Project | Structure | What we take |
|---|---|---|
| **NoiseTorch** (Linux) | RNNoise in a PipeWire/PulseAudio virtual source. Small because Linux exposes a user-mode virtual device API | Confirms our shape; the reason our Windows equivalent needs VB-Cable at all |
| **werman/noise-suppression-for-voice** | One `RnNoiseCommonPlugin` core + six thin format adapters (LADSPA, LV2, VST2/3, AU, AUv3). Exposes VAD threshold, grace period, **retroactive grace** | **Core-library layering (§2.1)** and **VAD pre-roll (§7.3)** — both adopted |
| **DeepFilterNet** | Core crate + `deep-filter` CLI + LADSPA plugin. CLI has a `--compensate-delay` flag | **Offline mode as a first-class artifact (§11)**, and delay compensation as an explicit concept (§7.2) |
| **noisegate** (Windows, Rust) | 3 MMCSS threads, lock-free SPSC rings, 480-sample frames end-to-end, tray UI, VB-Cable | Independent confirmation of our transport design — this is the closest existing analogue |
| **EasyEffects / OBS chains** | Full chain: gate → expander → **compressor** → limiter, with suppression as one node | **Optional compressor stage (§7.1)** |
| **CamillaDSP** | Config-file-driven WASAPI in/out, hot-reloadable, headless with a separate GUI | **Config hot-reload and headless operation (§12)** |
| **VAC / Sonarworks** | Both expose "safety buffer" settings whose entire purpose is absorbing clock drift | **The §5.8 omission** — this is where it surfaced |
| **Firefox / libcubeb** | Explicitly moved from frame-dropping to smooth resampling-ratio changes for WASAPI drift, citing Adriaensen | **The correct drift fix (§5.8)** |
| **Equalizer APO** | System-wide VST host on Windows, driven by a text config | An alternative delivery path a VST3 front-end would unlock (§2.1) — not planned |
| **NVIDIA Broadcast / Krisp** | Closed SDKs behind virtual devices | Nothing structural. Krisp inside Discord is the baseline to beat, and the reason §4.3 exists |

### 10.1 Know your baseline

Discord ships Krisp free; OBS ships RNNoise. Your defensible differentiators are: system-wide
rather than per-app, fully local, no telemetry, DFN3-class quality, complete control over the
chain, and the dry tap (§13). "Lower latency" is not on that list.

### 10.2 What the comparison did *not* find

Worth stating, so the absence is deliberate rather than accidental. No comparable project does
per-application processing, machine-specific noise profiling, or user-trained models — all three
are considerably more work than they're worth. Nobody offloads a model this small to a GPU. And
nobody in this space writes their own virtual audio driver unless selling a product.

---

## 11. Offline mode and the regression harness *(new in v5)*

`deep-filter` is a file-in/file-out binary; werman's core is testable without any audio host. Both
projects can validate their DSP without a microphone, and we should too.

Build a **second front-end over `denoise-core` (§2.1): a CLI that processes a WAV file through the
identical chain.** Same engines, same filters, same parameters, no WASAPI, no threads, no rings.

This is not a nice-to-have. It is:

- **The only sane way to validate the STFT/ISTFT/ERB work in Phase 2.** Process a fixed test file
  through your implementation and through the reference `deep-filter` binary, compare
  sample-for-sample. Debugging that against a live microphone is miserable.
- **A regression harness.** Keep a corpus: keyboard clatter, fan hum, a door, silence, clipping-loud
  speech, and a clean control. Run it after every change. Compare against the previous build's
  output — bit-identical where you expected no change, and listenable where you expected one.
- **A benchmark.** Frame-time distributions with no audio-device variance in the way, which is what
  §9.6 needs to judge p99.9 honestly.
- **The offline path for dry-tap recordings (§13)**, where you can afford much heavier settings.

Include a `--compensate-delay` equivalent so output files align with their inputs.

**Build this in Phase 2, before the engines are wired into the live path.**

---

## 12. Operational concerns *(new in v5)*

The unglamorous things every shipped program in §10 has and none of v1–v4.1 mentioned.

- **Single instance.** A named mutex at startup. Two instances fighting over the same capture
  endpoint and both writing to CABLE Input produces symptoms you will not diagnose quickly.
- **Autostart.** Run at login via the `Run` registry key or a Startup shortcut — **not** a Windows
  service (services can't easily reach per-user audio endpoints, and it's needless privilege).
- **Startup ordering.** At login your USB mic may not be enumerated yet, and VB-Cable may still be
  initialising. Retry device acquisition with backoff for ~30 s rather than failing and sitting
  dead in the tray.
- **Watchdog.** If the DSP thread stops producing for >1 s, tear down and rebuild the pipeline
  automatically. Log it, surface it in telemetry, keep going.
- **Config file.** Plain text (TOML or JSON), written by the audio process (§8.7), hot-reloaded on
  change like CamillaDSP. Never write it from the UI process.
- **Logging.** An out-of-band logger on a non-RT thread fed by a lock-free queue the RT thread can
  push to without blocking. The RT thread never touches a file, never formats a string. Rotate the
  log; cap total size.
- **Crash handling.** A minidump handler so an 8-hour session that dies at hour seven leaves
  evidence. Without it you get "it crashed overnight" and nothing else.
- **Clean shutdown.** Stop streams, flush the dry tap, `AvRevertMmThreadCharacteristics`, release
  the mutex. Leaking MMCSS registrations degrades the whole system, not just this app.
- **Version stamping.** Put the build version in telemetry and the log header. Once you have a
  regression corpus you'll want to know which build produced a given artifact.
- **No auto-update.** Nothing that phones home, downloads, or self-modifies — that's both
  unnecessary for a personal tool and precisely the behaviour profile you don't want on a machine
  running kernel anti-cheat (§3).

---

## 13. Dry tap — preserving an unprocessed original *(new in v4)*

Ring A already holds the raw pre-processing stream. Add a **second consumer** that writes it
verbatim to disk from a low-priority, non-RT thread — no DSP, no suppression, no AGC.

Costs almost nothing to build and resolves the quality question cleanly: the live path gets 48 kHz
processed audio where Opus makes any residual artifact moot, while anything worth keeping retains
a pristine original you can denoise offline later with much heavier non-realtime models. The
realtime path's compromises then cost you nothing archivally.

Off by default. Disk writes never touch the RT thread.

---

## 14. Roadmap

**Phase 0 — Measure. Gate everything.**
Pass-through only: capture → Ring A → Ring B → render into CABLE Input. No DSP.
- Zero-dropout verification over a full 8-hour day including a long gaming session
- **Clock drift measurement (§5.8): log Ring B occupancy once per second for the full 8 hours and
  plot it. The slope gives the ppm mismatch of your specific hardware pair. Do this before writing
  any compensator.**
- DPC latency baseline (LatencyMon), app idle vs running
- Frame-time comparison in one game with PresentMon, app off vs on
- The §5.5 pinning comparison, now **four** configurations: none / non-V-Cache CCD / one reserved
  core / **V-Cache CCD** — the last added in v4.1, since §9.5 gives the first real mechanism
  (DFN3's ~9 MB weight footprint) that would benefit from it
- End-to-end latency measured per profile, including the VB-Cable contribution, by impulse
  cross-correlation — this populates the real numbers behind §5.2

*Exit criteria:* zero dropouts over 8 hours; DPC and frame-time deltas inside run-to-run variance.

**Phase 1 — Transport hardening.** Device lifecycle, format guard, overrun policy, MMCSS, latency
profiles with the startup calibration guard (§5.3), first-run wizard including gain staging and
the Discord/Windows checklist. **Denormal flags, `VirtualLock`, pre-touched allocations and
cache-line-padded ring indices (§9.5) — these are cheap now and painful to retrofit.** Build the
input level meter here, since gain staging depends on it. Delete pinning code if Phase 0 says it
does nothing.

**Phase 1.5 — Drift compensation (§5.8).** Adaptive resampler on the output side, PI control loop
off filtered Ring B occupancy, ratio and trend in telemetry. Validate by repeating the 8-hour test
and confirming occupancy is now flat. **This must work before the engines go in**, or you will
spend Phase 2 debugging clicks that have nothing to do with your model.

**Phase 2 — Engines.** Build `denoise-core` and the offline CLI (§2.1, §11) **first**. RNNoise first: self-contained, verifiable against the reference, validates
the whole framing path cheaply. Then DFN3 — **STFT / ISTFT / ERB filterbank first, validated
against the reference binary sample-for-sample, before touching ONNX Runtime.** Then warm standby
and the delay-aligned crossfade.

**Phase 2.5 — Regression corpus (§11).** Assemble the test corpus and wire it into the build. Cheap
now, and it is what makes every later change safe.

**Phase 3 — DSP and UI.** Filter chain, VAD-gated AGC, limiter, aggression mapping, parameter
snapshot and command channel (§8.3, §8.5), tray UI process, telemetry block, **delay-matched bypass and dry/wet
mix (§7.2)**, VAD pre-roll (§7.3), dry tap. Verify the audio process runs correctly with no UI
attached (§8.7).

**Phase 3.5 — Performance pass.** Frame-time histogram in telemetry, then one change at a time:
concurrent engines, FFT library selection, the `intra_op_num_threads = 2` experiment. Judge every
change on p99.9 (§9.6).

**Phase 3.5 — Operational hardening (§12).** Single-instance mutex, autostart with retry backoff,
watchdog, config hot-reload, out-of-band logger, minidump handler, clean shutdown.

**Phase 4 — Live with it for a month.** Then delete what you don't use: Fast Mode, the reduced-
look-ahead toggle, the pinning flag, unused profiles. Real work, not a formality.

---

## 15. Decision log

| # | Decision | Rationale | Status |
|---|---|---|---|
| D1 | WASAPI + VB-Cable; no APO | Componentized APO model forbids global registration; `audiodg` enforces signing | Final |
| D2 | No custom kernel driver | EV certificate requires a verified legal entity; 2–4 months for a nicer device name | Final |
| D3 | VB-Cable over open-source alternatives | OSS options need test signing, which anti-cheat rejects; VB-Cable is production-signed. Its value is the signature, not the code | Final |
| D4 | No DirectML / iGPU / GPU path | iGPU is RDNA 2 / 2 CU; DirectML deprecated 2025; dispatch overhead dominates at 100 Hz | Final |
| D5 | No sidetone | User does not monitor own voice | Final |
| D6 | **Latency configurable via profiles, Balanced default** | Not a design *target*, but worth exposing; floors are enforced in code rather than trusted to the user | **New in v4** |
| D7 | **10 ms frame accumulation acknowledged as an irreducible floor** | Both models require 480 samples; sub-frame capture periods don't reduce it | **New in v4** |
| D8 | **Startup calibration guard; refuse unsustainable profiles** | Configurability without guard rails just relocates the bug | **New in v4** |
| D9 | Studio Mode (DFN3) default | Keyboard clatter is non-stationary — RNNoise's weakness, DFN3's strength | Final |
| D10 | Fast Mode retained | Overrun fallback, A/B reference, easier Phase 2 bring-up | Revisit Phase 4 |
| D11 | **Pretrained weights only, no training or fine-tuning** | Permissively licensed and better than anything reachable without a dataset and GPU weeks | **New in v4** |
| D12 | **ONNX Runtime CPU EP, combined graph, `intra_op_num_threads = 1`** | Natural C++ binding; fewer dispatch boundaries; threads trade jitter for throughput, wrong direction | **New in v4** |
| D13 | **STFT / ISTFT / ERB implemented by us and validated first** | Not in the ONNX graph; the bulk of Phase 2 | **New in v4** |
| D14 | **No quantization** | fp32 fits with margin; int8 risks quality for compute we don't need | **New in v4** |
| D15 | **No hand-written AVX-512** | RNNoise fits with ~20× margin; `/arch:AVX512` and the autovectorizer suffice | **New in v4** |
| D16 | **No hotkeys or global input capture** | User request; also eliminates the one anti-cheat-adjacent element | **New in v4** |
| D17 | **HPF default raised to 80 Hz, insurance only** | K688 capsule already rolls off below 70 Hz | **New in v4** |
| D18 | **No resampling path; 48 kHz guard instead** | 48 kHz is the K688's native ceiling; the guard fires only if an XLR interface appears later | **New in v4** |
| D19 | **Gain staging is a first-run wizard step** | Under-driven dynamic mic + digital boost raises the noise floor the model must fight | **New in v4** |
| D20 | **Dry tap for unprocessed capture** | Live path can be lossy where Opus dominates; archival path stays pristine | **New in v4** |
| D21 | Anti-cheat hygiene as a hard requirement | Secure Boot + HVCI on, no hooks, no injection, no obfuscation | Final |
| D22 | CCD pinning off by default, behind a flag | AMD parks the non-V-Cache CCD during gaming; benefit unarticulable | Delete after Phase 0 |
| D23 | Real MMCSS "Pro Audio" registration | `SetThreadPriority` alone doesn't engage MMCSS | Final |
| D24 | Warm standby for engine swaps | DFN3 needs ~100 ms recurrent context | Final |
| D25 | HPF after the model; AGC VAD-gated | Preserves training distribution; prevents noise pumping in pauses | Final |
| D26 | DC blocker R = 0.998 | Original R = 0.995 gave 38 Hz, not the claimed 15 Hz | Final |
| D27 | Butterworth, not "Linkwitz-Riley" | LR is a crossover topology with no low band to sum | Final |
| D28 | Aggression = band-gain exponent + floor | Original specified a slider with no mechanism | Final |
| D29 | AEC out of scope | Needs a loopback reference; headphones assumed | Final |
| D30 | Downstream config is part of the product | Double-processing is the likeliest cause of "it sounds worse" | Final |
| D31 | Phase 0 measurement gate precedes inference work | Transport, not inference, is where the risk concentrates | Final |
| D32 | **UI is a separate process, not a thread** | `Shell_NotifyIcon` can hang for seconds; modal dialogs pump their own loops; hard isolation means a UI crash can't take the mic down. Abstraction is identical either way, so collapsing later is cheap | **New in v4.1** |
| D33 | **One thread in the UI process, 20 Hz timer** | No UI work justifies a second thread; more threads only create wrong-thread drawing bugs | **New in v4.1** |
| D34 | **UI → RT via double-buffered parameter snapshot with atomic index flip** | Wait-free on the audio side; no lock to invert. Never a mutex, never a queue the RT thread drains | **New in v4.1** |
| D35 | **Parameter smoothing happens in the RT thread** | UI publishes targets; discontinuous jumps between frames click | **New in v4.1** |
| D36 | **Structural changes rebuild the pipeline from a non-RT thread** | Device and profile changes are not parameters; never mutate under a running RT thread | **New in v4.1** |
| D37 | **Audio process must run headless** | Cheapest possible proof no UI dependency leaked into the RT path | **New in v4.1** |
| D38 | **Optimise p99.9, not mean** | Deadline problem, not throughput problem. A change that improves mean and worsens p99.9 is a regression | **New in v4.1** |
| D39 | **Denormal flush-to-zero on every float DSP thread** | 50–100× penalty, triggered precisely when suppression is working hardest. Highest-value single change | **New in v4.1** |
| D40 | **Concurrent engines under warm standby** | Only genuinely independent task pair; ~0.5 ms of a 10 ms budget, single join fence | **New in v4.1** |
| D41 | **No thread pools, no cross-frame or intra-frame parallelism** | GRU state makes frames sequential by construction; IIR feedback makes the post chain sequential; pools bring allocation and priority inversion | **New in v4.1** |
| D42 | **Use an established FFT library; SIMD over threads** | STFT/ERB is where vectorisation pays, with no synchronisation cost | **New in v4.1** |
| D43 | **Cache-line-pad ring indices; `VirtualLock` and pre-touch RT buffers** | False sharing on the hottest structure; page faults in the RT path | **New in v4.1** |
| D45 | **Clock drift compensation via adaptive resampling** | Mic crystal and VB-Cable are independent clock domains; ~20 ppm is ~1.7 s of offset over 8 hours. Rings drift monotonically and click forever after. Frame drop/insert is the wrong fix — smooth ratio changes under a slow PI loop | **New in v5 — the biggest gap found** |
| D46 | **Drift measured in Phase 0, compensated in Phase 1.5, before engines** | Otherwise Phase 2 is spent debugging clicks that have nothing to do with the model | **New in v5** |
| D47 | **`denoise-core` as a standalone library with no Windows dependency** | Every mature project in this space separates algorithm from delivery; retrofitting means untangling WASAPI out of DSP code. Buys the offline harness immediately | **New in v5** |
| D48 | **Offline CLI front-end + regression corpus** | Only sane way to validate STFT/ISTFT/ERB against the reference; also the benchmark surface for p99.9 work | **New in v5** |
| D49 | **Delay-matched bypass; dry/wet mix falls out of it** | A naive bypass jumps output 40 ms forward — a click, and an A/B where the sides don't align. Partial mix often sounds more natural than full suppression | **New in v5** |
| D50 | **VAD pre-roll (retroactive grace) in addition to hangover** | Prevents the clipped consonant at word onset; costs ~50 ms we already tolerate | **New in v5** |
| D51 | **Optional compressor between AGC and limiter** | Standard in every broadcast voice chain; does more for perceived consistency than more suppression. Off by default | **New in v5** |
| D52 | **Operational baseline: single-instance mutex, autostart with retry backoff, watchdog, hot-reloadable config, out-of-band logger, minidump handler, clean MMCSS teardown** | Every shipped program in §10 has these; none of v1–v4.1 mentioned any | **New in v5** |
| D53 | **No auto-update, no phone-home** | Unnecessary for a personal tool, and the wrong behaviour profile on a machine running kernel anti-cheat | **New in v5** |
| D54 | **No per-app processing, noise profiling, or user-trained models** | No comparable project does these; all cost far more than they return | **New in v5** |
| D44 | **V-Cache CCD added as a Phase 0 configuration** | DFN3's ~9 MB weights exceed L2 and live in L3 — the first real mechanism favouring the V-Cache die, and it points opposite to the original blueprint's isolation scheme | **New in v4.1** |
