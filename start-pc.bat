@echo off
cd /d "%~dp0"
rem Starts the PC side of wave-os in two windows: the update server (the up command on the Chromebook) and the log server (writes wave-log.txt).
start "wave-os update server" cmd /c start-updateserver.bat
start "wave-os log server" cmd /c start-logserver.bat
