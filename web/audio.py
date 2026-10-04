"""Audio I/O — pyaudiowpatch, callback model (like main.cpp).

Why a single backend: sounddevice's bundled PortAudio reported zero
devices on real hardware where pyaudiowpatch's fork enumerates fine,
and sounddevice exposes no WASAPI-loopback API at all. Everything
(mic, loopback ref, CABLE out) runs on pyaudiowpatch.

Why callbacks: the first cut used blocking read()/write() threads.
A CABLE Input with no consumer (Discord/Zoom closed) fills the
cable's buffer and wedges stream.write() forever; teardown around a
wedged blocking call then kills the whole process (the "server
disconnected" report). main.cpp's mic_callback / loopback_callback /
output_callback never block — an unconsumed cable just underruns
with silence — so the same model is ported here:

- Mic: callback copies int16 input into a queue.
- Reference: WASAPI loopback callback, same queue pattern.
- Output: callback pulls 16 kHz chain output, upsamples to the
  cable's native rate, delivers exactly frame_count frames;
  starved -> silence. Never blocks.

The chain stays 16 kHz. One worker thread assembles 160-sample
frames from the two input queues (resampling native -> 16 kHz at
the edge) and feeds the chain; its output lands in the queue the
output callback drains. Queues never block: input overflow drops
(ring-overrun semantics), a starved loopback reads as silence
(speakers idle — the desktop's underflow path), output starvation
emits silence.
"""

import queue
import threading

import numpy as np

try:
    import pyaudiowpatch as pa_patch
except ImportError:  # pragma: no cover
    pa_patch = None

from .dsp import (FRAME_SIZE, SAMPLE_RATE, resample_from_16k,
                  resample_to_16k, rms)

_Q_DEPTH = 64


def _need_pa():
    if pa_patch is None:
        raise RuntimeError("pyaudiowpatch is not installed "
                           "(pip install pyaudiowpatch)")


# ============================================================
#  Device enumeration — display filter mirrors src/main.cpp
#  (BuildDisplayIndices / FindCableInputIndex /
#  FindSystemDefaultIndex):
#    mic  = capture devices, minus virtual cable, minus the
#           loopback wrapper endpoints pyaudiowpatch injects
#           into the capture list
#    ref  = WASAPI loopback endpoints, minus virtual cable
#           (loopback is the only way to capture what the
#           speakers play — same list main.cpp builds from
#           playback devices, minus cable)
#    out  = every playback endpoint; CABLE Input preferred
#  Same-named devices appear once per host API (MME,
#  DirectSound, WASAPI, WDM-KS); the best API wins (MME
#  truncates names to 31 chars, so matching is by prefix).
# ============================================================

def _is_virtual_cable(name: str) -> bool:
    low = name.lower()
    return "cable" in low or "vb-audio" in low


def _is_pseudo(name: str) -> bool:
    """MME/DirectSound pseudo-devices — the OS 'default
    device' aliases, not real endpoints (miniaudio, used by
    the desktop app, never lists them)."""
    low = name.lower()
    return "sound mapper" in low or "primary sound" in low


def _is_cable_input(name: str) -> bool:
    return _is_virtual_cable(name) and "input" in name.lower()


_HOSTAPI_RANK = {"wasapi": 3, "directsound": 2, "mme": 1,
                 "windows wdm-ks": 0}


def _hostapi_rank(pa, host_api) -> int:
    try:
        name = pa.get_host_api_info_by_index(host_api)["name"].lower()
    except Exception:
        # PortAudio registers host APIs worst-first on
        # Windows (MME=0, DirectSound=1, WASAPI=2,
        # WDM-KS=3), so the index itself is the rank.
        return host_api
    return _HOSTAPI_RANK.get(name, host_api)


# MME truncates device names to 31 chars (MAXPNAMELEN).
# Full names are recovered from two sources: the WASAPI
# loopback generator (pyaudiowpatch's own WASAPI path —
# it keeps working even when PortAudio's WASAPI and
# DirectSound host APIs enumerate nothing) and the fixed
# VB-CABLE product names.
_KNOWN_FULL_NAMES = [
    "CABLE Input (VB-Audio Virtual Cable)",
    "CABLE Output (VB-Audio Virtual Cable)",
]
_LOOPBACK_SUFFIX = " [Loopback]"


def _full_name(short: str, full_pool) -> str:
    for full in full_pool:
        if full.startswith(short):
            return full
    return short


def _same_device(a: str, b: str) -> bool:
    if a == b:
        return True
    # MME truncates device names to 31 chars (MAXPNAMELEN),
    # so the short name is a prefix of the full DirectSound/
    # WASAPI one. 20-char floor keeps unrelated devices with
    # short shared prefixes apart.
    if len(a) >= 20 and b.startswith(a):
        return True
    if len(b) >= 20 and a.startswith(b):
        return True
    return False


def _dedupe(entries, pa):
    """One entry per physical device; on a name collision
    keep the best host API (WASAPI > DirectSound > MME >
    WDM-KS)."""
    kept = []
    for e in entries:
        for i, k in enumerate(kept):
            if _same_device(e["name"], k["name"]):
                if (_hostapi_rank(pa, e["hostApi"])
                        > _hostapi_rank(pa, k["hostApi"])):
                    kept[i] = e
                break
        else:
            kept.append(e)
    return kept


def _snap(index, entries, by_name=None):
    """Default index snapped onto `entries`: exact index,
    else name match, else first entry, else None."""
    if index is not None and any(e["index"] == index for e in entries):
        return index
    if by_name:
        for e in entries:
            if _same_device(e["name"], by_name):
                return e["index"]
    return entries[0]["index"] if entries else None


def _find_cable_input(entries):
    """First CABLE Input in the playback list, else first cable
    device, else None (main.cpp FindCableInputIndex)."""
    fallback = None
    for e in entries:
        if not _is_virtual_cable(e["name"]):
            continue
        if fallback is None:
            fallback = e["index"]
        if _is_cable_input(e["name"]):
            return e["index"]
    return fallback


def list_devices():
    """Capture + loopback + playback lists with defaults.

    Never raises: a dead backend yields empty lists plus an
    error string, so enumeration failure can't blank the UI.
    """
    out = {"capture": [], "loopback": [], "playback": [],
           "defaults": {}, "errors": {}}
    if pa_patch is None:
        out["errors"]["audio"] = "pyaudiowpatch is not installed"
        return out
    try:
        pa = pa_patch.PyAudio()
        try:
            # WASAPI loopback endpoints — the only valid Speaker
            # Reference sources. pyaudiowpatch also injects them
            # into the plain capture enumeration; they are
            # wrappers, so the mic list drops them by index.
            loop_entries = []
            try:
                for info in pa.get_loopback_device_info_generator():
                    loop_entries.append({
                        "index": info["index"],
                        "name": info.get("name",
                                       f"loopback {info['index']}"),
                        "maxInputChannels": info.get(
                            "maxInputChannels", 1),
                        "maxOutputChannels": 0,
                        "defaultSamplerate": info.get(
                            "defaultSampleRate"),
                        "hostApi": info.get("hostApi"),
                    })
            except Exception as e:
                out["errors"]["loopback"] = f"loopback query failed: {e}"

            raw = []
            for i in range(pa.get_device_count()):
                try:
                    d = pa.get_device_info_by_index(i)
                except Exception:
                    continue
                raw.append({
                    "index": i,
                    "name": d.get("name", f"device {i}"),
                    "maxInputChannels": d.get("maxInputChannels", 0),
                    "maxOutputChannels": d.get("maxOutputChannels", 0),
                    "defaultSamplerate": d.get("defaultSampleRate"),
                    "hostApi": d.get("hostApi"),
                })

            cap = [e for e in raw
                   if e["maxInputChannels"] > 0
                   and e["index"] not in {l["index"]
                                          for l in loop_entries}
                   and not _is_virtual_cable(e["name"])
                   and not _is_pseudo(e["name"])]
            play = [e for e in raw
                    if e["maxOutputChannels"] > 0
                    and not _is_pseudo(e["name"])]
            ref = [e for e in loop_entries
                   if not _is_virtual_cable(e["name"])]

            # Recover full names for MME-truncated entries
            # (31-char cap) from the loopback generator's
            # full names and the known VB-CABLE names.
            full_pool = _KNOWN_FULL_NAMES + [
                e["name"][:-len(_LOOPBACK_SUFFIX)]
                for e in loop_entries
                if e["name"].endswith(_LOOPBACK_SUFFIX)
            ]
            for e in raw:
                e["name"] = _full_name(e["name"], full_pool)

            out["capture"] = _dedupe(cap, pa)
            out["loopback"] = _dedupe(ref, pa)
            out["playback"] = _dedupe(play, pa)

            # Windows system default input, snapped onto the
            # filtered mic list (main.cpp FindSystemDefaultIndex).
            def_in_idx, def_in_name = None, ""
            try:
                di = pa.get_default_input_device_info()
                def_in_idx, def_in_name = di["index"], di.get("name", "")
            except Exception:
                pass
            out["defaults"]["mic"] = _snap(
                def_in_idx, out["capture"], by_name=def_in_name)

            # Reference default: the loopback endpoint of the
            # Windows default OUTPUT device (name match, since
            # the loopback index differs from the playback one).
            def_out_idx, def_out_name = None, ""
            try:
                do = pa.get_default_output_device_info()
                def_out_idx = do["index"]
                def_out_name = do.get("name", "")
            except Exception:
                pass
            out["defaults"]["ref"] = _snap(
                None, out["loopback"], by_name=def_out_name)

            # Output default: CABLE Input when installed (the
            # routing target), else the system default output.
            cable = _find_cable_input(out["playback"])
            if cable is not None:
                out["defaults"]["out"] = cable
            else:
                out["defaults"]["out"] = _snap(
                    def_out_idx, out["playback"],
                    by_name=def_out_name)
        finally:
            pa.terminate()
    except Exception as e:  # pragma: no cover
        out["errors"]["audio"] = f"device query failed: {e}"
    return out


class AudioRunner:
    """Callback-mode streams + a 16 kHz worker pump.

    mic_idx / out_idx are pyaudiowpatch device indices;
    ref_idx is a pyaudiowpatch loopback index.
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
        # Output resampler state — touched ONLY by the
        # PortAudio callback thread, so no lock is needed.
        self._mic_rate = 48000
        self._ref_rate = 48000
        self._out_rate = 48000
        self._out_carry = np.zeros(0, dtype=np.float32)

    # -------------------------------------------------------
    #  PortAudio callbacks (run on PortAudio's thread;
    #  they must never block or raise)
    # -------------------------------------------------------

    def _on_mic(self, in_data, frame_count, time_info, status):
        try:
            pcm = np.frombuffer(in_data, dtype=np.int16).copy()
            if pcm.size:
                try:
                    self._in_q.put_nowait(pcm)
                except queue.Full:
                    pass  # ring overrun: drop, like the desktop app
        except Exception:
            pass
        return (None, pa_patch.paContinue)

    def _on_ref(self, in_data, frame_count, time_info, status):
        try:
            pcm = np.frombuffer(in_data, dtype=np.int16).copy()
            if pcm.size:
                try:
                    self._ref_q.put_nowait(pcm)
                except queue.Full:
                    pass
        except Exception:
            pass
        return (None, pa_patch.paContinue)

    def _on_out(self, in_data, frame_count, time_info, status):
        try:
            carry = self._out_carry
            rate = self._out_rate
            while len(carry) < frame_count:
                try:
                    frame = self._out_q.get_nowait()
                except queue.Empty:
                    break
                carry = np.concatenate([carry, resample_from_16k(
                    frame.astype(np.float32) / 32768.0, rate)])
            if len(carry) >= frame_count:
                out = carry[:frame_count]
                self._out_carry = carry[frame_count:]
            else:
                # Starved: pad with silence so the cable never
                # underruns audibly (main.cpp output_callback).
                out = np.zeros(frame_count, dtype=np.float32)
                if len(carry):
                    out[:len(carry)] = carry
                self._out_carry = np.zeros(0, dtype=np.float32)
            pcm = (np.clip(out, -1.0, 1.0)
                   * 32767.0).astype(np.int16).tobytes()
        except Exception:
            pcm = np.zeros(frame_count, dtype=np.int16).tobytes()
        return (pcm, pa_patch.paContinue)

    # -------------------------------------------------------
    #  Worker: input queues (native rate) -> 16 kHz frames
    #  -> chain -> output queue
    # -------------------------------------------------------

    def _pump(self, mic_i16, ref_i16):
        cleaned = self.chain.process_frame(mic_i16, ref_i16)
        self.in_rms = rms(mic_i16.astype(np.float32) / 32768.0)
        self.out_rms = rms(cleaned.astype(np.float32) / 32768.0)
        try:
            self._out_q.put_nowait(cleaned)
        except queue.Full:
            pass

    def _work(self):
        mic_buf = np.zeros(0, dtype=np.float32)   # 16 kHz domain
        ref_buf = np.zeros(0, dtype=np.float32)
        while not self._stop.is_set():
            try:
                raw_mic = self._in_q.get(timeout=0.05)
            except queue.Empty:
                continue
            # Loopback starves while the speakers are idle
            # (WASAPI delivers no frames until something
            # plays), so never stall the mic path on it:
            # a missing ref reads as digital silence,
            # which is exactly what "nothing playing"
            # means to the AEC. Same as the desktop's
            # ref-ring underflow path (main.cpp
            # output_callback) — without this, a fresh
            # start with no audio playing looked dead:
            # mic chunks were dropped waiting for a ref
            # that never came, until Discord opened and
            # woke the loopback.
            try:
                raw_ref = self._ref_q.get_nowait()
            except queue.Empty:
                n_ref = max(1, int(round(
                    len(raw_mic) * self._ref_rate
                    / self._mic_rate)))
                raw_ref = np.zeros(n_ref, dtype=np.int16)
            mic_buf = np.concatenate(
                [mic_buf, resample_to_16k(raw_mic, self._mic_rate)])
            ref_buf = np.concatenate(
                [ref_buf, resample_to_16k(raw_ref, self._ref_rate)])
            while len(mic_buf) >= FRAME_SIZE and len(ref_buf) >= FRAME_SIZE:
                mic_i16 = (mic_buf[:FRAME_SIZE]
                           * 32767.0).astype(np.int16)
                ref_i16 = (ref_buf[:FRAME_SIZE]
                           * 32767.0).astype(np.int16)
                self._pump(mic_i16, ref_i16)
                mic_buf = mic_buf[FRAME_SIZE:]
                ref_buf = ref_buf[FRAME_SIZE:]

    def start(self):
        self._pa = pa_patch.PyAudio()
        self._mic_rate = int(self._pa.get_device_info_by_index(
            self.mic_idx).get("defaultSampleRate", 48000) or 48000)
        self._ref_rate = int(self._pa.get_device_info_by_index(
            self.ref_idx).get("defaultSampleRate", 48000) or 48000)
        self._out_rate = int(self._pa.get_device_info_by_index(
            self.out_idx).get("defaultSampleRate", 48000) or 48000)
        self._out_carry = np.zeros(0, dtype=np.float32)
        mic_s = self._pa.open(format=pa_patch.paInt16, channels=1,
                              rate=self._mic_rate, input=True,
                              input_device_index=self.mic_idx,
                              stream_callback=self._on_mic)
        ref_s = self._pa.open(format=pa_patch.paInt16, channels=1,
                              rate=self._ref_rate, input=True,
                              input_device_index=self.ref_idx,
                              stream_callback=self._on_ref)
        out_s = self._pa.open(format=pa_patch.paInt16, channels=1,
                              rate=self._out_rate, output=True,
                              output_device_index=self.out_idx,
                              stream_callback=self._on_out)
        self._streams = [mic_s, ref_s, out_s]
        self._stop.clear()
        self._threads = [threading.Thread(target=self._work, daemon=True)]
        for t in self._threads:
            t.start()

    def stop(self):
        self._stop.set()
        for t in self._threads:
            t.join(timeout=1.5)
        self._threads = []
        # Callbacks are short and never block, so stop_stream()
        # only waits for any in-flight callback (microseconds) —
        # no wedge is possible, even on an unconsumed cable.
        for s in self._streams:
            try:
                s.stop_stream()
            except Exception:
                pass
            try:
                s.close()
            except Exception:
                pass
        self._streams = []
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
