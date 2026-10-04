"""LMS adaptive notch filter — port of src/notch.cpp.

Two cascaded second-order adaptive notches that track
narrowband howling/ringing loops (acoustic feedback)
without touching the rest of the voice band. Per-sample
cost is a handful of multiplies — negligible CPU, zero
latency.

Per section (zeros on the unit circle at e^{+-jw}, poles
at r*e^{+-jw}, bandwidth ~60 Hz):
    y = x - 2cos(w) x1 + x2 + 2r cos(w) y1 - r^2 y2
Frequency adapts by steepest descent on the output power
using the exact sensitivity signal s = dy/dw (Nehorai-
style gradient, EMA-smoothed so broadband noise averages
to ~0 and only coherent tones pull the notch):
    s  = 2 sin(w) x1 - 2 r sin(w) y1 + a1 s1 + a2 s2
    g' = EMA(y*s),  p' = EMA(s*s)
    dw = -mu * g' / (p' + eps)

Voice safety (nothing may sound unnatural):
  - adaptation FREEZES while the near-end person talks
    (set_speech): voice harmonics never capture a notch;
    the filter keeps applying at its parked frequencies.
  - adaptation also pauses below a ~-60 dBFS silence
    floor (no random walk on the noise floor).
  - dw is slew-limited (8000 Hz/s) and w is clamped to
    [80 Hz, 0.45*sr]: any input, however pathological,
    moves a notch at a bounded rate into a bounded band.
  - a 60 Hz cut parked on a vowel harmonic removes a
    single narrow band: bounded, static coloration —
    never pumping.

Fail-open: state is finite by construction (clamps +
slew), the stage passes audio unconditionally.
"""

import math

import numpy as np

_SECTIONS = 2
_BANDWIDTH_HZ = 60.0
_MIN_HZ = 80.0
_INIT_HZ = (500.0, 1500.0)
_SLEW_HZ_PER_SEC = 8000.0
_MU = 0.005
_EMA_ALPHA = 0.9995
_BLOCK_ALPHA = 0.967
_ENG_RATIO = 0.45
_ENG_BLOCKS = 20
_DEAD_FRAC = 0.002
_EPS = 1e-20
_FLOOR_RMS = 1e-3


class _Section:
    __slots__ = ("w", "w_init", "engaged", "eng_count",
                 "pin_e", "pout_e", "r", "b1", "a1", "a2",
                 "sw", "x1", "x2", "y1", "y2", "s1", "s2",
                 "ema_g", "ema_p")

    def __init__(self):
        self.w = 0.0
        self.w_init = 0.0
        self.engaged = False
        self.eng_count = 0
        self.pin_e = 0.0
        self.pout_e = 0.0
        self.r = 0.0
        self.b1 = 0.0
        self.a1 = 0.0
        self.a2 = 0.0
        self.sw = 0.0
        self.x1 = self.x2 = 0.0
        self.y1 = self.y2 = 0.0
        self.s1 = self.s2 = 0.0
        self.ema_g = 0.0
        self.ema_p = 0.0

    def set_freq(self, w):
        self.w = w
        self.sw = math.sin(w)
        cw = math.cos(w)
        self.b1 = -2.0 * cw
        self.a1 = 2.0 * self.r * cw
        self.a2 = -self.r * self.r

    def init(self, sr, hz):
        # Pole radius for ~_BANDWIDTH_HZ:
        # BW ~= (1-r)*sr/pi  =>  r = 1 - pi*BW/sr.
        self.r = 1.0 - math.pi * _BANDWIDTH_HZ / sr
        if self.r < 0.5:
            self.r = 0.5
        self.x1 = self.x2 = self.y1 = self.y2 = 0.0
        self.s1 = self.s2 = 0.0
        self.ema_g = self.ema_p = 0.0
        self.pin_e = self.pout_e = 0.0
        self.eng_count = 0
        self.engaged = False
        self.w_init = hz
        self.set_freq(2.0 * math.pi * hz / sr)


class Notch:
    """Two cascaded adaptive notches. process() filters an
    int16 numpy array in place, unconditionally."""

    def __init__(self, sample_rate=16000):
        self.sr = sample_rate
        self.w_min = 2.0 * math.pi * _MIN_HZ / sample_rate
        self.w_max = 2.0 * math.pi * 0.45
        # Per-sample slew for _SLEW_HZ_PER_SEC:
        # dw = 2*pi*(slew/sr)/sr.
        self.dw_max = (2.0 * math.pi
                       * (_SLEW_HZ_PER_SEC / sample_rate)
                       / sample_rate)
        self.sections = [_Section() for _ in range(_SECTIONS)]
        for s, hz in zip(self.sections, _INIT_HZ):
            s.init(sample_rate, hz)
        self.speaking = 1
        self.samples = 0

    def set_speech(self, speaking):
        self.speaking = 1 if speaking else 0

    def reset(self):
        for s, hz in zip(self.sections, _INIT_HZ):
            s.init(self.sr, hz)
        self.speaking = 1

    def process(self, buf):
        n = len(buf)
        if n <= 0:
            return
        self.samples += n

        # Block gate: adapt only above the silence floor.
        f = buf.astype(np.float64)
        rms = math.sqrt(float(np.dot(f, f)) / n) / 32768.0
        adapt = (not self.speaking) and rms > _FLOOR_RMS
        acc_pin = [0.0, 0.0]
        acc_pout = [0.0, 0.0]
        secs = self.sections
        dw_max = self.dw_max
        w_min = self.w_min
        w_max = self.w_max
        dead = _DEAD_FRAC * dw_max

        for i in range(n):
            x = float(buf[i]) / 32768.0
            for k in range(_SECTIONS):
                s = secs[k]
                xin = x

                # Notch difference equation.
                y = (x + s.b1 * s.x1 + s.x2
                     + s.a1 * s.y1 + s.a2 * s.y2)

                # Sensitivity dy/dw (same denominator
                # recursion).
                g = (2.0 * s.sw * s.x1
                     - 2.0 * s.r * s.sw * s.y1
                     + s.a1 * s.s1 + s.a2 * s.s2)

                if adapt:
                    s.ema_g = (_EMA_ALPHA * s.ema_g
                               + (1.0 - _EMA_ALPHA) * (y * g))
                    s.ema_p = (_EMA_ALPHA * s.ema_p
                               + (1.0 - _EMA_ALPHA) * (g * g))
                    dwe = -_MU * s.ema_g / (s.ema_p + _EPS)
                    if dwe > dw_max:
                        dwe = dw_max
                    elif dwe < -dw_max:
                        dwe = -dw_max
                    # Dead-zone: below 2% of the slew cap the
                    # estimate is noise — frozen instead, it
                    # settles deep and stays put.
                    if dwe > dead or dwe < -dead:
                        w = s.w + dwe
                        if w < w_min:
                            w = w_min
                        elif w > w_max:
                            w = w_max
                        if w != s.w:
                            s.set_freq(w)

                # Engage telemetry: how much energy would
                # this section remove right now?
                acc_pin[k] += xin * xin
                acc_pout[k] += y * y

                # Histories shift; the filter keeps running
                # while bypassed so adaptation stays coherent.
                s.x2 = s.x1
                s.x1 = xin
                s.y2 = s.y1
                s.y1 = y
                s.s2 = s.s1
                s.s1 = g
                x = y if s.engaged else xin

            v = x * 32768.0
            if v > 32767.0:
                v = 32767.0
            elif v < -32768.0:
                v = -32768.0
            buf[i] = int(v)

        # Engage gate: latch a section once it has
        # demonstrably captured a tone (>45% of its input
        # energy removed, sustained ~200 ms while adapting).
        # Voice is broadband — a 60 Hz cut removes a tiny
        # fraction — so no-howl users run an exact bypass.
        for k in range(_SECTIONS):
            s = secs[k]
            s.pin_e = (_BLOCK_ALPHA * s.pin_e
                       + (1.0 - _BLOCK_ALPHA) * acc_pin[k])
            s.pout_e = (_BLOCK_ALPHA * s.pout_e
                        + (1.0 - _BLOCK_ALPHA) * acc_pout[k])
        # Only the strongest candidate accrues a latch
        # streak; rivals decay. Once one section swallows
        # the tone the other's ratio collapses anyway.
        best = -1
        best_ratio = -1.0
        ratio = [0.0, 0.0]
        for k in range(_SECTIONS):
            s = secs[k]
            # An engaged section is not a candidate: its
            # ratio stays high (it is removing energy),
            # which would starve the free section of the
            # streak it needs to latch a second howl.
            ratio[k] = (-1.0 if s.engaged
                        else (s.pin_e - s.pout_e)
                        / (s.pin_e + _EPS))
            if ratio[k] > best_ratio:
                best_ratio = ratio[k]
                best = k
        for k in range(_SECTIONS):
            s = secs[k]
            if s.engaged:
                continue
            if adapt and k == best and ratio[k] > _ENG_RATIO:
                s.eng_count += 1
                if s.eng_count >= _ENG_BLOCKS:
                    s.engaged = True
            elif adapt and ratio[k] > _ENG_RATIO:
                if s.eng_count > 0:
                    s.eng_count -= 1
            else:
                s.eng_count = 0

    def engaged(self, idx):
        if 0 <= idx < _SECTIONS:
            return 1 if self.sections[idx].engaged else 0
        return 0

    def freq(self, idx):
        if 0 <= idx < _SECTIONS:
            return self.sections[idx].w * self.sr / (2.0 * math.pi)
        return 0.0
