@echo off
cd /d "%~dp0"
echo Building wave-os...
wsl -d Ubuntu -- sh build.sh >build.log 2>&1
if errorlevel 1 (
  echo BUILD FAILED. See build.log
  pause
  exit /b 1
)
echo Running it on the fake Chromebook (fakehana)...
echo.
wsl -d Ubuntu -- sh -c "cd fakehana && python3 fakehana.py ../out.kpart %*"
echo.
echo Screen picture: fakehana\fakehana-screen.png
pause
