"""DeepFilterNet3 @ 48 kHz over onnxruntime — the upstream full-band
model, no DLL needed.

Runs the upstream Rikorose/DeepFilterNet ONNX bundle
(models/DeepFilterNet3_onnx.tar.gz: enc + erb_dec + df_dec) through the
mainstream onnxruntime wheel with a NumPy port of the DeepFilterNet
streaming runtime. Same runtime as web/hush_onnx.py (DFN48 is a DF3 fork
at 16 kHz) — only the framing and the weights differ.

Config comes from the bundle's config.ini:
  sr=48000 fft_size=960 hop_size=480 nb_erb=32 nb_df=96
  df_lookahead=2 conv_lookahead=2 lsnr_min=-15 lsnr_max=35

The model is NOT causal (lookahead 2 = two 10 ms frames), so the output
is delayed by 2 frames to stay in sync with the input — the same thing
the Rust runtime does with its "min processing buffer". A caller that
wants the delay compensated must compensate it itself.

Why 48 kHz: the 16 kHz NS models (DTLN-NS / DFN48 / GTCRN) cannot reach
above 8 kHz, so a broadband transient (keyboard click) keeps its sharp
part no matter what the low band does. A full-band NS kills it
everywhere: measured on real speech, the click's 8-24 kHz band drops
79 dB while the speech is preserved to 0.18 dB.

Fail-closed everywhere except the sample loop (which fails open, like
every other stage). Never mutes.

STATUS: PORT INCOMPLETE — the model runs, but the frame PAIRING is wrong.
The upstream runtime (libDF/src/tract.rs) keeps a 7-frame queue of
enhanced spectra (df_order + lookahead) and a 5-frame queue of noisy
spectra (df_order), and at frame t it applies the mask+coefs produced
from frame t's features to the frame t-2 spectrum (y[df_order-1] of a
7-deep queue = 2 frames back), then the 5-frame deep filter over the
noisy queue. That is what gives the model its 2 frames of LOOKAHEAD:
the decision is made from data up to t but applied to the frame t-2.

This port instead enhances the CURRENT frame and delays the output
stream by 2 frames — so the coefs are paired with the wrong frame. The
measured symptom: the deep-filter range (0-4.8 kHz, the first 96 of the
481 bins) comes out ~12 dB too low vs the upstream runtime, while
4-24 kHz is within 1-2 dB. The click IS killed (its 8-24 kHz band drops
229 dB), but the voice is over-suppressed.

Fix (see libDF/src/tract.rs process() + df()):
  - keep a 7-deep queue of enhanced spectra and a 5-deep queue of
    noisy spectra, oldest-first;
  - at frame t: compute the features + lsnr/mask/coefs from frame t,
    push frame t onto both queues;
  - apply the ERB mask and the 5-frame deep filter to the queue entry
    at index df_order-1 (= 2 frames back), using the coefs from t;
  - synthesize THAT spectrum, so the output stream is naturally 2
    frames behind the input.
"""

import os
import re
import sys
import tarfile
import tempfile

import numpy as np

from .dsp import rms

try:
    import onnxruntime as ort
except ImportError:  # optional backend — dispatcher probes before use
    ort = None

SR = 48000
FFT = 960
HOP = 480
NB_ERB = 32
NB_DF = 96
DF_ORDER = 5
N_FREQ = FFT // 2 + 1  # 481
# calc_norm_alpha(48000, 480, 1.0): exp(-0.01) rounded down until
# < 1.0 -> 0.99 exactly (f32). NOT torch's get_norm_alpha (0.99005).
ALPHA = np.float32(0.99)
_ONE_M_ALPHA = np.float32(1.0) - ALPHA
_WNORM = np.float32(1.0 / 960.0)
# The upstream model is NOT causal: df_lookahead=2 and conv_lookahead=2
# mean it consumes two 10 ms frames of FUTURE context. The runtime
# therefore delays the output by 2 frames to stay in sync with the
# input (the Rust runtime does the same via its processing buffer).
LOOKAHEAD_FRAMES = 2
# weya RuntimeParams thresholds (min_db, max_erb_db, max_df_db).
MIN_DB = np.float32(-15.0)
MAX_ERB_DB = np.float32(35.0)
MAX_DF_DB = np.float32(35.0)
# Runtime silence shortcut compares MEAN(x^2) (misnamed `rms` there).
_SILENCE_MS = np.float32(1e-7)


def _erb_widths(sr=SR, fft=FFT, nb=NB_ERB, min_nf=2):
    """Exact f32 replica of df::erb_fb (lib.rs).

    freq2erb(f) = 9.265 * ln1p(f / (24.7 * 9.265)), all in f32;
    per-band bins with min-width carry-over; last band +1 then
    trimmed so the total is exactly fft/2 + 1.
    """
    f32 = np.float32
    c1 = f32(9.265)
    c2 = f32(24.7) * f32(9.265)
    freq_width = f32(sr) / f32(fft)
    e_low = c1 * np.log1p(f32(0.0) / c2).astype(f32)
    e_high = c1 * np.log1p(f32(sr // 2) / c2).astype(f32)
    step = (e_high - e_low) / f32(nb)
    widths = np.zeros(nb, dtype=np.int64)
    prev_freq = 0
    freq_over = 0
    for i in range(1, nb + 1):
        # erb2freq, f32 op order as Rust: ((n/9.265).exp() - 1).
        t = (e_low + f32(i) * step) / c1
        f = c2 * (np.exp(t).astype(f32) - f32(1.0))
        # Rust f32::round = half away from zero (f >= 0 here).
        fb = int(np.floor(f / freq_width + f32(0.5)).astype(f32))
        nbf = fb - prev_freq - freq_over
        if nbf < min_nf:
            freq_over = min_nf - nbf
            nbf = min_nf
        else:
            freq_over = 0
        widths[i - 1] = nbf
        prev_freq = fb
    widths[-1] += 1
    too_large = int(widths.sum() - (fft // 2 + 1))
    if too_large > 0:
        widths[-1] -= too_large
    assert widths.sum() == fft // 2 + 1, widths
    return widths


def _vorbis_window(n=FFT):
    """DFState::new's window: f64 sin chain, cast f32."""
    i = np.arange(n, dtype=np.float64)
    s = np.sin(0.5 * np.pi * (i + 0.5) / (n // 2))
    return np.sin(0.5 * np.pi * s * s).astype(np.float32)


def _linspace_f32(lo, hi, n):
    """Rust Array1::linspace replica (inclusive, all ops f32)."""
    lo, hi = np.float32(lo), np.float32(hi)
    step = (hi - lo) / np.float32(n - 1)
    return (lo + np.arange(n, dtype=np.float32) * step).astype(np.float32)


_WIDTHS = _erb_widths()
assert _WIDTHS.sum() == N_FREQ


def _erb_widths_f64(sr=SR, fft=FFT, nb=NB_ERB, min_nf=2):
    """Same formula in float64: must agree with the f32 replica.

    Agreement proves no band boundary sits on an f32 rounding cliff
    (the only way f32-vs-f64 arithmetic could change the integer
    widths). Checked once at import; a mismatch fails closed here,
    not as mysterious dB offsets mid-call.
    """
    freq_width = sr / fft
    c1 = 9.265
    c2 = 24.7 * 9.265
    e_low = c1 * np.log1p(0.0 / c2)
    e_high = c1 * np.log1p((sr // 2) / c2)
    step = (e_high - e_low) / nb
    widths = np.zeros(nb, dtype=np.int64)
    prev_freq = 0
    freq_over = 0
    for i in range(1, nb + 1):
        f = c2 * (np.exp((e_low + i * step) / c1) - 1.0)
        fb = int(np.floor(f / freq_width + 0.5))
        nbf = fb - prev_freq - freq_over
        if nbf < min_nf:
            freq_over = min_nf - nbf
            nbf = min_nf
        else:
            freq_over = 0
        widths[i - 1] = nbf
        prev_freq = fb
    widths[-1] += 1
    too_large = int(widths.sum() - (fft // 2 + 1))
    if too_large > 0:
        widths[-1] -= too_large
    return widths


_WIDTHS64 = _erb_widths_f64()
assert _WIDTHS64.sum() == N_FREQ
assert list(_WIDTHS) == list(_WIDTHS64), (
    f"ERB widths differ f32 vs f64: {_WIDTHS} vs {_WIDTHS64}")
print(f"[dfn48] ERB widths ok: {list(_WIDTHS)}")
_WINDOW = _vorbis_window()


class _DfFeats:
    """Streaming DF front-end: overlap mems + running norms (lib.rs)."""

    def __init__(self):
        self.widths = _WIDTHS
        self.win = _WINDOW
        self.reset()

    def reset(self):
        self.a_mem = np.zeros(HOP, dtype=np.float32)
        self.s_mem = np.zeros(HOP, dtype=np.float32)
        self.mean_st = _linspace_f32(-60.0, -90.0, NB_ERB)
        self.unit_st = _linspace_f32(0.001, 0.0001, NB_DF)

    def analyze(self, frame):
        """160 float samples -> 161-bin complex spectrum."""
        frame = np.asarray(frame, dtype=np.float32)
        buf = np.empty(FFT, dtype=np.float32)
        buf[:HOP] = self.a_mem * self.win[:HOP]
        buf[HOP:] = frame * self.win[HOP:]
        self.a_mem[:] = frame
        return (np.fft.rfft(buf) * _WNORM).astype(np.complex64)

    def synthesize(self, spec):
        """161-bin complex spectrum -> 160 float samples."""
        # realfft's inverse (what the runtime uses) is UNSCALED, and
        # wnorm=1/320 on analysis exactly compensates its x320 gain
        # (roundtrip unity — verified by energy, NOT by correlation:
        # correlation is gain-blind). numpy's irfft divides by N, so
        # compensate explicitly — without this the entire output sits
        # ~50 dB too low (found via 125x energy gap vs the DLL).
        td = (np.fft.irfft(np.asarray(spec), n=FFT).astype(np.float32)
              * np.float32(FFT))
        td *= self.win
        out = td[:HOP] + self.s_mem
        self.s_mem[:] = td[HOP:]
        return out.astype(np.float32, copy=False)

    def erb_feats(self, spec):
        """161-bin spectrum -> 32 ERB features (state updating).

        Band power MEAN, 10*log10(+1e-10), then band_mean_norm_erb
        against the UPDATED state, /40 (two roundings, as Rust).
        """
        p = np.empty(NB_ERB, dtype=np.float32)
        o = 0
        for b, w in enumerate(self.widths):
            seg = spec[o:o + w]
            # Per-bin (re^2+im^2)/N accumulated, like the Rust loop.
            p[b] = ((seg.real * seg.real + seg.imag * seg.imag)
                    * (np.float32(1.0) / np.float32(w))).sum(
                        dtype=np.float32)
            o += w
        db = np.log10(p + np.float32(1e-10)).astype(np.float32)
        db *= np.float32(10.0)
        s = self.mean_st
        s *= ALPHA
        s += db * _ONE_M_ALPHA
        out = db - s
        out /= np.float32(40.0)
        return out.astype(np.float32, copy=False)

    def norm_spec(self, spec):
        """Unit-norm first-64-bin spectrum (state updating).

        Returns complex64[64]; the caller arranges real/imag layout
        per the export's declared input.
        """
        seg = np.asarray(spec[:NB_DF])
        mag = np.sqrt(seg.real * seg.real
                      + seg.imag * seg.imag).astype(np.float32)
        s = self.unit_st
        s *= ALPHA
        s += mag * _ONE_M_ALPHA
        return (seg / np.sqrt(s)).astype(np.complex64)


def _is_state_name(name):
    n = name.lower()
    return ("state" in n or "hidden" in n or n.startswith("h_")
            or n.startswith("h.") or "/h" in n or ".h_" in n
            or n.endswith("_h") or "gru" in n or "lstm" in n
            or "cell" in n)


def _is_time_dim(d):
    return d is None or isinstance(d, str) or d < 0


def _role_hit(name, role):
    """Role fragment as a standalone token (not inside another word).

    Guards: 'e2' must not match 'layer20', 'm' must not match
    'Sigmoidm_dim_0' — but full names ('e2', 'lsnr', 'coefs',
    'feat_erb') always match.
    """
    return re.search(r"(?:^|[^a-z0-9])" + re.escape(role)
                     + r"(?:[^a-z0-9]|$)", name.lower()) is not None


def _fixed_total(shape):
    """Product of dims with dynamic/time dims counted as 1.

    Fixed time dims != 1 stay in the product (their totals can never
    match a single-frame role — that mismatch fails closed later).
    """
    total = 1
    for dd in shape:
        if not _is_time_dim(dd):
            total *= int(dd)
    return total


def _concrete_shape(shape):
    """Feed shape: dynamic dims -> 1 (streaming, one frame)."""
    return tuple(1 if _is_time_dim(d) else int(d) for d in shape)


def _vi_shape(vi):
    """ValueInfoProto shape -> tuple (ints, symbolic params, or -1)."""
    dims = []
    try:
        shape = vi.type.tensor_type.shape
    except Exception:
        return ()
    for d in shape.dim:
        if d.HasField("dim_value"):
            dims.append(d.dim_value)
        elif d.dim_param:
            dims.append(d.dim_param)
        else:
            dims.append(-1)
    return tuple(dims)


class _OnnxNet:
    """One .onnx file: graph proto, session, carried GRU states."""

    def __init__(self, path):
        if ort is None:
            raise RuntimeError("onnxruntime is not installed "
                               "(pip install -r web/requirements-engines.txt)")
        try:
            import onnx
        except ImportError:
            raise RuntimeError("onnx is not installed (GRU graph "
                               "inspection — pip install -r "
                               "web/requirements-engines.txt)")
        self._onnx = onnx
        self.proto = onnx.load(path)
        self._path = path
        self._sess = None
        self.ins = []
        self.outs = []
        self.states = {}  # h_in name -> {shape, out, arr}

    def proto_io(self):
        """I/O listing straight from the graph (pre/post-surgery).

        Initializers listed as inputs are excluded (they're weights,
        not feeds). Shapes use dim values, symbolic params, or -1.
        """
        inits = {i.name for i in self.proto.graph.initializer}
        self.ins = [
            (i.name, _vi_shape(i), f"tensor({i.type.tensor_type.elem_type})")
            for i in self.proto.graph.input if i.name not in inits]
        self.outs = [
            (o.name, _vi_shape(o), f"tensor({o.type.tensor_type.elem_type})")
            for o in self.proto.graph.output]

    def build_session(self):
        """Session from the (possibly rewired) graph.

        Default is BASIC optimization + no prepacking: that is the
        validated configuration (all parity numbers were measured
        with it). DFN48_ORT_FULL=1 opts into full optimization
        (faster, different fusion — re-validate parity if set).
        """
        import io
        from onnxruntime import GraphOptimizationLevel
        full = bool(os.environ.get("DFN48_ORT_FULL"))
        opts = ort.SessionOptions()
        opts.intra_op_num_threads = 1
        opts.inter_op_num_threads = 1
        if full:
            opts.graph_optimization_level = (
                GraphOptimizationLevel.ORT_ENABLE_ALL)
        else:
            opts.graph_optimization_level = (
                GraphOptimizationLevel.ORT_ENABLE_BASIC)
            try:
                opts.add_session_config_entry(
                    "session.disable_prepacking", "1")
            except Exception:
                pass
        print(f"[dfn48] optimizer: "
              f"{'FULL' if full else 'BASIC+no-prepack'}")
        buf = io.BytesIO()
        buf.write(self.proto.SerializeToString())
        self._sess = ort.InferenceSession(
            buf.getvalue(), sess_options=opts,
            providers=["CPUExecutionProvider"])
        self.ins = [(i.name, tuple(i.shape), i.type)
                    for i in self._sess.get_inputs()]
        self.outs = [(o.name, tuple(o.shape), o.type)
                     for o in self._sess.get_outputs()]

    def spec_lines(self, tag):
        lines = [f"[{tag}]"]
        for name, shape, typ in self.ins:
            lines.append(f"  in  {name} {shape} {typ}")
        for name, shape, typ in self.outs:
            lines.append(f"  out {name} {shape} {typ}")
        return lines


class Dfn48Core:
    """Streaming DFN48 over onnxruntime (tract.rs port). NOT thread-safe."""

    def __init__(self, bundle_path, atten_lim_db=None):
        self.last_error = ""
        self.in_rms = 0.0
        self.out_rms = 0.0
        # Attenuation limit (dB), parity with the DLL session param
        # (weya_nc_session_create atten). Caps max suppression: ERB
        # gains floor at 10^(-atten/20), and the zero-gate emits the
        # floor instead of silence. Default 18: caps suppression at
        # a natural noise floor (100 = unlimited, too aggressive).
        # DFN48_ATTEN_DB overrides.
        if atten_lim_db is None:
            try:
                atten_lim_db = float(
                    os.environ.get("DFN48_ATTEN_DB", "18.0"))
            except ValueError:
                atten_lim_db = 18.0
        self.atten_lim_db = float(atten_lim_db)
        self._gain_floor = np.float32(
            10.0 ** (-self.atten_lim_db / 20.0))
        self._tmp = tempfile.TemporaryDirectory(prefix="dfn48_")
        with tarfile.open(bundle_path, "r:gz") as tar:
            names = tar.getnames()
            parts = [n for n in names if n.endswith(
                ("enc.onnx", "erb_dec.onnx", "df_dec.onnx"))]
            if len(parts) != 3:
                raise RuntimeError(
                    f"bundle missing onnx parts ({names})")
            tar.extractall(
                self._tmp.name,
                # filter= needs 3.12+ (PEP 706); the tarball is flat
                # upstream output either way.
                **({"filter": "data"} if sys.version_info >= (3, 12)
                   else {}),
                members=[tar.getmember(n) for n in parts])
        d = self._tmp.name

        def _part(suffix):
            return os.path.join(
                d, [n for n in parts if n.endswith(suffix)][0])

        self.enc = _OnnxNet(_part("enc.onnx"))
        self.erb = _OnnxNet(_part("erb_dec.onnx"))
        self.df = _OnnxNet(_part("df_dec.onnx"))
        # Data mapping runs on the pristine graphs (no state tensors
        # in I/O at all); state surgery rewires protos next; sessions
        # build last so they see the final graphs.
        for net in (self.enc, self.erb, self.df):
            net.proto_io()
        self._map_all()
        for net in (self.enc, self.erb, self.df):
            net.build_session()
            net.proto_io()
        self._dump = "\n".join(
            self.enc.spec_lines("enc")
            + self.erb.spec_lines("erb_dec")
            + self.df.spec_lines("df_dec"))
        # Load-time proof the spec layout is decidable (raises here,
        # not mid-call, when ambiguous).
        self._arrange_spec(np.zeros(NB_DF, np.complex64),
                           self._enc_in["spec"][1])
        self._feats = _DfFeats()
        self._rolling = [np.zeros(N_FREQ, dtype=np.complex64)
                         for _ in range(DF_ORDER)]
        self._in_carry = np.zeros(0, dtype=np.float32)
        self._out_carry = np.zeros(0, dtype=np.float32)
        self._la_carry = []   # lookahead delay line (frames)
        self.ready = True
        # Frame stats (probe/diagnostic only — gated, never production).
        self._trace_lsnr = False
        self._lsnr_trace = []
        self._collect_stats = False
        self._stat_frames = []

    def spec_dump(self):
        return self._dump

    # -- I/O mapping (probed, strict; fail closed with the dump) --

    _TRIPLE = ("e2", "e3", "emb")  # all 128/frame; split separately

    def _assign_roles(self, tagged, roles, tag, what, e3_first):
        """Map {role: (name, shape)}: token names, exact totals, triple.

        tagged: [(name, shape)] in net order. roles: {role: want}.
        State-smelling names are never claimed. e2/e3/emb go through
        _split_128 when still missing afterwards. Anything missing
        or ambiguous fails closed with the dump.
        """
        cands = [(n, s) for n, s in tagged if not _is_state_name(n)]
        data = {}
        for name, shape in cands:  # token names first
            for role in roles:
                if _role_hit(name, role) and role not in data:
                    data[role] = (name, shape)
                    break
        for name, shape in cands:  # exact totals (triple excluded)
            if any(name == f[0] for f in data.values()):
                continue
            total = _fixed_total(shape)
            for role, want in roles.items():
                if role in self._TRIPLE:
                    continue
                if total == want and role not in data:
                    data[role] = (name, shape)
                    break
        missing3 = [r for r in self._TRIPLE
                    if r in roles and r not in data]
        if missing3:
            claimed = {f[0] for f in data.values()}
            bucket = [(n, s) for n, s in cands
                      if n not in claimed and _fixed_total(s) == 128]
            if set(missing3) == set(self._TRIPLE) and len(bucket) == 3:
                e2, e3, emb = self._split_128(
                    bucket, tag, what, e3_first)
                data.update({"e2": e2, "e3": e3, "emb": emb})
            elif len(missing3) == 1 and len(bucket) == 1:
                (only,), ((bn, bs),) = missing3, bucket
                if (only == "emb") != (len(bs) == 3):
                    raise RuntimeError(
                        f"{tag}: '{only}' rank mismatch ({bn} {bs}) "
                        f"for {what}:\n{self._dump}")
                data[only] = (bn, bs)
            else:
                raise RuntimeError(
                    f"{tag}: roles {missing3} unresolvable "
                    f"(bucket {bucket}) for {what}:\n{self._dump}")
        missing = [r for r in roles if r not in data]
        if missing:
            raise RuntimeError(
                f"{tag}: roles {missing} unrecognized for {what}:\n"
                f"{self._dump}\n(paste this dump back — mapping needs "
                "a one-line fix)")
        return data

    def _split_128(self, bucket, tag, what, e3_first):
        """Split a 128-total bucket into (e2, e3, emb).

        e2/e3 are 4D [...,16,.,8], emb is 3D: rank splits emb off.
        e2 vs e3 (identical shapes) go by name; else list order with
        a documented assumption (decoder INPUTS: e3 precedes e2 per
        torch forward-arg order; encoder OUTPUTS: e2 precedes e3 per
        return order), printed LOUD — the DLL parity check validates
        numerically (a swap shows as a large diff, never silent).
        """
        fours = [c for c in bucket if len(c[1]) == 4]
        threes = [c for c in bucket if len(c[1]) == 3]
        others = [c for c in bucket
                  if len(c[1]) not in (3, 4)]
        if others or len(threes) != 1 or len(fours) != 2:
            raise RuntimeError(
                f"{tag}: 128-bucket {bucket} is not "
                f"(e2, e3, emb) for {what}:\n{self._dump}")
        emb = threes[0]
        (a, b) = fours
        na, nb = a[0].lower(), b[0].lower()
        if "e2" in na and "e3" in nb:
            e2, e3 = a, b
        elif "e2" in nb and "e3" in na:
            e2, e3 = b, a
        else:
            (e3, e2) = (a, b) if e3_first else (b, a)
            print(f"[dfn48] {tag}: e2/e3 assumed list order "
                  f"({e3[0]} / {e2[0]}) for {what} — parity check decides")
        return e2, e3, emb

    def _map_all(self):
        self._enc_in = self._assign_roles(
            [(n, s) for n, s, _t in self.enc.ins],
            {"erb": 32, "spec": 128}, "enc", "inputs", e3_first=True)
        self._enc_out = self._assign_roles(
            [(n, s) for n, s, _t in self.enc.outs],
            {"c0": 1024, "e0": 512, "e1": 256, "e2": 128, "e3": 128,
             "emb": 128, "lsnr": 1},
            "enc", "outputs", e3_first=False)
        self._discover_states(self.enc, "enc")
        # Decoders: unique totals mapped directly, 128-bucket split.
        self._erb_in, self._erb_out = self._map_decoder(
            self.erb, "erb_dec",
            {"emb": 128, "e3": 128, "e2": 128, "e1": 256, "e0": 512},
            ("m", 32))
        self._df_in, self._df_out = self._map_decoder(
            self.df, "df_dec", {"emb": 128, "c0": 1024},
            ("coefs", NB_DF * DF_ORDER * 2))

    def _map_decoder(self, net, tag, in_roles, out_role):
        data = self._assign_roles(
            [(n, s) for n, s, _t in net.ins],
            dict(in_roles), tag, "inputs", e3_first=True)
        oname, owant = out_role
        out = self._assign_roles(
            [(n, s) for n, s, _t in net.outs],
            {oname: owant}, tag, "outputs", e3_first=False)
        self._discover_states(net, tag)
        return data, out[oname]

    def _discover_states(self, net, tag):
        """GRU/LSTM carry via graph inspection (NOT I/O probing).

        The exports carry no state tensors in I/O (recurrence lives
        inside, pulsed by tract) — so per-frame carry overrides each
        recurrent node's initial-state initializer, or rewires a
        node-fed state edge to a declared input, and fetches the
        state-output intermediate. All mainstream onnxruntime
        features. Anything else fails closed.
        """
        from collections import Counter
        from onnx import TensorProto, checker, helper, shape_inference
        proto = net.proto
        hist = Counter(n.op_type for n in proto.graph.node)
        print(f"[dfn48] {tag}: ops " +
              ", ".join(f"{k}×{v}" for k, v in sorted(hist.items())))
        try:
            inferred = shape_inference.infer_shapes(proto)
        except Exception:
            inferred = None  # best-effort only; R-dims below are primary
        types = {}
        if inferred is not None:
            for vi in (list(inferred.graph.value_info)
                       + list(inferred.graph.output)):
                if vi.type.HasField("tensor_type"):
                    tt = vi.type.tensor_type
                    dims = []
                    for d in tt.shape.dim:
                        if d.HasField("dim_value"):
                            dims.append(d.dim_value)
                        elif d.dim_param:
                            dims.append(d.dim_param)
                        else:
                            dims.append(-1)
                    types[vi.name] = tuple(dims)
        inits = {}
        for init in proto.graph.initializer:
            inits[init.name] = tuple(init.dims)

        # Some exports (the upstream DF3 48 kHz bundle) keep the GRU/LSTM
        # weight matrices in a Constant NODE instead of an initializer,
        # so the R-dims have to be read off that node's value tensor.
        const_dims = {}
        for nd in proto.graph.node:
            if nd.op_type != "Constant":
                continue
            for attr in nd.attribute:
                if attr.name == "value" and attr.HasField("t"):
                    const_dims[nd.output[0]] = tuple(attr.t.dims)

        def _rdims_of(name):
            if not name:
                return None
            if name in inits:
                return inits[name]
            return const_dims.get(name)

        pairs = {}
        for node in proto.graph.node:
            if node.op_type not in ("GRU", "LSTM"):
                continue
            ins, outs = list(node.input), list(node.output)
            direction = "forward"
            for attr in node.attribute:
                if attr.name == "direction":
                    s = attr.s
                    direction = (s.decode() if isinstance(s, bytes)
                                 else str(s))
            if direction != "forward":
                raise RuntimeError(
                    f"{tag}: non-forward {node.op_type} "
                    f"('{node.name}', {direction}) — streaming port "
                    "can't run it")
            h_in = ins[5] if len(ins) > 5 else ""
            h_out = outs[1] if len(outs) > 1 else ""
            extra = []
            if node.op_type == "LSTM":
                c_in = ins[6] if len(ins) > 6 else ""
                c_out = outs[2] if len(outs) > 2 else ""
                extra.append((c_in, c_out))
            for hin, hout in [(h_in, h_out)] + extra:
                if not hin or not hout:
                    raise RuntimeError(
                        f"{tag}: {node.op_type} '{node.name}' without "
                        f"carryable state (in={ins} out={outs})")
                if hin in pairs:
                    raise RuntimeError(
                        f"{tag}: state '{hin}' shared by two nodes")
                if hin in inits:
                    shape = inits[hin]
                    feed_name = hin
                    how = "initializer override"
                else:
                    # State fed by another node (here: ConstantOfShape
                    # zeros, whose shape inference leaves symbolic).
                    # Derive it from the recurrence matrix R instead:
                    # GRU/LSTM R is [D, gates*H, H], so h/c are
                    # [D, 1, H] (batch is fixed 1 in these exports,
                    # like every data input). Then rewire that edge
                    # to a new declared input and declare the state
                    # output. Dangling producer nodes are legal.
                    rdims = _rdims_of(ins[2] if len(ins) > 2 else "")
                    if rdims is None or len(rdims) != 3 or rdims[0] < 1:
                        raise RuntimeError(
                            f"{tag}: cannot derive state shape for "
                            f"'{hin}' (R dims {rdims})")
                    shape = (rdims[0], 1, rdims[2])
                    tshape = types.get(hin)
                    if tshape is not None and all(
                            isinstance(d, int) and d > 0 for d in tshape):
                        if tuple(tshape) != tuple(shape):
                            raise RuntimeError(
                                f"{tag}: state '{hin}' shape conflict: "
                                f"R says {tuple(shape)}, inference says "
                                f"{tuple(tshape)}")
                    feed_name = f"hush_state_{tag}_{len(pairs)}"
                    renamed = False
                    for nd in net.proto.graph.node:
                        for i, nm in enumerate(nd.input):
                            if nm == hin:
                                nd.input[i] = feed_name
                                renamed = True
                    if not renamed:
                        raise RuntimeError(
                            f"{tag}: state edge '{hin}' not found")
                    net.proto.graph.input.append(
                        helper.make_tensor_value_info(
                            feed_name, TensorProto.FLOAT, list(shape)))
                    net.proto.graph.output.append(
                        helper.make_tensor_value_info(
                            hout, TensorProto.FLOAT, list(shape)))
                    how = f"rewired to {feed_name}"
                pairs[feed_name] = {"shape": shape, "out": hout,
                                    "arr": np.zeros(shape, dtype=np.float32)}
                print(f"[dfn48] {tag}: carry {hin} "
                      f"{tuple(shape)} -> {hout} ({how})")
                # Prove the rewire took (protobuf repeated-field
                # assignment is silent on failure modes we can't see):
                wired = [nm for nd in net.proto.graph.node
                         for nm in nd.input].count(feed_name)
                print(f"[dfn48] {tag}: '{feed_name}' feeds "
                      f"{wired} edge(s)")
        try:
            checker.check_model(net.proto)
        except Exception as e:
            raise RuntimeError(f"{tag}: rewired graph invalid ({e})")
        net.states = pairs

    # -- streaming (tract.rs process()) --

    def reset(self):
        self._feats.reset()
        self._rolling = [np.zeros(N_FREQ, dtype=np.complex64)
                         for _ in range(DF_ORDER)]
        self._in_carry = np.zeros(0, dtype=np.float32)
        self._out_carry = np.zeros(0, dtype=np.float32)
        self._la_carry = []   # lookahead delay line (frames)
        self.in_rms = self.out_rms = 0.0
        self._lsnr_trace = []
        for net in (self.enc, self.erb, self.df):
            for st in net.states.values():
                st["arr"].fill(0)

    def _run_frame(self, frame):
        """One native 480-sample float frame in/out."""
        frame = np.asarray(frame, dtype=np.float32)
        if float(np.mean(frame * frame)) < float(_SILENCE_MS):
            # Runtime silence shortcut: states freeze, zeros out
            # (tract returns lsnr -15 there — record the same).
            if self._trace_lsnr:
                self._lsnr_trace.append(-15.0)
            return np.zeros(HOP, dtype=np.float32)
        spec = self._feats.analyze(frame)
        self._rolling.append(spec.copy())
        del self._rolling[0]
        erb = self._feats.erb_feats(spec)
        nspec = self._feats.norm_spec(spec)
        lsnr, mask, coefs = self._infer(erb, nspec)
        if self._collect_stats:
            self._stat_frames.append(
                (lsnr, float(np.mean(mask)),
                 float(np.mean(np.abs(coefs)))))
        if self._trace_lsnr:
            self._lsnr_trace.append(lsnr)
        if lsnr < float(MIN_DB):
            # Noise only: floor the mask instead of zeroing when an
            # atten limit is set (keeps a noise floor instead of
            # gating to silence); unlimited (100 dB) still zeros.
            gains = np.full(NB_ERB, self._gain_floor, dtype=np.float32)
            coefs = None
        elif lsnr > float(MAX_ERB_DB):
            return frame.copy()  # clean: bypass everything
        else:
            gains = np.maximum(mask, self._gain_floor).astype(np.float32)
            if lsnr > float(MAX_DF_DB):
                coefs = None
        # ERB gains onto the newest spec (rectangular bands).
        enh = self._rolling[-1].copy()
        o = 0
        for b, w in enumerate(self._feats.widths):
            enh[o:o + w] *= gains[b]
            o += w
        if coefs is not None:
            # Order-5 deep filter over the last 5 noisy specs:
            # out[f] = sum_o X[t-4+o][f] * C[o][f], bins < 64.
            acc = np.zeros(NB_DF, dtype=np.complex64)
            for oo in range(DF_ORDER):
                acc += self._rolling[oo][:NB_DF] * coefs[:, oo]
            enh[:NB_DF] = acc
        return self._feats.synthesize(enh)

    def _feed(self, net, data):
        """data: {in_name: array} -> {out_name: array} (states carried)."""
        feed = {}
        for name, arr in data.items():
            in_shape = next(s for n, s, _t in net.ins if n == name)
            feed[name] = np.asarray(arr, dtype=np.float32).reshape(
                _concrete_shape(in_shape))
        for iname, st in net.states.items():
            feed[iname] = st["arr"]
        outs = ([n for n, _s, _t in net.outs]
                + [st["out"] for st in net.states.values()])
        res = net._sess.run(outs, feed)
        got = dict(zip(outs, res))
        for _iname, st in net.states.items():
            back = np.asarray(got[st["out"]])
            if not st.get("_shape_logged"):
                # Definitive shape check, once: a Y_h whose shape
                # isn't what we feed back would corrupt carry
                # silently (feed matches our own declaration by
                # construction, so nothing else can catch it).
                print(f"[dfn48] state {st['out']} "
                      f"fetched {back.shape} fed {st['arr'].shape}")
                st["_shape_logged"] = True
            st["arr"] = np.asarray(back, dtype=np.float32)
        return got

    def _enc_raw(self, erb, nspec):
        """Encoder only: feats in, raw output dict out (probe helper).

        Advances GRU/norm states like any frame (not side-effect
        free) — callers re-warm around it.
        """
        iname = self._enc_in["erb"][0]
        ishape = self._enc_in["erb"][1]
        efeed = {iname: np.asarray(erb, dtype=np.float32).reshape(
            _concrete_shape(ishape))}
        iname = self._enc_in["spec"][0]
        ishape = self._enc_in["spec"][1]
        efeed[iname] = self._arrange_spec(nspec, ishape)
        return self._feed(self.enc, efeed)

    def _infer(self, erb, nspec):
        # Encoder: erb[32] + spec[128] (+ carried states).
        iname = self._enc_in["erb"][0]
        ishape = self._enc_in["erb"][1]
        efeed = {iname: np.asarray(erb, dtype=np.float32).reshape(
            _concrete_shape(ishape))}
        iname = self._enc_in["spec"][0]
        ishape = self._enc_in["spec"][1]
        efeed[iname] = self._arrange_spec(nspec, ishape)
        got = self._feed(self.enc, efeed)
        ls = np.asarray(got[self._enc_out["lsnr"][0]])
        if ls.size != 1:
            # The DLL reads a scalar; any extra elements mean the
            # wrong tensor is mapped (which element would be a guess).
            raise RuntimeError(
                f"lsnr tensor shape {ls.shape} is not scalar")
        lsnr = float(ls.ravel()[0])
        parts = {}
        for role in ("e0", "e1", "e2", "e3", "emb", "c0"):
            n, _s = self._enc_out[role]
            parts[role] = np.asarray(got[n])
        # ERB decoder -> mask[32].
        feed = {}
        for role in ("emb", "e3", "e2", "e1", "e0"):
            n, s = self._erb_in[role]
            feed[n] = np.asarray(parts[role]).reshape(
                _concrete_shape(s)).astype(np.float32)
        got = self._feed(self.erb, feed)
        moname = self._erb_out[0]
        mask = np.asarray(got[moname], dtype=np.float32).ravel()
        if mask.size != NB_ERB:
            raise RuntimeError(f"erb mask size {mask.size} != 32")
        # DF decoder -> coefs[NB_DF,5,2] complex.
        feed = {}
        for role in ("emb", "c0"):
            n, s = self._df_in[role]
            feed[n] = np.asarray(parts[role]).reshape(
                _concrete_shape(s)).astype(np.float32)
        got = self._feed(self.df, feed)
        coname = self._df_out[0]
        coefs = self._to_df_coefs(np.asarray(got[coname], dtype=np.float32),
                                 coname)
        return lsnr, mask.astype(np.float32), coefs

    @staticmethod
    def _to_df_coefs(co, coname):
        """Flat coefs -> complex64[NB_DF,5] (order-major complex pairs).

        Drops size-1 dims; requires the NB_DF-bin axis plus a
        remainder multiplying to 10 — either (5,2) in order or a
        single merged 10-dim (torch DfOutputReshapeMF splits it
        2-minor, same as the runtime's [C,N,F,2]). Anything else
        fails closed — a transposed filter sounds almost-right.
        """
        _ = coname
        dims = tuple(d for d in co.shape if d != 1)
        ibin = [i for i, d in enumerate(dims) if d == NB_DF]
        if len(ibin) != 1 or co.size != NB_DF * DF_ORDER * 2:
            raise RuntimeError(
                f"df coefs shape {co.shape} is not a "
                f"({NB_DF},{DF_ORDER},2) permutation")
        order = [ibin[0]] + [j for j in range(len(dims)) if j != ibin[0]]
        t = co.reshape(dims).transpose(order)
        if t.shape[1:] == (5, 2):
            pass
        elif t.shape[1:] == (10,):
            t = t.reshape(NB_DF, DF_ORDER, 2)
        else:
            raise RuntimeError(
                f"df coefs remainder {t.shape[1:]} is not (5,2)/(10,)")
        return (t[:, :, 0] + 1j * t[:, :, 1]).astype(np.complex64)

    def _arrange_spec(self, nspec, ishape):
        """Normalized complex64[64] -> the export's declared layout.

        [.,2,.,64] (or any shape with a non-trailing 2): real plane
        then imag plane. [...,64,2]: interleaved. Rank-1 [128]:
        refused (layout unknowable blind — dump guides the fix).
        """
        shape = _concrete_shape(ishape)
        total = 1
        for d in shape:
            total *= d
        if total != NB_DF * 2:
            raise RuntimeError(f"spec input shape {ishape} != 128 values")
        dims = list(shape)
        arr = np.empty(shape, dtype=np.float32)
        flat = arr.ravel()
        if 2 in dims and dims.index(2) != len(dims) - 1:
            flat[:NB_DF] = nspec.real
            flat[NB_DF:] = nspec.imag
            return arr
        if dims and dims[-1] == 2:
            flat[0::2] = nspec.real
            flat[1::2] = nspec.imag
            return arr
        raise RuntimeError(
            f"spec input layout ambiguous {ishape}:\n{self._dump}")

    def process(self, frame_f32):
        """Float in -> float out (same length). Never mutes."""
        frame = np.asarray(frame_f32, dtype=np.float32)
        n = len(frame)
        self.in_rms = rms(frame)
        if not self.ready:
            self.out_rms = self.in_rms
            return frame.copy()
        try:
            if n:
                self._in_carry = np.concatenate([self._in_carry, frame])
            while len(self._in_carry) >= HOP:
                blk = self._run_frame(self._in_carry[:HOP])
                self._in_carry = self._in_carry[HOP:]
                # The model is non-causal (lookahead 2): its output for
                # frame t is only final once frame t+2 has been seen,
                # so the stream is delayed by LOOKAHEAD_FRAMES frames
                # to keep the output aligned with the input.
                self._la_carry.append(blk)
                if len(self._la_carry) > LOOKAHEAD_FRAMES:
                    ready = self._la_carry.pop(0)
                    if len(self._out_carry):
                        self._out_carry = np.concatenate(
                            [self._out_carry, ready])
                    else:
                        self._out_carry = ready
            out = np.empty(n, dtype=np.float32)
            take = min(len(self._out_carry), n)
            if take:
                out[:take] = self._out_carry[:take]
                self._out_carry = self._out_carry[take:]
            if take < n:
                # Priming: not enough denoised audio yet — passthrough
                # for the shortfall (DtlnNs.process convention).
                out[take:] = frame[take:]
            self.out_rms = rms(out)
            self.last_error = ""
            return out
        except Exception as e:
            # Loud passthrough: latch the error into stats (the UI
            # err line shows ns.error) instead of failing silently
            # every frame. DFN48_DEBUG=1 also dumps the traceback
            # (probe runs, never production).
            import traceback
            self.last_error = f"dfn48 frame failed: {e}"
            if os.environ.get("DFN48_DEBUG"):
                traceback.print_exc()
            self.out_rms = self.in_rms
            return frame.copy()

    def stats(self):
        return {
            "backend": 2 if self.ready else 0,
            "rCount": int(len(self._out_carry)),
            "inRms": self.in_rms,
            "outRms": self.out_rms,
            "dropped": 0,
            "error": self.last_error,
        }

    def close(self):
        try:
            self._tmp.cleanup()
        except Exception:
            pass

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass
