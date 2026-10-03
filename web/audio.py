"""Audio I/O — sounddevice streams mirroring the desktop device model.

- Mic: InputStream on the selected capture device (int16, mono, 16 kHz)
- Reference: InputStream on the selected *output* device with
  WASAPI loopback (delivers silence when speakers are idle — same as
  the desktop ref-ring-underflow path)
- Output: OutputStream to the virtual cable (CABLE Input)

A worker thread assembles 160-sample frames, runs them through the
chain, and feeds the output callback. Callbacks never block: input
overflow drops (like a ring overrun), output underflow emits silence.
"""

import queue
import threading

import numpy as np

try:
    import sounddevice as sd
except ImportError:  # pragma: no cover
    sd = None

from .dsp import FRAME_SIZE, SAMPLE_RATE, rms

_Q_DEPTH = 64


def _need_sd():
    if sd is None:
        raise RuntimeError("sounddevice is not installed (pip install sounddevice)")


def list_devices():
    """Capture + playback device lists in the desktop app's shape."""
    _need_sd()
    devs = sd.query_devices()
    caps, plays = [], []
    for i, d in enumerate(devs):
        entry = {
            "index": i,
            "name": d["name"],
            "hostapi": sd.query_hostapis(d["host_api"])["name"],
            "maxInputChannels": d["max_input_channels"],
            "maxOutputChannels": d["max_output_channels"],
            "defaultSamplerate": d["default_samplerate"],
        }
        if d["max_input_channels"] > 0:
            caps.append(entry)
        if d["max_output_channels"] > 0:
            plays.append(entry)
    return {"capture": caps, "playback": plays}


class AudioRunner:
    def __init__(self, chain, mic_idx=None, ref_idx=None, out_idx=None):
        _need_sd()
        self.chain = chain
        self.mic_idx = mic_idx
        self.ref_idx = ref_idx
        self.out_idx = out_idx
        self._in_q = queue.Queue(maxsize=_Q_DEPTH)
        self._ref_q = queue.Queue(maxsize=_Q_DEPTH)
        self._out_q = queue.Queue(maxsize=_Q_DEPTH)
        self._stop = threading.Event()
        self._worker = None
        self._streams = []
        self.in_rms = 0.0
        self.out_rms = 0.0

    def _mic_cb(self, indata, frames, time, status):
        try:
            self._in_q.put_nowait(np.asarray(indata[:, 0], dtype=np.int16).copy())
        except queue.Full:
            pass  # input overrun: drop, like a ring overrun

    def _ref_cb(self, indata, frames, time, status):
        try:
            self._ref_q.put_nowait(np.asarray(indata[:, 0], dtype=np.int16).copy())
        except queue.Full:
            pass

    def _out_cb(self, outdata, frames, time, status):
        try:
            frame = self._out_q.get_nowait()
        except queue.Empty:
            frame = np.zeros(frames, dtype=np.int16)  # underflow: silence
        outdata[:, 0] = frame[:frames]

    def _pump(self, mic, ref):
        cleaned = self.chain.process_frame(mic, ref)
        self.in_rms = rms(mic.astype(np.float32) / 32768.0)
        self.out_rms = rms(cleaned.astype(np.float32) / 32768.0)
        try:
            self._out_q.put_nowait(cleaned)
        except queue.Full:
            pass

    def _work(self):
        mic_buf = np.zeros(0, dtype=np.int16)
        ref_buf = np.zeros(0, dtype=np.int16)
        while not self._stop.is_set():
            try:
                mic_buf = np.concatenate(
                    [mic_buf, self._in_q.get(timeout=0.05)])
                ref_buf = np.concatenate(
                    [ref_buf, self._ref_q.get(timeout=0.05)])
            except queue.Empty:
                continue
            while len(mic_buf) >= FRAME_SIZE and len(ref_buf) >= FRAME_SIZE:
                self._pump(mic_buf[:FRAME_SIZE].copy(),
                           ref_buf[:FRAME_SIZE].copy())
                mic_buf = mic_buf[FRAME_SIZE:]
                ref_buf = ref_buf[FRAME_SIZE:]

    def start(self):
        kw = dict(samplerate=SAMPLE_RATE, channels=1, dtype="int16",
                  blocksize=FRAME_SIZE)
        mic = sd.InputStream(device=self.mic_idx, callback=self._mic_cb, **kw)
        loop = sd.WasapiSettings(loopback=True)
        ref = sd.InputStream(device=self.ref_idx, callback=self._ref_cb,
                             extra_settings=loop, **kw)
        out = sd.OutputStream(device=self.out_idx, callback=self._out_cb, **kw)
        self._streams = [mic, ref, out]
        self._stop.clear()
        self._worker = threading.Thread(target=self._work, daemon=True)
        self._worker.start()
        for s in self._streams:
            s.start()

    def stop(self):
        self._stop.set()
        for s in self._streams:
            try:
                s.stop()
                s.close()
            except Exception:
                pass
        self._streams = []
        if self._worker is not None:
            self._worker.join(timeout=1.0)
            self._worker = None
        for q in (self._in_q, self._ref_q, self._out_q):
            while not q.empty():
                try:
                    q.get_nowait()
                except queue.Empty:
                    break
