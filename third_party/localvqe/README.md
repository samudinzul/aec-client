# third_party/localvqe — vendored LocalVQE v1.4-AEC adaptive frontend.

Verbatim copies (do not edit) from soniqo/speech-core (Apache-2.0),
`src/models/localvqe/`:

- `localvqe_aec_frontend.h` / `.cpp` — partitioned-block frequency-domain
  Kalman filter + 2,742-weight neural controller + GCC-PHAT prealignment.
  Derived from localai-org/LocalVQE's `ggml/daf_frontend.cpp`.
  Self-contained C ABI: C++17, STL only, no external dependencies.

The neural half (`LocalVQEAECResidualMask.onnx`) is NOT vendored — it is
downloaded by `exp/get_models.sh` like the other model weights. The mask
runner + 512/256 sqrt-Hann codec around this frontend are our own
(`src/localvqe_wrapper.cpp`, ported from speech-core's
`onnx_localvqe_echo_canceller.cpp` DSP flow: bins 1..256 packing,
Nyquist doubling, overlap-add).

Upstream: https://github.com/soniqo/speech-core
Weights:  https://huggingface.co/soniqo/LocalVQE-v1.4-AEC-200K-ONNX-FP32
Source:   https://github.com/localai-org/LocalVQE (Apache-2.0)
