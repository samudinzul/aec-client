@echo off
REM aec-web starter - ONE CLICK - local environment,
REM packages, server, GUI. Double-click this file.
REM No install, no admin rights. First run downloads
REM packages - about 1 min, needs internet. Later
REM runs start in seconds.
REM The server runs in THIS window - close it or
REM press Ctrl+C to stop. Click start.bat again any
REM time to reopen the GUI. Startup errors are also
REM logged to web\start.log.
REM
REM The web UI now uses Dear ImGui (WebAssembly) for a
REM native-like experience that matches the desktop version
REM exactly. The UI includes tabs, cards, custom widgets,
REM live controls, and a professional dark theme.
REM
setlocal
cd /d "%~dp0.." 2>nul
if errorlevel 1 (
  echo [-] ERROR - cannot find repo root.
  pause
  exit /b 1
)

REM --- find a usable Python - py launcher preferred, PATH fallback ---
set PY=
py -3 --version >nul 2>&1 && set PY=py -3
if not defined PY (
  python --version >nul 2>&1 && set PY=python
)
if not defined PY (
  echo [-] ERROR - no Python found.
  echo [-] Install Python 3.10-3.14 first - one time only.
  echo [-]   winget install -e --id Python.Python.3.12
  echo [-] Then close and reopen this window and try again.
  echo %DATE% %TIME% no-python >> web\start.log
  pause
  exit /b 1
)
%PY% -c "import sys; raise SystemExit(0 if (3,10) <= sys.version_info < (3,15) else 1)" >nul 2>&1
if errorlevel 1 (
  echo [-] ERROR - Python 3.10-3.14 required. Found -
  %PY% --version
  echo [-] Install 3.12 - winget install -e --id Python.Python.3.12
  pause
  exit /b 1
)

if not exist ".venv\Scripts\python.exe" (
  echo [-] creating local Python environment - first run only.
  %PY% -m venv .venv
  if errorlevel 1 (
    echo [-] ERROR - venv creation failed.
    echo %DATE% %TIME% venv-failed >> web\start.log
    pause
    exit /b 1
  )
)

call .venv\Scripts\activate
echo [-] installing packages - skipped if up to date.
python -m pip install -q --disable-pip-version-check -r web\requirements.txt
if errorlevel 1 (
  echo [-] ERROR - pip install failed - check your internet connection.
  echo %DATE% %TIME% pip-failed >> web\start.log
  pause
  exit /b 1
)

if not exist "models\dtln_aec_128_*.tflite" (
  echo [-] WARNING - echo-cancellation models missing from models.
  echo [-] The page will run, but audio passes through uncancelled.
)
REM --- one entry point: a native app window (Edge
REM --- WebView2 - no browser needed). Attaches to a
REM --- running server, or starts one in-process and
REM --- stops it when the window closes. Falls back to
REM --- the default browser if WebView2 is missing ---
REM --- The UI now uses Dear ImGui (WebAssembly) for a
REM --- native-like experience matching the desktop version
REM --- exactly, with tabs, cards, custom widgets, and live controls ---
echo [-] starting aec-web - native window opens when ready.
python -m web.gui
echo [-] aec-web stopped - exit code %ERRORLEVEL%.
pause
