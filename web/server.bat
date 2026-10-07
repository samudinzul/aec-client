@echo off
rem server.bat - lean aec-web launcher: server + YOUR OWN browser, no WebView2 window.
rem Same local environment as start.bat (creates .venv on first run, installs
rem packages), but instead of opening a native window it runs the server
rem minimized in the background and opens http://localhost:8000/ in your
rem default browser. Use this when you already run Edge/Chrome - no extra
rem renderer processes. Close the "aec-web server" window to stop.
rem Needs internet on the very first run only.
setlocal
cd /d "%~dp0.." 2>nul
if errorlevel 1 (
  echo [-] ERROR - cannot find repo root.
  pause
  exit /b 1
)

rem --- find a usable Python - py launcher preferred, PATH fallback ---
set PY=
py -3 --version >nul 2>&1 && set PY=py -3
if not defined PY (
  python --version >nul 2>&1 && set PY=python
)
if not defined PY (
  echo [-] ERROR - no Python found.
  echo [-] Install Python 3.10-3.14 first - one time only.
  echo [-]   winget install -e --id Python.Python.3.12
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

rem --- server in a minimized background window, browser up front ---
rem Already running (second double-click)? Attach to it instead of
rem starting a second copy that would die on the busy port.
curl -s -o nul http://127.0.0.1:8000/api/state
if not errorlevel 1 goto ready
start "aec-web server" /min python -m web.server
echo [-] waiting for the server (cold model preload takes a few seconds)...
for /l %%i in (1,1,30) do (
  curl -s -o nul http://127.0.0.1:8000/api/state
  if not errorlevel 1 goto ready
  timeout /t 2 >nul
)
echo [-] ERROR - server did not answer. See the "aec-web server" window.
pause
exit /b 1

:ready
start "" http://127.0.0.1:8000/
echo [-] aec-web is running in your browser. Close the "aec-web server"
echo [-] window to stop (its console holds the logs).
