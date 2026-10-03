@echo off
REM aec-web starter — double-click this file, or run web\start.bat from cmd.
REM No install, no admin rights. Creates a local venv on first run,
REM installs packages, starts the server, opens the GUI. Ctrl+C stops it.
REM If this window vanishes instantly, run it from an open terminal
REM instead (cmd.exe) so the error stays on screen — or check start.log.
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
  echo [aec-web] Install Python 3.10-3.13 first:
  echo [aec-web]   winget install -e --id Python.Python.3.12
  echo [aec-web] Then close and reopen your terminal and try again.
  echo %DATE% %TIME% no-python >> web\start.log
  pause & exit /b 1
)
%PY% -c "import sys; raise SystemExit(0 if (3,10) <= sys.version_info < (3,14) else 1)" >nul 2>&1
if errorlevel 1 (
  echo [aec-web] ERROR: Python 3.10-3.13 required. Found:
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

echo [aec-web] starting server — opening http://localhost:8000 ...
start "" "http://localhost:8000"
python -m web.server
echo [aec-web] server stopped (exit code %ERRORLEVEL%).
pause
