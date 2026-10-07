"""aec-web server — FastAPI REST + WebSocket, serves the browser UI.

Run from the repo root (so ``models/`` resolves)::

    pip install -r web/requirements.txt
    python -m web.server        # or: python web/server.py

Then open http://localhost:8000 — that page IS the GUI.
"""

import asyncio
import json
import os
import threading

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

from .audio import AudioRunner, list_devices, speaker_output_for_ref
from .chain import Chain

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)  # repo root: models/ lives here
UI_DIR = os.path.join(HERE, "ui")

app = FastAPI(title="aec-web")
chain = Chain(model_dir=os.path.join(ROOT, "models"))
runner = None
runner_lock = threading.Lock()
prefs = {"mic": None, "ref": None, "out": None,
         "nsEnabled": True, "micGain": 1.0,
         "trayEnabled": True, "listenToSelf": False}


@app.get("/")
def index():
    return FileResponse(os.path.join(UI_DIR, "imgui_web.html"))


@app.get("/imgui_web.js")
def imgui_js():
    # Direct route (not only /static/): the HTML references this
    # path relatively, so a missing route here was a 404 which
    # surfaced in the browser as "Failed to load DearImGui script".
    return FileResponse(os.path.join(UI_DIR, "imgui_web.js"),
                        media_type="application/javascript")


@app.get("/imgui_web.css")
def imgui_css():
    return FileResponse(os.path.join(UI_DIR, "imgui_web.css"),
                        media_type="text/css")


@app.get("/classic")
def classic():
    # Keep the old plain-HTML UI reachable for fallback/debug.
    return FileResponse(os.path.join(UI_DIR, "index.html"))


app.mount("/static", StaticFiles(directory=UI_DIR), name="static")


@app.get("/api/devices")
def devices():
    try:
        return list_devices()
    except Exception as e:
        return {"capture": [], "loopback": [], "playback": [],
                "defaults": {}, "errors": {"audio": str(e)}}


@app.get("/api/state")
def state():
    st = chain.state()
    st["listenToSelf"] = bool(prefs.get("listenToSelf"))
    if runner is not None:
        st["inRms"] = runner.in_rms
        st["outRms"] = runner.out_rms
    return st


@app.get("/api/prefs")
def get_prefs():
    return prefs


@app.post("/api/prefs")
def set_prefs(body: dict):
    for k in ("mic", "ref", "out"):
        if k in body:
            prefs[k] = body[k]
    if "nsEnabled" in body:
        # Live toggle (unlike devices): skipping the NS
        # stage needs no device reconfigure, so it
        # applies to the running chain immediately.
        prefs["nsEnabled"] = bool(body["nsEnabled"])
        chain.ns_enabled = prefs["nsEnabled"]
    if "micGain" in body:
        # Live preamp gain, linear, clamped to 0.1x..4.0x
        # (the UI shows 0..200%, like the desktop client);
        # applies to the running chain immediately.
        try:
            g = float(body["micGain"])
        except (TypeError, ValueError):
            g = 1.0
        g = min(4.0, max(0.1, g))
        prefs["micGain"] = g
        chain.mic_gain = g
    if "trayEnabled" in body:
        # Read by the native window (gui.py) to hide
        # to the system tray instead of quitting.
        prefs["trayEnabled"] = bool(body["trayEnabled"])
    if "listenToSelf" in body:
        # Stored only (like devices): the output stream
        # opens at Start, so this applies after Stop -> Start.
        prefs["listenToSelf"] = bool(body["listenToSelf"])
    return prefs


@app.post("/api/start")
def start():
    global runner
    with runner_lock:
        if chain.running:
            return {"ok": True, "state": chain.state()}
        if not chain.start():
            return {"ok": False, "error": chain.last_error or
                    "Failed to load DTLN model"}
        chain.ns_enabled = prefs.get("nsEnabled", True)
        chain.mic_gain = prefs.get("micGain", 1.0)
        if any(prefs[k] is None for k in ("mic", "ref", "out")):
            chain.stop()
            return {"ok": False,
                    "error": "pick a microphone, speaker reference "
                             "and output device first"}
        out_idx = prefs["out"]
        if prefs.get("listenToSelf"):
            # Route the cleaned mic to the speakers the ref
            # loopback listens to (desktop Listen-to-myself):
            # hear yourself for testing instead of feeding
            # the voice app. Fail closed with a clear error
            # rather than silently sending it elsewhere.
            devs = list_devices()
            ref_name = next((e["name"] for e in devs["loopback"]
                             if e["index"] == prefs["ref"]), "")
            out_idx = speaker_output_for_ref(
                ref_name, devs["playback"])
            if out_idx is None:
                chain.stop()
                return {"ok": False,
                        "error": "Listen to myself: no speaker output "
                                 f"matches '{ref_name or '?'}' — untick "
                                 "it or pick the speakers as Output"}
        try:
            runner = AudioRunner(chain, prefs["mic"], prefs["ref"], out_idx)
            runner.start()
        except Exception as e:
            chain.stop()
            runner = None
            return {"ok": False, "error": f"audio failed to start: {e}"}
        return {"ok": True, "state": chain.state()}


@app.post("/api/stop")
def stop():
    global runner
    with runner_lock:
        if runner is not None:
            try:
                runner.stop()
            except Exception:
                pass
            runner = None
        chain.stop()
        return {"ok": True, "state": chain.state()}


@app.websocket("/ws")
async def ws_meters(ws: WebSocket):
    await ws.accept()
    while True:
        try:
            st = chain.state()
            st["listenToSelf"] = bool(prefs.get("listenToSelf"))
            # Snapshot: the stop endpoint nulls `runner`
            # from a worker thread; reading it after the
            # None-check raised and dropped the socket on
            # every Stop (the "server disconnected" bug).
            r = runner
            if r is not None:
                st["inRms"] = r.in_rms
                st["outRms"] = r.out_rms
            await ws.send_text(json.dumps({"type": "state", **st}))
        except WebSocketDisconnect:
            break
        except Exception:
            pass  # transient: keep the meter stream alive
        await asyncio.sleep(0.1)  # ~10 Hz meters


def main():
    import uvicorn
    # Load the DTLN pairs BEFORE the port opens:
    # the browser (and the Start button) then see
    # a fully-loaded server, so Start is instant
    # on the first process too — without this, a
    # quick Start click waits out the ~5 s model
    # load (cold disk cache; a second process is
    # fast only because the cache is warm).
    print("Loading models… (a few seconds)",
          flush=True)
    chain.preload()
    # Single worker, always: extra workers would duplicate the
    # preloaded models in RAM and fight over the one audio pump.
    uvicorn.run(app, host="127.0.0.1", port=8000,
                workers=1, log_level="info")


if __name__ == "__main__":
    main()
