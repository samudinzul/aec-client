"""Audio I/O — one PortAudio (pyaudiowpatch) for all three streams.

Why a single backend: sounddevice's bundled PortAudio reported zero
devices on real hardware where pyaudiowpatch's fork enumerates fine.
Two PortAudios, two answers — so everything runs on the one proven to
see hardware here. (Both are mainstream reputable PyPI wheels; no PE
of our own either way. python-sounddevice exposes no WASAPI-loopback
API at all, which is why it can't cover the reference path anyway.)

Device model (mirrors the desktop app):
- Mic: pyaudiowpatch input stream at the device's native rate,
  resampled to 16 kHz in a feeder thread.
- Reference: pyaudiowpatch WASAPI-loopback stream at the mix rate,
  resampled to 16 kHz. Loopback delivers silence when speakers are
  idle — same as the desktop ref-ring-underflow path.
- Output: pyaudiowpatch output stream to the virtual cable, fed from
  16 kHz chain output resampled to the cable's native rate.

The chain itself stays 16 kHz always. The worker thread assembles
160-sample frames; feeder/writer threads bridge the native-rate
streams. Queues never block: input overflow drops (ring-overrun
semantics), output starvation emits silence.
"""

import queue
import threading

import numpy as np

try:
    import pyaudiowpatch as pa_patch
except ImportError:  # pragma: no cover
    pa_patch = None

from .dsp import FRAME_SIZE, SAMPLE_RATE, resample_from_16k, resample_to_16k, rms

_Q_DEPTH = 64


def _need_pa():
    if pa_patch is None:
        raise RuntimeError("pyaudiowpatch is not installed "
                           "(pip install pyaudiowpatch)")


def list_devices():
    """Capture + loopback + playback lists with defaults.

    Never raises: a dead backend yields empty lists plus an error
    string, so enumeration failure can't blank the UI.
    """
    out = {"capture": [], "loopback": [], "playback": [],
           "defaults": {}, "errors": {}}
    if pa_patch is None:
        out["errors"]["audio"] = "pyaudiowpatch is not installed"
        return out
    try:
        pa = pa_patch.PyAudio()
        try:
            loop_idx = set()
            try:
                for info in pa.get_loopback_device_info_generator():
                    loop_idx.add(info["index"])
                    out["loopback"].append({
                        "index": info["index"],
                        "name": info["name"],
                        "defaultSamplerate": info.get("defaultSampleRate"),
                    })
            except Exception as e:
                out["errors"]["loopback"] = f"loopback query failed: {e}"
            for i in range(pa.get_device_count()):
                try:
                    d = pa.get_device_info_by_index(i)
                except Exception:
                    continue
                entry = {
                    "index": i,
                    "name": d.get("name", f"device {i}"),
                    "maxInputChannels": d.get("maxInputChannels", 0),
                    "maxOutputChannels": d.get("maxOutputChannels", 0),
                    "defaultSamplerate": d.get("defaultSampleRate"),
                }
                if i in loop_idx:
                    continue
                if entry["maxInputChannels"] > 0:
                    out["capture"].append(entry)
                if entry["maxOutputChannels"] > 0:
                    out["playback"].append(entry)
            try:
                out["defaults"]["mic"] = pa.get_default_input_device_info()["index"]
            except Exception:
                pass
            try:
                out["defaults"]["ref"] = pa.get_default_wasapi_loopback()["index"]
            except Exception:
                if out["loopback"]:
                    out["defaults"]["ref"] = out["loopback"][0]["index"]
            try:
                out["defaults"]["out"] = pa.get_default_output_device_info()["index"]
            except Exception:
                pass
        finally:
            pa.terminate()
    except Exception as e:  # pragma: no cover
        out["errors"]["audio"] = f"device query failed: {e}"
    return out


class AudioRunner:
    """Native-rate streams + 16 kHz worker pump.

    mic_idx / out_idx are pyaudiowpatch device indices; ref_idx is a
    pyaudiowpatch loopback index.
    """

    def __init__(self, chain, mic_idx=None, ref_idx=None, out_idx=None):
        _need_pa()
        self.chain = chain
        self.mic_idx = int(mic_idx)
        self.ref_idx = int(ref_idx)
        self.out_idx = int(out_idx)
        self._in_q = queue.Queue(maxsize=_Q_DEPTH)
        self._ref_q = queue.Queue(maxsize=_Q_DEPTH)
        self._out_q = queue.Queue(maxsize=_Q_DEPTH)
        self._stop = threading.Event()
        self._threads = []
        self._pa = None
        self._streams = []
        self.in_rms = 0.0
        self.out_rms = 0.0

    def _feed_in(self, stream, rate, target):
        """Blocking input -> 16 kHz int16 frames into target queue."""
        chunk = max(FRAME_SIZE, int(rate * 0.02))
        carry = np.zeros(0, dtype=np.float32)
        while not self._stop.is_set():
            try:
                raw = stream.read(chunk, exception_on_overflow=False)
            except Exception:
                if self._stop.is_set():
                    break
                continue
            pcm = np.frombuffer(raw, dtype=np.int16)
            if pcm.size == 0:
                continue
            carry = np.concatenate([carry, resample_to_16k(pcm, rate)])
            while len(carry) >= FRAME_SIZE:
                try:
                    target.put_nowait(
                        (carry[:FRAME_SIZE] * 32767.0).astype(np.int16))
                except queue.Full:
                    pass  # input overrun: drop
                carry = carry[FRAME_SIZE:]

    def _feed_out(self, stream, rate):
        """16 kHz chain output -> native-rate cable stream."""
        carry = np.zeros(0, dtype=np.float32)
        chunk = max(FRAME_SIZE, int(rate * 0.02))
        while not self._stop.is_set():
            try:
                frame = self._out_q.get(timeout=0.05)
                carry = np.concatenate(
                    [carry, resample_from_16k(
                        frame.astype(np.float32) / 32768.0, rate)])
            except queue.Empty:
                pass
            while len(carry) >= chunk:
                try:
                    stream.write((np.clip(carry[:chunk], -1.0, 1.0)
                                  * 32767.0).astype(np.int16).tobytes())
                except Exception:
                    if self._stop.is_set():
                        break
                carry = carry[chunk:]
            if len(carry) < chunk and self._stop.is_set():
                break
            if self._out_q.empty() and len(carry) < chunk:
                # Starved: pad with silence so the cable never underruns.
                try:
                    stream.write(np.zeros(chunk, dtype=np.int16).tobytes())
                except Exception:
                    if self._stop.is_set():
                        break

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
        mic_rate = int(self._pa.get_device_info_by_index(
            self.mic_idx).get("defaultSampleRate", 48000) or 48000)
        ref_rate = int(self._pa.get_device_info_by_index(
            self.ref_idx).get("defaultSampleRate", 48000) or 48000)
        out_rate = int(self._pa.get_device_info_by_index(
            self.out_idx).get("defaultSampleRate", 48000) or 48000)
        mic_s = self._pa.open(format=pa_patch.paInt16, channels=1,
                              rate=mic_rate, input=True,
                              input_device_index=self.mic_idx)
        ref_s = self._pa.open(format=pa_patch.paInt16, channels=1,
                              rate=ref_rate, input=True,
                              input_device_index=self.ref_idx)
        out_s = self._pa.open(format=pa_patch.paInt16, channels=1,
                              rate=out_rate, output=True,
                              output_device_index=self.out_idx)
        self._streams = [mic_s, ref_s, out_s]
        self._stop.clear()
        self._threads = [
            threading.Thread(target=self._feed_in,
                             args=(mic_s, mic_rate, self._in_q), daemon=True),
            threading.Thread(target=self._feed_in,
                             args=(ref_s, ref_rate, self._ref_q), daemon=True),
            threading.Thread(target=self._feed_out,
                             args=(out_s, out_rate), daemon=True),
            threading.Thread(target=self._work, daemon=True),
        ]
        for t in self._threads:
            t.start()

    def stop(self):
        self._stop.set()
        for s in self._streams:
            try:
                s.stop_stream()
                s.close()
            except Exception:
                pass
        self._streams = []
        for t in self._threads:
            t.join(timeout=1.0)
        self._threads = []
        try:
            if self._pa is not None:
                self._pa.terminate()
        except Exception:
            pass
        self._pa = None
        for q in (self._in_q, self._ref_q, self._out_q):
            while not q.empty():
                try:
                    q.get_nowait()
                except queue.Empty:
                    break
