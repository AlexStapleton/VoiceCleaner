# VoiceCleaner — Remaining Work Roadmap (to v1)

> **⚠ SUPERSEDED (2026-08-21):** the project pivoted to **RNNoise quality + performance only** — DFN3 is
> frozen (shipped, untouched), and warm-standby/first-run-wizard/dryTap are no longer planned. The active
> plan is [`2026-08-21-rnnoise-performance-plan.md`](2026-08-21-rnnoise-performance-plan.md) (Workstreams A/B/C
> all merged) with findings in [`../notes/2026-08-21-rnnoise-performance-findings.md`](../notes/2026-08-21-rnnoise-performance-findings.md).
> This doc is kept for its still-relevant Phase A housekeeping + hardware-validation checklist; ignore the
> warm-standby/wizard/dryTap sections.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking. The two feature subsystems (Warm-standby toggle, First-run wizard) are scoped here but each gets its **own detailed TDD plan** before it is built — see the "Detailed plan" note in each.

**Goal:** Close out VoiceCleaner to a shippable v1 — finish the two remaining planned features (warm-standby UI toggle, first-run wizard), wire all deferred IPC/telemetry stubs, and pay down the accumulated **hardware/by-ear validation debt** in one deliberate pass instead of skipping it again.

**Architecture:** All *software* work lands first (test-driven, no hardware needed), then a **single comprehensive hardware validation session** covers the accumulated debt + the new features at once — so we don't touch the K688 twice. Warm-standby and first-run-wizard reuse existing machinery: warm-standby is already persisted per-mic (`MicProfile.warmStandby`) and read at worker start (`bridge.cpp:68`); toggling it live reuses the latency box's respawn pattern. Deferred telemetry fields already exist in the POD (`telemetry.h`) — only the worker-side writes are missing.

**Tech Stack:** C++20, `denoise-core`/`transport-core`/`vc-ipc`/`voicecleaner-core` libs, `voicecleaner-worker` + `voicecleaner` (Win32 + Direct2D), named-pipe/shm IPC, GoogleTest, CMake (ESP-IDF-bundled, not on PATH).

**Build/test (CMake NOT on PATH):**
```
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug
"C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe" --test-dir build -C Debug --output-on-failure
```

**Decisions locked (2026-08-21):** scope = *validate + finish the two planned features*; deferred stubs = *wire all of them*. The spectral (STFT-domain) declicker R&D is **out of scope** for v1.

---

## Ordering & rationale

The recurring "we skipped around" problem is **validation debt**: five merged features each shipped with an unrun hardware/by-ear checklist. The fix is to stop interleaving. This roadmap runs:

- **Phase A — Cleanup (quick):** commit the in-flight fix, tidy the repo, refresh STATUS. Clears the desk.
- **Phase B — Software features (no hardware):** warm-standby toggle → telemetry stubs → command/param stubs → first-run wizard. All TDD, all mergeable without a mic.
- **Phase C — ONE hardware validation pass:** the consolidated by-ear + robustness checklist covering *everything* (old debt + new features). The single sit-down with the K688 → VB-Cable.
- **Phase D — Ship v1:** final regression, STATUS + memory, tag.

Build software first so Phase C validates the *final* build once, not an intermediate one we'd have to re-check.

---

## Phase A — Cleanup

### Task A1: Commit the in-flight wide-char fix

`src/voicecleaner/app_window.cpp` has an uncommitted correctness fix: the LATENCY box and its help text used UTF-8 **byte** escapes (`\xE2\x89\x88`, `\xC3\x97`) inside **wide** (`L"..."`) strings, which render as garbage. Fixed to proper wide-char escapes (`\x2248` ≈, `\x00D7` ×).

- [ ] **Step 1: Confirm the diff is only the escape fix**

Run: `git diff src/voicecleaner/app_window.cpp`
Expected: two lines changed (paint's `swprintf` + `helpText()` case 102), escapes only.

- [ ] **Step 2: Build the UI target to confirm it compiles**

Run: `"C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build --config Debug --target voicecleaner`
Expected: builds clean.

- [ ] **Step 3: Commit**

```bash
git add src/voicecleaner/app_window.cpp
git commit -m "fix(ui): use wide-char escapes for approx/times glyphs in latency box"
```

### Task A2: Repo housekeeping — .gitignore the test artifacts

The working tree has ~50 untracked `*.wav` outputs, `test-phase2c.ps1`, `test-lean-rnnoise.toml`, `tools/__pycache__/`. These are test scratch, not source.

- [ ] **Step 1: Inspect what's untracked**

Run: `git status --porcelain | grep '^??'`
Expected: the `.wav` files, the test scripts, `tools/__pycache__/`.

- [ ] **Step 2: Add ignore rules**

Append to `.gitignore` (create if absent):
```
# test scratch / generated audio
*.wav
tools/__pycache__/
```
Decide per-file whether the two test scripts (`test-phase2c.ps1`, `test-lean-rnnoise.toml`) are worth committing as repo test harnesses. If yes, `git add` them (don't ignore); if no, add `test-phase2c.ps1` and `test-lean-rnnoise.toml` to `.gitignore` too. **Default: commit them** — they're the documented hands-free lean-switch harness referenced in the product-shell memory.

- [ ] **Step 3: Verify the tree is clean of scratch**

Run: `git status --porcelain`
Expected: only `.gitignore` (and the two harness files if you chose to commit them) show; no `.wav` noise.

- [ ] **Step 4: Commit**

```bash
git add .gitignore test-phase2c.ps1 test-lean-rnnoise.toml
git commit -m "chore: ignore generated wav scratch; commit lean-switch test harness"
```

### Task A3: Refresh STATUS.md to current reality

`docs/STATUS.md` is dated 2026-08-18 and predates the per-mic-reload + latency-box merge and the noise-gate merge.

- [ ] **Step 1: Update the "Active work" block** to reflect: per-mic profile reload on input switch + editable latency box (merged), noise gate + TARGET LOUDNESS (merged), current test count (run ctest to get the number), and that **the outstanding item is the consolidated hardware validation pass + the two remaining features** (warm-standby toggle, first-run wizard). Bump `_Last updated_` to the current date.

- [ ] **Step 2: Commit**

```bash
git add docs/STATUS.md
git commit -m "docs(status): refresh to current state; name remaining v1 work"
```

---

## Phase B — Software features (no hardware needed)

### Task B1: Warm-standby live toggle (UI checkbox)

**What & why:** Phase 4 parked the warm-standby control here. The engine mechanism already exists (`DynamicEngine::Mode::Warm` vs `Lean`, `bridge.cpp:68`), and `MicProfile.warmStandby` already persists per-mic. Missing: a UI control to flip it, and the plumbing to make the flip take effect. `SetWarmStandby` exists as a **no-op** pipe command (`main.cpp:146-147`) because switching lean→warm needs the inactive engine loaded (a pipeline/engine rebuild).

**Approach (locked):** reuse the **latency-box respawn pattern** rather than a live rebuild. Toggling the checkbox persists `warmStandby` to the active mic profile and calls `respawnWorker()` (full stop + fresh spawn) so `bridge` re-reads the mode at start — identical to how `ring_frames` already works (`app_window.cpp` `applyRing`/`respawnWorker`). This is simpler and race-free vs. wiring a live `SetWarmStandby` rebuild, and warm-standby is a rare, deliberate setting. (If a later phase wants a gapless live toggle, `SetWarmStandby` is the hook — out of scope for v1.)

**File structure:**
- `src/voicecleaner/app_window.h` / `.cpp` — a checkbox control rect (`rcWarm_`), hit-test entry in `hitControl`, paint, and a handler that persists + respawns; window grows to fit the row. Seed the checkbox from `settings_->activeProfile()->warmStandby` in `refreshDevices()`/`seedFromActiveProfile()`.
- `src/transport-core/settings.{h,cpp}` — `warmStandby` already persists (`MicProfile.warmStandby`); confirm `load`/`save` round-trip it (add a test if missing).
- No `Params`/relay change (respawn path, not live param).

**Test intent (pure units, before UI glue):**
- `Settings.WarmStandbyRoundTrips` — `MicProfile.warmStandby` survives save→load (if not already covered).
- The UI glue (checkbox paint/hit/persist) is Win32, verified manually in Phase C — matching how the sliders are treated.

**Detailed plan:** write `docs/superpowers/plans/2026-08-21-warm-standby-toggle.md` (full TDD steps for the settings round-trip test + the exact `app_window` control wiring) before building. Branch: `feat/warm-standby-toggle`.

- [ ] Write the detailed TDD plan for this task.
- [ ] Execute it (settings test → UI control → build clean, do not run exe).
- [ ] Commit + merge to master.

### Task B2: Wire deferred read-only telemetry (`ringAOcc`, `ringBHigh`, `uptimeMs`)

**What & why:** these fields exist in `telemetry.h` (lines 16, 18, 22) but the worker never writes them. Easy diagnostics wins; no protocol change.

**File structure:**
- `src/voicecleaner-worker/bridge.cpp` (and `bridge.h` if state must be held) — where `TelemetryWriter` publishes the ~1 Hz aggregate:
  - `ringAOcc` — capture-side ring occupancy (mirror however `ringBOcc` is already read).
  - `ringBHigh` — high-water mark of ring B occupancy since last publish (track a running max, reset each publish or keep monotonic — decide in the detailed plan; **monotonic-since-start** is the more useful "did we ever nearly overflow" signal).
  - `uptimeMs` — ms since bridge `start()`. **`Date.now()`/`Math.random()` are unavailable in workflow scripts, not in the worker** — the worker is normal C++; use `QueryPerformanceCounter`/`steady_clock` captured at start.
- (Optional) `src/voicecleaner/app_window.cpp` status line / help — surface uptime + ring high-water if useful; not required.

**Test intent:** the telemetry POD is already tested for round-trip; the new writes are worker-side runtime values. Add a small unit around the ring high-water tracker if it's extracted as a pure helper; otherwise these are verified by reading the live status line in Phase C.

- [ ] Implement the three writes in the bridge publish path.
- [ ] Build worker + full ctest green.
- [ ] Commit: `feat(worker): populate ringAOcc/ringBHigh/uptimeMs telemetry`.

### Task B3: Wire deferred commands & params (`ResetCounters`, `SetWarmStandby`, live `dspChain`/`driftComp`, `dryTap`)

**What & why:** the user chose "wire all of them." Handle by effort, smallest first. **`dryTap` is not a stub — it's the §13 dry-tap feature (a second, un-processed output/archive tap); flag it and confirm scope before building it, as it's materially larger than the others.**

**Sub-items:**
1. **`ResetCounters`** (pipe command, currently no-op) — bridge zeroes dropout + occupancy counters (and `ringBHigh` from B2) on receipt. Small. Add a UI affordance later if wanted.
2. **`SetWarmStandby`** — B1 chose the respawn path, so this command can stay a documented no-op **or** be implemented as "trigger a respawn with the new mode." Prefer: keep B1's respawn as the mechanism and **update the comment** at `main.cpp:146` to say warm-standby is applied via respawn, not this command — so it's no longer a misleading dangling stub. (Implementing a true live rebuild here is out of scope.)
3. **Live `dspChain` / `driftComp`** — currently set only at start via `Config`; `Params` carries them but the relay "does not consume live yet" (`params.h:13-14`). Make the relay apply them each hop (like the other DSP params). `dspChain` live = the bypass path already partly covers this; `driftComp` live-toggle needs the DriftController to enable/disable cleanly mid-run — **decide in the detailed plan** whether live drift toggling is safe or should remain start-only (if unsafe, mark it an explicit non-goal rather than force it).
4. **`dryTap`** — the real feature. Scope decision required: what is the dry tap's sink (a WAV file? a second render endpoint)? Per blueprint §13 it's the archival/dry path. **Gate this behind a short spec** (`docs/superpowers/specs/`) before implementing; do not build it blind.

**File structure:** `src/voicecleaner-worker/bridge.cpp` + `main.cpp` (command dispatch), `src/vc-ipc/params.h`/relay for the live params, plus a spec + plan for `dryTap`.

**Test intent:** command dispatch has existing pipe/codec tests — add cases for `ResetCounters` effect (counters return to 0) and live `dspChain`/`driftComp` application. `dryTap` tests come with its spec.

- [ ] Implement `ResetCounters`; correct the `SetWarmStandby` comment.
- [ ] Implement live `dspChain` (+ `driftComp` if the detailed plan deems it safe).
- [ ] Write the `dryTap` spec, confirm scope with the user, then plan + build it.
- [ ] Build + full ctest green; commit per sub-item.

### Task B4: First-run wizard (Phase 5)

**What & why:** the last planned feature — currently a deferred stub (device-pick + meter). Makes first launch self-explanatory: pick capture mic, pick render (VB-Cable), see the meter move, confirm.

**File structure:**
- `src/voicecleaner-core` (`ui_state` or a new `first_run.{h,cpp}`) — **pure** first-run policy: is this a first run? (no `settings.toml` / no `active_capture_id`) what step are we on? next/back/finish transitions. Unit-tested.
- `src/voicecleaner/app_window.{h,cpp}` — render the wizard overlay/panel and route its buttons through the pure policy; on finish, persist the picks (reuse `applyPick`) and spawn the worker.
- `settings.toml` — a `first_run_done` flag (or infer from `active_capture_id` presence).

**Test intent:** all step/transition/"is-first-run" logic in the pure unit (`FirstRun.*` tests); the D2D panel is manual (Phase C).

**Detailed plan:** write `docs/superpowers/plans/2026-08-21-first-run-wizard.md` (full TDD for the pure policy + exact panel wiring) before building. This is the largest UI task — keep the panel thin, the policy pure. Branch: `feat/first-run-wizard`.

- [ ] Write the detailed TDD plan.
- [ ] Execute (pure policy tests → panel wiring → build clean).
- [ ] Commit + merge.

---

## Phase C — ONE comprehensive hardware validation pass

**This is the accumulated debt.** Run it **once**, on the final Phase-B build, with the real rig (FIFINE K688 → VB-Cable "CABLE Input", both endpoints 48 kHz). Launch `build\src\voicecleaner\Release\voicecleaner.exe`. Note any threshold that felt wrong (`crestK`/`holdMs`/`hpHz` for the declicker; gate/AGC targets) — tuning fixes found here are higher-value than any feature.

### Task C1: Core audio + latency
- [ ] Audio flows K688 → app → CABLE Input; selectable as a mic in Discord/OBS.
- [ ] Latency is low (~tens of ms), **not ~5 s** (the old drift-config ring bug). Meter tracks voice.
- [ ] No dropouts/instability over a few minutes of continuous talk.

### Task C2: Engine switch (lean + warm) — the Phase 2c debt
- [ ] RNNoise ↔ DFN3 switch is instant and **gap-free by ear** (no click/dropout at the crossfade).
- [ ] Rapid toggle (spam the switch) stays stable — no UAF/crash, no stuck engine.
- [ ] Warm-standby ON (Task B1): switch is instant; OFF (lean): switch has the ~16–39 ms on-demand load but is still gap-free.

### Task C3: Bypass + isolation + lifecycle — the Phase 3 debt
- [ ] Bypass toggles to untouched passthrough and back; UI reflects true state on attach.
- [ ] **Kill the UI (`voicecleaner.exe`) — audio keeps flowing** (crash isolation, the user's #1 priority).
- [ ] Relaunch the UI — it re-attaches to the running worker, state correct.
- [ ] Quit from the window/tray — worker stops cleanly (the only path that stops audio).

### Task C4: Per-mic profiles + latency box — the 4d debt
- [ ] Switch capture device between two mics — each restores its **own** saved engine/tuning (no bleed).
- [ ] A slider drag after switching writes the **new** mic's profile, not the previous one.
- [ ] Latency box: type a value + Apply → brief dropout, then new buffering depth; clamp `[4,512]` holds; value survives an app restart.

### Task C5: DSP quality knobs — the 4c / declicker / gate debt
- [ ] TARGET LOUDNESS + the live AGC-gain/VAD readout behave (no railing to max over quiet gaps once mic level is sane).
- [ ] NOISE GATE raised → between-keystroke static drops out without chopping speech onsets.
- [ ] DE-CLICK raised → keyboard/mouse clicks drop **in the gaps**; voice stays crisp at moderate strength.
- [ ] SPEECH DECLICK behaves per design (voice-floor tradeoff audible only when pushed).
- [ ] Confirm the RNNoise-vs-DFN3 click story matches the memory (RNNoise wins clicky rooms).

### Task C6: New features by ear
- [ ] Warm-standby checkbox persists per-mic and its effect on switch latency is audible/correct.
- [ ] First-run wizard: delete/rename `settings.toml`, launch → wizard guides pick→render→meter→finish; second launch skips it.
- [ ] New telemetry (uptime / ring high-water) shows plausible live values.

- [ ] **Record results** in `docs/superpowers/notes/2026-08-21-v1-hardware-validation.md` (pass/fail per item + any tuning changes made). Apply and commit any threshold fixes found.

---

## Phase D — Ship v1

### Task D1: Final regression
- [ ] Full Debug + Release build clean; `ctest` 100% in both configs. Record the count.
- [ ] DFN3 golden gate: `git diff --stat master -- src/dfn3 src/offline-cli` empty (engine untouched by all Phase-B work).

### Task D2: Docs + memory
- [ ] STATUS.md → "v1 complete" with the validation-note link and final test count.
- [ ] Update memory `voicecleaner-product-shell` (Phases 4d/5 done, validation run) and `realworld-audio-quality` (any new tuning findings).

### Task D3: Tag
- [ ] Tag `v1.0` on master after the validation note is committed.

---

## Self-Review

**Scope coverage vs. the two decisions:** *validate + finish planned features* → warm-standby toggle (B1) + first-run wizard (B4) + the consolidated validation (Phase C). *Wire all deferred stubs* → telemetry (B2) + commands/params incl. dryTap (B3). Spectral declicker explicitly out. ✓

**Accumulated validation debt mapped:** 2c gap-free switch → C2; Phase 3 isolation/relaunch/bypass → C3; 4d per-mic + latency → C4; 4c/declicker/gate → C5. Every "STILL NEEDS: manual validation" note from the memory has a Phase-C item. ✓

**Placeholder scan:** the two feature tasks (B1, B4) intentionally defer to their **own** detailed TDD plans (per the writing-plans scope-check: independent subsystems get separate plans) — that's a decomposition decision, not a placeholder. `dryTap` (B3.4) is gated behind a spec because its sink is genuinely undecided — flagged, not hand-waved. Everything in Phases A/C/D is fully actionable as written. ✓

**Type/name consistency:** `MicProfile.warmStandby`, `Config.warmStandby`, `DynamicEngine::Mode::{Warm,Lean}`, `respawnWorker()`, `SetWarmStandby`/`ResetCounters` commands, `Telemetry.{ringAOcc,ringBHigh,uptimeMs}`, `Params.{dspChain,driftComp,dryTap}` all match the current headers read on 2026-08-21. ✓
