@echo off
REM aec-web starter — ONE CLICK: local environment,
REM packages, server, GUI. Double-click this file.
REM No install, no admin rights. First run downloads
REM packages (~1 min, needs internet); later runs
REM start in seconds.
REM The server runs in its own "aec-web server"
REM window — close it (or Ctrl+C in it) to stop.
REM Click start.bat again any time to reopen the GUI.
REM Startup errors are also logged to web\start.log.
setlocal
cd /d "%~dp0.." 2>nul || (echo [aec-web] ERROR: cannot find repo root. & pause & exit /b 1)

REM --- find a usable Python (py launcher preferred, PATH fallback) ---
set PY=
py -3 --version >nul 2>&1 && set PY=py -3
if not defined PY (
  python --version >nul 2>&1 && set PY=python
)
if not defined PY (
  echo [aec-web] ERROR: no Python found.
  echo [aec-web] Install Python 3.10-3.14 first (one time):
  echo [aec-web]   winget install -e --id Python.Python.3.12
  echo [aec-web] Then close and reopen this window and try again.
  echo %DATE% %TIME% no-python >> web\start.log
  pause & exit /b 1
)
%PY% -c "import sys; raise SystemExit(0 if (3,10) <= sys.version_info < (3,15) else 1)" >nul 2>&1
if errorlevel 1 (
  echo [aec-web] ERROR: Python 3.10-3.14 required. Found:
  %PY% --version
  echo [aec-web] Install 3.12: winget install -e --id Python.Python.3.12
  pause & exit /b 1
)

if not exist ".venv\Scripts\python.exe" (
  echo [aec-web] creating local Python environment (first run only)...
  %PY% -m venv .venv
  if errorlevel 1 (
    echo [aec-web] ERROR: venv creation failed.
    echo %DATE% %TIME% venv-failed >> web\start.log
    pause & exit /b 1
  )
)

call .venv\Scripts\activate
echo [aec-web] installing packages (skipped if up to date)...
python -m pip install -q -r web\requirements.txt
if errorlevel 1 (
  echo [aec-web] ERROR: pip install failed. Check your internet connection.
  echo %DATE% %TIME% pip-failed >> web\start.log
  pause & exit /b 1
)

if not exist "models\dtln_aec_128_*.tflite" (
  echo [aec-web] WARNING: echo-cancellation models missing from models\.
  echo [aec-web] The page will run, but audio passes through uncancelled.
)

REM --- already running? just open the GUI ---
python -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8000/', timeout=1)" >nul 2>&1
if not errorlevel 1 (
  echo [aec-web] server already running — opening http://localhost:8000
  start "" "http://localhost:8000"
  exit /b 0
)

REM --- start the server in its own window, wait until it
REM --- answers, then open the GUI (never a dead tab) ---
echo [aec-web] starting server (log shows in the "aec-web server" window)...
start "aec-web server" cmd /c "python -m web.server & pause"

for /l %%i in (1,1,20) do (
  python -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8000/', timeout=1)" >nul 2>&1 && goto :open
  timeout /t 1 /nobreak >nul
)
echo [aec-web] WARNING: server did not answer within 20 s.
echo [aec-web] Check the "aec-web server" window for the error.
pause & exit /b 1

:open
echo [aec-web] ready — opening http://localhost:8000 ...
start "" "http://localhost:8000"
echo [aec-web] running. Stop: close the "aec-web server" window or press Ctrl+C in it.
echo [aec-web] Click start.bat again any time to reopen the GUI.
exit /b 0
