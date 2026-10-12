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
            tray" is ticked (default), which hides
            the window to the tray instead (the
            server keeps running; the tray icon
            brings it back). With the tray on, the
            console hides once the page loads —
            only the window is visible.
  attach — a server is already running (second
           click): just open the window; closing it
           leaves the server up.

Fallbacks to the default browser whenever pywebview
or the WebView2 runtime is missing, so the GUI never
depends on a single engine.

Run:  python -m web.gui   (from the repo root)
"""

import json
import os
import sys
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
    part) and scan the soundboard, then serve. Runs
    before the window opens, so Start and the
    soundboard tab are instant the moment the GUI
    appears."""
    global _server
    from .server import app, chain, board
    from .soundboard import decoders_available

    print("Loading models… (a few seconds)", flush=True)
    try:
        chain.preload()
    except Exception:
        pass  # /api/start surfaces the error
    try:
        n = len(board.scan())
        print(f"Soundboard: dir={board.display_dir} "
              f"decoders={decoders_available()} loaded={n}",
              flush=True)
    except Exception as e:
        print(f"Soundboard: scan failed ({e})", flush=True)
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
                       port=PORT, workers=1, log_level="warning"))
    _server.run()


def _console_minimize():
    """While the window is trayed, hide the
    console completely — no taskbar button at
    all, only the tray icon is visible, like
    any tray app. (SW_SHOW brings it back,
    scrollback intact.)"""
    if sys.platform != "win32":
        return
    if "WT_SESSION" in os.environ:
        # Inside Windows Terminal the visible
        # window belongs to wt.exe and holds the
        # user's own tabs - never touch it. The
        # console here is headless (nothing to
        # hide), so say so instead.
        print("[-] running inside Windows Terminal - "
              "its window stays (it holds your "
              "tabs). Double-click start.bat for "
              "the hidden, tray-only experience, "
              "or minimize the terminal yourself.",
              flush=True)
        return
    try:
        import ctypes
        user32 = ctypes.windll.user32
        hwnd = ctypes.windll.kernel32.GetConsoleWindow()
        user32.ShowWindow(hwnd, 0)  # SW_HIDE
        user32.SetWindowPos(
            hwnd, 0, 0, 0, 0, 0,
            0x0080 | 0x0001 | 0x0002 | 0x0010)
        # SWP_HIDEWINDOW|SWP_NOSIZE|SWP_NOMOVE
        # |SWP_NOACTIVATE
    except Exception:
        pass


def _console_restore():
    if sys.platform != "win32":
        return
    try:
        import ctypes
        ctypes.windll.user32.ShowWindow(
            ctypes.windll.kernel32.GetConsoleWindow(),
            5)  # SW_SHOW
    except Exception:
        pass


def _set_window_icon():
    """Point the native window (and console) at app.ico via WM_SETICON.

    pywebview exposes no icon parameter, so the window otherwise shows
    python.exe's icon in the taskbar. A background thread waits for the
    top-level window by title, then sets the big (taskbar) and small
    (title-bar) icons from web/ui/favicon.ico. Fail-open: any error
    (or a missing file) leaves the Python icon in place. The icon
    handles stay alive for the life of the process (never destroyed).
    Windows only.
    """
    if sys.platform != "win32":
        return
    here = os.path.dirname(os.path.abspath(__file__))
    ico = os.path.join(here, "ui", "favicon.ico")
    if not os.path.isfile(ico):
        ico = os.path.join(here, "..", "resources", "app.ico")
    ico = os.path.abspath(ico)
    if not os.path.isfile(ico):
        return

    def worker():
        try:
            import ctypes
            user32 = ctypes.windll.user32
            WM_SETICON, ICON_SMALL, ICON_BIG = 0x80, 0, 1
            IMAGE_ICON, LR_LOADFROMFILE = 1, 0x0010

            def set_icon(hwnd):
                try:
                    h_big = user32.LoadImageW(
                        None, ico, IMAGE_ICON, 32, 32, LR_LOADFROMFILE)
                    h_small = user32.LoadImageW(
                        None, ico, IMAGE_ICON, 16, 16, LR_LOADFROMFILE)
                    if h_big:
                        user32.SendMessageW(hwnd, WM_SETICON, ICON_BIG, h_big)
                    if h_small:
                        user32.SendMessageW(hwnd, WM_SETICON, ICON_SMALL, h_small)
                except Exception:
                    pass

            # The pywebview window (taskbar button + title bar).
            hwnd = None
            for _ in range(120):  # ~30 s grace
                hwnd = user32.FindWindowW(None, TITLE)
                if hwnd:
                    break
                time.sleep(0.25)
            if hwnd:
                set_icon(hwnd)
            # The console window too (visible when the tray is off),
            # so both taskbar buttons match instead of mixing icons.
            try:
                con = ctypes.windll.kernel32.GetConsoleWindow()
                if con:
                    set_icon(con)
            except Exception:
                pass
        except Exception:
            pass

    threading.Thread(target=worker, daemon=True, name="window-icon").start()


def _tray_icon():
    """App icon (web/ui/favicon.ico, a copy of resources/app.ico)
    when available — same brand as the desktop exe/tray — else the
    drawn green-square fallback, so the tray never depends on an
    image asset being present."""
    from PIL import Image
    here = os.path.dirname(os.path.abspath(__file__))
    for cand in (os.path.join(here, "ui", "favicon.ico"),
                 os.path.join(here, "..", "resources", "app.ico")):
        try:
            if os.path.isfile(cand):
                img = Image.open(cand)
                img.load()
                return img.convert("RGBA").resize((64, 64), Image.LANCZOS)
        except Exception:
            pass
    from PIL import ImageDraw
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
        _console_restore()


def _tray_quit(icon=None, item=None):
    global _quitting
    _quitting = True
    if _tray is not None:
        _tray.visible = False
    _console_restore()  # the exit messages +
    if _window is not None:  # pause must be seen
        _window.destroy()


def _on_closing():
    """Returning False cancels the close (pywebview
    window.events.closing): hide to the tray instead
    of quitting when the Appearance tab asks for it."""
    if _quitting:
        return True
    if _prefs().get("trayEnabled"):
        _window.hide()
        _console_minimize()
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

    hide_timer = None
    try:
        import webview
        _window = webview.create_window(
            TITLE, URL, width=720, height=780,
            min_size=(640, 480), resizable=True)
        _window.events.closing += _on_closing
        if _prefs().get("trayEnabled"):
            # App-like: once the page loads, the
            # console hides (no taskbar button) —
            # only the native window is visible.
            # (Tray off keeps the console for logs
            # and Ctrl+C.) The timer is a fallback
            # in case the loaded event never fires.
            _window.events.loaded += (
                lambda *a: _console_minimize())
            hide_timer = threading.Timer(
                4.0, _console_minimize)
            hide_timer.daemon = True
            hide_timer.start()
        threading.Thread(target=_watch_prefs, daemon=True,
                         name="prefs").start()
        # Runtime window icon (taskbar + title bar): pywebview has no
        # icon parameter, so a watcher thread sets WM_SETICON once the
        # window exists. Fail-open — Python's icon stays on any error.
        _set_window_icon()
        webview.start()
    except Exception as e:
        if hide_timer is not None:
            hide_timer.cancel()
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
