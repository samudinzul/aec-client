"""Soundboard — mirrors the desktop soundboard (src/main.cpp).

Sounds live in a ``sounds/`` folder inside the ``web/`` package dir.
Files are decoded once into mono float32 at their native rate and
played back by linear interpolation into the 16 kHz pump, so a pad
click never touches the disk mid-call and never blocks an audio
callback.

One sound at a time: clicking a pad takes over from whatever is still
sounding (desktop parity — the C++ side collapsed to a single voice).

Decoding: the stdlib ``wave`` module always works for PCM WAV; when the
optional ``soundfile`` package is installed, MP3 / FLAC / OGG work too,
matching the desktop's miniaudio decoder.

Threading: ``mix()`` is called only from the pump thread; the API
threads set ``request`` and read ``playing``. Python's GIL makes those
single-attribute handoffs safe without a lock.
"""

import os
import threading
import wave

import numpy as np

try:  # MP3 / FLAC / OGG + float & 24-bit WAV
    import soundfile as _sf
except (ImportError, OSError) as _e:  # OSError: libsndfile missing
    _sf = None
    import sys
    print(f"Soundboard: soundfile unavailable ({_e}) — "
          "MP3/FLAC/OGG pads disabled, WAV only. "
          "Fix: pip install soundfile", file=sys.stderr, flush=True)

# web/ package dir — sounds/ sits next to server.py inside it.
PKG = os.path.dirname(os.path.abspath(__file__))
SOUNDS_DIR = os.path.join(PKG, "sounds")

MAX_SOUNDS = 32
MAX_SECONDS = 60  # longer files are truncated, like the desktop

_WAV_EXT = {".wav"}
_SF_EXT = {".mp3", ".flac", ".ogg", ".oga", ".opus", ".aiff", ".aif"}


def decoders_available():
    """Which extensions this install can actually decode."""
    return sorted(_WAV_EXT | _SF_EXT) if _sf is not None else sorted(_WAV_EXT)


def _decode_wav(path):
    """PCM WAV via the stdlib — no third-party dependency."""
    try:
        with wave.open(path, "rb") as w:
            rate = w.getframerate()
            channels = w.getnchannels()
            width = w.getsampwidth()
            raw = w.readframes(w.getnframes())
    except Exception:
        return None, 0
    if rate <= 0 or not raw:
        return None, 0
    if width == 2:
        a = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
    elif width == 4:
        # wave exposes no format tag, so 32-bit is read as int32;
        # float32 WAVs need soundfile (it detects them properly).
        a = np.frombuffer(raw, dtype="<i4").astype(np.float32) / 2147483648.0
    elif width == 1:
        a = ((np.frombuffer(raw, dtype=np.uint8).astype(np.float32) - 128.0)
             / 128.0)
    elif width == 3:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v & 0x800000, v - 0x1000000, v)
        a = v.astype(np.float32) / 8388608.0
    else:
        return None, 0
    if channels > 1:
        a = a[:(a.size // channels) * channels].reshape(-1, channels).mean(axis=1)
    return a.astype(np.float32), int(rate)


def _decode(path):
    """Decode one file to mono float32 at its native rate."""
    if _sf is not None:
        try:
            data, rate = _sf.read(path, dtype="float32", always_2d=True)
            if data.size and int(rate) > 0:
                return data.mean(axis=1).astype(np.float32), int(rate)
        except Exception:
            if os.path.splitext(path)[1].lower() not in _WAV_EXT:
                return None, 0  # not a format the stdlib can try either
    return _decode_wav(path)


class SoundBoard:
    """Pad list + the single playback voice.

    The voice is owned by the pump thread (only ``mix()`` touches it);
    everything else is request/handoff state.
    """

    def __init__(self, sounds_dir=SOUNDS_DIR):
        self.sounds_dir = sounds_dir
        self.sounds = []      # [{"name","path","pcm","rate"}]
        self.request = None   # ("play", i) | ("stop", i) | ("stopall",) | None
        # playing is a *property*: it always reflects self._voice, so a
        # stopAll() clears the pad highlight instantly instead of waiting
        # for the pump to consume the request. (The old mutable list could
        # stay lit while mix() was starved on a silent mic, and mix() could
        # re-light it after a stop had already cleared it — both observed
        # as "Stop all doesn't stop".)
        self._voice = None    # {"idx": int, "pos": float} — pump thread only
        self._scan_lock = threading.Lock()  # concurrent Refresh clicks

    @property
    def playing(self):
        return [self._voice["idx"]] if self._voice is not None else []

    # ----------------------------------------------------------
    #  Loading (any thread; UI only calls it while stopped)
    # ----------------------------------------------------------

    def scan(self):
        """Rescan the sounds folder. Stop-only: the running voice
        indexes into self.sounds, so the list must not move under it."""
        with self._scan_lock:
            self.request = None
            self._voice = None
            self.sounds = []

            exts = set(decoders_available())
            root = self.sounds_dir
            if not os.path.isdir(root):
                print(f"Soundboard: dir missing: {root}", flush=True)
                return self.list()

            files = []
            skipped = []
            for name in sorted(os.listdir(root)):
                path = os.path.join(root, name)
                if not os.path.isfile(path):
                    continue
                ext = os.path.splitext(name)[1].lower()
                if ext in exts:
                    files.append(path)
                else:
                    skipped.append(name)
            decoded = 0
            for path in files[:MAX_SOUNDS]:
                pcm, rate = _decode(path)
                if pcm is None or pcm.size == 0:
                    print(f"Soundboard: decode failed: {path}", flush=True)
                    continue
                cap = int(MAX_SECONDS * rate)
                if pcm.size > cap:
                    pcm = pcm[:cap]
                self.sounds.append({
                    "name": os.path.splitext(os.path.basename(path))[0],
                    "path": path,
                    "pcm": pcm,
                    "rate": rate,
                })
                decoded += 1
            if skipped:
                print(f"Soundboard: skipped (unsupported ext): {skipped}",
                      flush=True)
            print(f"Soundboard: {decoded} loaded / {len(files)} matched "
                  f"in {root}", flush=True)
            return self.list()

    def list(self):
        return [{"index": i, "name": s["name"], "path": s["path"]}
                for i, s in enumerate(self.sounds)]

    # ----------------------------------------------------------
    #  Triggering (any thread)
    # ----------------------------------------------------------

    def play(self, index):
        if not isinstance(index, int) or not (0 <= index < len(self.sounds)):
            return False
        self.request = ("play", index)
        return True

    def stop(self, index=None):
        self.request = ("stop", index) if index is not None else ("stopall",)

    def stop_all(self):
        """Stop everything now, not on the next pump tick.

        Sets the request for the pump to drain (so a running voice is
        stopped even if mix() is mid-frame) AND nulls the voice
        directly. The pad highlight reads ``playing``, which is a
        property over ``_voice`` — a request alone stays lit while the
        pump is starved on a silent mic, which is exactly the "Stop all
        doesn't stop" symptom."""
        self.request = ("stopall",)
        self._voice = None

    def reset(self):
        """Force-clear the voice immediately (session Stop). Unlike
        stop_all(), which the next mix() consumes, this takes effect
        now — no pump thread is running to drain the request, so a
        sounding pad must not survive Stop and resume on next Start."""
        self.request = None
        self._voice = None

    # ----------------------------------------------------------
    #  Mixing (pump thread only)
    # ----------------------------------------------------------

    def mix(self, n_frames, rate):
        """One frame of soundboard audio at `rate`, -1..1.

        Returns None when nothing is sounding, so the caller can skip
        the extra conversion entirely.
        """
        req, self.request = self.request, None
        if req is not None:
            kind = req[0]
            if kind == "stopall":
                self._voice = None
            elif kind == "stop":
                if self._voice is not None and self._voice["idx"] == req[1]:
                    self._voice = None
            elif kind == "play":
                if 0 <= req[1] < len(self.sounds):
                    # One sound at a time: the new click takes over.
                    self._voice = {"idx": req[1], "pos": 0.0}

        v = self._voice
        if v is None:
            return None
        snd = self.sounds[v["idx"]]
        pcm = snd["pcm"]
        if pcm.size == 0:
            self._voice = None
            return None

        # Linear interpolation, vectorised: a Python per-sample loop
        # would burn ~16k iterations/second at 128 frames x 125 Hz.
        step = snd["rate"] / float(rate)
        pos = v["pos"]
        p = pos + np.arange(n_frames, dtype=np.float64) * step
        k = int(np.count_nonzero(p < pcm.size))
        out = np.zeros(n_frames, dtype=np.float32)
        if k:
            pv = p[:k]
            i0 = pv.astype(np.int64)
            frac = (pv - i0).astype(np.float32)
            i1 = np.minimum(i0 + 1, pcm.size - 1)
            s0 = pcm[i0]
            out[:k] = s0 + (pcm[i1] - s0) * frac
        pos += k * step

        if pos >= pcm.size:
            self._voice = None
        else:
            v["pos"] = pos
        return out
