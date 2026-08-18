# Transient / Click Suppressor — Design

_Date: 2026-08-17 · Status: draft for review_

Motivated by the real-hardware finding (see [`realworld-audio-quality` memory]): on clicky input, **RNNoise beats
DFN3 at mouse/keyboard suppression, but both leak short broadband transients** (clicks). Neither engine, nor the
Phase 4c aggression slider, targets impulses. This adds a dedicated, voice-safe **declicker** DSP stage with a
tuning slider. Builds on the Phase 4c slider + per-mic persistence machinery (merged to master).

## 1. Problem & scope

Keyboard/mouse clicks are short (<~5 ms), high-crest, broadband **transients**. Spectral suppressors (DFN3) let
them through; RNNoise gates them better but not fully. A speech consonant (plosive `/t/ /k/`, sibilant `/s/`, lip
smack) is *also* a short broadband transient, so there is **no perfect click-vs-consonant discriminator** — the
design is deliberately **conservative / voice-safe** (chosen in brainstorming): kill clicks in the gaps between
words reliably, back off hard when the voice is active, accept that a click landing on top of a loud word may
partly leak.

**In scope:** a new `Declicker` DSP stage (detection + look-ahead ducking + VAD back-off), placed in the chain; a
`Params.declickStrength` field wired live end-to-end; one "De-click" tuning slider (per-mic persisted, like 4c).
**Out of scope:** sample-level "repair by interpolation" (better quality, complex — deferred); multi-band or
FFT-based detection; any engine change; auto-tuning.

## 2. Locked decisions (from brainstorming)

| Decision | Choice |
|---|---|
| Aggressiveness | **Conservative / voice-safe** — act on short strong broadband impulses; **duck depth scales down as VAD rises**. |
| Method | **Duck** (short smoothed gain dip) with **~3 ms look-ahead**, not sample repair. |
| Detection | **Time-domain, no FFT** — HF-emphasis → fast envelope → spike-above-slow-baseline (crest/flux), short events only. |
| Placement | New stage **after engine + HPF, before AGC**: `DC → engine → HPF → declick → AGC → limiter`. |
| Control | **One "De-click" slider** (0 = off … 1 = strongest), per-mic persisted, live over the pipe. |
| Default | **Off (0.0)** out of the box — opt-in, since it can (mildly) affect speech; the user dials it up. |

## 3. The `Declicker` DSP unit (`src/denoise-core/declicker.{h,cpp}`)

A pure, RT-safe class in the `Agc`/`Limiter` mould (no allocation in `process`, fixed internal buffers).

```cpp
namespace vc {
class Declicker {
 public:
  struct Config {
    float fs = 48000.0f;
    size_t lookahead = 144;      // 3 ms delay so the duck is down before the click
    float hpHz = 3000.0f;        // detection HF-emphasis cutoff
    float fastMs = 0.5f;         // fast detection-envelope time constant
    float slowMs = 50.0f;        // slow baseline envelope time constant
    float releaseMs = 8.0f;      // duck recovery after a click
  };
  explicit Declicker(const Config& c);
  // Process n samples. `strength` in [0,1] (0 = bypass). `vad` in [0,1] scales the duck DOWN as it rises.
  // out may NOT alias in (there is a look-ahead delay). Deterministic, allocation-free.
  void process(const float* in, float* out, size_t n, float strength, float vad) noexcept;
  void  setStrength(float s) noexcept;   // stored for currentStrength(); process() also takes it live
  float currentStrength() const noexcept;
  void  reset() noexcept;
 private:
  // fixed-size delay ring (lookahead), fast/slow one-pole envelopes, current duck gain + release state
  ...
};
}
```

**Algorithm (per sample, conceptually):**
1. Push the input into a `lookahead`-length delay ring; the **output** sample is the delayed one.
2. On the *incoming* (undelayed) sample, run an HF-emphasis one-pole (`hpHz`) → magnitude → a **fast** envelope
   `env_f` (`fastMs`) and a **slow baseline** `env_s` (`slowMs`).
3. A click is flagged when `env_f > k · env_s` (crest ratio) with `k` and the resulting duck depth driven by
   `strength`. The target duck gain for the *delayed* region is `1 - depth`, where
   `depth = strength · (1 - vad)` (VAD back-off — near-silent voice ⇒ full depth; strong voice ⇒ ~none).
4. The applied gain moves to the target with a fast attack (reaches it within the look-ahead) and a
   `releaseMs` recovery, so the dip is a short smooth notch around the click, not a chop.
5. `strength == 0` ⇒ `depth == 0` ⇒ output is the delayed input unchanged (pure `lookahead` delay only).

The `lookahead` delay is a **fixed algorithmic latency** (3 ms) whenever the stage exists; the crossfade/limiter
timing is unaffected because declick sits inside the chain, not the engine-switch container.

## 4. Chain integration (`DspChain`)

- Add a `Declicker declick_;` member and a `Config::declickStrength = 0.0f` (ramped like the other params).
- In `process_frame`, insert the declick call **after the HPF, before the VAD-gated AGC**, passing the engine's
  VAD probability (already computed as `float vad = engine_->LastVadProbability();`) and the ramped strength.
  Because `Declicker::process` needs a non-aliasing out, use the existing `t1_`/`t2_` scratch ping-pong.
- Live control: `void setDeclickStrength(float s)` (ramped target) + `float currentDeclickStrength()`.
- The chain's **algorithmic latency grows by `lookahead` (3 ms)** — update the DFN3 delay pinning note only if it
  measures end-to-end latency; the engine-switch delay constants (RNNoise 0 / DFN3 1440) are unchanged (declick is
  outside the `DynamicEngine`). Confirm no regression in the delay-alignment logic.

## 5. Param + IPC wiring

- `src/vc-ipc/params.h` — add `float declickStrength;` to the `Params` POD (+ `0.0f` in `defaults()`). Both worker
  and UI share the struct, so the pipe/shm memcpy carries it automatically.
- `src/voicecleaner-worker/bridge.cpp` — the relay already applies params each hop; add
  `chain_->setDeclickStrength(sp.declickStrength);` next to the existing `setHpfCutoffHz`/`setAgcTargetDb` calls.
- `src/vc-ipc/command.cpp` — the `SetParams` codec memcpy's the whole `Params`, so the new field round-trips with
  no codec change (the existing `test_command` round-trip covers it once the field exists).

## 6. Settings + UI (reuses 4c machinery)

- `src/transport-core/settings.h/.cpp` — `MicProfile` gains `float declickStrength = 0.0f;` with a
  `declick_strength` key in load/save.
- `src/voicecleaner/ui_state.{h,cpp}` — add `onSetDeclick(float)` and extend `seedTuning(...)` to carry declick.
- `src/voicecleaner/slider.h` — reuse `SliderFmt::Ratio2` for a `0.00…1.00` display.
- `src/voicecleaner/app_window.{h,cpp}` — a **fifth slider row "DE-CLICK"** (0..1, default 0), always enabled
  (engine-agnostic); seeded from the profile, pushed on ready, persisted on drag-end — identical plumbing to the
  four 4c sliders. The window grows one row taller.

## 7. Testing

**Pure `Declicker` units (`tests/denoise-core/test_declicker.cpp`):**
- **Removes an impulse in silence:** feed near-silence with a 1–2-sample spike; with `strength=1, vad=0` the peak
  around the click is attenuated well below the input spike; with `strength=0` output equals the input delayed by
  `lookahead` (bit-exact aside from the delay).
- **Preserves steady/voiced signal:** a sustained 300 Hz tone (proxy for voiced speech) passes through with
  amplitude ~unchanged (no false ducking) at `strength=1`.
- **VAD back-off:** the same impulse with `vad=1` is ducked far less than with `vad=0` (depth scales with
  `1 - vad`).
- **RT-safety:** `process` does no allocation (fixed buffers); `reset()` returns to a clean state.

**Chain/wiring:** a `DspChain` test that a click after the engine is reduced with declick on and passes through
with `declickStrength=0`; `Params` default is `0.0`; `Settings` `declick_strength` round-trips; `UiState`
`onSetDeclick`/`seedTuning` carry it; the `command` round-trip still passes.

**Regression:** the DFN3 **offline golden is unaffected** (engine untouched; declick defaults off and lives in the
chain, which the golden path — `offline-cli --engine dfn3` without `--chain` — does not run). All existing tests
stay green.

**Manual / hardware:** with De-click up, keyboard/mouse clicks **between words** are clearly reduced; speech
consonants stay crisp at moderate strength; pushing to 1.0 may soften consonants (expected); clicks on top of loud
speech partly leak (expected, voice-safe tradeoff); latency remains low.

## 8. Risks & forward notes

- **By-ear tuning required.** The `k` crest threshold and envelope constants ship at sensible defaults but will
  need listening iteration; they are `Config` fields so a later phase can expose an "advanced" sensitivity control
  if the single slider proves too coarse.
- **Repair-by-interpolation** (replace the click samples from neighbours) is the higher-quality future upgrade if
  ducking proves audible; deferred.
- If on-speech clicks remain the dominant complaint, a **spectral (per-bin) declicker** riding the engine's STFT
  is the heavier future option — out of scope here.
