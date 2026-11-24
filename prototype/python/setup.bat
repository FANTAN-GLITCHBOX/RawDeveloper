@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo === RawDeveloper setup ===
where py >nul 2>nul
if %errorlevel%==0 (set "PY=py -3") else (set "PY=python")
%PY% --version
if errorlevel 1 (
  echo Python not found. Install Python 3.11 or 3.12 from https://www.python.org/ and check "Add python.exe to PATH".
  pause
  exit /b 1
)
if not exist .venv %PY% -m venv .venv
call .venv\Scripts\activate.bat
python -m pip install --upgrade pip
pip install -r requirements.txt
if errorlevel 1 (
  echo Install failed. See the messages above.
  pause
  exit /b 1
)
pip install lensfunpy
if errorlevel 1 echo [warning] lensfunpy could not be installed. Lens profile correction will be disabled, everything else works.
echo.
echo Setup finished. Double-click run.bat to start.
pause
