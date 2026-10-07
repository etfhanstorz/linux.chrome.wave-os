@echo off
cd /d "%~dp0"
echo Building wave-os...
wsl -d Ubuntu -- sh build.sh >build.log 2>&1
if errorlevel 1 (
  echo BUILD FAILED. See build.log
  pause
  exit /b 1
)
echo Build ok. Opening the flasher (admin needed to write to the USB stick)...
powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File ""%~dp0flash.ps1""'"
