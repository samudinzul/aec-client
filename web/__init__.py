"""aec-web package.

Thread-pool caps live here (not in start.bat) so EVERY entry point
(server, native window, tests) gets them: this module runs before
any sibling import in the process, i.e. before numpy/OpenBLAS is
loaded and its pool sizes freeze.

Why: the pump is a single 8 ms worker thread plus tiny model
invokes — extra BLAS threads add spinning stacks, not speed.
Measured on the dev box: single-thread BLAS changes nothing
per-frame and removes the idle heat (Task Manager % with the
app sitting Started but quiet). Inference pools are capped
separately (Chain num_threads -> LiteRT XNNPACK, 1 for ONNX).
"""

import os

for _var in ("OPENBLAS_NUM_THREADS", "OMP_NUM_THREADS",
             "MKL_NUM_THREADS"):
    os.environ.setdefault(_var, "1")
