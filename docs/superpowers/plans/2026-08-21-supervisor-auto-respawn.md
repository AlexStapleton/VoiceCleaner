# Supervisor Auto-Respawn — Implementation Plan (Workstream C3)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans (or subagent-driven-development). Steps use checkbox (`- [ ]`) syntax.

**Goal:** When the worker dies *after having run* (e.g. device-loss exit), the UI auto-respawns it with bounded backoff instead of waiting for a manual Relaunch. A worker that never became ready (genuine launch failure) does NOT auto-loop.

**Architecture:** A new pure `RespawnPolicy` in `voicecleaner-core` (fully unit-tested) decides — from `(alive, ready, everReady, nowMs)` — whether to do nothing, respawn now, or give up after a cap. `AppWindow::tick()` consults it each 100 ms poll and calls the existing `startOrAttach()` path on `Respawn`. The policy carries a **reconnect-episode latch** so that once a reconnect starts, subsequent *pre-ready* deaths of the respawned worker still count toward the cap (otherwise a device that stays gone would be seen as a fresh launch-failure after the first retry and stop immediately).

**Tech Stack:** C++20, `voicecleaner-core` (Windows-free), GoogleTest; thin Win32 glue in `app_window`.

**Findings:** [`../notes/2026-08-21-rnnoise-performance-findings.md`](../notes/2026-08-21-rnnoise-performance-findings.md) §4.

---

## Design

**Backoff schedule:** `delay(n) = min(maxDelayMs, baseDelayMs << n)` → 0.5s, 1s, 2s, 4s, 8s (cap). `maxAttempts = 5`, then give up.

**State machine** — `onTick(alive, ready, everReady, nowMs) -> Action{None, Respawn, GiveUp}`:
- `alive && ready` → **healthy**: clear episode (attempts=0, unscheduled, not inReconnect, not gaveUp). `None`.
- `gaveUp` → `None` (terminal until `reset()`).
- `alive && !ready` → connecting (initial or post-respawn): `None`.
- `!alive`:
  - reconnect-worthy = `everReady || inReconnect` (this instance ran, or we're already mid-episode).
  - if reconnect-worthy:
    - `inReconnect = true`.
    - if `attempts >= maxAttempts` → `gaveUp = true`; `GiveUp`.
    - if not scheduled → `scheduledAt = nowMs + delay(attempts)`; `None`.
    - if `nowMs >= scheduledAt` → `++attempts`; unschedule; `Respawn`.
    - else `None`.
  - else (never ready, not in episode) → genuine launch failure: `GiveUp`.

`reset()` (manual Relaunch) clears the whole episode. The caller relies on the supervisor's `LivenessTracker` (5 s ready-timeout) to eventually turn a hung respawn into `!alive`, so the policy needs no timer of its own.

**Wiring:** `tick()` computes `everReady = !r.launchFailure` (valid at death) and calls `onTick`. On `Respawn`: `sup_->shutdown(0)`-free — just `pushedTuning_ = false; sup_->startOrAttach();` (same as `requestRelaunch`, minus the manual reset). `requestRelaunch()` additionally calls `respawn_.reset()`. Paint's Down band shows "Reconnecting… (n/5)" while `reconnecting()` is true, else "Relaunch worker".

---

## Task 1: `RespawnPolicy` pure unit

**Files:** Create `src/voicecleaner/respawn_policy.h`, `respawn_policy.cpp`; add to `voicecleaner-core` lib in `src/voicecleaner/CMakeLists.txt`. Create `tests/voicecleaner/test_respawn_policy.cpp`; add to `tests/voicecleaner/CMakeLists.txt`.

- [ ] **Step 1: Header** — `RespawnPolicy` with `Config{maxAttempts=5, baseDelayMs=500, maxDelayMs=8000}`, `enum class Action{None,Respawn,GiveUp}`, `Action onTick(bool alive,bool ready,bool everReady,uint64_t nowMs)`, `void reset()`, `bool reconnecting() const`, `int attempts() const`, `int maxAttempts() const`.
- [ ] **Step 2: Failing tests** (matrix below).
- [ ] **Step 3: Wire CMake, build, verify fail.**
- [ ] **Step 4: Implement** the state machine above.
- [ ] **Step 5: Build + run, verify pass.**
- [ ] **Step 6: Commit.**

**Test matrix:**
1. `HealthyReturnsNone` — `onTick(true,true,true,t)` → None; `reconnecting()` false.
2. `LaunchFailureGivesUpNoRespawn` — `onTick(false,false,false,t)` → GiveUp; a later tick still GiveUp/None, never Respawn.
3. `DeathAfterReadySchedulesThenRespawns` — `onTick(false,false,true,1000)` → None (scheduled +500); `onTick(false,false,true,1400)` → None; `onTick(false,false,true,1500)` → Respawn; `attempts()==1`.
4. `BackoffDoublesAndCaps` — drive repeated pre-ready deaths (`everReady=false` after the first, relying on the latch); assert successive scheduled delays 500,1000,2000,4000,8000 then GiveUp on the 6th.
5. `ReadyAgainResetsEpisode` — after one Respawn, `onTick(true,true,true,t)` → None and `attempts()` back to 0; a subsequent death re-starts backoff at 500.
6. `ResetClearsEpisode` — mid-episode `reset()` → `reconnecting()` false, next death starts fresh.
7. `LatchCountsPreReadyDeaths` — first death everReady=true → schedule; fire Respawn; then deaths with everReady=false keep counting to the cap (does not immediately GiveUp).

## Task 2: Wire into `AppWindow`

**Files:** `src/voicecleaner/app_window.h` (add `RespawnPolicy respawn_;` + include), `app_window.cpp` (`tick()`, `requestRelaunch()`, paint Down band).

- [ ] **Step 1:** include + member.
- [ ] **Step 2:** in `tick()`, replace the bare `if (!r.alive) state_->onWorkerDown();` with: always call `onWorkerDown()`/`onTelemetry` as today for UiState, then feed the policy and act:
  ```cpp
  RespawnPolicy::Action a = respawn_.onTick(r.alive, r.ready, !r.launchFailure, GetTickCount64());
  if (a == RespawnPolicy::Action::Respawn) { pushedTuning_ = false; sup_->startOrAttach(); }
  ```
  (Keep the existing UiState update + tuning-push logic for the `alive` branch.)
- [ ] **Step 3:** `requestRelaunch()` → add `respawn_.reset();` before `startOrAttach()`.
- [ ] **Step 4:** paint Down band: `std::wstring lbl = respawn_.reconnecting() ? (L"Reconnecting… (" + std::to_wstring(respawn_.attempts()) + L"/" + std::to_wstring(respawn_.maxAttempts()) + L")") : L"Relaunch worker";` — clicking still works (manual override).
- [ ] **Step 5:** build `voicecleaner` clean (do NOT run — hardware). Commit.

## Task 3: Regression + docs

- [ ] Full Debug + Release build; ctest 100% (adds `RespawnPolicy.*`).
- [ ] Update the findings doc §4 + Status; commit.
- [ ] Merge via superpowers:finishing-a-development-branch.

## Self-Review
Covers findings §4 (auto-recovery). The latch (test 7) is the non-obvious correctness point — without it a persistently-gone device stops after one retry. Launch-failure still fails fast (test 2). Manual Relaunch always available and resets the episode (Task 2 Steps 3–4). Pure policy is fully testable; Win32 glue is thin and build-verified.
