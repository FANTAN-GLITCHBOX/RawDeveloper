@echo off
rem Console version (shows error messages)
cd /d "%~dp0"
if not exist .venv\Scripts\python.exe call setup.bat
.venv\Scripts\python.exe run.py %*
pause
