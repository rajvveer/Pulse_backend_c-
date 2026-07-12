@echo off
REM build.bat — configure + build the Pulse C++ backend with the user-local
REM toolchain installed under %USERPROFILE% (no admin). Run from anywhere.
setlocal

set "PROJ=%~dp0.."
set "VCPKG_ROOT=%USERPROFILE%\vcpkg"
set "CMAKE=%USERPROFILE%\devtools\cmake-3.30.5-windows-x86_64\bin"
set "NINJA=%USERPROFILE%\devtools\ninja"
set "VCVARS=%USERPROFILE%\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

REM Put CMake + Ninja + the VS Installer (for vswhere, needed by vcvars) on PATH.
set "PATH=%CMAKE%;%NINJA%;C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"

REM Bring in the MSVC x64 environment (compiler, linker, Windows SDK).
call "%VCVARS%"
if errorlevel 1 (
  echo [build] Failed to initialize MSVC environment
  exit /b 1
)

cd /d "%PROJ%"

echo [build] Configuring (vcpkg will build dependencies on first run)...
cmake -B build -S . -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
  -DVCPKG_TARGET_TRIPLET=x64-windows ^
  -DVCPKG_OVERLAY_PORTS="%USERPROFILE%\vcpkg-overlays"
if errorlevel 1 (
  echo [build] CMake configure failed
  exit /b 1
)

echo [build] Building...
cmake --build build --config Release
if errorlevel 1 (
  echo [build] Build failed
  exit /b 1
)

echo [build] SUCCESS: build\pulse_backend.exe
endlocal
