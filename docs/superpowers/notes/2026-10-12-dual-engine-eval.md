# Dual engine — evaluation notes

## Task 2: DFN3 automatic input level (pre-gain)

Linux harness (ORT 1.28.3), real `Dfn3StreamAdapter` (built with the pre-gain) vs `Options{autoInputLevel=false}`
("off", bit-identical to the old behavior). Synthetic click clips; metric = click-window RMS reduction (dB, more negative is
better). "3s+" counts only clicks after 3 s (after the fast-slew phase and the model's feature normalization settled).

Chosen `InputLevelNormalizer::Config::targetDbfs` **T = -40 dBFS** (the -45 dBFS-peak clip settles at +20.9 dB).

| Clip peak | off, all | off, 3s+ | auto, all | auto, 3s+ | settled gain |
|---|---|---|---|---|---|
| -55 | -9.4 | -8.5 | -21.3 | **-65.4** | +29.5 dB |
| -45 | -18.5 | -21.5 | -19.9 | **-64.6** | +20.9 dB |
| -35 | -32.5 | -63.7 | -15.3 | -13.9 | +30.0 dB |
| -25 | -52.0 | -65.6 | -18.2 | -17.0 | +29.7 dB |
| -15 | -32.5 | -31.2 | -17.9 | -16.7 | +19.7 dB |

Findings:
- Quiet clips (-55, -45): once settled, DFN3 matches its best (-25 off) result (-65 dB). Whole-clip numbers are dominated by
  the first ~2 s, while the gain ramps up (30 dB/s) and the model's feature normalization re-adapts.
- **Acceptance NOT met at -35/-25/-15.** On these synthetic clips the normalizer measures the *noise floor*
  (clicks occupy < 25% of frames, so the 75th percentile is the floor). The floor scales with the clip
  (-90/-80/-70/-60 dBFS RMS for -45/-35/-25/-15), and crosses the -80 dBFS silence gate between -45 and -35, so
  the estimated level is not monotonic in clip level and the gain saturates at +30 dB, which overdrives the model
  (clicks at about +5 dBFS) and is worse than no gain. Same mechanism explains the real typing clip below.
- The synthetic clips contain no speech, which is what the percentile level is designed around.

## Settled normalizer gain on the user's real clips (T = -40; at the earlier trial T = -26 in parentheses)

| Clip | Settled gain |
|---|---|
| typing.wav | +15.6 dB (+29.6) |
| speech.wav | +2.9 dB (+16.9) |
| both.wav | +0.8 dB (+14.8) |

Coordinator's fixed-gain data (DFN3 alone): typing click RMS reduction -42.5 dB at +0 vs -28.9 dB at +20; speech LSD 2.14 at
+0 vs 0.89 at +20; speech-band retention 0.887 vs 0.824. So with T = -40 the speech clips get little gain (the speech
benefit needs about +17..20), while typing-only (floor/click dominated) gets +15.6, which the data says hurts. A single
percentile level cannot separate the two cases; options: higher T plus a headroom limit on boosted peaks, or leave
`autoInputLevel` off where clicks matter (the adapter option exists; the bridge decision is pending).
