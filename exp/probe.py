"""exp/probe.py — dump an ONNX model's IO contract for wrapper work.

Usage:  python exp/probe.py exp/models/gtcrn_simple.onnx
        python exp/probe.py exp/models/LocalVQEAECResidualMask.onnx

Prints every input/output name, element type, and shape (symbolic dims kept),
plus initializers that look like state. Copy the table into the wrapper's V1
section before writing the Run() call — guessing node names is how wrappers
silently run the wrong graph.
"""

import sys


def main(path):
    import onnxruntime as ort

    so = ort.SessionOptions()
    so.intra_op_num_threads = 1
    so.inter_op_num_threads = 1
    sess = ort.InferenceSession(path, sess_options=so,
                                providers=["CPUExecutionProvider"])
    print(f"model: {path}")
    for io, tag in ((sess.get_inputs(), "IN"), (sess.get_outputs(), "OUT")):
        for x in io:
            print(f"  {tag} {x.name}  {x.type}  shape={x.shape}")
    print("providers:", sess.get_providers())


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    main(sys.argv[1])
