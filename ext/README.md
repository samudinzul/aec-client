# ext/ — Rust accelerators for aec-web (optional)

`aec_dsp` is an optional PyO3 extension that replaces the hot
number-crunching kernels in `web/dsp.py` (int16/float converts,
rFFT/masked-iFFT, RMS, FIR + linear resamplers). `web/dsp.py`
imports it behind a `try/except ImportError` and falls back to
pure NumPy when it is absent — the extension is never required.

## Status

- **DSP kernels: done** (`ext/aec_dsp/src/lib.rs`). Parity: integer
  paths bit-exact, FIR paths bit-exact, FFT paths ≤5e-6
  (`python3 /tmp/opencode/parity_dsp.py` pattern), end-to-end smoke
  output identical (`frames=250 out_rms=902.3 peak=8385`).
- **TFLite-in-Rust: not started.** Blocked on the TF Lite C library
  toolchain (no prebuilt `tensorflowlite_c` in this environment;
  building TF from source was judged out of scope here). Recipe:
  install TF Lite C API (Windows: prebuilt `tensorflowlite_c.dll`
  from the TensorFlow releases), add the `tflite` crate, port
  `_TflitePair` session handling (`web/dtln.py`, `web/dtln_ns.py`)
  keeping fail-open semantics, A/B delegate behavior vs LiteRT.

## Build (Windows, the shipped platform)

1. Install Rust stable (https://rustup.rs/) and Python 3.10+.
2. `pip install maturin`
3. From the repo root:
   ```
   maturin develop --manifest-path ext/aec_dsp/Cargo.toml
   ```
   (dev install into the active venv), or
   ```
   maturin build --manifest-path ext/aec_dsp/Cargo.toml --release
   ```
    for the shipping wheel. The wheel is for local dev installs
    only (`maturin develop` / `pip install` into your venv) —
    a `.pyd` is a PE binary and must NEVER ship in a release zip
    (`scripts/make-release-web.sh` asserts zero `.pyd` and fails
    the build otherwise; `web/dsp.py` falls back to NumPy).

Linux dev loop is identical (`maturin develop`).

## Verify after building

```bash
python3 -m web.test_offline --smoke   # must print the same RMS/peak
python3 /tmp/opencode/parity_dsp.py    # kernel-level parity (dev script)
```

## AV note

The web zip's promise was "no binaries". Shipping `aec_dsp*.pyd`
narrows that to "one small local extension, everything else
pure Python + mainstream wheels". The fallback keeps working
without it — the `.pyd` is an accelerator, not a requirement.
