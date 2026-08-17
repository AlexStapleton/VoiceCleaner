# Phase 2c — Hardware Validation Findings

_Date: 2026-08-16 · Context: manual hardware check of the merged Phase 2c lean-engine-swap on the author's machine
(fifine K688 mic → CABLE Input, Protocol Max headphones). Worker = Debug build._

## Status

- **Worker health: GOOD.** Lean-RNNoise run: `bridge running: engine=rnnoise, warm_standby=off, dsp_chain=on`,
  frame-time **mean ~2.0 ms, p99 ~2.5 ms, max ~4.6 ms, 0% over the 10 ms budget, 0 dropouts** steady-state. The
  RT path is healthy.
- **By-ear gap-free-switch validation: STILL PENDING.** The actual "does the lean/warm switch sound gap-free and
  click-free" judgment was not completed — the session was spent getting monitoring + latency usable first. Resume
  with `test-phase2c.ps1` (below) once monitoring is set up.

## Finding 1 (IMPORTANT, product-blocking) — the shipped configs have ~5 s of latency

**Symptom:** ~5 seconds from talking to hearing the monitored output.

**Root cause:** it is the **render ring buffer**, not the engine. All `drift*.toml` configs default `ring_frames`
to **1024** and the bridge primes ringB half-full (`ringB.capacity()/2` = 512 frames). Each frame is 480 samples =
**10 ms**, so 512 primed frames = **5.12 s** of buffered audio. The drift controller's setpoint is also
`ring_frames/2`, so it holds occupancy near that. These are **clock-drift *test* configs** (big buffer to ride out
drift over multi-hour runs), never real-time-latency configs.

**Evidence** (`voicecleaner-worker --config test-lean-rnnoise.toml --ring N --seconds 4`):

| `ring_frames` | steady ringB occupancy | buffered latency | startup underruns |
|---|---|---|---|
| 1024 (default) | ~512 frames | **~5.1 s** | 0 |
| 16 | ~1–2 frames | **~10–20 ms** | 1 |
| 8 | ~1 frame | ~10 ms | 5 |
| 4 | ~0–1 frame | ~10 ms | 7 |

Underruns are **startup transients only** (they occur during priming/convergence in the first ~second, then stop);
occupancy is stable afterward. Smaller ring = fewer primed frames = more startup glitches but lower latency.
`ring_frames = 16` (~15 ms buffered, 1 startup glitch) is a good real-time default.

**Real end-to-end latency with a small ring** ≈ ring buffer (~15 ms) + engine algorithmic delay (DFN3 1440 samples
= 30 ms; note DynamicEngine pins output delay to 1440 even on RNNoise) + WASAPI capture/render periods (~tens of ms)
≈ **well under ~100 ms**. Fine for conversation.

**⇒ ACTION FOR PHASE 3 (and any real-use config):** the config the UI hands the worker MUST use a small ring
(~16), NOT 1024. Shipping a `settings.toml` with the drift-test default gives every user 5 s of latency out of the
box. The `ring_frames` knob exists (`Config::ringFrames`, CLI `--ring`, `validate()` floors at 3). This is recorded
as a forward-note in the Phase 3 spec.

## Finding 2 (limitation, possible future work) — worker demands 48 kHz endpoints

The worker requires the capture AND render devices to already be **48000 Hz**; otherwise it fails at start:
`audio error: Endpoint is not 48000 Hz`. It does **not** resample at the device boundary. Consequences:

- For the **real product path** (mic 48 k → CABLE Input 48 k → Discord) this is invisible — VB-Cable and the mic
  are 48 k, nothing to change.
- But pointing render/capture at an arbitrary device (e.g. the author's 192 kHz Protocol Max headphones) is
  rejected. This is a Phase-0 shortcut.

**Possible future improvement:** add a resampler on each WASAPI boundary (the codebase already has the resampling
machinery it uses for drift comp), so the worker accepts any device at any rate and no user ever touches a
sample-rate setting. Out of scope for Phase 2c/3; spec it as its own small phase if endpoint rigidity matters.

## Test harness (repo root, untracked)

- **`test-lean-rnnoise.toml`** — lean config: `engine=rnnoise`, `warm_standby=false`, `dsp_chain=true`, K688 →
  CABLE Input. Starts lean on RNNoise so the first switch to DFN3 exercises on-demand load.
- **`test-phase2c.ps1`** — hands-free validation. Run:
  `powershell -ExecutionPolicy Bypass -File "I:\Coding-Projects\VoiceCleaner\test-phase2c.ps1"`.
  It launches the worker (`--ring 16 --dev-keys --seconds 30`), auto-switches engines on a timer (keys 1=RNNoise,
  2=DFN3 fed to stdin), prints memory (watch it rise on DFN3, drop back on RNNoise = eager retire working), does a
  rapid-toggle burst, and auto-stops. Pass = voice continuous across every switch, memory rises/drops, no crash,
  exit code 0.
- Warm-standby regression (both engines resident, instant switch):
  `voicecleaner-worker --config drift-warmstandby.toml --ring 16 --dev-keys` (type 1/2, Ctrl+C).

## Monitoring setup (how to hear the CABLE)

VoiceCleaner renders to **CABLE Input** (a virtual *playback* device — the "into the cable" end); Discord reads the
paired **CABLE Output** (virtual *mic* — the "out of the cable" end). To monitor by ear without changing any device
rate: Sound settings → More sound settings → **Recording** → **CABLE Output** → Properties → **Listen** → tick
"Listen to this device" → play through the headphones. That monitor path resamples via the Windows mixer (so 192 kHz
headphones are fine) but **adds its own ~100–200 ms** — a monitoring artifact, not VoiceCleaner's real latency.
Untick it when done to avoid echo during normal Discord use.
