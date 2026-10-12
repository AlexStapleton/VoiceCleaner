# VoiceCleaner already matches the field's models; buffering costs it latency

VoiceCleaner's model choices are competitive. Its 10 ms / 480-sample RNNoise path is the same design OBS and NoiseTorch ship. Its DeepFilterNet3 path sits in the top quality tier of open, causal, full-band models, and in the one same-test-set comparison that includes both, DFN3 scores 0.82 PRISM against RNNoise's 0.51. Only Ceva's 2025 DPDFNet scores higher. Where VoiceCleaner falls behind is everything around the model. **End-to-end latency of roughly 60–100 ms comes mostly from priming ring B to ~40 ms and from VB-Cable's own buffer, not from the denoiser.** On top of that, the shipped RNNoise runs float32 "debug" weights through pure-scalar C on MSVC, and the DFN3 runner recomputes 1–6 frames of history per graph on every hop. The highest-value work is therefore cheap. The first step is a CMake-only RNNoise fix that restores the int8 weights and SIMD paths upstream intends (it needs an A/B and timing check). Next comes a smaller ring-B setpoint with a better-filtered drift loop, then the open review items P2/P5, then OS power-throttling opt-outs. After those, the medium projects are a stateful (pulsed) DFN3 export, an optional DPDFNet-2 engine, and a Windows-native AEC "speaker mode". Shipping your own signed virtual-mic driver is the only way to remove VB-Cable's buffer and second clock domain. It is real but expensive for a solo developer, so the report defers it until the cheaper latency work has been measured.

## Ten-millisecond framing and DFN3 put VoiceCleaner at parity or ahead

**The RNNoise path matches the industry baseline.** OBS's noise-suppression filter processes 10 ms (480-sample) segments "to keep latency low", reports exactly one segment of latency, and resamples to and from 48 kHz around RNNoise when needed ([OBS source](https://raw.githubusercontent.com/obsproject/obs-studio/master/plugins/obs-filters/noise-suppress-filter.c)). NoiseTorch states 10 ms inherent suppression latency plus variable PulseAudio latency ([NoiseTorch README](https://github.com/noisetorch/NoiseTorch)). Krisp's only published figure is an old vendor claim of ~15 ms ([TechCrunch 2018](https://techcrunch.com/2018/12/10/krisp-reduces-noise-on-calls-using-machine-learning-and-its-coming-to-windows-soon)). VoiceCleaner's framing is the same, so its model-side latency is on par with all of these.

**The DFN3 engine puts VoiceCleaner ahead of most RNNoise-only tools.** NoiseTorch, werman's plugin and OBS's default all wrap RNNoise ([werman README](https://github.com/werman/noise-suppression-for-voice); [NoiseTorch README](https://github.com/noisetorch/NoiseTorch)). EasyEffects offers DeepFilterNet next to RNNoise, and its users report DFN sounding much better ([Solus forum](https://discuss.getsol.us/d/10034-using-deepfilternet-with-easy-effects)). On Ceva's low-SNR multilingual set, DFN3 outscores every other open causal baseline tested: DEMUCS, DPCRN, aTENNuate, NSNet2, DTLN, FullSubNet, RNNoise and GTCRN ([DPDFNet, arXiv:2512.16420](https://arxiv.org/abs/2512.16420)). Offering both a near-free engine and a DFN-class engine is a feature mix few open tools ship on Windows.

**CPU-only is a reach advantage, not a weakness.** NVIDIA Broadcast needs an RTX 2060 or better, and Studio Voice needs an RTX 3060 desktop GPU ([TechSpot](https://www.techspot.com/downloads/7304-nvidia-broadcast.html); [NVIDIA FAQ](https://www.nvidia.com/en-gb/geforce/broadcasting/broadcast-app/faq)). Windows Studio Effects voice focus needs an NPU, and the full set needs a 40+ TOPS Copilot+ PC ([Microsoft Support](https://support.microsoft.com/id-id/windows/ai/ai-features/windows-studio-effects)). SteelSeries is the only competitor that advertises its CPU choice, saying ClearCast AI is "CPU-based to avoid frame drops" ([SteelSeries](https://steelseries.com/pt-br/gg/sonar-for-streamers)). VoiceCleaner can market the same property.

**The drift design is the standard, correct one.** PipeWire also places an adaptive resampler at the sink side, driven by a loop that pushes buffer fill toward a target. It does this specifically "to avoid signal distortion when capturing audio" ([PipeWire props(7)](https://docs.pipewire.org/page_man_pipewire-props_7.html); [spa_io_rate_match](https://docs.pipewire.org/structspa__io__rate__match.html)). VoiceCleaner's PI-controlled resampler on ring-B occupancy has that topology. Without drift correction, a 50 ppm mismatch accumulates ~180 ms/hour ([Forasoft](https://www.forasoft.com/learn/audio-for-video/articles-audio/drift-correction-resampling-frame-drop)). NoiseTorch openly lives with variable latency it cannot fix ([NoiseTorch README](https://github.com/noisetorch/NoiseTorch)). The Catmull-Rom interpolator is not a priority to replace either. At ratios within ~150 ppm of 1.0, the main risk is jitter in the control signal, and Adriaensen identifies ratio jitter, not the interpolator, as the source of "parasitic signals, noise or distortion" ([Adriaensen](https://kokkinizita.linuxaudio.org/papers/adapt-resamp.pdf)).

**Several runtime choices already follow vendor guidance.** ONNX Runtime's documented defaults include a thread pool sized to physical cores, with spinning on in non-client builds ([ORT threading docs](https://github.com/microsoft/onnxruntime/blob/gh-pages/docs/performance/tune-performance/threading.md); [config keys](https://github.com/microsoft/onnxruntime/blob/main/include/onnxruntime/core/session/onnxruntime_session_options_config_keys.h)). VoiceCleaner instead runs one intra-op thread, sequential execution and bound I/O. With a 10 ms deadline, that removes pool wake-up jitter entirely, which is the right call. FTZ/DAZ on all audio threads is already shipped (review item P1). It closed the classic denoising-in-silence stall, which measured 25x in the DC-blocker/HPF stage. The DFN3 WindowRunner carries GRU state across hops with the advance-by-one rule, so its output is exact streaming. The issue is cost only, covered below. The DSP chain covers the HPF/gate/compression/limiter set that Logitech Blue VO!CE, Elgato Wave Link and Sonar ship as their differentiators ([Logitech](https://www.logitechg.com/discover/technology/blue-voice); [KitGuru](https://www.kitguru.net/?p=472816)).

**Where it is behind** comes down to four things:

- **Total latency.** Ring priming and VB-Cable buffering dominate it.
- **Integration polish.** There is no branded mic and a separate cable install is required. NVIDIA and Krisp ship their own virtual devices ([SimpleClean](https://simpleclean.app/blog/remove-background-noise-in-nvidia-broadcast); [daveswift.com](https://daveswift.com/krisp/)).
- **Missing features.** There is no echo or reverb handling, which NVIDIA Broadcast and Windows Voice Clarity both offer ([Engadget](https://www.engadget.com/nvidia-broadcast-app-update-1-2-110034015.html); [Analytics Vidhya](https://www.analyticsvidhya.com/blog/2024/02/microsofts-ai-powered-voice-clarity-revolutionizes-audio-on-windows/)).
- **Engine CPU efficiency.** Both engines are less efficient than they need to be.

## DPDFNet leads on quality, but the numbers are hard to compare

The table combines every source that measures more than one of the four candidates. **Read across columns with care.** The headline comparison comes from Ceva, which authored DPDFNet. Ceva ran every model at 16 kHz on its own low-SNR multilingual set, not at VoiceCleaner's 48 kHz. VCTK numbers come from different papers with different scoring. DNSMOS figures for the same model also differ between papers: DFN2's DNS4 OVRL is 3.882 in its own paper but 3.019 in Ceva's. MAC counts follow different conventions, and GTCRN's own README and paper disagree by about 2x on parameters ([DFN2, arXiv:2205.05474](https://arxiv.org/abs/2205.05474); [arXiv:2512.16420](https://arxiv.org/abs/2512.16420); [GTCRN](https://github.com/Xiaobin-Rong/gtcrn)).

| | RNNoise | DFN3 | DPDFNet-2 (48k "hr") | DPDFNet-8 (48k "hr") | GTCRN |
|---|---|---|---|---|---|
| Native rate | 48 kHz | 48 kHz | 48 kHz release (trained at 16 kHz) | 48 kHz release | **16 kHz only** |
| Params / compute | 0.06 M, 0.04 GMAC/s (2017 model); Ceva lists 0.087 M | 2.14 M, 0.35 GMAC/s | 2.58 M, 2.42 GMAC/s (~7x DFN3) | 3.63 M, 7.17 GMAC/s | 24–48 K, 33–40 MMAC/s |
| Ceva low-SNR set, 16 kHz: PESQ / OVRL / PRISM | 2.05 / 2.50 / 0.51 | 2.45 / 2.71 / 0.82 | 2.79 / 2.85 / 0.95 (16 kHz variant) | 2.85 / 2.89 / 1.00 (16 kHz variant) | 2.00 / 2.24 / 0.49 |
| DNS4 blind OVRL (Ceva Table II) | — | 2.958 | 3.104 | 3.122 | — |
| VCTK PESQ (source paper) | 2.33 (DFN2 paper) | 3.17 (DFN3 paper) | not published | not published | 2.87 (own README) |
| Latency | 10 ms frame; one segment per OBS | 40 ms by paper accounting (20 ms window + 2-frame look-ahead); VoiceCleaner counts 30 ms; no-look-ahead LADSPA build 20 ms | same ℓ=2 look-ahead as DFN2, so likely DFN-class; model card says first output ~20 ms | same as DPDFNet-2 | not verified here |
| CPU evidence | RTF 0.027 single-thread i5-8250U (2017 model) | RTF 0.19 single-thread notebook (paper) | no CPU RTF published; real time only claimed on Ceva NPU | likely too heavy for one laptop core (unmeasured) | RTF 0.07 on i5-12400 |
| License / format | BSD-3, C | MIT/Apache-2.0, ONNX | Apache-2.0, ONNX/TFLite | Apache-2.0, ONNX/TFLite | MIT, ONNX via sherpa-onnx |

Sources: [arXiv:2512.16420](https://arxiv.org/abs/2512.16420), [arXiv:2205.05474](https://arxiv.org/abs/2205.05474), [DFN3 arXiv:2305.08227](https://arxiv.org/abs/2305.08227), [DFN LADSPA README](https://github.com/Rikorose/DeepFilterNet/tree/main/ladspa), [Ceva-IP/DPDFNet model card](https://huggingface.co/Ceva-IP/DPDFNet), [GTCRN README](https://github.com/Xiaobin-Rong/gtcrn), [OBS source](https://raw.githubusercontent.com/obsproject/obs-studio/master/plugins/obs-filters/noise-suppress-filter.c).

The ranking is consistent even though the absolute numbers are not. **DPDFNet beats DFN3 on both test sets Ceva reports.** Ceva also fine-tuned it on 30–40 s continuous segments to fix "always-on" streaming failures: first-syllable truncation after 3–4 s of silence, and noise leakage ([arXiv:2512.16420](https://arxiv.org/abs/2512.16420)). That robustness matters directly for a live mic. The cost is roughly 7x DFN3's compute, with no published CPU timing. **GTCRN is not a better RNNoise for this product.** It beats RNNoise comfortably on VCTK, but that lead disappears on Ceva's hard set (0.49 vs 0.51 PRISM). It is also 16 kHz only, so VoiceCleaner would need a band-split wrapper to keep full-band output. For the low-CPU tier, a SIMD-enabled RNNoise is the better deal. Heavier offline models such as MossFormer2_SE_48K beat DeepFilterNet by only +0.12 PESQ at 48 kHz and are not documented as causal ([ClearVoice README](https://github.com/modelscope/ClearerVoice-Studio/blob/main/clearvoice/README.md)). They fit an offline "clean a recording" feature at most. No open DFN successor from the original author, no open-weight personalized model and no open causal dereverberation model with weights turned up.

## Quick wins: a CMake fix, a smaller ring, and OS hints (days, low risk)

These are ordered by value per hour. Each one is self-contained and testable with the existing 218-test suite plus `offline-cli` A/B.

### QW1 — Rebuild RNNoise the way upstream ships it (extends review P3)

**What.** Make these changes in `third_party/rnnoise/CMakeLists.txt`:

- Define `DISABLE_DEBUG_FLOAT` so `compute_linear` uses the int8 weights instead of `float_weights`.
- Define `OPUS_X86_MAY_HAVE_SSE`, `OPUS_X86_MAY_HAVE_SSE2` and `OPUS_X86_MAY_HAVE_SSE4_1`. The vendored `x86_arch_macros.h` maps these to the `__SSE2__` and `__SSE4_1__` macros that `vec.h` tests and MSVC never defines.
- Add `src/x86/x86cpu.c`, `x86_dnn_map.c`, `nnet_sse4_1.c` and `nnet_avx2.c`, and define `RNN_ENABLE_X86_RTCD`.
- Compile only `nnet_avx2.c` with `/arch:AVX2`.

**Correction to the review's P3 recipe:** do not add `CPU_INFO_BY_ASM`. The vendored `x86cpu.c` already uses `__cpuid` under `_MSC_VER`.

**Why.** Upstream's configure defines `DISABLE_DEBUG_FLOAT` by default. The README asks builders to enable AVX2 or `--enable-x86-rtcd` ([xiph/rnnoise README](https://github.com/xiph/rnnoise); [configure.ac](https://github.com/xiph/rnnoise/blob/main/configure.ac)). As shipped today, the GRU1 input matrix alone is ~1.77 MB of float32 instead of ~442 KB of int8, and every kernel runs the `NO_OPTIMIZATIONS` scalar path. MSVC even prints a `#pragma message` warning about this ([xiph/rnnoise src](https://github.com/xiph/rnnoise/tree/main/src/x86)). The team's Linux g++ timings are not representative, because GCC defines `__SSE2__` and silently gets SIMD kernels.

**Expected gain.** About 4x less weight memory traffic. Defining `OPUS_X86_MAY_HAVE_SSE2` alone moves the non-dispatched code onto SSE2 kernels. xiph publishes no SIMD speedup figure ([Phoronix](https://www.phoronix.com/news/RNNoise-0.2-Released)). A several-fold or larger cut is plausible but must be measured on MSVC. This also makes the shipped engine match the configuration RNNoise was validated in.

**Effort.** Under a day. **Risk.** Output is not bit-exact: int8 quantization, FMA and different `tanh`/`sigmoid` approximations all change it. Switch any golden tests to tolerance comparisons, run a listening A/B on the quality WAVs, and time the result with `hopStats_` on the CI runner and on a low-end laptop. Optionally test the v0.2 "little" model for very weak CPUs. No public quality numbers exist for it, so this needs in-house measurement.

### QW2 — Cut ring B's setpoint from ~40 ms toward 15–20 ms

**What.** Lower the default drift setpoint from 4 of 8 frames to about 1.5–2 frames. Instrument underruns and `GetCurrentPadding` history before and after.

**Why.** This is the single largest app-owned latency term. OBS, NoiseTorch and the DFN LADSPA plugin all run with one segment or one STFT of model latency plus host buffering. VoiceCleaner adds ~40 ms on top of that by design ([OBS source](https://raw.githubusercontent.com/obsproject/obs-studio/master/plugins/obs-filters/noise-suppress-filter.c); [DFN LADSPA](https://github.com/Rikorose/DeepFilterNet/tree/main/ladspa)).

**Expected gain.** About 20–25 ms off every engine path. **Effort.** Small, since it is a default change plus telemetry. **Risk.** Underruns on machines with jittery render scheduling. Pair it with MP2, which makes the fill signal trustworthy enough to run closer to empty, and keep `ring_frames` user-adjustable as an escape hatch.

### QW3 — Finish the cheap DSP items P2 and P5

The limiter's O(n·L) scan (60 µs/hop) becomes O(1) amortized with Lemire's monotonic-deque streaming max, which needs at most 3 comparisons per element and adds no latency ([Lemire, arXiv cs/0610046](https://arxiv.org/abs/cs/0610046)). The output is bit-identical, which the review already noted. The AGC can compute its gain once per block in the log domain and ramp linearly per sample, replacing 480 `pow`/`log10`/`sqrt` calls with 1–3. Optionally compile the DSP files with `/fp:fast` or `/fp:contract`. MSVC refuses to vectorize float reductions under `/fp:precise` (reason code 1105) and has defaulted to no FMA contraction since VS2022 ([MS Docs: vectorizer messages](https://github.com/MicrosoftDocs/cpp-docs/blob/main/docs/error-messages/tool-errors/vectorizer-and-parallelizer-messages.md); [MS Docs: /fp](https://github.com/MicrosoftDocs/cpp-docs/blob/main/docs/build/reference/fp-specify-floating-point-behavior.md)). Expected gain is about 65 µs/hop on the default RNNoise+DSP path, which is battery and headroom rather than latency. Effort is a day or two, and the risk is low.

### QW4 — Opt out of Windows 11 power throttling

**What.** At startup, call `SetProcessInformation(ProcessPowerThrottling)` with `EXECUTION_SPEED | IGNORE_TIMER_RESOLUTION` and StateMask 0. On each MMCSS thread, call `SetThreadInformation(ThreadPowerThrottling)`.

**Why.** EcoQoS "schedules to efficient cores". Windows 11 may also ignore timer requests from non-visible, "non-audible" processes ([MS Learn: QoS](https://learn.microsoft.com/en-us/windows/win32/procthread/quality-of-service); [MS Learn: SetProcessInformation](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setprocessinformation)). A tray app whose only output is a virtual cable is exactly the case where "audible" is uncertain, and there are anecdotal reports of background programs confined to E-cores on hybrid laptops ([MS Q&A](https://learn.microsoft.com/en-us/answers/questions/1027852/windows-11-22h2-thread-director-issues-with-intel)).

**Expected gain.** Fewer p99 hop spikes on hybrid laptops. This is unmeasured, so watch the hop p99 telemetry. **Effort.** About an hour. **Risk.** Slightly higher power draw.

### QW5 — Stop double-processing: user guidance first, RAW capture as an option

**What.** First, add a one-line UX hint to turn off Discord, Teams and Zoom suppression when VoiceCleaner is in use. Stacked suppressors cause artifacts, and Discord runs Krisp locally by default ([Discord Krisp FAQ](https://support.discord.com/hc/en-us/articles/360040843952); [voxbooster](https://voxbooster.com/blog/voice-changer-krisp-ai-integration)). Second, add an option to open capture with `AUDCLNT_STREAMOPTIONS_RAW` when the endpoint reports `RawProcessingSupported`. Raw mode bypasses OEM noise suppression and AGC, which fight VoiceCleaner's own DSP and add latency ([MS Learn: Low Latency Audio](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/low-latency-audio); [signal processing modes](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/audio-signal-processing-modes)).

**Risk.** Raw mode also strips any OEM echo cancellation. Keep it off for speaker users (see MP4) and do not force it by default.

### QW6 — A VAD-driven gate with grace periods

**What.** werman's plugin gates output on RNNoise's own voice probability. It uses three knobs: a threshold (85–95% suggested), a grace period (e.g. 200 ms) that protects word endings, and a retroactive grace period that protects word onsets at the cost of added latency ([werman README](https://github.com/werman/noise-suppression-for-voice)). NoiseTorch exposes the same threshold, where 0% means "never gate" ([NoiseTorch README](https://github.com/noisetorch/NoiseTorch)). VoiceCleaner already gets RNNoise's VAD probability for free. DFN3 offers its local-SNR output as an analogous signal ([arXiv:2305.08227](https://arxiv.org/abs/2305.08227)).

**Expected gain.** Cleaner silences and fewer keyboard transients between words, at feature parity with the Linux tools. Default the retroactive grace to 0 to protect latency. **Effort.** One to two days. **Risk.** Clipped onsets on poor mics if the threshold is set too high.

The open hygiene items from the review belong in this tier too, without further research. P4 makes the logger opt-in and polls it at 1 Hz. C1 adds a telemetry seqlock. C2 builds the DFN3 leaf outside `engMutex_`. S2 handles UTF-8 paths and moves settings to `%LOCALAPPDATA%`. S3 caches the tray tooltip. S4 fixes stale copy. P7 covers small cleanups.

## Medium projects: make DFN3 cheaper, then offer a better model and AEC (1–4 weeks each)

### MP1 — Stateful one-frame DFN3 graphs (supersedes review P6)

**What.** Re-export enc, erb_dec and df_dec with explicit state inputs and outputs. Each causal conv gets a cache of its last `kt−1` frames, and each GRU gets `h` in and out. Run T=1 frame per hop and ping-pong two bound OrtValues per state. The reference real-time implementation, libDF on Rust/tract, avoids window recompute entirely: it loads the stock graphs with a symbolic time axis and converts them into stateful one-frame "pulsed" models ([libDF tract.rs](https://github.com/Rikorose/DeepFilterNet/blob/main/libDF/src/tract.rs)). Community ports already do the same with explicit state tensors, including a Core ML DFN3 streaming conversion and torchDF-derived ONNX exports ([iky1e/DeepFilterNet3-Streaming-CoreML](https://huggingface.co/iky1e/DeepFilterNet3-Streaming-CoreML); [kimtos-labs/denoiser-dfn3](https://huggingface.co/kimtos-labs/denoiser-dfn3)).

**Why.** Today's WindowRunner is exact, but each hop pays for W frames: 6 for enc_conv, 1 for enc_gru, 3 for erb_dec and 5 for df_dec.

**Expected gain.** An estimated 3–5x lower DFN3 hop time, before fixed per-`Run()` overhead. That overhead then dominates, so fusing graphs is a follow-up. This estimate is unmeasured. **Effort.** One to three weeks, mostly export tooling. Doing P6's per-model W reduction first is a cheaper partial step, but it becomes throwaway work once MP1 lands. **Risk.** Parity drift. The existing `tools/dfn3_stream_check.py` and `dfn3_compare.py` are the acceptance gate. A fallback is tract via libDF's C API, which pulses the unmodified graphs ([DeepFilterNet README](https://github.com/Rikorose/DeepFilterNet)).

### MP2 — A smoother drift loop that lets ring B run nearly empty

**What.**
- Sample ring-B fill at a fixed point, or time-interpolate it with QPC positions from `GetBuffer`.
- Low-pass the error before the PI.
- Run around 0.05–0.1 Hz loop bandwidth once locked, with a wider fast-acquire phase for the first few seconds.
- Slew-limit the ratio.

This is Adriaensen's zita-a2j design: a DLL-timestamped error, a 2nd-order prefilter, a 2nd-order loop filter at "around 0.05 Hz", and wider bandwidth for the first 4 s. He warns that raw buffer fill "won't work" as a control signal because it jumps every period ([Adriaensen](https://kokkinizita.linuxaudio.org/papers/adapt-resamp.pdf)).

**Expected gain.** It makes QW2's smaller setpoint safe. Fast acquisition also complements the B2 fix after a device reselect. **Effort.** About a week, including tests on synthetic clocks. **Risk.** Loop tuning. An alternative is to open the cable render stream with `AUDCLNT_STREAMFLAGS_RATEADJUST` and let audiodg's SRC apply the PI ratio, but the quality of that SRC is undocumented ([MS Learn: AUDCLNT_STREAMFLAGS](https://learn.microsoft.com/en-us/windows/win32/coreaudio/audclnt-streamflags-xxx-constants)). Do not spend effort replacing Catmull-Rom with sinc for speech.

### MP3 — An optional DPDFNet-2 "max quality" engine, with a no-look-ahead DFN option

**What.** Integrate `dpdfnet2_48khz_hr`. It is Apache-2.0, ships as ONNX, uses 480-sample blocks in its `StreamEnhancer` example, and stays in the DFN family ([Ceva-IP/DPDFNet](https://huggingface.co/Ceva-IP/DPDFNet); [GitHub ceva-ip/DPDFNet](https://github.com/ceva-ip/DPDFNet)). A one-frame-plus-state ONNX export already exists, so MP1's state-binding code carries over ([TigreGotico/audiosronnx-dpdfnet](https://huggingface.co/TigreGotico/audiosronnx-dpdfnet)).

**Expected gain.** On Ceva's set, PRISM rises from 0.82 to 0.95 and PESQ from 2.45 to 2.79, along with the streaming-stability fine-tune.

**Effort.** One to two weeks after MP1. **Risk.**
- About 7x DFN3's MACs, and no CPU RTF has been published. Benchmark on a low-end laptop and gate the engine behind a measured-headroom check.
- Ceva's numbers are self-reported.

In the same project, evaluate a zero-look-ahead DFN model like the one the LADSPA plugin uses, at a stated 20 ms minimum latency ([DFN LADSPA](https://github.com/Rikorose/DeepFilterNet/tree/main/ladspa)). Compare it against the existing `low_latency` mode on both quality and actual delay, and use one latency definition throughout: the DFN3 paper counts 40 ms where VoiceCleaner counts 30.

### MP4 — "Speaker mode" echo handling through Windows' own AEC

**What.** Add an opt-in mode that opens capture as `AudioCategory_Communications` without RAW, so OEM or Voice Clarity echo cancellation runs before VoiceCleaner. On build 22621+, call `IAcousticEchoCancellationControl::SetEchoCancellationRenderEndpoint` to point the AEC at the user's real speakers ([MS Learn: IAcousticEchoCancellationControl](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nn-audioclient-iacousticechocancellationcontrol); [MS Learn: Windows 11 APO APIs](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/windows-11-apis-for-audio-processing-objects)).

**Why.** Speaker users currently send far-end echo through a nonlinear denoiser, and that weakens the downstream app's AEC. AEC conventionally runs before denoising. Voice Clarity, where present, adds echo, noise and reverb handling on CPU without an NPU ([Croma](https://www.croma.com/unboxed/microsoft-windows-11-voice-clarity-feature)).

**Effort.** Days. **Risk.** Behavior varies by OEM, the mode adds OEM processing latency, and `E_NOINTERFACE` is common, so treat it as best-effort.

### MP5 — Low-latency shared periods and format tolerance

**What.**
- Try `IAudioClient3::InitializeSharedAudioStream` at the smallest legal period on capture. Fall back on `AUDCLNT_E_ENGINE_PERIODICITY_LOCKED`, on `E_NOINTERFACE`, or when min equals default ([MS Learn: Low Latency Audio](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/low-latency-audio)).
- Log what VB-Cable reports, since nobody has published that.
- Accept non-48 kHz endpoints with `AUTOCONVERTPCM | SRC_DEFAULT_QUALITY` instead of refusing them ([AUDCLNT_STREAMFLAGS](https://learn.microsoft.com/en-us/windows/win32/coreaudio/audclnt-streamflags-xxx-constants)).

**Expected gain.** Bounded at under ~10 ms, because the 480-sample framer stays the floor. The format tolerance is a robustness gain. **Effort.** About a week. **Risk.** Small periods switch every client on that endpoint and raise power use.

### MP6 — ORT hygiene and int8 DFN

**What.** Move to one process-wide `Ort::Env` with global thread pools (`DisablePerSessionThreads`), env-registered allocators with a fixed initial arena, and an offline-optimized model saved at `ORT_ENABLE_EXTENDED` for portability ([ORT C/C++ docs](https://github.com/microsoft/onnxruntime/blob/gh-pages/docs/get-started/with-c.md); [graph optimizations](https://github.com/microsoft/onnxruntime/blob/gh-pages/docs/performance/model-optimizations/graph-optimizations.md)). After MP1, try S8S8 QDQ dynamic quantization, which ORT recommends for RNNs ([ORT quantization](https://github.com/microsoft/onnxruntime/blob/gh-pages/docs/performance/model-optimizations/quantization.md)).

**Expected gain.** Memory and startup improve now; the speed gain from quantization is model-dependent and unmeasured. **Risk.** DFN's mask and filter outputs are sensitive to quantization error, so gate the change on PESQ/DNSMOS.

Skip GPU/NPU execution providers. DirectML is in sustained engineering ([DirectML EP](https://github.com/microsoft/onnxruntime/blob/gh-pages/docs/execution-providers/DirectML-ExecutionProvider.md)), and per-call submission overhead on four tiny graphs every 10 ms likely costs more than it saves.

## Large projects: a signed virtual mic is the only way past VB-Cable

### LP1 — Your own virtual-mic driver (SysVAD- or ACX-derived)

**What.** Replace VB-Cable with a root-enumerated virtual capture endpoint that VoiceCleaner owns. Microsoft's SysVAD sample shows the structure, including how to declare small-buffer `KSAUDIO_PACKETSIZE_CONSTRAINTS2` ([SysVAD](https://github.com/microsoft/Windows-driver-samples/tree/main/audio/sysvad)). ACX is the newer KMDF route ([MS Learn: ACX](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/acx-audio-class-extensions-overview)). Scream proves an indie MSVAD-derived driver can ship signed builds ([duncanthrax/scream](https://github.com/duncanthrax/scream)).

**Why.** VB-Cable's default 7168-sample buffer lets a stream sit at up to ~70 ms. VB-Audio staff themselves advise against lowering it and recommend at least 3x the larger client buffer ([VB-Audio forum p=6289](https://forum.vb-audio.com/viewtopic.php?p=6289); [p=1594](https://forum.vb-audio.com/viewtopic.php?p=1594)). An owned endpoint removes the double "render → cable → capture" buffering and the second clock domain, because the virtual clock can follow the mic as VAC's "clock control" does ([VAC manual](https://vac.muzychenko.net/en/manual/repeater.htm)). It also delivers a branded "VoiceCleaner Microphone" and a one-step install.

**Expected gain.** Likely the largest single latency cut available, plus a polished product. The size is unquantified because no independent VB-Cable delay measurement exists.

**Effort and cost.** Large: kernel development, an EV code-signing certificate, Partner Center registration, and an attestation submission on every release. Attestation-signed drivers install through your installer but are not distributed through Windows Update ([MS Learn: Attestation signing](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-attestation)).

**Risk.** Bugchecks on user machines, and install failures of the kind users report with other vendors' virtual mics ([AudioRelay community](https://community.audiorelay.net/t/virtual-mic-driver-does-not-install/1482)). For a solo developer, defer this until QW2 and MP2 have shown how much latency is left, and until the user base justifies the yearly certificate and maintenance load. Do not pursue a capture APO instead. APOs ship only inside a device's driver package and need CAPX/HLK on Windows 11 ([MS Learn: Implementing APOs](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/implementing-audio-processing-objects); [Windows 11 APO APIs](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/windows-11-apis-for-audio-processing-objects)), so that route is built for OEMs, not for arbitrary user mics.

### LP2 — In-app AEC3 with a loopback reference

Vendoring WebRTC AEC3 out of its GN build and feeding it a WASAPI loopback of the render device would give echo cancellation on every Windows build. AEC3 has its own echo-path delay estimator ([WebRTC review](https://codereview.webrtc.org/2644123002/patch/180001/190001)). The approach costs an estimated 10–20 ms of alignment buffering plus substantial build work. Build it only if MP4 proves too inconsistent across OEMs.

**Not recommended now:**
- GTCRN, LiSenNet or UL-UNAS as a low tier: 16 kHz only, and no robustness win over RNNoise on hard audio.
- RT-SEMamba and other SSMs: no weights, no ONNX path.
- PercepNet and Personalized PercepNet: no public weights.
- MossFormer2 for live use: likely non-causal and heavy.

## Recommended order and latency arithmetic

| Order | Item | Tier | Main payoff | Cross-ref |
|---|---|---|---|---|
| 1 | QW1 RNNoise int8 + SSE2/RTCD build, with A/B and timing | Quick | CPU on the default engine; matches upstream | P3 (amended) |
| 2 | QW3 limiter deque + AGC block gain | Quick | ~65 µs/hop on the default path | P2, P5, P7 |
| 3 | QW4 power-throttling opt-out; P4 logger; C1; C2 | Quick | p99 stability, idle power, robustness | P4, C1, C2 |
| 4 | MP2 filtered drift loop, then QW2 ring setpoint ~15–20 ms | Medium → Quick | **~20–25 ms less latency** | builds on B2 |
| 5 | QW5 double-processing hint / RAW option; QW6 VAD grace gate; S2–S4 | Quick | Quality and UX parity | S2, S3, S4 |
| 6 | MP1 stateful DFN3 export | Medium | Est. 3–5x cheaper DFN3 hops | supersedes P6 |
| 7 | MP3 DPDFNet-2 engine + no-look-ahead DFN evaluation | Medium | Quality jump or ~10 ms less DFN latency | — |
| 8 | MP4 speaker-mode AEC; MP5 IAudioClient3 + format tolerance; MP6 ORT hygiene/int8 | Medium | Speaker users, robustness | — |
| 9 | LP1 own virtual-mic driver (decide after measuring 1–8) | Large | Removes cable buffer and drift domain | — |

Before step 4, add one measurement the field lacks: a loopback test of actual mic-to-cable-capture delay. No vendor publishes end-to-end numbers (NVIDIA gives no latency figure at all ([NVIDIA FAQ](https://www.nvidia.com/broadcast-app-faq))), so a measured figure in VoiceCleaner's UI would be both a tuning tool and a differentiator. The arithmetic shows why the order matters. Today the RNNoise path is roughly 10 ms capture + up to 10 ms framer + ~40 ms ring B + ~10 ms render, then VB-Cable's own buffer on top. Step 4 alone removes about a third of the app-owned part. Only step 9 touches the remainder.

## Conclusion

The research changes the framing of the problem. VoiceCleaner does not have a model gap. Its RNNoise path matches OBS, and its DFN3 path beats every open causal model except one released ten months ago. It has a plumbing gap and a build gap. The biggest measurable losses are self-inflicted and cheap to fix: a ring setpoint chosen for safety rather than measured need, and an RNNoise build that silently runs debug float weights in scalar C on the very compiler it ships with. Both are fixable in days.

The strategic decision is whether to own the virtual device. Every polished competitor (NVIDIA, Krisp, SteelSeries, Elgato) does, and that is where the remaining tens of milliseconds and the "it just appears as a mic" experience live. For a solo developer, the right move is to collect the cheap wins first, measure what latency is left, and only then decide whether a driver's signing and maintenance burden is worth it. If DPDFNet-class quality also lands on the stateful-graph infrastructure, VoiceCleaner can credibly claim better-than-Krisp-class quality on any CPU, which is a position no GPU- or NPU-bound competitor can take.
