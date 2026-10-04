"""Near-end speech gate — port of src/speech_gate.h.

One RMS-hysteresis detector on the engine output feeds
two flags:

  for_wpe   — raw voice flag (drives the NKF-only WPE
              stage in the desktop app; unused on the
              DTLN path).

  for_notch — adaptation gate for the feedback-
              suppression notch. Two complementary stuck
              detectors, both fed by the ON flag
              (hysteresis: ema > kOn starts, ema < kOff
              ends):

    1. continuous run: ON without a single release for
       kRunMs (3 s — web path; the desktop NKF path
       uses 4.5 s) — a howl never pauses. Fast and
       exact for the classic sustained tone. Speech
       can trip it too: the EMA's ~0.8 s release lag
       means loud phrase pauses don't always drop
       ema below kOff, so run_ms accumulates across
       conversation. That trip is harmless — the
       notch's engage gate (>45% narrowband energy
       removed) cannot latch broadband voice, and a
       latched notch releases within ~1 s of
       broadband input. The burst detector below
       (6.8 s) stays the conversation-safe backstop.
    2. burst evidence: +1 ms per ON frame, decayed with
       kGapTauMs (700 ms) while quiet, trip at kBurstMs
       (6.8 s). The old continuous counter re-armed from
       scratch on every gap, so ring-under-voice that
       starts/stops (Discord mic-test playback) never
       released the notch. The EMA's ~0.8 s release lag
       counts as ON, so gaps drain hard; ordinary
       conversation (short utterances + real pauses)
       peaks near 6.0 s and stays under the threshold.

Safe even while the notch adapts during a "stuck"
stretch: the notch's engage gate only latches a section
after >45% of its input energy is demonstrably removed
(~200 ms). Voice is broadband — a 60 Hz cut removes a
tiny fraction — so voice can never latch a cut.

Attack ~20 ms, release ~300 ms. Thresholds are int16-
domain RMS of the engine output (-30 dBFS on / -36 dBFS
off); the detector never looks at the stages' own output,
so a notch cut can't flip the gate that froze it.
"""

import math


class SpeechGate:
    def __init__(self):
        self.ema = 0.0
        self.on = True      # speech-safe default
        self.stuck = False  # sustained-loudness override
        self.run_ms = 0.0       # continuous-ON time
        self.evidence_ms = 0.0  # burst evidence
        self.total_ms = 0.0     # session clock

    def update(self, frame_rms, frame_dur_ms):
        self.total_ms += frame_dur_ms
        k_atk = 1.0 - math.exp(-frame_dur_ms / 20.0)
        k_rel = 1.0 - math.exp(-frame_dur_ms / 300.0)
        k_on = 500.0      # int16 RMS: -30 dBFS
        k_off = 250.0     # -36 dBFS (hiss floor sits below)
        k_run_ms = 3000.0
        k_burst_ms = 6800.0
        k_gap_tau_ms = 700.0

        if frame_rms > self.ema:
            self.ema += k_atk * (frame_rms - self.ema)
        else:
            self.ema += k_rel * (frame_rms - self.ema)

        if self.on:
            if self.ema < k_off:
                self.on = False      # a real gap: release
        elif self.ema > k_on:
            self.on = True           # fresh onset

        if self.on:
            self.run_ms += frame_dur_ms
            self.evidence_ms += frame_dur_ms
        else:
            self.run_ms = 0.0
            self.evidence_ms *= math.exp(-frame_dur_ms / k_gap_tau_ms)
        self.stuck = (self.run_ms >= k_run_ms
                      or self.evidence_ms >= k_burst_ms)

    def for_wpe(self):
        return 1 if self.on else 0

    def for_notch(self):
        return 1 if (self.on and not self.stuck) else 0
