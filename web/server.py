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
prefs = {"mic": None, "ref": None, "out": None}


@app.get("/")
def index():
    return FileResponse(os.path.join(UI_DIR, "index.html"))


app.mount("/static", StaticFiles(directory=UI_DIR), name="static")


@app.get("/api/devices")
def devices():
    try:
        return list_devices()
    except Exception as e:
        return {"error": str(e), "capture": [], "playback": []}


@app.get("/api/state")
def state():
    st = chain.state()
    if runner is not None:
        st["inRms"] = runner.in_rms
        st["outRms"] = runner.out_rms
    return st


@app.post("/api/prefs")
def set_prefs(body: dict):
    for k in ("mic", "ref", "out"):
        if k in body:
            prefs[k] = body[k]
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
    try:
        while True:
            st = chain.state()
            if runner is not None:
                st["inRms"] = runner.in_rms
                st["outRms"] = runner.out_rms
            await ws.send_text(json.dumps({"type": "state", **st}))
            await asyncio.sleep(0.1)  # ~10 Hz meters
    except WebSocketDisconnect:
        pass


def main():
    import uvicorn
    uvicorn.run(app, host="127.0.0.1", port=8000, log_level="info")


if __name__ == "__main__":
    main()
