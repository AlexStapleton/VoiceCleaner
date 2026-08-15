#!/usr/bin/env python3
"""Rewrite the whole-utterance DeepFilterNet3 `_ll` ONNX models into streaming variants.

Two kinds of recurrent state exist in DFN3 and are handled by their nature:

  1. GRUs -> made STATEFUL: the GRU `initial_h` (input index 5, a baked constant) is
     replaced by a graph INPUT, and a graph OUTPUT carries the next hidden state. For a
     multi-layer decoder the state is stacked `[num_layers, 1, hidden]`, Sliced per layer
     and Concated back.

  2. TEMPORAL CONVS (time-kernel > 1) -> handled by HOST-SIDE WINDOWING: the host feeds
     the sub-graph a rolling window of the last W input frames each hop, and the graph
     `Slice`s its data outputs to the LAST time frame only. A window covering the compounded
     causal receptive field reproduces the whole-utterance result EXACTLY.

You cannot window a graph that still contains a GRU whose state is carried by simple
h_out->h_in (windowing re-processes already-seen frames and would advance the GRU by W
frames per hop). Two model-specific resolutions:

  * ENCODER  -- split at the GRU boundary:
      enc_conv_streaming.onnx : pre-GRU convs, stateless, fed a window, outputs sliced to
                                the last frame (e0..e3,c0 on axis 2 [B,C,T,F]; emb on axis 1
                                [B,T,H]).
      enc_gru_streaming.onnx  : GRU + post-GRU (lsnr), stateful h0->h1, run one frame/hop
                                (its `emb` input is already the single sliced frame).

  * DECODERS -- windowed AND stateful in one graph. The whole decoder (GRU included) is fed
      a rolling window; the GRU is given the pre-window hidden state as `*_h0` and emits, as
      `*_h1`, the hidden state AFTER THE FIRST window frame (GRU `Y[0]`), so the host can
      advance the carried state by exactly ONE frame per hop. Data outputs are Sliced to the
      last frame. This is exact for any window W >= the post-GRU conv receptive field, and it
      degrades to a plain stateful GRU when W==1 (first frame == last frame).

Pure onnx graph surgery + onnxsim cleanup -- no PyTorch, no retraining.
Deps: onnx, onnxsim (best-effort), onnxruntime (optional, for the shape print).
Usage: python tools/dfn3_make_streaming.py [models/dfn3-ll]
"""
import configparser
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

INT64_MAX = np.iinfo(np.int64).max


# --------------------------------------------------------------------------- #
# config
# --------------------------------------------------------------------------- #
def read_config(model_dir):
    """Return {conv_lookahead, df_lookahead} read from the model's config.ini.

    conv_lookahead is the number of FUTURE frames the encoder convolutions see; it is the
    number of frames the streaming conv sub-graph must slice BEHIND the newest window frame
    (the newest frame's future context is missing / zero-padded). For the purely-causal `_ll`
    model both are 0, so the surgery degrades to plain slice-last -- unchanged behaviour.
    """
    cfg = configparser.ConfigParser()
    ini = model_dir / "config.ini"
    conv_la = df_la = 0
    if ini.exists():
        cfg.read(str(ini))
        if cfg.has_option("deepfilternet", "conv_lookahead"):
            conv_la = cfg.getint("deepfilternet", "conv_lookahead")
        if cfg.has_option("df", "df_lookahead"):
            df_la = cfg.getint("df", "df_lookahead")
    print(f"  config.ini: conv_lookahead={conv_la}  df_lookahead={df_la}")
    return {"conv_lookahead": conv_la, "df_lookahead": df_la}


def _slice_bounds(offset):
    """Slice starts/ends selecting the single time frame `offset` positions before the newest.

    offset=0 -> the LAST frame (slice-last): starts=[-1], ends=[INT64_MAX].
    offset=L -> the frame with FULL context under an L-frame lookahead: starts=[-(L+1)],
                ends=[-L] (negative bounds keep it independent of the window length S).
    """
    if offset <= 0:
        return -1, INT64_MAX
    return -(offset + 1), -offset


# --------------------------------------------------------------------------- #
# graph helpers
# --------------------------------------------------------------------------- #
def get_node_by_output(graph, output_name):
    for node in graph.node:
        for out in node.output:
            if out == output_name:
                return node
    return None


def get_all_predecessors(graph, target_names):
    """All node names that (transitively) produce the given tensor names."""
    out_to_node = {}
    for node in graph.node:
        for out in node.output:
            out_to_node[out] = node
    visited, result, queue = set(), set(), list(target_names)
    while queue:
        name = queue.pop()
        if name in visited:
            continue
        visited.add(name)
        if name in out_to_node:
            node = out_to_node[name]
            result.add(node.name)
            queue.extend(node.input)
    return result


def get_initializer_names(graph):
    return {init.name for init in graph.initializer}


def prune_unused_initializers(graph):
    used = set()
    for node in graph.node:
        used.update(node.input)
    for i in reversed(range(len(graph.initializer))):
        if graph.initializer[i].name not in used:
            del graph.initializer[i]


def finalize(model, out_path, sim=True):
    if sim:
        try:
            from onnxsim import simplify
            simp, ok = simplify(model)
            if ok:
                model = simp
        except Exception as e:  # noqa: BLE001
            print(f"  (onnxsim skipped: {e})")
    onnx.checker.check_model(model)
    onnx.save(model, str(out_path))
    m = onnx.load(str(out_path))
    ins = [(i.name, _shape(i)) for i in m.graph.input]
    outs = [(o.name, _shape(o)) for o in m.graph.output]
    print(f"  Saved {out_path.name} ({out_path.stat().st_size/1024:.0f} KB)")
    print(f"    IN : {ins}")
    print(f"    OUT: {outs}")
    return model


def _shape(vi):
    dims = []
    for d in vi.type.tensor_type.shape.dim:
        dims.append(d.dim_value if d.HasField("dim_value") else (d.dim_param or "?"))
    return dims


# --------------------------------------------------------------------------- #
# ENCODER SPLIT
# --------------------------------------------------------------------------- #
def split_encoder(model_dir, conv_lookahead=0):
    enc_path = model_dir / "enc.onnx"
    model = onnx.load(str(enc_path))
    graph = model.graph

    grus = [n for n in graph.node if n.op_type == "GRU"]
    if not grus:
        raise SystemExit("enc.onnx: no GRU found")
    # The encoder split assumes a SINGLE emb GRU (pre-GRU convs -> GRU -> lsnr). Both the `_ll`
    # and the standard exports have exactly one encoder GRU (emb_num_layers is realised as a
    # single ONNX GRU node here). If a future export stacks encoder GRUs, the split point below
    # (pre-GRU convs) still holds, but enc_gru would need the decoders' stacked-state treatment.
    if len(grus) != 1:
        raise SystemExit(
            f"enc.onnx: expected 1 encoder GRU, found {len(grus)} -- the encoder split "
            f"assumes a single GRU; stacked encoder GRUs need the decoder stacked-state path")
    gru_node = grus[0]
    hidden_size = next(a.i for a in gru_node.attribute if a.name == "hidden_size")
    print(f"  Encoder GRU: {gru_node.name}, hidden_size={hidden_size}, "
          f"conv_lookahead={conv_lookahead} (conv slice offset)")

    # GRU X comes from a Transpose whose input is the pre-GRU embedding tensor.
    transpose_node = get_node_by_output(graph, gru_node.input[0])
    assert transpose_node is not None and transpose_node.op_type == "Transpose", \
        "enc GRU.X not fed by expected Transpose; re-export changed the pre-GRU layout — revisit the encoder split"
    pre_gru_emb = transpose_node.input[0]
    print(f"  Pre-GRU embedding tensor: {pre_gru_emb}")

    pre_gru_names = get_all_predecessors(graph, [pre_gru_emb])
    print(f"  Pre-GRU conv nodes: {len(pre_gru_names)}")

    out_to_node = {}
    for node in graph.node:
        for out in node.output:
            out_to_node[out] = node
    in_to_consumers = defaultdict(list)
    for node in graph.node:
        for inp in node.input:
            in_to_consumers[inp].append(node)

    # Post-GRU nodes (everything reachable from the GRU outputs, minus pre-GRU/GRU).
    post_gru_names, queue, visited = set(), list(gru_node.output), set()
    while queue:
        tensor = queue.pop()
        if tensor in visited:
            continue
        visited.add(tensor)
        for consumer in in_to_consumers.get(tensor, []):
            if consumer.name not in pre_gru_names and consumer.name != gru_node.name:
                post_gru_names.add(consumer.name)
                queue.extend(consumer.output)
    print(f"  Post-GRU nodes: {len(post_gru_names)}")

    init_names = get_initializer_names(graph)
    gru_const_names = set()
    for inp in gru_node.input:
        if inp and inp not in init_names:
            producer = out_to_node.get(inp)
            if producer and producer.name not in pre_gru_names:
                gru_const_names.add(producer.name)
                for p in get_all_predecessors(graph, [inp]):
                    if p not in pre_gru_names:
                        gru_const_names.add(p)

    _build_enc_conv(enc_path, pre_gru_emb, model_dir / "enc_conv_streaming.onnx",
                    conv_lookahead)
    _build_enc_gru(enc_path, gru_node, hidden_size, pre_gru_emb, gru_const_names,
                   post_gru_names, model_dir / "enc_gru_streaming.onnx")


def _build_enc_conv(enc_path, pre_gru_emb, out_path, conv_lookahead=0):
    model = onnx.load(str(enc_path))
    graph = model.graph
    keep = get_all_predecessors(graph, [pre_gru_emb])
    for i in reversed(range(len(graph.node))):
        if graph.node[i].name not in keep:
            del graph.node[i]

    sl_starts, sl_ends = _slice_bounds(conv_lookahead)
    print(f"  enc_conv slice offset = {conv_lookahead}  (starts={sl_starts}, ends={sl_ends})")
    graph.initializer.append(numpy_helper.from_array(np.array([sl_starts], np.int64), "_sl_starts"))
    graph.initializer.append(numpy_helper.from_array(np.array([sl_ends], np.int64), "_sl_ends"))
    graph.initializer.append(numpy_helper.from_array(np.array([2], np.int64), "_sl_axis2"))
    graph.initializer.append(numpy_helper.from_array(np.array([1], np.int64), "_sl_axis1"))

    orig = {o.name: o for o in graph.output}
    while len(graph.output):
        graph.output.pop()

    def slice_last(tensor_name, axis_init, out_name, rank):
        raw = f"_raw_{out_name}"
        for node in graph.node:
            for i, o in enumerate(node.output):
                if o == tensor_name:
                    node.output[i] = raw
            for i, inp in enumerate(node.input):
                if inp == tensor_name:
                    node.input[i] = raw
        graph.node.append(helper.make_node(
            "Slice", [raw, "_sl_starts", "_sl_ends", axis_init], [out_name],
            name=f"_slice_{out_name}"))
        graph.output.append(helper.make_tensor_value_info(
            out_name, TensorProto.FLOAT, [None] * rank))

    for name in ["e0", "e1", "e2", "e3", "c0"]:
        if name in orig:
            slice_last(name, "_sl_axis2", name, 4)   # [B,C,T,F] -> time axis 2
    slice_last(pre_gru_emb, "_sl_axis1", "emb", 3)   # [B,T,H]   -> time axis 1

    prune_unused_initializers(graph)
    finalize(model, out_path)


def _build_enc_gru(enc_path, gru_ref, hidden_size, pre_gru_emb, gru_const_names,
                   post_gru_names, out_path):
    model = onnx.load(str(enc_path))
    graph = model.graph

    keep = set(gru_const_names) | set(post_gru_names)
    for node in graph.node:
        if node.op_type == "GRU" or any(o == gru_ref.input[0] for o in node.output):
            keep.add(node.name)

    out_to_node = {}
    for node in graph.node:
        for out in node.output:
            out_to_node[out] = node
    pre_gru_copy = get_all_predecessors(graph, [pre_gru_emb])
    needed = set()
    for node in graph.node:
        if node.name in keep:
            needed.update(node.input)
    changed = True
    while changed:
        changed = False
        for tensor in list(needed):
            producer = out_to_node.get(tensor)
            if producer and producer.name not in keep and producer.name not in pre_gru_copy:
                keep.add(producer.name)
                needed.update(producer.input)
                changed = True

    for i in reversed(range(len(graph.node))):
        if graph.node[i].name not in keep:
            del graph.node[i]

    gru = next(n for n in graph.node if n.op_type == "GRU")
    assert not any(a.name == "direction" and a.s not in (b"", b"forward") for a in gru.attribute), \
        "bidirectional GRU — state shapes assume forward"

    old_h = gru.input[5] if len(gru.input) > 5 else ""
    while len(gru.input) < 6:
        gru.input.append("")
    gru.input[5] = "h0"
    if old_h:
        for i in reversed(range(len(graph.node))):
            if graph.node[i].name in get_all_predecessors(graph, [old_h]):
                del graph.node[i]

    # post-GRU model output "emb" -> "emb_out"; pre-GRU tensor -> model input "emb"
    for node in graph.node:
        for i, o in enumerate(node.output):
            if o == "emb":
                node.output[i] = "emb_out"
        for i, inp in enumerate(node.input):
            if inp == "emb":
                node.input[i] = "emb_out"
    for o in graph.output:
        if o.name == "emb":
            o.name = "emb_out"
    for node in graph.node:
        for i, inp in enumerate(node.input):
            if inp == pre_gru_emb:
                node.input[i] = "emb"

    if len(gru.output) < 2 or not gru.output[1]:
        while len(gru.output) < 2:
            gru.output.append("")
        gru.output[1] = "_gru_h1_raw"
    if gru.output[1] != "h1":
        graph.node.append(helper.make_node("Identity", [gru.output[1]], ["h1"],
                                           name="_patch_h1_identity"))

    w_name = gru.input[1]
    emb_size = hidden_size
    for init in graph.initializer:
        if init.name == w_name:
            emb_size = init.dims[2]
            break
    print(f"  Pre-GRU embedding size: {emb_size}")

    while len(graph.input):
        graph.input.pop()
    graph.input.append(helper.make_tensor_value_info("emb", TensorProto.FLOAT, [1, "S", emb_size]))
    graph.input.append(helper.make_tensor_value_info("h0", TensorProto.FLOAT, [1, "B", hidden_size]))
    while len(graph.output):
        graph.output.pop()
    graph.output.append(helper.make_tensor_value_info("emb_out", TensorProto.FLOAT, [None, None, None]))
    graph.output.append(helper.make_tensor_value_info("lsnr", TensorProto.FLOAT, [None, None, None]))
    graph.output.append(helper.make_tensor_value_info("h1", TensorProto.FLOAT, [1, "B", hidden_size]))

    prune_unused_initializers(graph)
    finalize(model, out_path)


# --------------------------------------------------------------------------- #
# DECODER (windowed + stateful, advance-by-one)
# --------------------------------------------------------------------------- #
def patch_decoder(model_dir, filename, prefix, slice_specs, data_slice_offset=0):
    """Windowed + stateful decoder surgery.

    slice_specs: {output_name: time_axis} for data outputs sliced to a single frame.
    data_slice_offset: frames behind the newest window frame to slice as the valid output
        (0 = slice-last, as for the causal `_ll` decoders). The DFN3 decoders are causal in
        time -- the model's lookahead lives in the encoder convs and, for the DF path, in the
        host-side DeepFilterRing (leftPad), NOT in these graphs -- so this stays 0. It is
        plumbed through only so the surgery is uniform with the encoder and Gate A can prove
        the choice.
    Emits *_streaming.onnx with:
        input  <prefix>_h0 [num_layers,1,hidden]  (GRU state BEFORE the window)
        output <prefix>_h1 [num_layers,1,hidden]  (GRU state AFTER the FIRST window frame)
    """
    path = model_dir / filename
    stem = Path(filename).stem
    out_path = model_dir / f"{stem}_streaming.onnx"
    model = onnx.load(str(path))
    graph = model.graph

    grus = [n for n in graph.node if n.op_type == "GRU"]
    if not grus:
        raise SystemExit(f"{filename}: no GRU nodes")
    num_layers = len(grus)
    hidden = next(a.i for a in grus[0].attribute if a.name == "hidden_size")
    print(f"  {filename}: {num_layers} GRU layer(s), hidden_size={hidden}")

    # Drop any original graph output that isn't a windowed/sliced data output (e.g. df_dec's
    # `302`, an un-sliced [.,S,1] sigmoid the host never consumes). Leaving it un-sliced would
    # return W frames — an untested trap. Removing it orphans its producers; onnxsim prunes them.
    dropped = [o.name for o in graph.output if o.name not in slice_specs]
    for name in dropped:
        for j in reversed(range(len(graph.output))):
            if graph.output[j].name == name:
                del graph.output[j]
    if dropped:
        print(f"    dropped unused output(s): {dropped}")

    h0_name, h1_name = f"{prefix}_h0", f"{prefix}_h1"
    graph.initializer.append(numpy_helper.from_array(np.array([0], np.int64), f"_{prefix}_y0_starts"))
    graph.initializer.append(numpy_helper.from_array(np.array([1], np.int64), f"_{prefix}_y0_ends"))
    graph.initializer.append(numpy_helper.from_array(np.array([0], np.int64), f"_{prefix}_y0_axes"))
    graph.initializer.append(numpy_helper.from_array(np.array([1, 1, -1], np.int64), f"_{prefix}_y0_reshape"))

    # pre_nodes must precede the GRUs (they feed initial_h); post_nodes follow the GRUs.
    pre_nodes, post_nodes, h1_parts = [], [], []
    for i, gru in enumerate(grus):
        assert not any(a.name == "direction" and a.s not in (b"", b"forward") for a in gru.attribute), \
            "bidirectional GRU — state shapes assume forward"
        # per-layer initial_h = h0[i:i+1]  -> [1,1,hidden]
        s_out = f"_{prefix}_h0_L{i}"
        graph.initializer.append(numpy_helper.from_array(np.array([i], np.int64), f"_{prefix}_hs_{i}"))
        graph.initializer.append(numpy_helper.from_array(np.array([i + 1], np.int64), f"_{prefix}_he_{i}"))
        graph.initializer.append(numpy_helper.from_array(np.array([0], np.int64), f"_{prefix}_ha_{i}"))
        pre_nodes.append(helper.make_node(
            "Slice", [h0_name, f"_{prefix}_hs_{i}", f"_{prefix}_he_{i}", f"_{prefix}_ha_{i}"],
            [s_out], name=f"_{prefix}_slice_h0_{i}"))
        while len(gru.input) < 6:
            gru.input.append("")
        gru.input[5] = s_out

        # Y (full sequence, output index 0) must be present.
        if not gru.output or not gru.output[0]:
            while len(gru.output) < 1:
                gru.output.append("")
            gru.output[0] = f"_{prefix}_Y_{i}"
        y_name = gru.output[0]
        # h1 layer = reshape(Y[0:1 on seq axis]) -> [1,1,hidden]
        y0 = f"_{prefix}_Y0_{i}"
        post_nodes.append(helper.make_node(
            "Slice", [y_name, f"_{prefix}_y0_starts", f"_{prefix}_y0_ends", f"_{prefix}_y0_axes"],
            [y0], name=f"_{prefix}_slice_Y0_{i}"))
        y0r = f"_{prefix}_Y0r_{i}"
        post_nodes.append(helper.make_node(
            "Reshape", [y0, f"_{prefix}_y0_reshape"], [y0r], name=f"_{prefix}_reshape_Y0_{i}"))
        h1_parts.append(y0r)

    if len(h1_parts) == 1:
        post_nodes.append(helper.make_node("Identity", h1_parts, [h1_name],
                                           name=f"_{prefix}_h1_identity"))
    else:
        post_nodes.append(helper.make_node("Concat", h1_parts, [h1_name],
                                           name=f"_{prefix}_h1_concat", axis=0))

    # slice a single valid frame from data outputs (offset frames behind the newest)
    sl_starts, sl_ends = _slice_bounds(data_slice_offset)
    graph.initializer.append(numpy_helper.from_array(np.array([sl_starts], np.int64), f"_{prefix}_sl_starts"))
    graph.initializer.append(numpy_helper.from_array(np.array([sl_ends], np.int64), f"_{prefix}_sl_ends"))
    axis_inits = {}
    for out_name, axis in slice_specs.items():
        ai = axis_inits.get(axis)
        if ai is None:
            ai = f"_{prefix}_sl_axis{axis}"
            graph.initializer.append(numpy_helper.from_array(np.array([axis], np.int64), ai))
            axis_inits[axis] = ai
        raw = f"_raw_{out_name}"
        for node in graph.node:
            for i, o in enumerate(node.output):
                if o == out_name:
                    node.output[i] = raw
            for i, inp in enumerate(node.input):
                if inp == out_name:
                    node.input[i] = raw
        post_nodes.append(helper.make_node(
            "Slice", [raw, f"_{prefix}_sl_starts", f"_{prefix}_sl_ends", ai], [out_name],
            name=f"_{prefix}_slice_{out_name}"))

    for i, n in enumerate(pre_nodes):
        graph.node.insert(i, n)
    graph.node.extend(post_nodes)
    graph.input.append(helper.make_tensor_value_info(h0_name, TensorProto.FLOAT, [num_layers, 1, hidden]))
    graph.output.append(helper.make_tensor_value_info(h1_name, TensorProto.FLOAT, [num_layers, 1, hidden]))

    prune_unused_initializers(graph)
    # onnxsim can constant-fold the slice-last away on symbolic time dims incorrectly on
    # some builds; keep it but it is best-effort and re-checked.
    finalize(model, out_path)


def main():
    model_dir = Path(sys.argv[1] if len(sys.argv) > 1 else "models/dfn3-ll")
    cfg = read_config(model_dir)
    print(f"=== enc split ===")
    split_encoder(model_dir, conv_lookahead=cfg["conv_lookahead"])
    print(f"=== erb_dec ===")
    patch_decoder(model_dir, "erb_dec.onnx", "erb", {"m": 2})
    print(f"=== df_dec ===")
    patch_decoder(model_dir, "df_dec.onnx", "df", {"coefs": 1})


if __name__ == "__main__":
    main()
