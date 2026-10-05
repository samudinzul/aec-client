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

from .audio import AudioRunner, list_devices
from .chain import Chain

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)  # repo root: models/ lives here
UI_DIR = os.path.join(HERE, "ui")

app = FastAPI(title="aec-web")
chain = Chain(model_dir=os.path.join(ROOT, "models"))
runner = None
runner_lock = threading.Lock()
prefs = {"mic": None, "ref": None, "out": None,
         "nsEnabled": True, "trayEnabled": False}


@app.get("/")
def index():
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
    if "trayEnabled" in body:
        # Read by the native window (gui.py) to hide
        # to the system tray instead of quitting.
        prefs["trayEnabled"] = bool(body["trayEnabled"])
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
        if any(prefs[k] is None for k in ("mic", "ref", "out")):
            chain.stop()
            return {"ok": False,
                    "error": "pick a microphone, speaker reference "
                             "and output device first"}
        try:
            runner = AudioRunner(chain, prefs["mic"], prefs["ref"], prefs["out"])
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
    uvicorn.run(app, host="127.0.0.1", port=8000,
                log_level="info")


if __name__ == "__main__":
    main()
