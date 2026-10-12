# Real-time CPU speech enhancement / noise suppression models to replace or complement RNNoise and DeepFilterNet3 (state as of Oct 2026)

Scope note: VoiceCleaner runs 48 kHz full-band audio in 480-sample (10 ms) hops. The baseline engines are RNNoise (xiph, built-in model, scalar build) and DeepFilterNet3 (ONNX Runtime CPU, split graphs). The research used arXiv HTML tables (fetched directly), GitHub/Hugging Face READMEs, and web search. Numbers from different papers are generally NOT comparable. Test sets (VCTK/VoiceBank+DEMAND, DNS3 synthetic, DNS2020, DNS4 blind, Ceva's private low-SNR set), sample rates (16 vs 48 kHz) and MAC-counting conventions all differ. Each number below is tagged with its test set.

## 1. RNNoise updates (v0.2, default vs little model, SIMD/RTCD)

### Takeaway
RNNoise 0.2 (April 2024) mainly brings retrained models (public datasets only, "improved training"), a smaller "little" model, and SSE4.1/AVX2 kernels with runtime CPU detection. No published PESQ/DNSMOS numbers for the v0.2 models were found. VoiceCleaner's scalar-only build leaves easy speed on the table, but quality stays in the "lowest tier" of modern models.

### Cited Findings
- RNNoise 0.2 was released around 15 April 2024. Phoronix highlights SSE4.1 and AVX2 optimizations and RTCD (runtime CPU detection) as the main user-visible changes, plus "improved training" — [Phoronix](https://www.phoronix.com/news/RNNoise-0.2-Released)
- README: "The models distributed with RNNoise are now trained using only the publicly available datasets"; the noise-contribution dataset was updated 30 Jan 2025 — [xiph/rnnoise README](https://github.com/xiph/rnnoise) (raw: https://raw.githubusercontent.com/xiph/rnnoise/main/README)
- README recommends either `-march=` with AVX2 or `--enable-x86-rtcd` "so that AVX2 (or SSE4.1) can at least be used as an option". Model files are downloaded by `autogen.sh` because they are too large for Git. A `-DUSE_WEIGHTS_FILE` option lets the model be loaded from a file at runtime instead of compiled in — [xiph/rnnoise](https://github.com/xiph/rnnoise)
- A "little" model (`rnnoise_data_little.c`) is shipped and selected by renaming it to `rnnoise_data.c`. It is described as about half as large / sparser than the default — [xiph/rnnoise](https://github.com/xiph/rnnoise). (The fetched summary's wording on size was ambiguous: "half as large" and "same size but sparser" both appeared. Verify in source.)
- License: BSD-3-Clause — [xiph/rnnoise](https://github.com/xiph/rnnoise)
- Original RNNoise (2017/2018 model) third-party numbers:
  - VCTK/VoiceBank+DEMAND: PESQ 2.33, STOI 0.922, 0.06 M params, 0.04 GMAC/s, RTF 0.027 single-thread on an i5-8250U (measured by DFN2 authors with the xiph weights) — [DeepFilterNet2, arXiv:2205.05474, Table 1](https://arxiv.org/abs/2205.05474)
  - DNS4 blind test set DNSMOS: SIG 3.884 / BAK 3.694 / OVRL 3.378 (noisy: 4.144 / 2.94 / 3.291) — [arXiv:2205.05474, Table 2](https://arxiv.org/abs/2205.05474)
  - VCTK @48 kHz: PESQ-WB 2.29, P.808 MOS 3.70 — [PercepNet, arXiv:2008.04259, Table 1](https://arxiv.org/abs/2008.04259)
  - VCTK: PESQ 2.29; DNS3 blind DNSMOS P.808 3.15, BAK 3.45, SIG 3.00, OVRL 2.53 — [GTCRN README](https://github.com/Xiaobin-Rong/gtcrn)
- The RNNoise version Ceva evaluated is not stated. On its multilingual low-SNR test set (16 kHz), RNNoise scored 0.087 M params, 0.04 GMACs, PESQ 2.05, STOI 88.5, SI-SNR 8.97, DNSMOS SIG 2.81 / BAK 3.93 / OVRL 2.50, PRISM 0.51 — [DPDFNet, arXiv:2512.16420, Table I](https://arxiv.org/abs/2512.16420). The 0.087 M param count differs from the 0.06 M quoted for the 2017 model, which suggests Ceva may have used the v0.2 default model. This is an inference only.

### Inferences
- Rebuilding with `--enable-x86-rtcd` (or MSVC equivalents for the AVX2/SSE4.1 files) should cut the measured 0.7–2 ms/hop. RNNoise's ~40 MMAC/s budget is tiny, so the scalar build is the main cost.
- Swapping to the v0.2 default model is the cheapest quality step. The "little" model suits only very weak CPUs. No public metric quantifies either change.
- RNNoise keeps a niche as the "ultra-low CPU / near-zero latency" option. Its BAK (noise removal) is decent, but SIG/OVRL trail DFN-class models.

### Gaps
- No official quality numbers (PESQ/DNSMOS) for the v0.2 default vs little models vs the 2017 model were found. The xiph README and release notes give none. Measuring in-house is required.
- Exact MACs/params of the v0.2 default and little models were not found in a primary source.

## 2. DeepFilterNet: post-DFN3 versions, low-latency variants, numbers

### Takeaway
No DeepFilterNet 4 from the original author (Rikorose/Schröter) was found. The repo still tops out at DFN3 (Interspeech 2023 demo). The notable successor is Ceva's DPDFNet (arXiv:2512.16420, Dec 2025, Apache-2.0). It is DFN2 plus causal dual-path RNN blocks, ships 48 kHz ONNX models, and beats DFN3 on Ceva's low-SNR set and on DNS4 DNSMOS, at 2.4–7.2 GMAC/s (vs ~0.35 for DFN3).

### Cited Findings
- The DeepFilterNet repo lists DeepFilterNet, DeepFilterNet2 (the default for `deep-filter`) and DeepFilterNet3; no newer model is mentioned. License: MIT or Apache-2.0 at the user's option — [Rikorose/DeepFilterNet](https://github.com/Rikorose/DeepFilterNet)
- DFN3 design (from the Interspeech 2023 demo paper):
  - 48 kHz, 20 ms window, 10 ms hop, "a look-ahead of 2 frames is used resulting in an overall latency of 40 ms"
  - ERB stage: 32 ERB gains; DF stage: 5-tap complex filter on the lowest 96 bins (~4.8 kHz)
  - a local-SNR estimate can disable decoders
  - RTF 0.19 single-threaded on a notebook CPU
  
  — [arXiv:2305.08227](https://arxiv.org/abs/2305.08227)
- DFN3 VCTK/VoiceBank+DEMAND: PESQ 3.17, CSIG 4.34, CBAK 3.61, COVL 3.77, STOI 0.944. DFN2 on the same set: PESQ 3.08, STOI 0.9429 — [arXiv:2305.08227, Table 1](https://arxiv.org/abs/2305.08227)
- DFN2: 2.31 M params, 0.36 GMAC/s, RTF 0.04 on Core i5-8250U, PESQ 3.08 on VCTK. DNS4 blind DNSMOS: SIG 4.196, BAK 4.427, OVRL 3.882 — [arXiv:2205.05474](https://arxiv.org/abs/2205.05474)
- Ceva's evaluation, all models run at 16 kHz on Ceva's new multilingual low-SNR test set:

  | Model | Params | MACs | PESQ | STOI | SI-SNR | SIG / BAK / OVRL | P.808 | PRISM |
  |---|---|---|---|---|---|---|---|---|
  | DFN3 | 2.14 M | 0.35 G | 2.45 | 90.6 | 12.10 | 3.03 / 4.01 / 2.71 | 3.22 | 0.82 |
  | DFN2 | 2.31 M | 0.36 G | 2.35 | 91.3 | 11.95 | 2.92 / 3.87 / 2.58 | – | 0.76 |

  — [arXiv:2512.16420, Table I](https://arxiv.org/abs/2512.16420)
- DNS4 blind (DNSMOS SIG/BAK/OVRL/P.808), from the same paper:

  | Model | SIG | BAK | OVRL | P.808 |
  |---|---|---|---|---|
  | DFN2 | 3.318 | 3.997 | 3.019 | 3.748 |
  | DFN3 | 3.279 | 3.947 | 2.958 | 3.729 |
  | DPDFNet-2 | 3.386 | 4.054 | 3.104 | 3.844 |
  | DPDFNet-4 | 3.389 | 4.047 | 3.105 | 3.848 |
  | DPDFNet-8 | 3.403 | 4.060 | 3.122 | 3.849 |

  — [arXiv:2512.16420, Table II](https://arxiv.org/abs/2512.16420). These DNS4 numbers differ from those in the DFN2 paper (e.g. DFN2 OVRL 3.882 vs 3.019), probably because of a DNSMOS version or setup difference. They are not cross-comparable.
- DPDFNet on Ceva's low-SNR set:

  | Model | Params | GMACs | PESQ | STOI | SI-SNR | OVRL | PRISM |
  |---|---|---|---|---|---|---|---|
  | DFN2-arch baseline (retrained + OA loss + fine-tune) | 2.31 M | 0.36 | 2.68 | 91.6 | 13.27 | 2.83 | 0.91 |
  | DPDFNet-2 | 2.49 M | 1.35 | 2.79 | 92.6 | 13.72 | 2.85 | 0.95 |
  | DPDFNet-4 | 2.84 M | 2.36 | 2.82 | 93.0 | 14.11 | 2.86 | 0.98 |
  | DPDFNet-8 | 3.54 M | 4.37 | 2.85 | 93.4 | 14.47 | 2.89 | 1.00 |

  — [arXiv:2512.16420, Table I](https://arxiv.org/abs/2512.16420)
- DPDFNet keeps DFN2's STFT (20 ms window, 10 ms hop) and DF with order N=5 and look-ahead ℓ=2. It was trained at 16 kHz; the authors say the architecture "permits training at lower sampling rates (e.g., 16 kHz) while deploying at higher rates with negligible performance loss". A fine-tuning stage on 30–40 s continuous segments fixed "always-on" streaming instabilities: first-syllable truncation after 3–4 s of silence, and noise leakage — [arXiv:2512.16420](https://arxiv.org/abs/2512.16420)
- DPDFNet release details:
  - 48 kHz models: `dpdfnet2_48khz_hr` (2.58 M params, 2.42 GMACs) and `dpdfnet8_48khz_hr` (3.63 M, 7.17 GMACs); 16 kHz and 8 kHz variants also exist
  - formats: ONNX and TFLite, plus a CPU-only ONNX PyPI package
  - `StreamEnhancer` example uses 480-sample (10 ms @ 48 kHz) blocks
  - latency note: "first enhanced output arrives after one full model window (~20 ms)… subsequent blocks… ~10 ms additional delay"
  - license: Apache-2.0
  
  — [Hugging Face Ceva-IP/DPDFNet model card](https://huggingface.co/Ceva-IP/DPDFNet); [GitHub ceva-ip/DPDFNet](https://github.com/ceva-ip/DPDFNet)
- No CPU RTF numbers are published for DPDFNet. The only real-time claim is on Ceva NeuPro-Nano NPUs: DPDFNet-4 is real-time on NPN32 with int8 weights and int16 activations — [arXiv:2512.16420](https://arxiv.org/abs/2512.16420); [GitHub ceva-ip/DPDFNet](https://github.com/ceva-ip/DPDFNet)

### Inferences
- The DFN3 paper's "40 ms overall" counts window plus 2-frame look-ahead. VoiceCleaner's "~30 ms" figure likely uses a different accounting, such as excluding one hop of buffering. Use one consistent definition when comparing.
- DPDFNet has the same ℓ=2 DF look-ahead as DFN2, so its true algorithmic latency is probably similar to DFN2/3 (~30–40 ms) despite the "~20 ms" model-card note. Verify by measuring.
- DPDFNet2_48khz_hr is the most direct "drop-in upgrade" to DFN3: same DFN-family I/O (ERB + DF at 48 kHz, 10 ms hop), ONNX, Apache-2.0. Expect about 7x DFN3's MACs (2.42 vs ~0.35 G). DFN3 runs at roughly 0.19 RTF single-thread per its paper, so DPDFNet-2 may sit near or above real-time on a single weak laptop core. It needs benchmarking, and possibly 2 ORT threads.

### Gaps
- No official DFN3 "_ll" low-latency quality numbers were found. The repo page does not mention _ll variants (they appear only in model archives/releases).
- No evidence of "DeepFilterNet 4" was found; this absence is not proof it does not exist.
- No measured CPU ms/hop for DPDFNet was found.

## 3. Other lightweight causal models: GTCRN, UL-UNAS, LiSenNet, FSPEN, ULCNet/Fast-ULCNet, CoFi-Lite, PercepNet, DNS-challenge entries, Mamba/SSM

### Takeaway
The ultra-light family (GTCRN, LiSenNet, FSPEN, UL-UNAS, CoFi-Lite, ULCNet) costs 13–100 MMAC/s, about the same as RNNoise. On VCTK it clearly beats RNNoise in PESQ and roughly matches or exceeds DFN1/2. All of these are 16 kHz only. They are the best candidates for a "better RNNoise" tier but need a band-split or resampling wrapper for 48 kHz. Among Mamba/SSM models, only RT-SEMamba (Interspeech 2026) and aTENNuate are explicitly causal/streaming. PercepNet (48 kHz) has no public weights.

### Cited Findings
**GTCRN** (ICASSP 2024)
- 48.2 K params by the README count (23.7 K in the paper), 33.0 MMAC/s by the README count (39.6 MMAC/s in the paper)
- VCTK: PESQ 2.87, STOI 0.940, SI-SNR 18.83
- DNS3 blind DNSMOS: P.808 3.44, BAK 3.90, SIG 3.00, OVRL 2.70
- streaming implementation RTF 0.07 on i5-12400
- license MIT; DNS3 and VCTK checkpoints included; sherpa-onnx support; LADSPA plugin for PipeWire (Jan 2026)

— [Xiaobin-Rong/gtcrn](https://github.com/Xiaobin-Rong/gtcrn)

**GTCRN on Ceva's low-SNR set** (16 kHz): 0.023 M params, 0.039 GMACs, PESQ 2.00, OVRL 2.24, PRISM 0.49. This is slightly below RNNoise's 2.05 / 2.50 / 0.51 there — [arXiv:2512.16420](https://arxiv.org/abs/2512.16420). On a hard low-SNR multilingual set, GTCRN's advantage over RNNoise disappears. This conflicts with GTCRN's own VCTK/DNS3 claims.

**LiSenNet** (arXiv:2409.13285)

| Model | Params | MACs | PESQ (VCTK) | STOI (VCTK) |
|---|---|---|---|---|
| LiSenNet | 37 k | 56 M | 3.07 | 0.939 |
| FSPEN | 79 k | 89 M | 2.97 | 0.942 |
| RNNoise | 0.06 M | 0.04 G | 2.33 | 0.922 |
| FullSubNet | 5.64 M | 30.87 G | 2.89 | – |

- On WSJ0+ESC50, LiSenNet's RTF is 0.028 on a Xeon E5-2620 v4.
- An optional noise detector cuts cost to 15–70 MMAC/s.
- All clips were resampled to 16 kHz. Code: github.com/hyyan2k/LiSenNet.

— [arXiv:2409.13285](https://arxiv.org/abs/2409.13285)

**CoFi-Lite** (arXiv:2607.10142, 2026). All baselines were retrained by the authors on the same data; simulated DNS3 test set at 16 kHz.

| Model | Params | MMAC/s | RTF | PESQ | OVRL |
|---|---|---|---|---|---|
| GTCRN | 23.67 k | 31.97 | 0.050 | 2.07 | 2.63 |
| LiSenNet | 36.78 k | 55.77 | 0.035 | 2.17 | 2.69 |
| UL-UNAS | 171 k | 34.91 | 0.066 | 2.25 | 2.69 |
| AdaptCRN | 134.5 k | 40.80 | 0.053 | 2.30 | 2.75 |
| CoFi-Lite (Large) | 221 k | 32.91 | 0.036 | 2.30 | 2.75 |
| CoFi-Lite (<13 MMAC/s) | 83 k | – | – | 2.16 | 2.70 |

— [arXiv:2607.10142, Table I](https://arxiv.org/abs/2607.10142)

This is one of the few same-test-set comparisons of the ultra-light models. Note the LiSenNet MAC figure here (55.77 M) matches its paper (56 M).

**ULCNet** (arXiv:2312.08132)

| Model | Params | GMAC/s | RTF | PESQ / SI-SDR (VCTK) | PESQ / SI-SDR (DNS2020 synth no-reverb) | SIG / BAK (DNS2020) |
|---|---|---|---|---|---|---|
| ULCNet_MS | 0.688 M | 0.098 | 0.02 | 2.87 / 16.89 | 2.64 / 16.34 | 3.46 / 4.06 |
| DFN2 | – | – | – | 3.08 / 15.71 | 2.65 / 16.60 | 3.51 / 4.12 |

- ULCNet runs at 0.127 RTF at 16 kHz on one Cortex-A53 core.

— [arXiv:2312.08132, Table 1](https://arxiv.org/abs/2312.08132)

I found no official public weights for ULCNet.

**Fast-ULCNet** (arXiv:2601.14925, 2026)
- FastGRNN replaces the GRUs: 0.338 M params (vs 0.685 M for ULCNet), 1.691 vs 2.057 M MACs (per frame), RTF 0.657 vs 0.976 on a Raspberry Pi 3B+.
- DNS2020 synthetic: OVRL 3.09 vs 3.10, PESQ 2.51 vs 2.62.
- Plain FastGRNN state drifts on 90 s signals (PESQ falls to 2.24); a "comfi" variant fixes it.
- 16 kHz, 32 ms window / 16 ms hop.

— [arXiv:2601.14925](https://arxiv.org/abs/2601.14925)

**PercepNet** (Interspeech 2020, Valin et al., Amazon)
- full-band 48 kHz, "less than 5% of a CPU core"
- VCTK @48 kHz: PESQ-WB 2.54 (2.73 without post-filter), P.808 MOS 4.05, vs RNNoise 2.29 / 3.70

— [arXiv:2008.04259](https://arxiv.org/abs/2008.04259)

PercepNet's quoted cost is 8.0 M params and 0.8 GMAC/s — [arXiv:2205.05474, Table 1](https://arxiv.org/abs/2205.05474). There is no official open-source release with trained weights from Amazon (only community reimplementations, not verified here).

**PercepNet+** (2022, phase/SNR-aware) was found only as a title. No release info was found — [search result listing, Personalized PercepNet arXiv:2106.04129 related](https://arxiv.org/abs/2106.04129)

**Other causal open baselines on Ceva's low-SNR set** (16 kHz):

| Model | Params | GMACs | PESQ | OVRL | PRISM |
|---|---|---|---|---|---|
| aTENNuate (causal deep SSM on raw audio) | 0.80 M | 0.33 | 2.43 | 2.74 | 0.69 |
| DPCRN | 0.53 M | 1.10 | 2.47 | 2.39 | 0.69 |
| NSNet2 | 2.60 M | 0.26 | 2.06 | 2.36 | 0.51 |
| DTLN | 0.99 M | 0.12 | 1.94 | 2.15 | 0.46 |
| DEMUCS (causal) | 33.5 M | 7.70 | 2.27 | 2.69 | 0.71 |
| CleanUNet | 46 M | 15.44 | 2.38 | 2.66 | 0.69 |
| FullSubNet | 5.6 M | 30 | 2.19 | 2.43 | 0.63 |

All are below DFN3 (PRISM 0.82) — [arXiv:2512.16420, Table I](https://arxiv.org/abs/2512.16420)

**Mamba / SSM**
- SEMamba has causal and non-causal configurations. The headline PESQ 3.55 (3.69 with PCS) on VCTK is for the non-causal advanced configuration — [arXiv:2405.06573](https://arxiv.org/abs/2405.06573)
- RT-SEMamba (Interspeech 2026) is "fully causal". The 8-layer model reaches PESQ 3.32 on VoiceBank-DEMAND under a 25 ms algorithmic latency constraint. A distilled 1-layer student reaches 3.18 PESQ at 2.64x the teacher's speed — [arXiv:2608.12099](https://arxiv.org/abs/2608.12099)
- Schrödinger Bridge Mamba (Interspeech 2026) does joint denoising and dereverberation with one-step inference and a "competitive real-time factor for streaming feasibility". A search snippet reports a 2–4 frame look-ahead giving under 40 ms latency (not verified in the paper body) — [arXiv:2510.16834](https://arxiv.org/abs/2510.16834)
- Wu & Braun (Microsoft) study ultra-low-latency (<5 ms) SE techniques, including Mamba — [arXiv:2409.10358](https://arxiv.org/abs/2409.10358)

**DNS Challenge**
- ICASSP 2023 DNS (5th edition) used full-band 48 kHz data with headset and speakerphone tracks (personalized). The same teams won both tracks; organizers verified real-time compliance — [arXiv:2303.11510](https://arxiv.org/abs/2303.11510); [Microsoft DNS page](https://www.microsoft.com/en-us/research/academic-program/deep-noise-suppression-challenge-icassp-2023)

### Inferences
- For a "better RNNoise" engine at similar CPU cost, GTCRN (MIT, weights, ONNX via sherpa-onnx) is the most deployable. UL-UNAS, LiSenNet and CoFi-Lite score higher in the CoFi-Lite comparison but have less mature deployment tooling.
- The ultra-light 16 kHz models discard 8–24 kHz. VoiceCleaner would need to run the model on a 16 kHz downsampled band and handle the upper band separately: pass-through with gain from the low band, or a DSP/ERB gain extrapolation like DFN's ERB stage. This adds resampler latency (a few ms) and complexity.
- Ceva's low-SNR results show the ultra-light models (GTCRN) are not robustly better than RNNoise in hard conditions. DFN-class models (DFN3, DPDFNet) are the clear quality tier for live use.
- RT-SEMamba is research-grade (25 ms latency, high PESQ). Its weights, ONNX path and CPU cost are unknown, and Mamba's selective scan is awkward in ONNX Runtime. It is not a near-term candidate.

### Gaps
- No public weights or measured CPU cost found for RT-SEMamba, FSPEN, ULCNet, CoFi-Lite or AdaptCRN.
- The names of the ICASSP 2023/2024 DNS real-time track winners and their latencies were not retrieved. Top entries generally did not release weights (not verified).
- URGENT challenge results were not reviewed. They are mostly non-real-time universal SE.
- No 48 kHz variant of GTCRN, LiSenNet or UL-UNAS was found.

## 4. ClearerVoice-Studio (Alibaba): MossFormer2_SE_48K, FRCRN, MossFormerGAN

### Takeaway
ClearerVoice's SE models give strong offline quality, and MossFormer2_SE_48K is a genuine 48 kHz model. None is documented as causal or streaming; MossFormer2 is an attention-based separation-style model. They suit an offline "clean recording" feature, not live calls.

### Cited Findings
- Models: MossFormer2_SE_48K (48 kHz), FRCRN_SE_16K, MossFormerGAN_SE_16K; repo license Apache-2.0 — [ClearerVoice-Studio clearvoice README](https://github.com/modelscope/ClearerVoice-Studio/blob/main/clearvoice/README.md); [repo](https://github.com/modelscope/ClearerVoice-Studio)
- VoiceBank+DEMAND tested at 48 kHz, the only cross-tool 48 kHz table found that includes DeepFilterNet. It was scored with the vendor's SpeechScore tool; the DeepFilterNet version is unspecified.

  | Model | PESQ | STOI | SI-SDR | DNSMOS SIG / BAK / OVRL |
  |---|---|---|---|---|
  | MossFormer2_SE_48K | 3.15 | 0.95 | 19.36 | 3.41 / 4.10 / 3.15 |
  | DeepFilterNet | 3.03 | 0.94 | 15.71 | 3.40 / 4.00 / 3.10 |
  | Resemble-enhance | 2.84 | – | 12.42 | – |

  — [ClearVoice README](https://github.com/modelscope/ClearerVoice-Studio/blob/main/clearvoice/README.md)
- At 16 kHz on VCTK: MossFormerGAN_SE_16K PESQ 3.47, FRCRN_SE_16K PESQ 3.23 — [ClearVoice README](https://github.com/modelscope/ClearerVoice-Studio/blob/main/clearvoice/README.md)
- FRCRN costs 10.27 M params and 12.3 GMAC/s, "not feasible for embedded devices" per the DFN2 authors — [arXiv:2205.05474, Table 1](https://arxiv.org/abs/2205.05474)
- The README and repo page state nothing about causality or streaming — [repo](https://github.com/modelscope/ClearerVoice-Studio)

### Inferences
- MossFormer2_SE_48K's quality edge over DeepFilterNet at 48 kHz on VCTK is modest (+0.12 PESQ, +0.05 OVRL). It is likely non-causal and much heavier, so it does not justify replacing DFN3 for live use. It could be an optional offline mode.

### Gaps
- Param and MAC counts and latency for MossFormer2_SE_48K were not found; the Hugging Face card could not be fetched.

## 5. Personalized / target-speaker enhancement and dereverberation

### Takeaway
Personalized PercepNet shows that a causal, low-complexity, 48 kHz target-speaker model is feasible: 4.7% of a laptop core at 10 ms frames. No open weights were found. Open lightweight personalized or dereverberation options remain scarce. Schrödinger Bridge Mamba and CoFi-Lite (trained with early reverberation targets) are research directions, not products.

### Cited Findings
- Personalized PercepNet (Amazon, Interspeech 2021) uses a speaker-embedding-conditioned PercepNet. PPN-512 has 8.5 M params and needs 4.7% of one i7-8565U core at 10 ms frames; PPN-1024 has 26.5 M params and needs 17.2%. Both "significantly outperform" PercepNet and NSNet2 in PESQ and P.808 — [arXiv:2106.04129](https://arxiv.org/abs/2106.04129)
- A follow-up paper describes a unified personalized/non-personalized PercepNet with the embedding fed into the GRU — [arXiv:2302.11768](https://arxiv.org/abs/2302.11768)
- The ICASSP 2023 DNS Challenge had personalized headset/speakerphone tracks at 48 kHz — [arXiv:2303.11510](https://arxiv.org/abs/2303.11510)
- CoFi-Lite trains with early reverberation in the target and reports DNS2020 "With Reverb" results — [arXiv:2607.10142](https://arxiv.org/abs/2607.10142)
- Schrödinger Bridge Mamba does joint denoising and dereverberation with streaming-feasible RTF — [arXiv:2510.16834](https://arxiv.org/abs/2510.16834)
- RNNoise v0.2 training uses measured room impulse responses for reverberation augmentation — [xiph/rnnoise](https://github.com/xiph/rnnoise)

### Inferences
- Target-speaker enhancement would need VoiceCleaner to train its own model (e.g. a DFN/DPDFNet variant with a speaker embedding). No off-the-shelf licensed weights are available.

### Gaps
- No released weights were found for Personalized PercepNet, pDCCRN or similar. No open, causal, lightweight dereverberation model with weights was identified in this pass.

## 6. Sample-rate coverage, head-to-head comparisons, and upgrade recommendation

### Takeaway
Full-band 48 kHz candidates with weights: RNNoise, DFN2/DFN3, DPDFNet *_48khz_hr, and MossFormer2_SE_48K (offline only). Everything else (GTCRN, LiSenNet, UL-UNAS, CoFi-Lite, ULCNet, FSPEN, aTENNuate, DPCRN, RT-SEMamba) is 16 kHz. The best published same-set head-to-head that includes both RNNoise and DFN3 is Ceva's DPDFNet Table I (16 kHz, low-SNR multilingual). It ranks DPDFNet-8 > DPDFNet-4 > DPDFNet-2 > DFN3 > DFN2 > DEMUCS ≈ DPCRN ≈ aTENNuate > RNNoise ≈ NSNet2 > GTCRN > DTLN.

### Cited Findings
- **48 kHz native:**
  - RNNoise — [xiph/rnnoise](https://github.com/xiph/rnnoise)
  - DFN3 (48 kHz, 40 ms with 2-frame look-ahead) — [arXiv:2305.08227](https://arxiv.org/abs/2305.08227)
  - DPDFNet2/8 48 kHz "hr" — [HF Ceva-IP/DPDFNet](https://huggingface.co/Ceva-IP/DPDFNet)
  - PercepNet (no public weights) — [arXiv:2008.04259](https://arxiv.org/abs/2008.04259)
  - MossFormer2_SE_48K — [ClearVoice README](https://github.com/modelscope/ClearerVoice-Studio/blob/main/clearvoice/README.md)
- **16 kHz only:** GTCRN, LiSenNet, UL-UNAS, CoFi-Lite (all 16 kHz in [arXiv:2607.10142](https://arxiv.org/abs/2607.10142) and [arXiv:2409.13285](https://arxiv.org/abs/2409.13285)); ULCNet / Fast-ULCNet ([arXiv:2601.14925](https://arxiv.org/abs/2601.14925))
- **Head-to-heads that include RNNoise and DFN on the same set:**
  - Ceva low-SNR set at 16 kHz: RNNoise PRISM 0.51, DFN3 0.82, DPDFNet-8 1.00 — [arXiv:2512.16420](https://arxiv.org/abs/2512.16420)
  - DFN2 paper, VCTK (RNNoise PESQ 2.33 vs DFN2 3.08) and DNS4 blind (RNNoise OVRL 3.378 vs DFN2 3.882) — [arXiv:2205.05474](https://arxiv.org/abs/2205.05474)
  - GTCRN README, VCTK: RNNoise 2.29, DFN(v1) 2.81, GTCRN 2.87 — [gtcrn](https://github.com/Xiaobin-Rong/gtcrn)
  - LiSenNet paper, VCTK: RNNoise 2.33 vs LiSenNet 3.07 — [arXiv:2409.13285](https://arxiv.org/abs/2409.13285)

### Inferences (upgrade options for VoiceCleaner, ranked)
1. **Quick wins, no new model:** build RNNoise 0.2 with RTCD/AVX2 and the v0.2 default model. Keep DFN3 but try ORT graph optimizations and session options; DFN3's ~0.35 GMAC/s is already light.
2. **Best quality upgrade for live use: DPDFNet-2 48 kHz (`dpdfnet2_48khz_hr`).**
   - same DFN family, 48 kHz, 10 ms hop, ONNX, Apache-2.0
   - beats DFN3 on both reported test sets
   - fine-tuned for always-on streaming stability
   - trade-off: about 7x DFN3's MACs (2.42 G) and similar ~30–40 ms latency; needs CPU benchmarking on low-end laptops
   - DPDFNet-8 48 kHz (7.17 GMACs) is likely too heavy for single-thread real time on ordinary laptops (inference; unmeasured)
   - the ONNX graph layout differs from DFN3's split enc/erb_dec/df_dec graphs, so integration work is needed
3. **Low-CPU tier replacement for RNNoise: GTCRN** (MIT, ~33 MMAC/s, RTF 0.07, sherpa-onnx). UL-UNAS and LiSenNet are alternatives.
   - caveats: 16 kHz only (needs band-split/resampling) and no robustness win over RNNoise on Ceva's low-SNR set
   - possibly lower latency than DFN (inference: GTCRN uses a 32 ms window / 16 ms hop at 16 kHz in typical configs; unverified here)
4. **Not recommended for live use now:** MossFormer2/FRCRN/MossFormerGAN (non-causal/heavy; offline mode only), SEMamba non-causal, PercepNet/Personalized PercepNet (no weights), RT-SEMamba (no weights/ONNX evidence).

### Gaps
- No published CPU ms/hop numbers on a common laptop for DFN3 vs DPDFNet vs GTCRN. A benchmark inside VoiceCleaner is needed.
- No same-test-set 48 kHz full-band comparison covering RNNoise, DFN3 and DPDFNet-48k was found. Ceva's table is 16 kHz, and ClearVoice's 48 kHz table omits RNNoise and DPDFNet.
