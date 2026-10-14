"""Hush noise suppression — optional NS backend via the weya_nc C ABI.

Hush (pulp-vision/Hush, Apache-2.0: a DeepFilterNet3 fork with a
background-speaker auxiliary loss) denoises 16 kHz mono float audio
through a prebuilt shared library + ONNX bundle. Only the LIBRARY
is user-supplied (never shipped, never staged — the release asserts
ban every .dll/.pyd): the MODEL bundle ships in models/ like the
DTLN pairs and NKF (data, allowlisted), resolved via an explicit
path, env, the chain's models dir, or the per-user data dir, in
that order. Stdlib ctypes only: no new dependency.
- Backend selection is fail-closed (Chain.set_ns_backend refuses
  "hush" while library + bundle are both unresolvable); a mid-run
  load failure is fail-open (passthrough + the error rides along
  in state, the same contract as a missing DTLN-NS pair).

Frame bridge: the chain pumps 128-sample frames (the DTLN DSP
geometry), Hush consumes fixed 160-sample frames (10 ms @ 16 kHz,
queried via weya_nc_get_frame_length). process() carries a small
FIFO both ways and emits exactly len(input) per call; while
priming, the shortfall is input passthrough (same convention as
DtlnNs.process). Steady state adds at most one Hush frame of delay
on top of the model's ~20 ms algorithmic latency.
"""

import ctypes
import os
import platform

import numpy as np

from .dsp import rms

# Chain.py reports the active NS stage through ns["backend"]:
# 0 = none/broken, 1 = DTLN-NS (dtln_ns.py), 2 = Hush (here).
BACKEND_HUSH = 2

_MODEL_ENV = ("WEYA_NC_MODEL", "WEYA_NC_MODEL_PATH")
_LIB_ENV = "WEYA_NC_LIB"
# Bundle filenames, in lookup priority: the repo/release copy was
# renamed (the desktop release asserts ban any staged filename
# containing "dfn"); the per-user data dir keeps the upstream name
# users download from Hugging Face.
_BUNDLE_NAMES = ("hush_16k_onnx.tar.gz",
                 "advanced_dfnet16k_model_best_onnx.tar.gz")


def _lib_name():
    sysname = platform.system()
    if sysname == "Windows":
        return "weya_nc.dll"
    if sysname == "Darwin":
        return "libweya_nc.dylib"
    return "libweya_nc.so"


def _data_dir():
    """Per-user folder for user-supplied Hush files. The LIBRARY only
    ever resolves here or under env paths (never shipped, never the
    repo); the model additionally resolves from the chain's models
    dir (release bundles ship it, like the DTLN pairs)."""
    if platform.system() == "Windows":
        base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
        return os.path.join(base, "aec-web", "hush")
    return os.path.join(os.path.expanduser("~"), ".local", "share",
                        "aec-web", "hush")


def _find_bundle(d):
    """Best bundle candidate inside dir d (either known name, else
    newest *.tar.gz) — or None. Keeps working when upstream renames
    the file again."""
    if not d or not os.path.isdir(d):
        return None
    for name in _BUNDLE_NAMES:
        cand = os.path.join(d, name)
        if os.path.isfile(cand):
            return cand
    try:
        cands = sorted(f for f in os.listdir(d) if f.endswith(".tar.gz"))
    except OSError:
        cands = []
    return os.path.join(d, cands[-1]) if cands else None


def _default_paths():
    """(lib_path, model_path) under the per-user data dir (may not exist)."""
    d = _data_dir()
    found = _find_bundle(d)
    return (os.path.join(d, _lib_name()),
            found if found is not None
            else os.path.join(d, _BUNDLE_NAMES[-1]))


def resolve_paths(lib_path=None, model_path=None, model_dir=None):
    """Where the Hush library + bundle SHOULD be (existence not checked).

    Model order: explicit path, env (WEYA_NC_MODEL[_PATH]), the
    chain's models dir (release bundles ship it there, like the
    DTLN pairs), per-user data dir. A lone directory is accepted
    for either (known bundle names win, else newest *.tar.gz).
    """
    if lib_path is None:
        lib_path = os.environ.get(_LIB_ENV)
    if model_path is None:
        for key in _MODEL_ENV:
            model_path = os.environ.get(key)
            if model_path:
                break
    if model_path is None and model_dir is not None:
        model_path = _find_bundle(model_dir)
    def_lib, def_model = _default_paths()
    if not lib_path:
        lib_path = def_lib
    if not model_path:
        model_path = def_model
    elif os.path.isdir(model_path):
        found = _find_bundle(model_path)
        if found is not None:
            model_path = found
    return lib_path, model_path


def hush_available(lib_path=None, model_path=None, model_dir=None):
    """Stage runnable (cheap stat only — no library load, no model read).

    True via either path: onnxruntime + bundle (no DLL needed), or
    the user-supplied library + bundle (ctypes fallback).
    """
    lib, model = resolve_paths(lib_path, model_path, model_dir)
    if os.path.isfile(lib) and os.path.isfile(model):
        return True
    try:
        from .hush_onnx import ort
    except ImportError:
        return False
    return ort is not None and os.path.isfile(model)


class _HushCt:
    """Streaming Hush NS over the prebuilt library (fallback path).

    Mirrors the DtlnNs surface (process/reset/stats). Used when the
    onnxruntime path is unavailable; the HushNs dispatcher below
    tries onnx first (no DLL needed) and falls back here.

    Construction never raises: missing files or a broken library
    leave ready == False with last_error set, and process() passes
    audio through untouched (fail-open — a broken stage must never
    mute the voice path).
    """

    def __init__(self, lib_path=None, model_path=None, model_dir=None,
                 atten_lim_db=100.0):
        self.lib_path, self.model_path = resolve_paths(
            lib_path, model_path, model_dir)
        self.atten_lim_db = float(atten_lim_db)
        self.last_error = ""
        self.in_rms = 0.0
        self.out_rms = 0.0
        self._lib = None
        self._model = None
        self._session = None
        self._frame_len = 0
        self._in_carry = np.zeros(0, dtype=np.float32)
        self._out_carry = np.zeros(0, dtype=np.float32)
        # LSNR trace (probe/diagnostic only — gated, never in production).
        self.trace_lsnr = False
        self._lsnr_trace = []
        try:
            self._load()
        except Exception as e:
            self._close()
            self.last_error = (
                "hush unavailable "
                f"(lib={self.lib_path} model={self.model_path}): {e}")
        if self._session is None and not self.last_error:
            self.last_error = (
                "hush unavailable "
                f"(lib={self.lib_path} model={self.model_path})")

    @property
    def ready(self):
        return self._session is not None

    def _load(self):
        if not os.path.isfile(self.lib_path):
            raise RuntimeError(
                f"library not found — download weya_nc for your OS "
                f"into {os.path.dirname(self.lib_path)} "
                f"(or set {_LIB_ENV})")
        if not os.path.isfile(self.model_path):
            raise RuntimeError(
                "model bundle not found — hush_16k_onnx.tar.gz (or the "
                "upstream-named bundle) next to the library "
                f"(or set {_MODEL_ENV[0]})")
        lib = ctypes.CDLL(self.lib_path)
        lib.weya_nc_model_load_from_path.argtypes = [ctypes.c_char_p]
        lib.weya_nc_model_load_from_path.restype = ctypes.c_void_p
        lib.weya_nc_model_free.argtypes = [ctypes.c_void_p]
        lib.weya_nc_model_free.restype = None
        lib.weya_nc_session_create.argtypes = [
            ctypes.c_void_p, ctypes.c_size_t, ctypes.c_float]
        lib.weya_nc_session_create.restype = ctypes.c_void_p
        lib.weya_nc_session_free.argtypes = [ctypes.c_void_p]
        lib.weya_nc_session_free.restype = None
        lib.weya_nc_get_frame_length.argtypes = [ctypes.c_void_p]
        lib.weya_nc_get_frame_length.restype = ctypes.c_size_t
        lib.weya_nc_process_frame.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float)]
        lib.weya_nc_process_frame.restype = ctypes.c_float
        lib.weya_nc_reset.argtypes = [ctypes.c_void_p]
        lib.weya_nc_reset.restype = None
        model = lib.weya_nc_model_load_from_path(
            os.fsencode(self.model_path))
        if not model:
            raise RuntimeError("weya_nc_model_load_from_path returned NULL")
        session = lib.weya_nc_session_create(
            model, 16000, ctypes.c_float(self.atten_lim_db))
        if not session:
            lib.weya_nc_model_free(model)
            raise RuntimeError("weya_nc_session_create returned NULL")
        frame_len = int(lib.weya_nc_get_frame_length(session))
        if frame_len <= 0:
            lib.weya_nc_session_free(session)
            lib.weya_nc_model_free(model)
            raise RuntimeError(f"bogus frame length {frame_len}")
        self._lib, self._model = lib, model
        self._session = session
        self._frame_len = frame_len

    def _close(self):
        if self._lib is not None:
            try:
                if self._session is not None:
                    self._lib.weya_nc_session_free(self._session)
            except Exception:
                pass
            try:
                if self._model is not None:
                    self._lib.weya_nc_model_free(self._model)
            except Exception:
                pass
        self._lib = self._model = self._session = None
        self._frame_len = 0

    def reset(self):
        self._in_carry = np.zeros(0, dtype=np.float32)
        self._out_carry = np.zeros(0, dtype=np.float32)
        self.in_rms = self.out_rms = 0.0
        self._lsnr_trace = []
        if self._lib is not None and self._session is not None:
            try:
                self._lib.weya_nc_reset(self._session)
            except Exception:
                pass  # stale session: next process() fails open anyway

    def process(self, frame_f32):
        """Float in -> float out (same length). Never mutes."""
        frame = np.asarray(frame_f32, dtype=np.float32)
        n = len(frame)
        self.in_rms = rms(frame)
        if not self.ready:
            self.out_rms = self.in_rms
            return frame.copy()
        try:
            return self._process_streaming(frame)
        except Exception:
            # Fail-open: a broken frame ships the mic, never silence.
            self.out_rms = self.in_rms
            return frame.copy()

    def _process_streaming(self, frame):
        n = len(frame)
        fl = self._frame_len
        if n:
            self._in_carry = np.concatenate([self._in_carry, frame])
        while len(self._in_carry) >= fl:
            chunk = np.ascontiguousarray(self._in_carry[:fl])
            self._in_carry = self._in_carry[fl:]
            out = np.zeros(fl, dtype=np.float32)
            lsnr = float(self._lib.weya_nc_process_frame(
                self._session,
                chunk.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
                out.ctypes.data_as(ctypes.POINTER(ctypes.c_float))))
            if self.trace_lsnr:
                self._lsnr_trace.append(lsnr)
            self._out_carry = np.concatenate([self._out_carry, out])
        out = np.empty(n, dtype=np.float32)
        avail = min(len(self._out_carry), n)
        if avail:
            out[:avail] = self._out_carry[:avail]
            self._out_carry = self._out_carry[avail:]
        if avail < n:
            # Priming (first ~10 ms): not enough denoised audio yet —
            # passthrough for the shortfall, like DtlnNs.process.
            out[avail:] = frame[avail:]
        self.out_rms = rms(out)
        return out

    def stats(self):
        return {
            "backend": BACKEND_HUSH if self.ready else 0,
            "rCount": int(len(self._out_carry)),
            "inRms": self.in_rms,
            "outRms": self.out_rms,
            "dropped": 0,
            "error": self.last_error,
        }


class HushNs:
    """NS-stage Hush backend: onnxruntime first, ctypes fallback.

    Same surface as DtlnNs (ready/process/reset/stats) — Chain is
    unaware which path runs. Construction never raises; with neither
    path available process() passes audio through (fail-open).
    stats() carries "path": "onnx" | "ctypes" | "none".
    """

    def __init__(self, lib_path=None, model_path=None, model_dir=None,
                 atten_lim_db=100.0):
        lib, model = resolve_paths(lib_path, model_path, model_dir)
        self.lib_path, self.model_path = lib, model
        self._impl = None
        self._path = "none"
        self.last_error = ""
        self.in_rms = self.out_rms = 0.0
        onnx_error = ""
        if os.path.isfile(model):
            try:
                from .hush_onnx import HushOnnxCore
                self._impl = HushOnnxCore(model)
                self._path = "onnx"
            except Exception as e:
                onnx_error = f"hush onnx path failed: {e}"
                self._impl = None
        if self._impl is None:
            ct = _HushCt(lib_path=lib, model_path=model,
                         atten_lim_db=atten_lim_db)
            if ct.ready:
                self._impl = ct
                self._path = "ctypes"
            else:
                self.last_error = " / ".join(
                    s for s in (onnx_error, ct.last_error) if s) or (
                        "hush unavailable "
                        f"(lib={lib} model={model})")

    @property
    def ready(self):
        return self._impl is not None

    def reset(self):
        self.in_rms = self.out_rms = 0.0
        if self._impl is not None:
            try:
                self._impl.reset()
            except Exception:
                pass

    def process(self, frame_f32):
        frame = np.asarray(frame_f32, dtype=np.float32)
        self.in_rms = rms(frame)
        if self._impl is None:
            self.out_rms = self.in_rms
            return frame.copy()
        try:
            out = self._impl.process(frame)
        except Exception:
            self.out_rms = self.in_rms
            return frame.copy()
        self.out_rms = rms(out)
        return out

    def stats(self):
        if self._impl is None:
            return {
                "backend": 0, "rCount": 0, "inRms": self.in_rms,
                "outRms": self.out_rms, "dropped": 0,
                "error": self.last_error, "path": "none",
            }
        st = dict(self._impl.stats())
        st["path"] = self._path
        return st
