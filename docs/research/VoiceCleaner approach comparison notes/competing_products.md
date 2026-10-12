# Competing real-time microphone noise-suppression products vs VoiceCleaner

Research date: 2026-10-11. Method note: the research proxy could not resolve several primary vendor domains (nvidia.com, learn.microsoft.com, obsproject.com) for direct fetches. Some claims below therefore come from search-result snippets of those pages, or from secondary sources, and they are flagged where that applies. Directly fetched primary sources were GitHub READMEs (werman, NoiseTorch-ng, DeepFilterNet, DeepFilterNet LADSPA) and the OBS noise-suppress filter source code.

VoiceCleaner reference (from the assignment brief): Windows, WASAPI shared-mode capture, 480-sample/10 ms frames, RNNoise (scalar) or DeepFilterNet3 via ONNX Runtime CPU (about 30 ms algorithmic latency), DSP chain, drift-compensated cubic resampler with a PI controller on ring occupancy, render into third-party VB-Cable, about 60-100 ms end to end.

## NVIDIA Broadcast / RTX Voice: virtual device mechanism, models, latency, GPU requirement

### Takeaway
NVIDIA Broadcast is a GPU-only (RTX Tensor Core) app. It ships its own virtual audio devices ("Microphone (NVIDIA Broadcast)" and a matching speaker) and offers noise removal, room echo removal and, on higher-end GPUs, Studio Voice. NVIDIA publishes no latency figure, and I found no independent measurement. Its cost shows up as GPU and VRAM use (about 350 MB of VRAM for input noise removal in v1.3), not CPU.

### Cited Findings
- Noise removal "uses Tensor Cores on NVIDIA RTX GPUs to accelerate AI calculations" (vendor FAQ, seen as a search snippet) — [NVIDIA Broadcast FAQ](https://www.nvidia.com/en-eu/geforce/broadcasting/broadcast-app/faq/)
- Minimum GPU is GeForce RTX 2060, Quadro RTX 3000, TITAN RTX or higher (third-party download listing) — [TechSpot](https://www.techspot.com/downloads/7304-nvidia-broadcast.html)
- Virtual Key Light and Studio Voice require a GeForce RTX 3060 desktop GPU or higher (vendor FAQ snippet) — [NVIDIA Broadcast FAQ](https://www.nvidia.com/en-gb/geforce/broadcasting/broadcast-app/faq)
- HISTORICAL (April 2020): RTX Voice was a beta plugin. NVIDIA later patched it to run on GTX GPUs "your mileage may vary", and it required driver 456.38+ for Broadcast — [NVIDIA RTX Voice setup guide](https://www.nvidia.com/en-eu/geforce/guides/nvidia-rtx-voice-setup-guide/)
- Broadcast creates a virtual mic, "Microphone (NVIDIA Broadcast)", which the user selects in OBS, Discord, Zoom and similar apps (third-party guide) — [SimpleClean](https://simpleclean.app/blog/remove-background-noise-in-nvidia-broadcast)
- v1.3 reduced system impact. Input noise cancelling used about 350 MB of VRAM (press coverage, about 2021, historical) — [Techgage](https://techgage.com/news/nvidia-releases-broadcast-1-3/)
- v1.2 added room echo removal (historical, about 2021) — [Engadget](https://www.engadget.com/nvidia-broadcast-app-update-1-2-110034015.html)
- NVIDIA says performance impact depends on the GPU, the game, the webcam resolution and frame rate, and the number of effects. It gives no audio latency figure — [NVIDIA Broadcast FAQ (search snippet)](https://www.nvidia.com/broadcast-app-faq)
- OBS exposes the same NVIDIA Audio Effects SDK as a "NVIDIA Noise Removal" filter. Users have reported it "stops working midstream" — [OBS forum thread](https://obsproject.com/forum/threads/nvidia-noise-removal-stops-working-midstream.184466/)

### Inferences
- NVIDIA ships and signs its own WDM virtual audio driver, so users avoid installing a third-party cable. Because NVIDIA owns both ends, it can in principle let the virtual endpoint's clock follow the capture device. No public documentation confirms this.
- The GPU dependency is the main architectural difference from VoiceCleaner. VoiceCleaner's CPU-only design reaches any Windows PC, at the cost of model capacity.

### Gaps
- No vendor or independent latency number for Broadcast noise removal was found.
- I could not fetch the current (2025-2026, v2.x) system requirements page directly because of a DNS failure on nvidia.com.
- The drift-handling mechanism is undocumented.

## Krisp (desktop app and Discord's Krisp integration): virtual mic driver, model size, latency, CPU, echo cancellation

### Takeaway
The Krisp desktop app installs its own virtual microphone and speaker devices and runs on-device CPU models. The only latency figure is an old vendor claim of about 15 ms (2018-2020), and the only SDK memory figure is about 30 MB. Discord embeds Krisp in-app (SDK integration, no virtual device), with on-device processing that filters only the local user's mic.

### Cited Findings
- 2018: "very short latency (around 15 milliseconds)", processing entirely on device (vendor claim reported by press, historical) — [TechCrunch 2018](https://techcrunch.com/2018/12/10/krisp-reduces-noise-on-calls-using-machine-learning-and-its-coming-to-windows-soon)
- Krisp SDK sheet (archived 2020): "minimal latency impact (15ms)". The noise suppression component uses 30 MB of RAM and storage (vendor, historical) — [Krisp SDK PDF, Wayback](https://webcf.waybackmachine.org/web/20200808123722/https://krisp.ai/KrispSDK.pdf)
- Krisp creates a virtual microphone and speaker on the device (third-party review) — [daveswift.com review](https://daveswift.com/krisp/)
- 3-7% CPU and 150-200 MB of memory (unverified blog claim). Krisp auto-disables when device CPU is very high (secondary source) — [callin.io](https://callin.io/krisp-ai-pros-and-cons/)
- Discord: the ML model runs on your device. No data is sent to Krisp, and only noise from your side is filtered — [Discord Krisp FAQ](https://support.discord.com/hc/en-us/articles/360040843952)
- Discord brought Krisp to iOS and Android — [Discord blog](https://discord.com/blog/tune-into-discord-with-krisp-noise-suppression-on-ios-and-android)
- Echo/room-echo removal in Krisp is mentioned only by secondary sources. I found no primary source confirming AEC in Discord's integration — [callin.io company guide](https://callin.io/krisp-company-complete-guide); [voxbooster](https://voxbooster.com/blog/voice-changer-krisp-ai-integration)
- One secondary blog advises disabling Discord's built-in Krisp when external Krisp is in use, to avoid double processing — [voxbooster](https://voxbooster.com/blog/voice-changer-krisp-ai-integration)

### Inferences
- Krisp's 15 ms claim refers to processing latency only. It likely excludes virtual-device buffering. It is still a useful target for VoiceCleaner's algorithmic path, since RNNoise at 10 ms frames is comparable.
- The double-processing problem applies to VoiceCleaner users too. Discord/Teams/Zoom built-in suppression stacked on VoiceCleaner output can cause artifacts, so a UX hint to disable app-side suppression would help.

### Gaps
- No current (2025-2026) official Krisp latency, CPU or model-size data was found. Krisp's driver signing and drift handling are undocumented.

## Microsoft Voice Clarity / Windows Studio Effects "voice focus" / APO integration

### Takeaway
Windows now offers OS-level suppression in two ways. Voice Clarity is a low-complexity AI model for echo, noise and reverb that runs on the CPU in the capture path for apps using Communications Signal Processing Mode, and it originated on Surface. Windows Studio Effects "voice focus" requires an NPU, with the full set on 40+ TOPS Copilot+ PCs. Both live in the Windows audio engine as APOs and effects rather than as virtual devices, so no extra endpoint is needed. They only apply where the app or stream opts into the right mode.

### Cited Findings
- Voice Clarity uses "low complexity AI models" to cancel echo, suppress background noise and reduce reverberation in real time. It is on by default for apps using Communications Signal Processing Mode (Phone Link, WhatsApp). It was previously Surface-only and expanded to all users via Canary builds in early 2024 — [Entrepreneur India](https://india.entrepreneur.com/technology/microsoft-adds-ai-backed-voice-clarity-feature-to-windows/468988); [Analytics Vidhya](https://www.analyticsvidhya.com/blog/2024/02/microsofts-ai-powered-voice-clarity-revolutionizes-audio-on-windows/)
- Apps can offer a toggle to disable the "Deep Noise Suppression" effect (secondary report) — [Analytics Vidhya](https://www.analyticsvidhya.com/blog/2024/02/microsofts-ai-powered-voice-clarity-revolutionizes-audio-on-windows/)
- Under the Windows 11 APO APIs, an AEC APO in the mic capture pipeline takes two inputs, the mic and a render reference. The audio engine supplies the reference stream and switches it when the render device changes — [Microsoft Learn: Windows 11 APIs for APOs](https://learn.microsoft.com/en-sg/WINDOWS-HARDWARE/DRIVERS/audio/windows-11-apis-for-audio-processing-objects)
- Windows Studio Effects require an NPU. The full set needs a Copilot+ PC with an NPU over 40 TOPS. Voice focus "isolates your voice and minimizes background noise" — [Microsoft Support: Windows Studio Effects](https://support.microsoft.com/id-id/windows/ai/ai-features/windows-studio-effects)

### Inferences
- The APO approach gives in-place processing on the real mic endpoint. There is no virtual cable, no second clock domain, no drift compensation, and the engine supplies the loopback reference for AEC. That is the main structural advantage over VoiceCleaner's capture-then-cable design.
- Third-party APOs (as in Equalizer APO) must be registered per endpoint. Microsoft's APO model targets driver-packaged (IHV/OEM) APOs, so a standalone app shipping an APO faces signing and packaging hurdles. This is general knowledge and was not sourced here.

### Gaps
- I could not confirm whether voice focus is in the under-40-TOPS effect set (the snippet was truncated).
- No latency or CPU numbers were found for Voice Clarity or voice focus.
- I could not fetch learn.microsoft.com to confirm the Voice Clarity driver-side docs (DNS failure).

## Linux and open-source: NoiseTorch, EasyEffects, werman noise-suppression-for-voice (incl. VAD grace design)

### Takeaway
All three wrap RNNoise (EasyEffects also wraps DeepFilterNet and Speex) as a filter node inside the system sound server (PulseAudio or PipeWire) or a plugin host (Equalizer APO/VST on Windows). The sound server then owns device creation and clock-domain bridging. werman's plugin adds a three-parameter VAD gate (threshold, grace period, retroactive grace period), where the retroactive grace period trades added latency for keeping word onsets.

### Cited Findings
- werman/noise-suppression-for-voice:
  - Formats: LADSPA, LV2, VST2, VST3, AU/AUv3 (macOS build options; macOS usage marked "TODO"). Based on RNNoise, GPL-3.0 — [GitHub README](https://github.com/werman/noise-suppression-for-voice)
  - Windows use: the VST2 DLL loaded through Equalizer APO on the mic endpoint — [GitHub README](https://github.com/werman/noise-suppression-for-voice)
  - PipeWire (>=0.3.45): LADSPA plugin through `libpipewire-module-filter-chain`. PulseAudio: `module-ladspa-sink` plus loopback. Chrome ignores monitor devices, so a remap-source is needed — [GitHub README](https://github.com/werman/noise-suppression-for-voice)
  - Requires 48 kHz input: "Do NOT use any other sample rates." Stereo costs 2x resources — [GitHub README](https://github.com/werman/noise-suppression-for-voice)
  - VAD Threshold (%) silences output when voice probability is below the threshold, with 85-95% suggested. VAD Grace Period (ms) keeps output open after the last voice detection, so word endings aren't cut. Retroactive VAD Grace Period (ms) does the same for word starts but "adds latency". Example configs use 50 / 200 / 0, though one PulseAudio example uses 50,20,0 (inconsistent) — [GitHub README](https://github.com/werman/noise-suppression-for-voice)
  - For PulseAudio crackling or growing latency, `latency_msec=1` on the loopback "might help" — [GitHub README](https://github.com/werman/noise-suppression-for-voice)
  - Limitations: it can make bad mics worse by misclassifying voice, loud transients such as keyboard clicks are only reduced while voice is present, and MinGW builds are broken — [GitHub README](https://github.com/werman/noise-suppression-for-voice)
- NoiseTorch-ng:
  - A one-click "Load" creates a "Filtered Microphone" virtual mic on PulseAudio or PipeWire, using RNNoise — [GitHub README](https://github.com/noisetorch/NoiseTorch)
  - Stated latency: 10 ms inherent to suppression, plus variable PulseAudio latency. Output filtering adds about 100 ms, flagged as possibly unsuitable for competitive gaming — [GitHub README](https://github.com/noisetorch/NoiseTorch)
  - The "Voice Activation Threshold" slider goes up to 95% for good mics. At 0% noise is reduced but the mic is never gated. Changes require a reload — [GitHub README](https://github.com/noisetorch/NoiseTorch)
  - Security history: a suspected compromise of the update server and repo was followed by a community audit, and "No malicious code has been found." Setup requires `setcap CAP_SYS_RESOURCE` — [GitHub README](https://github.com/noisetorch/NoiseTorch)
- EasyEffects:
  - "Noise reduction" uses RNNoise, "Deep noise remover" uses DeepFilterNet, and "Speech processor" uses SpeexDSP, which provides noise suppression and VAD (secondary summaries) — [scour.ing mirror of the EasyEffects README](https://scour.ing/p/https://github.com/wwmm/easyeffects); [Solus forum](https://discuss.getsol.us/d/9596-showcase-noise-reduction-with-pipewire-and-easyeffects)
  - User anecdote: DeepFilterNet sounded much better than NoiseTorch/RNNoise — [Solus forum](https://discuss.getsol.us/d/10034-using-deepfilternet-with-easy-effects)
  - In 8.1.3 the virtual sink stopped the ALSA node switching to the source rate (regression) — [EasyEffects issue #4920](https://github.com/wwmm/easyeffects/issues/4920)

### Inferences
- werman's retroactive grace period is effectively a lookahead delay line on the gate. VoiceCleaner's noise gate and 3 ms lookahead limiter could adopt the same three-knob VAD model, with the retroactive grace defaulting to 0 to protect latency. Users with a latency budget could raise it to stop clipped plosive and fricative onsets.
- The VAD-gate design in both NoiseTorch and werman uses RNNoise's own VAD probability, which VoiceCleaner already gets for free from RNNoise. That output can drive gating and AGC without a separate VAD.

### Gaps
- No measured CPU numbers were found for NoiseTorch or werman. No measured EasyEffects latency was found.

## DeepFilterNet's own realtime offerings

### Takeaway
DeepFilterNet ships a Rust `deep-filter` CLI (offline WAV, 48 kHz only) and a LADSPA plugin with PipeWire filter-chain configs, both as a virtual source (mic) and as a virtual sink. The LADSPA build uses a no-lookahead model with a stated minimum latency of 20 ms (STFT), plus host latency. That is lower than VoiceCleaner's about 30 ms DFN3/ONNX path.

### Cited Findings
- Model line: DeepFilterNet (2021), DeepFilterNet2 (default in `deep-filter`), and DeepFilterNet3 ("Perceptually Motivated Real-Time Speech Enhancement", Interspeech 2023). 48 kHz full band. `--compensate-delay` compensates "delay of STFT and model lookahead". GPU only through the PyTorch path. Training is tested only on Linux — [GitHub README](https://github.com/Rikorose/DeepFilterNet)
- LADSPA plugin: the model has no lookahead, and "the minimum latency is 20 ms (STFT processing)" plus host latency. Configs exist for `deepfilter-mono-source.conf` (virtual mic) and `deepfilter-stereo-sink.conf` (virtual sink) — [DeepFilterNet LADSPA README](https://github.com/Rikorose/DeepFilterNet/tree/main/ladspa)

### Inferences
- VoiceCleaner could cut the DFN path's algorithmic latency by about 10 ms by exporting or using a zero-lookahead DFN3 variant, as the LADSPA plugin does. The LADSPA plugin runs on the Rust libDF/tract runtime, which hints that a tract/libDF native build is a viable alternative to ONNX Runtime. That runtime detail is not confirmed on the fetched page, which mentioned only libDF and PyTorch.

### Gaps
- No RTF or CPU figures were on the fetched pages. No published attenuation-limit defaults were found for the LADSPA plugin.

## OBS, SteelSeries Sonar ClearCast AI, Elgato Wave Link, Logitech G HUB Blue VO!CE, Voicemod, Zoom/Teams

### Takeaway
OBS processes in-app in 10 ms (480-sample) blocks with Speex, RNNoise or NVIDIA, and reports 10 ms latency. That mirrors VoiceCleaner's framing. SteelSeries says ClearCast AI is CPU-based "to avoid dropped frames". Elgato and Logitech add DSP chains (Clipguard, noise reduction, HPF, EQ, compression, de-esser) to their own mixer or virtual-device software. Teams and Zoom run in-client DNN suppression with Low/Auto/High levels.

### Cited Findings
- OBS `noise-suppress-filter.c`: BUFFER_SIZE_MSEC = 10 ("Process 10 millisecond segments to keep latency low"). RNNOISE_FRAME_SIZE = 480. RNNoise needs 48 kHz, so OBS creates to-48k and from-48k resamplers when the output rate differs. Reported latency is one 10 ms segment, and timestamps are offset by it. Buffers clear on a timestamp jump over 1 s. The Speex default suppress level is -30 dB (range -60 to 0). The default method is RNNoise when available — [OBS source](https://raw.githubusercontent.com/obsproject/obs-studio/master/plugins/obs-filters/noise-suppress-filter.c)
- SteelSeries Sonar: ClearCast AI noise cancellation is "CPU-based to avoid frame drops", works with any mic, and pairs with Sonar's noise gate and compressor — [SteelSeries Sonar for streamers](https://steelseries.com/pt-br/gg/sonar-for-streamers)
- Elgato: Wave Link routes the mic and up to eight sources with "virtually zero latency" (marketing). The Wave FX processor runs Clipguard 2.0, DSP and VST inserts. Clipguard routes a secondary lower-gain path used when peaks occur — [KitGuru](https://www.kitguru.net/?p=472816); [Scan Wave XLR MK.2](https://www.scan.co.uk/products/elgato-wave-xlr-mk2-80db-gain-onboard-effects-led-feedback-touch-mute-works-with-stream-deck-pc-and)
- Logitech Blue VO!CE: real-time filters whose noise reduction targets constant noise (fans, road, rain). The PRO filters add noise reduction, HPF, EQ, compression and de-essing. PC only, via G HUB — [Logitech G Blue VO!CE](https://www.logitechg.com/discover/technology/blue-voice); [Logitech support](https://support.logi.com/hc/en-hk/articles/14149730448407-What-is-Blue-VO-CE)
- Microsoft Teams: DNN suppression in the client with Auto/Low/High levels. It is disabled during cloud recording and live captions (2020, historical) — [Office365itpros](https://office365itpros.com/2020/11/17/teams-noise-suppression-meetings/); [Techzine](https://www.techzine.eu/news/collaboration/51113/microsoft-improves-noise-suppression-in-teams/)
- Zoom: background noise suppression with Low/Medium/High choices (2020 report, historical) — [T3](https://t3.com/news/zoom-under-fire-as-microsoft-teams-gets-killer-new-feature-heres-how-to-use-it-2)

### Inferences
- OBS's design shows that RNNoise at 10 ms frames with an explicit latency report of one frame is the baseline. VoiceCleaner's extra 50-90 ms comes from cable and ring priming, not from the model.
- Elgato's Clipguard (a parallel lower-gain path chosen at peaks) is an alternative to VoiceCleaner's lookahead limiter for clip protection at near-zero latency. It is mainly useful pre-ADC on hardware, though.

### Gaps
- No details were found on Sonar's virtual-device driver or ClearCast latency, Elgato "Voice Focus", Voicemod's driver or noise suppression, or Razer's products. Treat these as unresearched.

## Clock drift handling and own-driver vs third-party-cable trade-offs

### Takeaway
Graph-based sound servers such as PipeWire handle drift centrally, by making the capture source the graph driver and adaptively resampling at sinks. Hardware vendors (NVIDIA, Krisp, SteelSeries, Elgato) ship their own virtual drivers but publish nothing on drift handling. VB-Cable users must tune cable buffer latency, which should be at least 3x the larger app buffer. VoiceCleaner's PI-controlled resampler is a sound, standard approach, but its ring priming sets a high latency floor.

### Cited Findings
- PipeWire: the graph driver is the highest-priority linked device. Sources are normally given higher priority "so adaptive resampling happens on the sinks", which avoids capture distortion. Devices sharing a `clock.name` skip resampling. A filter-chain is a capture stream plus a playback stream, so it can act as a virtual source or sink — [pipewire-devices(7)](https://manpages.opensuse.org/Leap-15.6/pipewire/pipewire-devices.7.en.html); [PipeWire filter-chain docs](https://sanchayanmaity.pages.freedesktop.org/pipewire/page_module_filter_chain.html)
- Adaptive resampling can audibly drift in pitch on network tunnels, and there are reports of resampler distortion — [PipeWire issue 2637](https://gitlab.freedesktop.org/pipewire/pipewire/-/issues/2637); [PipeWire issue 1646](https://gitlab.freedesktop.org/pipewire/pipewire/-/issues/1646)
- VB-Cable (vendor forum, not docs): internal cable latency should be at least 3x the larger of the player's and recorder's buffer sizes. The cable applies sample-rate conversion between internal and app rates — [VB-Audio forum](https://forum.vb-audio.com/viewtopic.php?p=7429); [VB-Audio forum "Reduce delay"](https://forum.vb-audio.com/viewtopic.php?p=5709)
- A 50 ppm mismatch at 48 kHz drifts about 2.4 samples/s, about 180 ms/hour, if uncorrected. Fixes are continuous resampling or frame drop/insert — [Forasoft](https://www.forasoft.com/learn/audio-for-video/articles-audio/drift-correction-resampling-frame-drop)
- NoiseTorch on PulseAudio has variable latency it cannot fix without PulseAudio changes. Output filtering adds about 100 ms — [NoiseTorch README](https://github.com/noisetorch/NoiseTorch)

### Inferences (comparison to VoiceCleaner and adoption candidates)
- Integration: VoiceCleaner (app + third-party VB-Cable) is closest to the Krisp and NVIDIA model, without the vendor-signed driver. Consequences are a separate VB-Cable install, a generic "CABLE Output" name instead of a branded mic, a cable buffer adding latency, and a second clock domain needing drift correction. Shipping an own driver requires an EV cert plus Microsoft attestation signing. An APO approach (Equalizer APO style, or Voice Clarity) avoids the second endpoint but is per-endpoint and harder to distribute.
- Latency: the competitors' model latency (OBS 10 ms, NoiseTorch 10 ms, Krisp about 15 ms vendor claim, DFN LADSPA 20 ms) is in line with or below VoiceCleaner's. VoiceCleaner's 60-100 ms total comes mostly from priming the ring to about 40 ms plus cable buffers. Candidates to adopt: (a) a smaller ring set point (for example 10-20 ms), letting the PI controller handle drift; (b) a zero-lookahead DFN variant (20 ms, per the DFN LADSPA plugin); (c) a "low latency" preset that recommends minimal VB-Cable buffer settings.
- Features to adopt: werman-style three-parameter VAD gating with an optional retroactive grace; a reference-signal AEC (an engine-provided loopback reference as in the Windows AEC APO model, or WASAPI loopback capture of the render device); a dereverb/room-echo option (NVIDIA, Voice Clarity); suppression-strength levels (Teams/Zoom Low/Auto/High, OBS -30 dB default) mapped onto VoiceCleaner's dry/wet; and a UX warning to disable Discord/Teams built-in suppression to avoid double processing.
- GPU/NPU: no CPU-only competitor besides SteelSeries ("CPU-based to avoid frame drops") advertises its CPU choice. VoiceCleaner could treat CPU-only as a feature, and optionally use the DirectML or QNN execution providers in ONNX Runtime for GPU/NPU offload of DFN3. That is an inference, not something competitors were confirmed to do.

### Gaps
- No vendor documents drift handling for NVIDIA, Krisp, Sonar, Wave Link or Voicemod virtual drivers.
- No independent end-to-end latency measurements comparing these products were found.
- No 2025-2026 official latency or CPU data was found for Krisp, NVIDIA Broadcast v2.x or Sonar.
