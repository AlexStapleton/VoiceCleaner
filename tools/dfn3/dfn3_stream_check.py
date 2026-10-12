#!/usr/bin/env python3
"""GATE A: prove the streaming graphs reproduce the whole-utterance DFN3 models.

Works for BOTH the purely-causal `_ll` split (`conv_lookahead=0`) and the STANDARD split
(`conv_lookahead=2`). Same random feature input (seed 0, T=40) is run two ways and compared to
max|diff| < 1e-3 on every data output:

  * enc      -- whole enc.onnx  vs  chain enc_conv_streaming (windowed, slice frame
                S-1-conv_lookahead) -> enc_gru_streaming (stateful h0->h1, one frame/hop).
                The conv sub-graph's newest window frame is missing its `conv_lookahead` future
                frames, so the frame with FULL context is `conv_lookahead` behind the newest.
                This makes the stream lag the whole-utterance frames by `conv_lookahead`: feeding
                real frames 0..T-1 yields logical encoder frames 0..T-1-conv_lookahead, which are
                compared against whole[0..T-1-conv_lookahead]. (For `_ll`, lookahead=0 -> no lag,
                all T frames compared -- the original behaviour, unchanged.)
  * erb_dec  -- whole erb_dec.onnx  vs  erb_dec_streaming driven windowed + stateful
                (advance-by-one). DFN3 decoders are causal (slice-last); the DF path's lookahead
                lives host-side (DeepFilterRing leftPad), not in df_dec.
  * df_dec   -- whole df_dec.onnx   vs  df_dec_streaming, same driving.

Each windowed graph's required window W is found empirically (grow W until that graph's outputs
reproduce the aligned whole-utterance frames). GRU hidden size is auto-detected per streaming
graph (512 for `_ll`, 256 for standard). Prints per-output max|diff|, the WORST, and PASS/FAIL.

Deps: onnx, onnxruntime, numpy.  Usage: python tools/dfn3/dfn3_stream_check.py [models/dfn3]
"""
import configparser
import sys
from pathlib import Path

import numpy as np
import onnxruntime as ort

# T=40 is chosen unique among all model output dims (1/2/8/16/32/64/96/512/10) so that
# time_axis() can identify the time axis unambiguously by matching the dim value == T.
T = 40
TOL = 1e-3
W_CANDIDATES = [1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 20]
RNG = np.random.default_rng(0)


def sess(path):
    so = ort.SessionOptions()
    so.intra_op_num_threads = 1
    so.log_severity_level = 3
    return ort.InferenceSession(path, so, providers=["CPUExecutionProvider"])


def onames(s):
    return [o.name for o in s.get_outputs()]


def run_dict(s, feed):
    return dict(zip(onames(s), s.run(None, feed)))


def state_input(s):
    """The `h0`/`*_h0` GRU-state input value-info of a streaming graph."""
    return next(i for i in s.get_inputs() if i.name == "h0" or i.name.endswith("_h0"))


def hidden_of(s):
    """GRU hidden size = last dim of the state input (512 for `_ll`, 256 for standard)."""
    return state_input(s).shape[-1]


def num_layers_of(s):
    """Stacked GRU layer count = first dim of the decoder state input ([num_layers,1,hidden])."""
    return state_input(s).shape[0]


def read_conv_lookahead(md):
    cfg = configparser.ConfigParser()
    ini = Path(md) / "config.ini"
    if ini.exists():
        cfg.read(str(ini))
        if cfg.has_option("deepfilternet", "conv_lookahead"):
            return cfg.getint("deepfilternet", "conv_lookahead")
    return 0


def time_axis(shape):
    axes = [ax for ax, d in enumerate(shape) if d == T]
    assert len(axes) == 1, f"ambiguous/absent time axis in {shape}"
    return axes[0]


def frame_slice(arr, t0, t1):
    ax = time_axis(arr.shape)
    idx = [slice(None)] * arr.ndim
    idx[ax] = slice(t0, t1)
    return np.ascontiguousarray(arr[tuple(idx)])


def head_frames(arr, n, axis):
    """First n frames along `axis` (used to trim the whole-utterance ref by the lookahead lag)."""
    idx = [slice(None)] * arr.ndim
    idx[axis] = slice(0, n)
    return np.ascontiguousarray(arr[tuple(idx)])


def maxdiff(a, b):
    return float(np.max(np.abs(a.astype(np.float64) - b.astype(np.float64))))


# --------------------------------------------------------------------------- #
# encoder: enc_conv (windowed, lookahead-sliced) -> enc_gru (stateful)
# --------------------------------------------------------------------------- #
ENC_AXIS = {"e0": 2, "e1": 2, "e2": 2, "e3": 2, "c0": 2, "emb": 1, "lsnr": 1}


def stream_enc(conv_s, gru_s, feats, Wc, L, hidden):
    """Feed real frames 0..T-1; the conv graph slices `L` frames behind the newest, so the
    valid GRU input at hop f is logical frame f-L. Warmup hops f<L have no valid frame and are
    skipped. Returns logical frames 0..T-1-L (concatenated on their time axis)."""
    conv_names = onames(conv_s)          # e0,e1,e2,e3,c0,emb
    gru_names = onames(gru_s)            # emb_out, lsnr, h1
    h = np.zeros([1, 1, hidden], np.float32)
    acc = {n: [] for n in ["e0", "e1", "e2", "e3", "c0", "emb", "lsnr"]}
    for f in range(T):
        a = max(0, f - Wc + 1)
        feed = {k: frame_slice(v, a, f + 1) for k, v in feats.items()}
        co = dict(zip(conv_names, conv_s.run(None, feed)))
        # Warmup (or a window shorter than L+1) makes the offset slice select 0 frames -- there
        # is no valid logical frame yet, so don't advance the GRU (an empty seq crashes it).
        if co["emb"].shape[1] == 0:
            continue
        go = dict(zip(gru_names, gru_s.run(None, {"emb": co["emb"], "h0": h})))
        h = go["h1"]
        for n in ["e0", "e1", "e2", "e3", "c0"]:
            acc[n].append(co[n])         # [1,64,1,F]
        acc["emb"].append(go["emb_out"])  # [1,1,512]
        acc["lsnr"].append(go["lsnr"])    # [1,1,1]
    if not acc["emb"]:                     # window < L+1: no valid frame at all
        return {n: np.empty((0,)) for n in acc}
    out = {n: np.concatenate(acc[n], axis=2) for n in ["e0", "e1", "e2", "e3", "c0"]}
    out["emb"] = np.concatenate(acc["emb"], axis=1)
    out["lsnr"] = np.concatenate(acc["lsnr"], axis=1)
    return out


# --------------------------------------------------------------------------- #
# decoder: windowed + stateful (advance-by-one via h1 = state after first frame)
# --------------------------------------------------------------------------- #
def stream_decoder(s, feats, data_outs, num_layers, Wd, hidden):
    h0_name = next(i.name for i in s.get_inputs() if i.name.endswith("_h0"))
    h1_name = next(o.name for o in s.get_outputs() if o.name.endswith("_h1"))
    h = np.zeros([num_layers, 1, hidden], np.float32)
    buf = []                              # list of frame indices currently in window
    pending_h1 = None
    acc = {o: [] for o in data_outs}
    for t in range(T):
        if len(buf) == Wd:
            h = pending_h1
            buf.pop(0)
        buf.append(t)
        a = buf[0]
        feed = {k: frame_slice(v, a, t + 1) for k, v in feats.items()}
        feed[h0_name] = h
        res = dict(zip(onames(s), s.run(None, feed)))
        pending_h1 = res[h1_name]
        for o in data_outs:
            acc[o].append(res[o])         # slice-last: single frame
    return {o: np.concatenate(acc[o], axis=data_outs[o]) for o in data_outs}


def search(whole_cmp, stream_fn, data_outs):
    """Grow W until every data output reproduces the (aligned) whole ref; return (W,worst,diffs,ok).
    whole_cmp holds the reference arrays already trimmed to the streamed frame count."""
    best = None
    for W in W_CANDIDATES:
        st = stream_fn(W)
        # A window smaller than the lookahead yields too few streamed frames -- treat as no match.
        diffs = {k: (maxdiff(whole_cmp[k], st[k]) if whole_cmp[k].shape == st[k].shape
                     else float("inf")) for k in data_outs}
        worst = max(diffs.values())
        if best is None or worst < best[1]:
            best = (W, worst, diffs)
        if worst < TOL:
            return W, worst, diffs, True
    W, worst, diffs = best
    return W, worst, diffs, False


def main(md):
    md = md.rstrip("/\\")
    L = read_conv_lookahead(md)
    print(f"model dir: {md}   conv_lookahead (encoder lag) = {L}")

    feat_erb = RNG.standard_normal((1, 1, T, 32)).astype(np.float32)
    feat_spec = RNG.standard_normal((1, 2, T, 96)).astype(np.float32)
    emb = RNG.standard_normal((1, T, 512)).astype(np.float32)
    e3 = RNG.standard_normal((1, 64, T, 8)).astype(np.float32)
    e2 = RNG.standard_normal((1, 64, T, 8)).astype(np.float32)
    e1 = RNG.standard_normal((1, 64, T, 16)).astype(np.float32)
    e0 = RNG.standard_normal((1, 64, T, 32)).astype(np.float32)
    c0 = RNG.standard_normal((1, 64, T, 96)).astype(np.float32)

    worst_all = 0.0
    results = []

    # --- enc ---
    enc_w = sess(f"{md}/enc.onnx")
    conv_s = sess(f"{md}/enc_conv_streaming.onnx")
    gru_s = sess(f"{md}/enc_gru_streaming.onnx")
    enc_hidden = hidden_of(gru_s)
    enc_feats = {"feat_erb": feat_erb, "feat_spec": feat_spec}
    enc_whole = run_dict(enc_w, enc_feats)
    enc_outs = ["e0", "e1", "e2", "e3", "emb", "c0", "lsnr"]
    n_cmp = T - L                                   # streamed logical frames 0..T-1-L
    enc_whole_cmp = {k: head_frames(enc_whole[k], n_cmp, ENC_AXIS[k]) for k in enc_outs}
    W, worst, diffs, ok = search(
        enc_whole_cmp,
        lambda Wc: stream_enc(conv_s, gru_s, enc_feats, Wc, L, enc_hidden),
        {k: ENC_AXIS[k] for k in enc_outs})
    print(f"\n=== enc (enc_conv window W={W}, hidden={enc_hidden}, slice offset={L}, "
          f"frames compared={n_cmp}) ===")
    for k in enc_outs:
        print(f"  enc.{k:5s} max|diff| = {diffs[k]:.3e}")
    worst_all = max(worst_all, worst)
    results.append(("enc", W, worst, ok))

    # --- erb_dec ---
    erb_w = sess(f"{md}/erb_dec.onnx")
    erb_s = sess(f"{md}/erb_dec_streaming.onnx")
    erb_layers = num_layers_of(erb_s)
    erb_hidden = hidden_of(erb_s)
    erb_feats = {"emb": emb, "e3": e3, "e2": e2, "e1": e1, "e0": e0}
    erb_whole = run_dict(erb_w, erb_feats)
    W, worst, diffs, ok = search(
        {"m": erb_whole["m"]},
        lambda Wd: stream_decoder(erb_s, erb_feats, {"m": 2}, erb_layers, Wd, erb_hidden),
        {"m": 2})
    print(f"\n=== erb_dec (window W={W}, layers={erb_layers}, hidden={erb_hidden}) ===")
    print(f"  erb_dec.m   max|diff| = {diffs['m']:.3e}")
    worst_all = max(worst_all, worst)
    results.append(("erb_dec", W, worst, ok))

    # --- df_dec ---
    df_w = sess(f"{md}/df_dec.onnx")
    df_s = sess(f"{md}/df_dec_streaming.onnx")
    df_layers = num_layers_of(df_s)
    df_hidden = hidden_of(df_s)
    df_feats = {"emb": emb, "c0": c0}
    df_whole = run_dict(df_w, df_feats)
    W, worst, diffs, ok = search(
        {"coefs": df_whole["coefs"]},
        lambda Wd: stream_decoder(df_s, df_feats, {"coefs": 1}, df_layers, Wd, df_hidden),
        {"coefs": 1})
    print(f"\n=== df_dec (window W={W}, layers={df_layers}, hidden={df_hidden}) ===")
    print(f"  df_dec.coefs max|diff| = {diffs['coefs']:.3e}")
    worst_all = max(worst_all, worst)
    results.append(("df_dec", W, worst, ok))

    print("\n" + "=" * 52)
    print("window sizes:  " + "  ".join(f"{n}=W{W}" for n, W, _, _ in results))
    print(f"encoder lag (conv_lookahead) = {L} frame(s)")
    print(f"WORST max|diff| = {worst_all:.3e}   TOL = {TOL:.0e}")
    ok = all(r[3] for r in results) and worst_all < TOL
    print("GATE A: PASS" if ok else "GATE A: FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "models/dfn3"))
