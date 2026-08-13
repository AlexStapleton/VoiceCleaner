#!/usr/bin/env python3
"""Analyze a Phase 0 drift.csv: least-squares fit of Ring B occupancy vs time -> ppm.

Pure standard-library (no numpy needed). Usage:
    python tools/analyze_drift.py drift.csv
"""
import sys

FRAME_SAMPLES = 480
SAMPLE_RATE = 48000


def main(path):
    ts = []          # seconds
    b = []           # ring B occupancy (frames)
    last = None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or line.startswith("elapsed_ms"):
                continue
            parts = line.split(",")
            if len(parts) < 7:
                continue
            try:
                elapsed_ms = float(parts[0])
                ringB = float(parts[2])
            except ValueError:
                continue
            ts.append(elapsed_ms / 1000.0)
            b.append(ringB)
            last = parts

    n = len(ts)
    if n < 2:
        print(f"not enough data rows ({n}) to fit")
        return 1

    # ordinary least squares slope/intercept for b = slope*t + intercept
    mt = sum(ts) / n
    mb = sum(b) / n
    sxx = sum((t - mt) ** 2 for t in ts)
    sxy = sum((ts[i] - mt) * (b[i] - mb) for i in range(n))
    if sxx == 0:
        print("all timestamps identical; cannot fit")
        return 1
    slope = sxy / sxx                      # frames per second
    intercept = mb - slope * mt

    # correlation coefficient (fit quality)
    syy = sum((v - mb) ** 2 for v in b)
    r = (sxy / (sxx ** 0.5 * syy ** 0.5)) if syy > 0 else 0.0

    drift_ppm = slope * FRAME_SAMPLES / SAMPLE_RATE * 1e6

    dur_min = (ts[-1] - ts[0]) / 60.0
    print(f"rows:            {n}")
    print(f"duration:        {dur_min:.1f} min ({ts[-1]-ts[0]:.0f} s)")
    print(f"ring B start/end:{b[0]:.0f} -> {b[-1]:.0f} frames  (min {min(b):.0f}, max {max(b):.0f})")
    print(f"slope:           {slope:+.5e} frames/s")
    print(f"fit r:           {r:+.4f}")
    print(f"DRIFT:           {drift_ppm:+.2f} ppm")
    sign = "capture faster than render" if slope > 0 else "render faster than capture"
    print(f"                 ({sign})")
    if last is not None:
        print(f"final under/over:{last[5]} / {last[6]}")
    # crude time-to-rail projection at this slope, from mid-ring, for context
    if abs(slope) > 1e-9 and b:
        half = (max(b) + 1)  # rough capacity/2 proxy; occupancy started near capacity/2
        # frames of headroom to a rail from the setpoint ~ b[0]
        headroom = b[0]  # to the empty rail (0) if draining; symmetric-ish
        secs_to_rail = headroom / abs(slope)
        print(f"~time to a rail: {secs_to_rail/3600.0:.1f} h at this slope (uncompensated)")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: python tools/analyze_drift.py <drift.csv>")
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
