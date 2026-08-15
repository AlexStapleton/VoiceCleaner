#!/usr/bin/env python3
"""Rewrite the shipped whole-utterance DFN3 ONNX models into streaming variants
that expose each GRU's hidden state as graph I/O.

For every GRU node in enc/erb_dec/df_dec:
  * its `initial_h` input (GRU input index 5, currently a baked ConstantOfShape
    zeros tensor) is replaced by a new graph INPUT  `<prefix>_h<i>_in`  [1,1,256];
  * its `Y_h` output (GRU output index 1, currently dangling) is exposed as a new
    graph OUTPUT `<prefix>_h<i>_out` of the same shape.
The i-th GRU (in node order) of each model gets index i. Models stay dynamic on
the time axis; run them with sequence length T=1 per hop.

Pure onnx graph surgery + onnxsim cleanup -- no PyTorch, no retraining.
Deps: onnx, onnxsim.  Usage: python tools/dfn3_make_streaming.py [models/dfn3]
"""
import os
import sys
import onnx
from onnx import helper, TensorProto

HIDDEN = 256
STATE_SHAPE = [1, 1, HIDDEN]          # [num_directions, batch, hidden]


def patch(model_path, out_path, prefix):
    model = onnx.load(model_path)
    graph = model.graph
    grus = [n for n in graph.node if n.op_type == "GRU"]
    if not grus:
        raise SystemExit(f"{model_path}: no GRU nodes found")

    new_inputs, new_outputs = [], []
    for i, node in enumerate(grus):
        in_name = f"{prefix}_h{i}_in"
        # GRU inputs: X, W, R, B, sequence_lens, initial_h. Pad optionals, set [5].
        while len(node.input) < 6:
            node.input.append("")
        node.input[5] = in_name
        # GRU outputs: Y, Y_h. Force a canonical Y_h name and expose it.
        # NOTE: the shipped whole-utterance models already carry a *named but
        # dangling* Y_h (e.g. "/emb_gru/GRU_output_1"). A `if not node.output[1]`
        # guard would keep that name, which (a) fails the plan's required output
        # name `<prefix>_h<i>_out` and (b) breaks Gate A, whose run_stream pairs
        # state I/O by the `_out` suffix and treats non-`_out` outputs as data.
        # So we always rename output[1] to the canonical streaming state name.
        out_name = f"{prefix}_h{i}_out"
        while len(node.output) < 2:
            node.output.append("")
        node.output[1] = out_name
        new_inputs.append(
            helper.make_tensor_value_info(in_name, TensorProto.FLOAT, STATE_SHAPE))
        new_outputs.append(
            helper.make_tensor_value_info(out_name, TensorProto.FLOAT, STATE_SHAPE))

    graph.input.extend(new_inputs)
    graph.output.extend(new_outputs)

    # Best-effort cleanup: prune the now-dead ConstantOfShape/Shape subgraphs that
    # used to feed initial_h. A dead node with no consumer is harmless if this fails.
    try:
        from onnxsim import simplify
        simp, ok = simplify(model)
        if ok:
            model = simp
    except Exception as e:  # noqa: BLE001
        print(f"  (onnxsim skipped: {e})")

    onnx.checker.check_model(model)
    onnx.save(model, out_path)
    names = ", ".join(f"{prefix}_h{i}_in/out" for i in range(len(grus)))
    print(f"{out_path}: exposed {len(grus)} GRU state(s): {names}")


def main(model_dir):
    jobs = [
        ("enc.onnx", "enc_streaming.onnx", "enc"),
        ("erb_dec.onnx", "erb_dec_streaming.onnx", "erb"),
        ("df_dec.onnx", "df_dec_streaming.onnx", "df"),
    ]
    for src, dst, prefix in jobs:
        patch(os.path.join(model_dir, src), os.path.join(model_dir, dst), prefix)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "models/dfn3")
