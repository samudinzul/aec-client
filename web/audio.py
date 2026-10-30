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

The chain stays 16 kHz. One worker thread assembles 128-sample
frames from the two input queues (resampling native -> 16 kHz at
the edge) and feeds the chain; its output lands in the queue the
output callback drains. Queues never block: input overflow drops
(ring-overrun semantics), a starved loopback reads as silence
(speakers idle — the desktop's underflow path), output starvation
emits silence.
"""

import queue
import sys
import threading

import numpy as np

try:
    import pyaudiowpatch as pa_patch
except ImportError:  # pragma: no cover
    pa_patch = None

from .dsp import (FRAME_SIZE, SAMPLE_RATE, i16_to_f32, rms)
from . import dsp  # soft_limit (tanh knee) is referenced as dsp.soft_limit

_Q_DEPTH = 8


def _pump_qos():
    """Real-time scheduling for the calling (pump) thread, Windows only.

    Mirrors what PortAudio does for its own callback threads: MMCSS
    "Pro Audio" task + time-critical priority, so the pump is scheduled
    like an audio thread instead of a normal Python worker fighting
    the browser, the WS loop, and whatever else is open. Returns a
    revert callable (call it when the thread exits, releasing the
    MMCSS slot) or None when QoS is unavailable. Fail-open everywhere:
    no QoS is always safe, just less glitch-resistant. Stdlib ctypes
    only — no new dependency, no binary, SECURITY.md-clean.
    """
    if sys.platform != "win32":
        return None
    try:
        import ctypes
        kernel32 = ctypes.windll.kernel32
        avrt = ctypes.windll.avrt
        task_index = ctypes.c_ulong(0)
        handle = avrt.AvSetMmThreadCharacteristicsW(
            "Pro Audio", ctypes.byref(task_index))
        if not handle:
            return None
        try:
            kernel32.SetThreadPriority(
                kernel32.GetCurrentThread(), 15)  # TIME_CRITICAL
        except Exception:
            pass

        def _revert():
            try:
                avrt.AvRevertMmThreadCharacteristics(handle)
            except Exception:
                pass
        return _revert
    except Exception:
        return None


def _process_qos(high):
    """Nudge the whole server process class while audio runs.

    ABOVE_NORMAL (not HIGH: never starve the system on a weak
    machine) on start, back to NORMAL on stop — best-effort, Windows
    only. The pump thread itself gets MMCSS separately (_pump_qos);
    this just keeps the GUI/WS threads from losing scheduling fights
    during a call.
    """
    if sys.platform != "win32":
        return
    try:
        import ctypes
        kernel32 = ctypes.windll.kernel32
        kernel32.SetPriorityClass(
            kernel32.GetCurrentProcess(),
            0x00008000 if high else 0x00000020)  # ABOVE_NORMAL / NORMAL
    except Exception:
        pass


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


def speaker_output_for_ref(ref_name, playback):
    """Playback index driving the same speakers a loopback ref
    listens to (desktop Listen-to-myself routing).

    Loopback entries are named "<speakers> [Loopback]"; strip
    the suffix and match the underlying playback endpoint
    (prefix-tolerant: MME truncates to 31 chars). Returns the
    device index, or None when no playback endpoint matches.
    """
    if not ref_name:
        return None
    base = ref_name
    if base.endswith(_LOOPBACK_SUFFIX):
        base = base[:-len(_LOOPBACK_SUFFIX)]
    for e in playback or []:
        if e.get("maxOutputChannels", 1) > 0 and _same_device(
                e.get("name", ""), base):
            return e["index"]
    return None


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
                msg = str(e)
                if "9992" in msg or "insufficient" in msg.lower():
                    msg = "a device is unavailable (replug it, then Refresh)"
                out["errors"]["loopback"] = f"loopback query failed: {msg}"

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
        msg = str(e)
        if "9992" in msg or "insufficient" in msg.lower() or "unavailable" in msg.lower():
            msg = "a device is unavailable (replug it, then Refresh)"
        out["errors"]["audio"] = f"device query failed: {msg}"
    return out


class AudioRunner:
    """Callback-mode streams + a 16 kHz worker pump.

    mic_idx / out_idx are pyaudiowpatch device indices;
    ref_idx is a pyaudiowpatch loopback index.
    """

    def __init__(self, chain, mic_idx=None, ref_idx=None, out_idx=None,
                 soundboard=None, sb_gain=1.0,
                 monitor_idx=None, monitor_gain=1.0, monitor_enabled=True):
        _need_pa()
        self.chain = chain
        self.mic_idx = int(mic_idx)
        self.ref_idx = int(ref_idx)
        self.out_idx = int(out_idx)
        # Soundboard (desktop parity): the mix is summed into the
        # chain output before the output resampler, so voice apps
        # hear mic + sounds together and nothing clips each other.
        self.soundboard = soundboard
        self.sb_gain = float(sb_gain)
        # Local monitor: a second playback device gets a copy of the
        # soundboard mix only (never the mic), so you can hear your
        # own pads while the cable still carries them to voice apps.
        # monitor_enabled is a live toggle — the stream stays open
        # but stops being fed when unticked (starves to silence).
        self.monitor_idx = monitor_idx
        self.monitor_gain = float(monitor_gain)
        self.monitor_enabled = bool(monitor_enabled)
        self._in_q = queue.Queue(maxsize=_Q_DEPTH)
        self._ref_q = queue.Queue(maxsize=_Q_DEPTH)
        self._out_q = queue.Queue(maxsize=_Q_DEPTH)
        self._mon_q = queue.Queue(maxsize=_Q_DEPTH)
        # Starvation telemetry: frames discarded per queue (see
        # _offer). Served in state() so dropouts are observable
        # instead of just audible. Benign races only (same pattern
        # as the rms floats below).
        self.dropped = {"in": 0, "ref": 0, "out": 0, "mon": 0}
        self._stop = threading.Event()
        self._threads = []
        self._pa = None
        self._streams = []
        self.in_rms = 0.0
        self.out_rms = 0.0
        self.ref_rms = 0.0
        # Stateful resamplers (one per stream — positions are stream
        # state and must never be shared). Built in start(), when the
        # real device rates are known; each callback/output thread
        # touches only its own, so no lock is needed. Replaces the old
        # stateless per-chunk mapping, whose phase reset every chunk
        # put a periodic seam on every engine.
        self._rs_mic = self._rs_ref = None
        self._rs_out = self._rs_mon = None
        self._mic_rate = 48000
        self._ref_rate = 48000
        self._out_rate = 48000
        self._out_carry = dsp._StreamBuf()
        self._mon_rate = 48000
        self._mon_carry = dsp._StreamBuf()
        # Last emitted callback frames (float, post-resample domain) —
        # starvation fades FROM these toward zero (desktop EmitFading
        # parity) instead of hard-cutting to silence, which clicks on
        # quiet speech tails. Callback-thread-only, like the carries.
        self._last_out = np.zeros(0, dtype=np.float32)
        self._last_mon = np.zeros(0, dtype=np.float32)

    # -------------------------------------------------------
    #  PortAudio callbacks (run on PortAudio's thread;
    #  they must never block or raise)
    # -------------------------------------------------------

    def _offer(self, q, item, key):
        """Enqueue one frame without ever blocking the audio thread.

        On a full queue the OLDEST frame is discarded to make room —
        desktop SPSC-ring parity (rings overwrite the oldest on
        overrun). Dropping the newest instead would punch a hole in
        the stream later; discarding stale audio keeps the pipe live
        at the cost of one dropped frame, counted in dropped[key].
        """
        try:
            q.put_nowait(item)
            return
        except queue.Full:
            pass
        try:
            q.get_nowait()  # discard oldest; consumer may have beaten us
        except queue.Empty:
            pass  # freed by the consumer meanwhile: no loss, no count
        else:
            self.dropped[key] += 1
        try:
            q.put_nowait(item)
        except queue.Full:
            pass

    def _on_mic(self, in_data, frame_count, time_info, status):
        try:
            pcm = np.frombuffer(in_data, dtype=np.int16).copy()
            if pcm.size:
                self._offer(self._in_q, pcm, "in")
        except Exception:
            pass
        return (None, pa_patch.paContinue)

    def _on_ref(self, in_data, frame_count, time_info, status):
        try:
            pcm = np.frombuffer(in_data, dtype=np.int16).copy()
            if pcm.size:
                self._offer(self._ref_q, pcm, "ref")
        except Exception:
            pass
        return (None, pa_patch.paContinue)

    @staticmethod
    def _fade_fill(out, n, last):
        """Fill out[n:] from the last emitted frame, fading to zero.

        Ports main.cpp EmitFading: out[i] = last[i] * (1 - i/m),
        so a starved tail decays from the waveform instead of
        stepping to 0 (the step is the click). `last` shorter than
        the gap contributes what it has; the rest stays silence —
        the fade already landed near zero by then.
        """
        m = len(out) - n
        if m <= 0:
            return
        k = min(m, len(last))
        if k > 0:
            fade = 1.0 - np.arange(n, n + k, dtype=np.float32) / len(out)
            out[n:n + k] = last[:k] * fade

    def _on_out(self, in_data, frame_count, time_info, status):
        try:
            carry = self._out_carry
            while len(carry) < frame_count:
                try:
                    frame = self._out_q.get_nowait()
                except queue.Empty:
                    break
                # No astype: the pump only queues float32 (chain output
                # converted once in _pump). Stateful exact-ratio
                # resampler (built in start()): chunked streaming is
                # seam-free, unlike the old per-chunk mapping.
                carry.append(self._rs_out(frame))
            # Drift trim (desktop mic/ref skip parity): the pump runs
            # on the mic clock while this callback runs on the output
            # clock; sustained skew would otherwise balloon the carry
            # into seconds of latency. Cap ~43 ms @48 kHz — normal
            # jitter never reaches it, true skew gets cut, not kept.
            if len(carry) > 2048:
                carry.pop(len(carry) - 2048)
            n = min(len(carry), frame_count)
            if n == frame_count:
                out = carry.pop(n)
            else:
                # Starved: fade from the last emitted frame (desktop
                # EmitFading) — Python GC/GIL pauses make the pump
                # starve more often than the C++ desktop ever does,
                # and a hard cut to zero clicks on quiet speech tails.
                out = np.zeros(frame_count, dtype=np.float32)
                if n:
                    out[:n] = carry.pop(n)
                self._fade_fill(out, n, self._last_out)
            self._last_out = out.copy()
            # Soft limiter (tanh knee at -3 dBFS), not a hard clip at 0 dBFS:
            # the resampled mix can crest past 1.0 and a hard clip is
            # square-wave distortion. Mirrors main.cpp SoftLimit.
            pcm = dsp.soft_limit(out).astype(np.int16).tobytes()
        except Exception:
            pcm = np.zeros(frame_count, dtype=np.int16).tobytes()
        return (pcm, pa_patch.paContinue)

    def _on_mon(self, in_data, frame_count, time_info, status):
        """Local monitor playback. Same drain-and-resample shape as
        _on_out, but reading the soundboard-only queue. Starves to
        silence, never blocks."""
        try:
            carry = self._mon_carry
            while len(carry) < frame_count:
                try:
                    frame = self._mon_q.get_nowait()
                except queue.Empty:
                    break
                # No astype (same float32-only-queue argument as _on_out).
                carry.append(self._rs_mon(frame))
            if len(carry) > 2048:
                carry.pop(len(carry) - 2048)
            n = min(len(carry), frame_count)
            if n == frame_count:
                out = carry.pop(n)
            else:
                # Starved: fade from the last emitted frame, same as
                # _on_out (a cut to zero clicks on the local speakers
                # just as it does on the cable).
                out = np.zeros(frame_count, dtype=np.float32)
                if n:
                    out[:n] = carry.pop(n)
                self._fade_fill(out, n, self._last_mon)
            self._last_mon = out.copy()
            # Soft limiter (tanh knee at -3 dBFS), not a hard clip at 0 dBFS:
            # the resampled mix can crest past 1.0 and a hard clip is
            # square-wave distortion. Mirrors main.cpp SoftLimit.
            pcm = dsp.soft_limit(out).astype(np.int16).tobytes()
        except Exception:
            pcm = np.zeros(frame_count, dtype=np.int16).tobytes()
        return (pcm, pa_patch.paContinue)

    # -------------------------------------------------------
    #  Worker: input queues (native rate) -> 16 kHz frames
    #  -> chain -> output queue
    # -------------------------------------------------------

    def _pump(self, mic_i16, ref_i16):
        # Chain output as float — the limiter runs ONCE, in the output
        # callback after resampling, mirroring main.cpp (SoftLimit is
        # the last step before the device). Limiting here too, then
        # quantizing to int16 and re-limiting in _on_out double-compressed
        # the signal (two tanh knees) and added a quantization step
        # before resampling — audible as crackle vs the desktop.
        out = cleaned = self.chain.process_frame(mic_i16, ref_i16).astype(np.float32) / 32768.0

        # Soundboard: mixed into the same frame the voice apps hear,
        # summed before the limiter so mic + sounds cannot clip each
        # other (desktop sums into SoftLimit the same way).
        sb = None
        if self.soundboard is not None:
            try:
                sb = self.soundboard.mix(FRAME_SIZE, SAMPLE_RATE)
            except Exception:
                sb = None  # fail-open: a broken pad never mutes the mic
        if sb is not None:
            out = out + sb * self.sb_gain
        # Monitor gets the soundboard alone — never the mic, or you
        # would hear yourself on top of the pads. Nothing is queued
        # while idle (sb is None): a starved callback emits the same
        # silence the zero-frames carried, so skipping them only removes
        # queue traffic + resampling — and a pad starts playing
        # immediately instead of behind a drain of stale zeros.
        if (self.monitor_idx is not None and self.monitor_enabled
                and sb is not None):
            self._offer(self._mon_q, sb * self.monitor_gain, "mon")

        self.in_rms = rms(mic_i16.astype(np.float32) / 32768.0)
        self.ref_rms = rms(ref_i16.astype(np.float32) / 32768.0)
        self.out_rms = rms(out)
        self._offer(self._out_q, out, "out")

    def _work(self):
        # Linear stream buffers (16 kHz domain): the old
        # concatenate-per-chunk + slice-copy-per-frame churn is gone;
        # appends copy into spare capacity, frames pop a head view.
        import gc
        gc.disable()  # eliminate GC pauses in the audio hot path
        mic_buf = dsp._StreamBuf()
        ref_buf = dsp._StreamBuf()
        qos_revert = _pump_qos()
        try:
            while not self._stop.is_set():
                self._work_step(mic_buf, ref_buf)
        finally:
            gc.enable()
            if qos_revert is not None:
                qos_revert()

    def _work_step(self, mic_buf, ref_buf):
        try:
            # 10 ms poll (was 50 ms): a hiccuped mic stalls the pump
            # one frame instead of five. Steady-state cost is nil —
            # get() returns immediately when data flows; the timeout
            # only bounds idle recovery. Cannot starve output.
            raw_mic = self._in_q.get(timeout=0.01)
        except queue.Empty:
            return
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
        mic_buf.append(self._rs_mic(dsp.i16_to_f32(raw_mic)))
        ref_buf.append(self._rs_ref(dsp.i16_to_f32(raw_ref)))
        while len(mic_buf) >= FRAME_SIZE and len(ref_buf) >= FRAME_SIZE:
            mic_i16 = (mic_buf.pop(FRAME_SIZE)
                       * 32767.0).astype(np.int16)
            ref_i16 = (ref_buf.pop(FRAME_SIZE)
                       * 32767.0).astype(np.int16)
            self._pump(mic_i16, ref_i16)

    def start(self):
        _process_qos(True)
        self._pa = pa_patch.PyAudio()
        self._mic_rate = int(self._pa.get_device_info_by_index(
            self.mic_idx).get("defaultSampleRate", 48000) or 48000)
        self._ref_rate = int(self._pa.get_device_info_by_index(
            self.ref_idx).get("defaultSampleRate", 48000) or 48000)
        self._out_rate = int(self._pa.get_device_info_by_index(
            self.out_idx).get("defaultSampleRate", 48000) or 48000)
        # Exact-ratio resamplers, one per stream (positions are stream
        # state and must never be shared or reused across restarts).
        self._rs_mic = dsp.LinearResampler(self._mic_rate / dsp.SAMPLE_RATE)
        self._rs_ref = dsp.LinearResampler(self._ref_rate / dsp.SAMPLE_RATE)
        self._rs_out = dsp.LinearResampler(dsp.SAMPLE_RATE / self._out_rate)
        self._rs_mon = dsp.LinearResampler(dsp.SAMPLE_RATE / self._mon_rate)
        self._out_carry = dsp._StreamBuf()
        mic_s = self._pa.open(format=pa_patch.paInt16, channels=1,
                              rate=self._mic_rate, input=True,
                              input_device_index=self.mic_idx,
                              frames_per_buffer=128,
                              stream_callback=self._on_mic)
        ref_s = self._pa.open(format=pa_patch.paInt16, channels=1,
                              rate=self._ref_rate, input=True,
                              input_device_index=self.ref_idx,
                              frames_per_buffer=128,
                              stream_callback=self._on_ref)
        out_s = self._pa.open(format=pa_patch.paInt16, channels=1,
                              rate=self._out_rate, output=True,
                              output_device_index=self.out_idx,
                              frames_per_buffer=128,
                              stream_callback=self._on_out)
        self._streams = [mic_s, ref_s, out_s]

        # Local monitor: fail-open — a device that refuses to open
        # must never block Start, the pads just stay in the cable.
        if self.monitor_idx is not None:
            try:
                self._mon_rate = int(self._pa.get_device_info_by_index(
                    self.monitor_idx).get("defaultSampleRate", 48000) or 48000)
                self._mon_carry = dsp._StreamBuf()
                self._rs_mon = dsp.LinearResampler(
                    dsp.SAMPLE_RATE / self._mon_rate)
                while not self._mon_q.empty():
                    try:
                        self._mon_q.get_nowait()
                    except queue.Empty:
                        break
                self._streams.append(self._pa.open(
                    format=pa_patch.paInt16, channels=1,
                    rate=self._mon_rate, output=True,
                    output_device_index=self.monitor_idx,
                    frames_per_buffer=128,
                    stream_callback=self._on_mon))
            except Exception:
                self.monitor_idx = None

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
        for q in (self._in_q, self._ref_q, self._out_q, self._mon_q):
            while not q.empty():
                try:
                    q.get_nowait()
                except queue.Empty:
                    break
        _process_qos(False)
