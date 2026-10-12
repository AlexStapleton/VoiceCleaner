> **Historical (Phase 0, 2026-08).** `phase0-app.exe` and the flat `drift.toml` format no longer exist.
> The equivalent today is a console run of the worker:
> `voicecleaner-worker.exe --capture <id> --render <id> --ring 512 --csv drift.csv`
> (`--list-devices` prints the ids; the ring is now capped at 512 frames ≈ 2.5 s).

# Phase 0 — 8-Hour Zero-Dropout & Clock-Drift Acceptance Run

The Phase 0 acceptance test. Proves the pass-through bridge holds **zero dropouts over a full
8-hour session** and measures the **clock-drift (ppm) between the K688's ADC clock and VB-Cable's
software clock** (blueprint §5.8, §14). Run this *before* building the Phase 1.5 drift compensator —
the slope you measure here is what that compensator will cancel.

---

## 0. Prerequisites (do these first)

**Hardware / OS**
- [ ] FIFINE **K688 plugged in over USB** and enumerating. Verify: `phase0-app.exe --list-devices`
      shows it under `CAPTURE` at **48000 Hz**. (If it shows as absent, unplug/replug or check
      `Get-PnpDevice` — a status of `Unknown` means Windows isn't enumerating it.)
- [ ] VB-Cable installed; **both** "CABLE Input" and "CABLE Output" set to **48000 Hz** in
      Sound Settings → Device Properties → Advanced (blueprint §4.1 step 2). A rate mismatch here
      is the #1 cause of "sounds wrong" and will show up as a *pinned* drift ratio.
- [ ] K688 endpoint set to **16-bit, 48000 Hz**; **microphone boost off**, level 100% (§4.2).
- [ ] Windows **Audio Enhancements unchecked** on the K688 endpoint (§4.3).

**Gain staging (§4.2)** — do this before the run, it matters more than any software setting:
- [ ] Set the on-mic gain knob so normal speech peaks around **−18 to −12 dBFS** at the hardware.

**Downstream (so a parallel Discord session is representative, §4.3)**
- [ ] In Discord: Noise Suppression = **None**; Echo Cancellation, Noise Reduction, Automatic
      Gain Control all **off**. (Not required for the drift measurement itself, but keeps the
      session realistic if you talk to people during it.)

**Measurement tools (external, no app code)**
- [ ] **LatencyMon** installed (DPC latency baseline).
- [ ] **PresentMon** installed (in-game frame-time capture).

---

## 1. Build the Release binary

CMake is not on PATH — use the bundled one:

```bash
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Release
```

The binary is at `build\src\phase0-app\Release\phase0-app.exe`. (Debug works too, but Release is
the honest configuration for an 8-hour/measurement run.)

## 2. Find the device IDs and write the config

```bash
build\src\phase0-app\Release\phase0-app.exe --list-devices
```

Copy the K688's `CAPTURE` id and the "CABLE Input" `RENDER` id into `drift.toml` in the repo root:

```toml
# drift.toml — Phase 0 8-hour drift run (deep ring)
capture_device_id = "{0.0.1.00000000}.{PASTE-K688-ID}"
render_device_id  = "{0.0.0.00000000}.{PASTE-CABLE-INPUT-ID}"
ring_frames       = 1024        # deep ring (~10.7 s) so ordinary drift can't rail it in 8 h
csv_path          = "drift.csv"
```

> **Why 1024 frames, not a §5.2 profile depth?** At ~20 ppm a 16-frame ring rails in ~66 min, so no
> production profile survives 8 h uncompensated. The deep ring keeps occupancy off both rails for
> the whole session, letting you read a clean slope *and* hit zero dropouts. (Phase 1.5 then makes
> the shallow production rings survive 8 h.) See the spec §12.

## 3. Baseline the system (before the long run)

- [ ] **LatencyMon**, ~10 min with the app **not** running → record max DPC / "conclusion".
- [ ] **LatencyMon**, ~10 min with the app running (start it per §4) → compare. Delta should be
      indistinguishable from baseline (§1.2).
- [ ] In one game: **PresentMon** capture ~2 min app-off, then ~2 min app-on → compare frame-time
      distributions. Delta should sit within run-to-run variance (§1.2).

## 4. Start the 8-hour run

```bash
build\src\phase0-app\Release\phase0-app.exe --config drift.toml
```

Expect:
- a `bridge running: ring=1024 frames (primed 512), csv=drift.csv` line;
- ~1 Hz console telemetry: `[  Ns] A=... B=... (hi A=... B=...) under=... over=...`;
- Ring B occupancy sitting near **512** (half of 1024) and drifting slowly over hours.

Now **use the machine for a normal 8-hour day**, including a long gaming session. Leave the app and
its console running the whole time. When done, press **Ctrl+C** in the app console for a clean
shutdown (`stopping...`, flushes `drift.csv`).

## 5. Analysis — recover the ppm

`drift.csv` columns: `elapsed_ms,ringA_frames,ringB_frames,ringA_hi,ringB_hi,underruns,overruns`
(after a `# started <wall clock>` header line).

Plot **`ringB_frames` vs `elapsed_ms`** (any spreadsheet, or the snippet below) and fit a line.
`elapsed_ms` is real QueryPerformanceCounter time, so the fit is valid even though rows land ~1 s
apart.

```python
# quick fit: pip install numpy pandas
import numpy as np, pandas as pd
df = pd.read_csv("drift.csv", comment="#")
t_s = df["elapsed_ms"] / 1000.0
slope_frames_per_s, intercept = np.polyfit(t_s, df["ringB_frames"], 1)
drift_ppm = slope_frames_per_s * 480 / 48000 * 1e6   # 480 samples/frame, 48 kHz
print(f"slope = {slope_frames_per_s:.4e} frames/s  ->  drift = {drift_ppm:+.1f} ppm")
```

Interpretation:
- A **monotonic** trend is real clock drift; the sign tells you which clock is faster (rising Ring B
  ⇒ capture faster than render).
- A **flat** line ⇒ negligible drift for this pair (unusual but possible if both endpoints share a
  clock).
- Occupancy **pinned at a rail** ⇒ pathological — almost always a 48 kHz **format mismatch** (§5.7);
  fix the endpoint rates and rerun before trusting anything.

## 6. Record the exit criteria

Fill in and keep with the run (append results here or in a dated note):

| Criterion | Target | Result |
|---|---|---|
| Underruns over 8 h | **0** | |
| Overruns over 8 h | **0** | |
| Ring B stayed off both rails | yes | |
| Measured drift | (record ppm + sign) | |
| DPC delta (LatencyMon), app off→on | within variance | |
| Frame-time delta (PresentMon), app off→on | within run-to-run variance | |

**Pass** = zero dropouts over the full 8 h, occupancy off the rails, and DPC/frame-time deltas
inside variance. Record the ppm figure — it sizes the Phase 1.5 compensator and its control-loop
setpoint tolerance.

---

## Notes
- Do **not** commit `drift.csv` (it's gitignored).
- If you see under/overruns climbing steadily (not just a couple at startup), that's a genuine
  transport problem to diagnose (scheduling, buffer sizing) — not drift, which the deep ring is
  specifically sized to keep from causing dropouts within 8 h.
- The `--seconds N` flag runs a fixed-duration session then stops cleanly — handy for shorter
  validation passes (e.g. a 30-min pre-check) without needing Ctrl+C.
