@echo off
setlocal
cd /d "%~dp0"

set "BIN=StopWatch_Pedometer_v2.8.bin"
set "APP=app_only.bin"
set "KEEP="
if /i "%~2"=="keep" set "KEEP=1"

if defined KEEP (
    if not exist "%APP%" (
        echo [x] firmware not found: %APP%
        pause
        exit /b 1
    )
) else (
    if not exist "%BIN%" (
        echo [x] firmware not found: %BIN%
        pause
        exit /b 1
    )
)

set "PORT="
if not "%~1"=="" set "PORT=-p %~1"

set ESP=
if exist "%~dp0esptool.exe" set ESP="%~dp0esptool.exe"
if not defined ESP for /d %%D in ("%LOCALAPPDATA%\Arduino15\packages\esp32\tools\esptool_py\*") do if not defined ESP if exist "%%D\esptool.exe" set ESP="%%D\esptool.exe"
if defined ESP goto :found

python -m esptool version >nul 2>&1
if not errorlevel 1 set ESP=python -m esptool
if defined ESP goto :found

py -3 -m esptool version >nul 2>&1
if not errorlevel 1 set ESP=py -3 -m esptool
if defined ESP goto :found

echo [x] esptool not found.
echo     python -m pip install esptool
pause
exit /b 1

:found
echo.
echo esptool : %ESP%
if defined PORT (echo port    : %~1) else (echo port    : auto-detect)
echo.
echo Download mode: hold power button about 2 s, release when the LED lights.
echo Do not unplug until VERIFY OK is printed.
echo.
if defined KEEP goto :upgrade

echo mode    : full image at 0x0  (this ERASES settings and step history)
echo image   : %BIN%
echo.
echo [1/3] erase flash
%ESP% --chip esp32s3 %PORT% -b 460800 erase_flash
if errorlevel 1 goto :fail

echo.
echo [2/3] write image
%ESP% --chip esp32s3 %PORT% -b 460800 write_flash --flash_mode keep --flash_freq keep --flash_size keep 0x0 "%BIN%"
if errorlevel 1 goto :fail

echo.
echo [3/3] verify
%ESP% --chip esp32s3 %PORT% -b 460800 verify_flash 0x0 "%BIN%"
if errorlevel 1 goto :fail
goto :done

:upgrade
echo mode    : app only at 0x10000  (settings and step history are KEPT)
echo image   : %APP%
echo.
echo [1/2] write app
%ESP% --chip esp32s3 %PORT% -b 460800 write_flash --flash_mode keep --flash_freq keep --flash_size keep 0x10000 "%APP%"
if errorlevel 1 goto :fail

echo.
echo [2/2] verify
%ESP% --chip esp32s3 %PORT% -b 460800 verify_flash 0x10000 "%APP%"
if errorlevel 1 goto :fail

:done
echo.
echo VERIFY OK. Press the power button once to reboot.
pause
exit /b 0

:fail
echo.
echo FAILED at this step. If the log above contains "DEVICE LOST", the cable or the
echo download mode dropped: put the watch back in download mode and run again.
echo.
echo Ports: %ESP% chip_id  (no -p means auto)
pause
exit /b 1
