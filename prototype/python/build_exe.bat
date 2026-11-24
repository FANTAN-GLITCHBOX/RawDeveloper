@echo off
rem Build a standalone RawDeveloper.exe with PyInstaller (optional)
chcp 65001 >nul
cd /d "%~dp0"
call .venv\Scripts\activate.bat
pip install pyinstaller
pyinstaller --noconfirm --windowed --name RawDeveloper --collect-all rawpy --collect-all lensfunpy --collect-data exifread run.py
echo Output: dist\RawDeveloper\RawDeveloper.exe
pause
