@echo off
REM start.bat — run the Pulse C++ backend. Double-click it, or run `start.bat`
REM from a terminal. Must launch from the build dir so it finds its DLLs + config.
cd /d "%~dp0build"
echo [start] Launching pulse_backend.exe on port 3000 ...
pulse_backend.exe
