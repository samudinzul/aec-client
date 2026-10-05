"""Native-window launcher — runs the aec-web server
in-process and opens it in a pywebview window
(Edge WebView2 on Windows; no browser needed).

Two modes, picked automatically:
  own    — no server running (first click of
           start.bat): preload the models, run
           uvicorn in a background thread, open
           the window once it answers. Closing the
           window stops the server — unless the
           Appearance tab's "minimize to system
           tray" is ticked, which hides the window
           to the tray instead (the server keeps
           running; the tray icon brings it back).
  attach — a server is already running (second
           click): just open the window; closing it
           leaves the server up.

Fallbacks to the default browser whenever pywebview
or the WebView2 runtime is missing, so the GUI never
depends on a single engine.

Run:  python -m web.gui   (from the repo root)
"""

import json
import threading
import time
import urllib.request
import webbrowser

PORT = 8000
URL = f"http://127.0.0.1:{PORT}/"
TITLE = "AEC Web — AEC Client"

_server = None  # uvicorn.Server, when this process owns one
_window = None  # pywebview window, once opened
_tray = None    # pystray.Icon, once the tray is on
_quitting = False


def _port_answers():
    try:
        urllib.request.urlopen(URL, timeout=1)
        return True
    except Exception:
        return False


def _prefs():
    """The server's prefs (the tray tick lives
    there); works in both own and attach mode."""
    try:
        with urllib.request.urlopen(
                f"http://127.0.0.1:{PORT}/api/prefs",
                timeout=1) as r:
            return json.loads(r.read())
    except Exception:
        return {}


def _serve():
    """Server thread: preload the models (the ~5 s
    part), then serve. Runs before the window opens,
    so Start is instant the moment the GUI appears."""
    global _server
    from .server import app, chain

    print("Loading models… (a few seconds)", flush=True)
    try:
        chain.preload()
    except Exception:
        pass  # /api/start surfaces the error
    import uvicorn
    # Server.run() skips signal installation when
    # not on the main thread (uvicorn does this
    # itself), so a background thread is safe.
    # Log at warning: the tray watcher polls
    # /api/prefs every second and the meters
    # stream over WebSocket — at info, every
    # request floods the console. Errors still
    # appear.
    _server = uvicorn.Server(
        uvicorn.Config(app, host="127.0.0.1",
                       port=PORT, log_level="warning"))
    _server.run()


def _tray_icon():
    """Draw the tray icon at runtime (green square +
    waveform bars, the UI's palette) — no image
    asset to ship."""
    from PIL import Image, ImageDraw
    img = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([4, 4, 60, 60], radius=14,
                        fill=(31, 111, 67, 255))
    d.rectangle([18, 25, 25, 39], fill=(255, 255, 255, 255))
    d.rectangle([29, 18, 36, 46], fill=(255, 255, 255, 255))
    d.rectangle([40, 27, 47, 37], fill=(255, 255, 255, 255))
    return img


def _start_tray():
    global _tray
    import pystray
    _tray = pystray.Icon(
        "aec-web", _tray_icon(), TITLE,
        pystray.Menu(
            pystray.MenuItem("Show", _tray_show),
            pystray.MenuItem("Quit", _tray_quit)))
    threading.Thread(target=_tray.run, daemon=True,
                     name="tray").start()


def _tray_show(icon=None, item=None):
    if _window is not None:
        _window.show()


def _tray_quit(icon=None, item=None):
    global _quitting
    _quitting = True
    if _tray is not None:
        _tray.visible = False
    if _window is not None:
        _window.destroy()


def _on_closing():
    """Returning False cancels the close (pywebview
    window.events.closing): hide to the tray instead
    of quitting when the Appearance tab asks for it."""
    if _quitting:
        return True
    if _prefs().get("trayEnabled"):
        _window.hide()
        return False
    return True


def _watch_prefs():
    """The tray tick lives in the server's prefs; show
    or remove the tray icon as it changes. Polling (not
    a callback) so it works in attach mode too, where
    the server runs in another process."""
    while not _quitting:
        try:
            want = bool(_prefs().get("trayEnabled"))
        except Exception:
            want = False
        if want and _tray is None:
            _start_tray()
        elif _tray is not None and _tray.visible != want:
            _tray.visible = want
        time.sleep(1.0)


def main():
    global _window
    owned = not _port_answers()
    server_thread = None
    if owned:
        server_thread = threading.Thread(
            target=_serve, daemon=True, name="server")
        server_thread.start()
        # The window opens only once the server
        # answers (models are preloaded first).
        for _ in range(120):  # ~30 s grace
            if _port_answers():
                break
            time.sleep(0.25)
        else:
            print("[-] ERROR - server did not start "
                  "(see the messages above).", flush=True)
            return 1
    else:
        print("[-] attached to the running server - "
              "close the window to exit.", flush=True)

    try:
        import webview
        _window = webview.create_window(
            TITLE, URL, width=720, height=780,
            min_size=(640, 480), resizable=True)
        _window.events.closing += _on_closing
        threading.Thread(target=_watch_prefs, daemon=True,
                         name="prefs").start()
        webview.start()
    except Exception as e:
        # No pywebview / no WebView2 runtime —
        # fall back to the default browser.
        print(f"[-] native window unavailable ({e}); "
              "opening the default browser.", flush=True)
        try:
            webbrowser.open(URL)
        except Exception:
            pass  # headless / no browser - the server
                  # is up; open the URL manually
        if owned:
            # Serve in the foreground (Ctrl+C stops).
            try:
                while True:
                    time.sleep(3600)
            except KeyboardInterrupt:
                pass
        return 0

    # Window closed.
    if owned:
        if _server is not None:
            _server.should_exit = True
        if server_thread is not None:
            server_thread.join(timeout=2.0)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
