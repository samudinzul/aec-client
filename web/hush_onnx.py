"""Hush via onnxruntime — DTLN-NS-style backend, no DLL needed.

Runs the shipped ONNX bundle (enc/erb_dec/df_dec) through the
mainstream onnxruntime wheel (requirements-engines.txt, same as NKF:
CPUExecutionProvider only, 1/1 threads) with a NumPy port of the
DeepFilterNet streaming runtime. When this works, no user-supplied
files are needed at all; the ctypes path in web/hush.py stays as
fallback, tried second by the HushNs dispatcher.

Math sources (read all three before touching this file):
- Hush model/dfnet_se.py — network topology + training features.
  WARNING: its norms (ones-init, /sqrt), Hann window, and centered
  STFT describe TRAINING, not inference. The runtime differs.
- vendor/deep_filter/src/lib.rs — the RUNTIME ground truth:
  Vorbis window both sides, exact ERB widths, wnorm 1/320,
  frame_analysis/synthesis overlap mems, band_mean_norm_erb
  ((x-s)/40, linspace(-60,-90) init), band_unit_norm (x/sqrt(s),
  linspace(.001,.0001) init).
- vendor/deep_filter/src/tract.rs — the RUNTIME forward: LSNR
  stage gating (-15/35/35), rectangular ERB gain expansion,
  order-5 DF over the last 5 noisy specs (bins < 64; bins >= 64
  keep the ERB-masked current spec), atten/post-filter skipped
  (weya passes post_filter=false, atten 100 = unlimited).

Deliberate runtime-matching choices:
- Vorbis window (f64 sin, cast f32), wnorm 1/320 on analysis.
- alpha 0.99 exactly (calc_norm_alpha rounds exp(-0.01) down).
- ERB widths computed in float32 with round-half-away, mirroring
  f32 Rust (numpy defaults are float64/banker's — both wrong here).
- Silence shortcut on mean(x^2) < 1e-7 BEFORE analysis (norm
  states freeze, like the runtime's early return).
- GRU states carried explicitly per invoke, paired in/out by size.
- ONNX I/O mapped by probing (names first, then size+rank, then
  documented torch-forward-order position) — anything ambiguous
  fails closed WITH the probed spec dump, never guessed.

Fail-closed everywhere except the sample loop (which fails open
like every other stage). Never mutes.
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

SR = 16000
FFT = 320
HOP = 160
NB_ERB = 32
NB_DF = 64
DF_ORDER = 5
N_FREQ = FFT // 2 + 1  # 161
# calc_norm_alpha(16000, 160, 1.0): exp(-0.01) rounded down until
# < 1.0 -> 0.99 exactly (f32). NOT torch's get_norm_alpha (0.99005).
ALPHA = np.float32(0.99)
_ONE_M_ALPHA = np.float32(1.0) - ALPHA
_WNORM = np.float32(1.0 / 320.0)
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
        td = np.fft.irfft(np.asarray(spec), n=FFT).astype(np.float32)
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


class _OnnxNet:
    """One .onnx file: session + graph proto + carried GRU states."""

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
        opts = ort.SessionOptions()
        opts.intra_op_num_threads = 1
        opts.inter_op_num_threads = 1
        opts.graph_optimization_level = (
            ort.GraphOptimizationLevel.ORT_ENABLE_ALL)
        self._sess = ort.InferenceSession(
            path, sess_options=opts, providers=["CPUExecutionProvider"])
        self.ins = [(i.name, tuple(i.shape), i.type)
                    for i in self._sess.get_inputs()]
        self.outs = [(o.name, tuple(o.shape), o.type)
                     for o in self._sess.get_outputs()]
        self.proto = onnx.load(path)
        self.states = {}  # h_in name -> {shape, out, arr}

    def spec_lines(self, tag):
        lines = [f"[{tag}]"]
        for name, shape, typ in self.ins:
            lines.append(f"  in  {name} {shape} {typ}")
        for name, shape, typ in self.outs:
            lines.append(f"  out {name} {shape} {typ}")
        return lines


class HushOnnxCore:
    """Streaming Hush over onnxruntime (tract.rs port). NOT thread-safe."""

    def __init__(self, bundle_path):
        self.last_error = ""
        self.in_rms = 0.0
        self.out_rms = 0.0
        self._tmp = tempfile.TemporaryDirectory(prefix="hush_onnx_")
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
        self._dump = "\n".join(
            self.enc.spec_lines("enc")
            + self.erb.spec_lines("erb_dec")
            + self.df.spec_lines("df_dec"))
        self._map_all()
        self._feats = _DfFeats()
        self._rolling = [np.zeros(N_FREQ, dtype=np.complex64)
                         for _ in range(DF_ORDER)]
        self._in_carry = np.zeros(0, dtype=np.float32)
        self._out_carry = np.zeros(0, dtype=np.float32)
        self.ready = True

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
            print(f"[hush_onnx] {tag}: e2/e3 assumed list order "
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
        # Load-time proof the spec layout is decidable (raises here,
        # not mid-call, when ambiguous).
        self._arrange_spec(np.zeros(NB_DF, np.complex64),
                           self._enc_in["spec"][1])
        print(self._dump)

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
        recurrent node's initial-state initializer and fetches its
        state-output intermediate. Both are mainstream onnxruntime
        features (feed-by-initializer-name, fetch-intermediate).
        Anything else (no recurrent nodes is fine — pure conv;
        missing initializers, non-forward direction) fails closed.
        """
        from collections import Counter
        proto = net.proto
        hist = Counter(n.op_type for n in proto.graph.node)
        print(f"[hush_onnx] {tag}: top ops " +
              ", ".join(f"{k}×{v}" for k, v in hist.most_common(8)))
        inits = {}
        for init in proto.graph.initializer:
            inits[init.name] = tuple(init.dims)
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
                if hin not in inits:
                    raise RuntimeError(
                        f"{tag}: state '{hin}' is not an overridable "
                        "initializer")
                if hin in pairs:
                    raise RuntimeError(
                        f"{tag}: state '{hin}' shared by two nodes")
                pairs[hin] = {"shape": inits[hin], "out": hout,
                              "arr": np.zeros(inits[hin], dtype=np.float32)}
                print(f"[hush_onnx] {tag}: carry {hin} "
                      f"{inits[hin]} -> {hout}")
        net.states = pairs

    # -- streaming (tract.rs process()) --

    def reset(self):
        self._feats.reset()
        self._rolling = [np.zeros(N_FREQ, dtype=np.complex64)
                         for _ in range(DF_ORDER)]
        self._in_carry = np.zeros(0, dtype=np.float32)
        self._out_carry = np.zeros(0, dtype=np.float32)
        self.in_rms = self.out_rms = 0.0
        for net in (self.enc, self.erb, self.df):
            for st in net.states.values():
                st["arr"].fill(0)

    def _run_frame160(self, frame):
        """One native 160-sample float frame in/out."""
        frame = np.asarray(frame, dtype=np.float32)
        if float(np.mean(frame * frame)) < float(_SILENCE_MS):
            # Runtime silence shortcut: states freeze, zeros out.
            return np.zeros(HOP, dtype=np.float32)
        spec = self._feats.analyze(frame)
        self._rolling.append(spec.copy())
        del self._rolling[0]
        erb = self._feats.erb_feats(spec)
        nspec = self._feats.norm_spec(spec)
        lsnr, mask, coefs = self._infer(erb, nspec)
        if lsnr < float(MIN_DB):
            # Noise only: zero mask, no DF (bins<64 AND >=64 zeroed,
            # like apply_mask over the full spec).
            gains = np.zeros(NB_ERB, dtype=np.float32)
            coefs = None
        elif lsnr > float(MAX_ERB_DB):
            return frame.copy()  # clean: bypass everything
        else:
            gains = mask
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
                acc += self._rolling[oo][:NB_DF] * coefs[:, oo, :]
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
            st["arr"] = np.asarray(got[st["out"]], dtype=np.float32)
        return got

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
        lsnr = float(np.asarray(got[self._enc_out["lsnr"][0]]).ravel()[0])
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
        # DF decoder -> coefs[64,5,2] complex.
        feed = {}
        for role in ("emb", "c0"):
            n, s = self._df_in[role]
            feed[n] = np.asarray(parts[role]).reshape(
                _concrete_shape(s)).astype(np.float32)
        got = self._feed(self.df, feed)
        coname = self._df_out[0]
        coefs = self._to_642(np.asarray(got[coname], dtype=np.float32),
                             coname)
        return lsnr, mask.astype(np.float32), coefs

    @staticmethod
    def _to_642(co, coname):
        """Flat coefs -> complex64[64,5,2], permutation-robust.

        Drops size-1 dims; the rest must be {2,5,64} in some order
        (transposed to canonical). Anything else fails closed —
        a silently transposed filter would sound almost-right.
        """
        dims = tuple(d for d in co.shape if d != 1)
        if sorted(dims) != [2, 5, 64] or co.size != NB_DF * DF_ORDER * 2:
            raise RuntimeError(
                f"df coefs shape {co.shape} is not a (64,5,2) "
                f"permutation")
        t = co.reshape(dims).transpose(
            (dims.index(64), dims.index(5), dims.index(2)))
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
                blk = self._run_frame160(self._in_carry[:HOP])
                self._in_carry = self._in_carry[HOP:]
                if len(self._out_carry):
                    self._out_carry = np.concatenate(
                        [self._out_carry, blk])
                else:
                    self._out_carry = blk
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
            # every frame.
            self.last_error = f"hush onnx frame failed: {e}"
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


def _read_wav16(path):
    import wave
    with wave.open(path, "rb") as w:
        assert w.getframerate() == 16000, f"{path}: need 16 kHz"
        assert w.getnchannels() == 1, f"{path}: need mono"
        assert w.getsampwidth() == 2, f"{path}: need int16"
        return np.frombuffer(
            w.readframes(w.getnframes()), dtype=np.int16).copy()


def main(argv=None):
    """Validation vehicle: probe dump, determinism, file run, DLL parity.

    Usage: python -m web.hush_onnx BUNDLE [--wav mic16k.wav]
           [--dll weya_nc.dll] [--out out.wav]
    """
    import argparse
    import time
    import wave
    ap = argparse.ArgumentParser()
    ap.add_argument("bundle")
    ap.add_argument("--wav", default=None)
    ap.add_argument("--dll", default=None)
    ap.add_argument("--out", default=None)
    a = ap.parse_args(argv)
    core = HushOnnxCore(a.bundle)
    try:
        print(core.spec_dump())
        if a.wav is None:
            # Determinism probe: identical input twice -> identical out.
            rng = np.random.default_rng(7)
            x = (rng.standard_normal(HOP * 20) * 0.2).astype(np.float32)
            o1 = core.process(x.copy())
            core.reset()
            o2 = core.process(x.copy())
            same = np.array_equal(o1, o2)
            print(f"determinism: {'EXACT' if same else 'MISMATCH'} "
                  f"rms={float(np.sqrt(np.mean(o1.astype(float)**2))):.1f}")
            return 0 if same else 1
        pcm = _read_wav16(a.wav)
        x = pcm.astype(np.float32) / 32768.0
        t0 = time.perf_counter()
        o = core.process(x)
        dt = time.perf_counter() - t0
        print(f"onnx: {dt:.2f}s wall for {len(x)/16000:.1f}s audio "
              f"({dt/(len(x)/16000)*1000:.1f} ms/s) "
              f"rms={float(np.sqrt(np.mean(o.astype(float)**2))):.4f}")
        if a.dll is not None:
            from .hush import _HushCt
            ct = _HushCt(lib_path=a.dll, model_path=a.bundle)
            assert ct.ready, ct.last_error
            ct.reset()
            c = ct.process(x)
            d = np.abs(o - c)
            print(f"parity vs ctypes: max_abs={d.max():.6f} "
                  f"mean_abs={d.mean():.6f}")
        if a.out is not None:
            v = np.clip(np.rint(o * 32768.0),
                        -32768.0, 32767.0).astype(np.int16)
            with wave.open(a.out, "wb") as w:
                w.setframerate(16000)
                w.setnchannels(1)
                w.setsampwidth(2)
                w.writeframes(v.tobytes())
            print(f"wrote {a.out}")
        return 0
    finally:
        core.close()


if __name__ == "__main__":
    raise SystemExit(main())
