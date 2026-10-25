@echo off
setlocal
cd /d "%~dp0"

set "PY="
where py >nul 2>nul && set "PY=py -3"
if not defined PY where python >nul 2>nul && set "PY=python"
if not defined PY (
    echo [fail] Python 3 not found.
    echo        https://www.python.org/downloads/
    exit /b 1
)

%PY% tools\build.py %*
if errorlevel 1 (
    echo.
    echo [fail] Build failed.
    exit /b 1
)

echo.
echo [ok] Firmware is in out\
exit /b 0
