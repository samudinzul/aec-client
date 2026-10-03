@echo off
REM aec-web starter — double-click this file. No install, no admin rights.
REM Creates a local venv on first run, installs packages, starts the server,
REM then opens the GUI in your browser. Ctrl+C in this window stops it.
setlocal
cd /d "%~dp0.."

if not exist ".venv\Scripts\python.exe" (
  echo [aec-web] creating local Python environment (first run only)...
  python -m venv .venv || (echo [aec-web] ERROR: install Python 3.10+ from python.org first. & pause & exit /b 1)
)

call .venv\Scripts\activate
echo [aec-web] installing packages (skipped if up to date)...
pip install -q -r web\requirements.txt || (echo [aec-web] ERROR: pip install failed. & pause & exit /b 1)

echo [aec-web] starting server — opening http://localhost:8000 ...
start "" "http://localhost:8000"
python -m web.server
pause
