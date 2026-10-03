"""Audio I/O — mic + loopback reference + virtual-cable output.

Device model (mirrors the desktop app):
- Mic: sounddevice InputStream on the selected capture device
  (int16, mono, 16 kHz, callback -> queue).
- Reference: pyaudiowpatch WASAPI-loopback input on the selected
  loopback endpoint, read in a feeder thread and resampled to 16 kHz.
  Loopback delivers silence when speakers are idle — same as the
  desktop ref-ring-underflow path. (python-sounddevice exposes no
  loopback API at all, hence the second library; both are mainstream
  reputable PyPI wheels, no PE of our own either way.)
- Output: sounddevice OutputStream to the virtual cable.

The worker thread assembles 160-sample frames, runs the chain, and
feeds the output callback. Callbacks never block: input overflow
drops (like a ring overrun), output underflow emits silence.
"""

import queue
import threading

import numpy as np

try:
    import sounddevice as sd
except ImportError:  # pragma: no cover
    sd = None

try:
    import pyaudiowpatch as pa_patch
except ImportError:  # pragma: no cover
    pa_patch = None

from .dsp import FRAME_SIZE, SAMPLE_RATE, resample_to_16k, rms

_Q_DEPTH = 64


def _need_sd():
    if sd is None:
        raise RuntimeError("sounddevice is not installed (pip install sounddevice)")


def list_devices():
    """Capture + loopback + playback lists; per-source errors included.

    Never raises: a dead source yields an empty list plus an error
    string, so one broken backend can't blank the whole UI.
    """
    out = {"capture": [], "loopback": [], "playback": [], "errors": {}}
    if sd is None:
        out["errors"]["audio"] = "sounddevice is not installed"
    else:
        try:
            for i, d in enumerate(sd.query_devices()):
                entry = {
                    "index": i,
                    "name": d["name"],
                    "hostapi": sd.query_hostapis(d["host_api"])["name"],
                    "maxInputChannels": d["max_input_channels"],
                    "maxOutputChannels": d["max_output_channels"],
                }
                if d["max_input_channels"] > 0:
                    out["capture"].append(entry)
                if d["max_output_channels"] > 0:
                    out["playback"].append(entry)
        except Exception as e:  # pragma: no cover
            out["errors"]["audio"] = f"device query failed: {e}"
    if pa_patch is None:
        out["errors"]["loopback"] = "pyaudiowpatch is not installed"
    else:
        try:
            pa = pa_patch.PyAudio()
            try:
                for info in pa.get_loopback_device_info_generator():
                    out["loopback"].append({
                        "index": info["index"],
                        "name": info["name"],
                        "defaultSamplerate": info.get("defaultSampleRate"),
                    })
            finally:
                pa.terminate()
        except Exception as e:  # pragma: no cover
            out["errors"]["loopback"] = f"loopback query failed: {e}"
    return out


class AudioRunner:
    """Streams + worker pump. ref_idx is a pyaudiowpatch loopback index."""

    def __init__(self, chain, mic_idx=None, ref_idx=None, out_idx=None):
        _need_sd()
        if pa_patch is None:
            raise RuntimeError("pyaudiowpatch is not installed "
                               "(pip install pyaudiowpatch)")
        self.chain = chain
        self.mic_idx = mic_idx
        self.ref_idx = ref_idx
        self.out_idx = out_idx
        self._in_q = queue.Queue(maxsize=_Q_DEPTH)
        self._ref_q = queue.Queue(maxsize=_Q_DEPTH)
        self._out_q = queue.Queue(maxsize=_Q_DEPTH)
        self._stop = threading.Event()
        self._worker = None
        self._ref_thread = None
        self._pa = None
        self._pa_stream = None
        self._streams = []
        self.in_rms = 0.0
        self.out_rms = 0.0

    # -- sounddevice callbacks (mic in, cable out) --
    def _mic_cb(self, indata, frames, time, status):
        try:
            self._in_q.put_nowait(np.asarray(indata[:, 0], dtype=np.int16).copy())
        except queue.Full:
            pass  # input overrun: drop, like a ring overrun

    def _out_cb(self, outdata, frames, time, status):
        try:
            frame = self._out_q.get_nowait()
        except queue.Empty:
            frame = np.zeros(frames, dtype=np.int16)  # underflow: silence
        outdata[:, 0] = frame[:frames]

    # -- loopback feeder (blocking reads, resampled to 16 kHz) --
    def _ref_feed(self):
        rate = int(self._pa_rate)
        chunk = max(FRAME_SIZE, int(rate * 0.02))  # ~20 ms reads
        carry = np.zeros(0, dtype=np.float32)
        while not self._stop.is_set():
            try:
                raw = self._pa_stream.read(chunk, exception_on_overflow=False)
            except Exception:
                if self._stop.is_set():
                    break
                continue
            pcm = np.frombuffer(raw, dtype=np.int16)
            if pcm.size == 0:
                continue
            f = resample_to_16k(pcm, rate)
            carry = np.concatenate([carry, f])
            while len(carry) >= FRAME_SIZE:
                try:
                    self._ref_q.put_nowait(
                        (carry[:FRAME_SIZE] * 32767.0).astype(np.int16))
                except queue.Full:
                    pass
                carry = carry[FRAME_SIZE:]

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
        self._pa = pa_patch.PyAudio()
        info = self._pa.get_device_info_by_index(int(self.ref_idx))
        self._pa_rate = int(info.get("defaultSampleRate", 48000) or 48000)
        self._pa_stream = self._pa.open(
            format=pa_patch.paInt16, channels=1, rate=self._pa_rate,
            input=True, input_device_index=int(self.ref_idx),
            frames_per_buffer=0)
        kw = dict(samplerate=SAMPLE_RATE, channels=1, dtype="int16",
                  blocksize=FRAME_SIZE)
        mic = sd.InputStream(device=self.mic_idx, callback=self._mic_cb, **kw)
        out = sd.OutputStream(device=self.out_idx, callback=self._out_cb, **kw)
        self._streams = [mic, out]
        self._stop.clear()
        self._ref_thread = threading.Thread(target=self._ref_feed, daemon=True)
        self._worker = threading.Thread(target=self._work, daemon=True)
        self._ref_thread.start()
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
        try:
            if self._pa_stream is not None:
                self._pa_stream.stop_stream()
                self._pa_stream.close()
        except Exception:
            pass
        self._pa_stream = None
        try:
            if self._pa is not None:
                self._pa.terminate()
        except Exception:
            pass
        self._pa = None
        if self._ref_thread is not None:
            self._ref_thread.join(timeout=1.0)
            self._ref_thread = None
        if self._worker is not None:
            self._worker.join(timeout=1.0)
            self._worker = None
        for q in (self._in_q, self._ref_q, self._out_q):
            while not q.empty():
                try:
                    q.get_nowait()
                except queue.Empty:
                    break
