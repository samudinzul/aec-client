"""Native-window launcher — runs the aec-web server
in-process and opens it in a pywebview window
(Edge WebView2 on Windows; no browser needed).

Two modes, picked automatically:
  own    — no server running (first click of
           start.bat): preload the models, run
           uvicorn in a background thread, open
           the window once it answers. Closing the
           window stops the server.
  attach — a server is already running (second
           click): just open the window; closing it
           leaves the server up.

Fallbacks to the default browser whenever pywebview
or the WebView2 runtime is missing, so the GUI never
depends on a single engine.

Run:  python -m web.gui   (from the repo root)
"""

import threading
import time
import urllib.request
import webbrowser

PORT = 8000
URL = f"http://127.0.0.1:{PORT}/"
TITLE = "AEC Web — AEC Client"

_server = None  # uvicorn.Server, when this process owns one


def _port_answers():
    try:
        urllib.request.urlopen(URL, timeout=1)
        return True
    except Exception:
        return False


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
    _server = uvicorn.Server(
        uvicorn.Config(app, host="127.0.0.1",
                       port=PORT, log_level="info"))
    _server.run()


def main():
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
        webview.create_window(
            TITLE, URL, width=720, height=780,
            min_size=(640, 480), resizable=True)
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
