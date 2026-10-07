@echo off
cd /d "%~dp0"
echo Building the boot beacon test...
wsl -d Ubuntu -- sh build_beacon.sh >build.log 2>&1
if errorlevel 1 (
  echo BUILD FAILED. See build.log
  pause
  exit /b 1
)
echo Build ok. Starting the flasher (Windows will ask for admin to write to the USB stick)...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0flash.ps1" -Image beacon.kpart
echo.
pause