@echo off
REM Build the standalone igcfs.exe on Windows.
REM
REM Requires Python 3.8+ on PATH (https://python.org - tick "Add to PATH").
REM Produces dist\igcfs.exe, a single file with no Python dependency. Double-
REM click users still run it from a terminal, e.g.:
REM     igcfs.exe --root C:\victor --dev COM1 --baud 38400 -v
REM
REM PyInstaller cannot cross-compile: run THIS script on Windows to get the .exe.

setlocal
cd /d "%~dp0"

if not exist .build-venv (
    echo Creating build venv...
    python -m venv .build-venv || goto :err
)

call .build-venv\Scripts\activate.bat || goto :err
python -m pip install --quiet --upgrade pip || goto :err
python -m pip install --quiet pyserial pyinstaller || goto :err

echo Building igcfs.exe...
pyinstaller --clean --noconfirm igcfs.spec || goto :err

echo.
echo Done. Executable: dist\igcfs.exe
goto :eof

:err
echo.
echo BUILD FAILED.
exit /b 1
