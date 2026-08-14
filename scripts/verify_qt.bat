@echo off
REM ============================================================
REM ESPView M2: Qt 6 Virtual Display host build check.
REM
REM Configures pc/ with ESPVIEW_BUILD_QT_GUI=ON and builds
REM espview_virtual_display (Qt 6 Widgets + SerialPort, C++17).
REM Does NOT require COM3 / ESP32 (GUI app runs only when the
REM user starts it against a real port).
REM Requires: MSYS2 MinGW64 with Qt 6 (C:\msys64\mingw64).
REM Override the compiler bin dir with MINGW64_BIN if needed.
REM ============================================================
setlocal

if "%MINGW64_BIN%"=="" set "MINGW64_BIN=C:\msys64\mingw64\bin"
set "PATH=%MINGW64_BIN%;%PATH%"

set "ROOT=%~dp0.."
set "BUILD=%ROOT%\build\verify_qt"

echo [1/3] Configure pc/ with Qt GUI target
cmake -S "%ROOT%\pc" -B "%BUILD%" -G "MinGW Makefiles" -DESPVIEW_BUILD_QT_GUI=ON
if errorlevel 1 goto :fail

echo [2/3] Build espview_virtual_display
cmake --build "%BUILD%" --target espview_virtual_display -j 8
if errorlevel 1 goto :fail

echo [3/3] Qt GUI target exists
if not exist "%BUILD%\espview_virtual_display.exe" goto :fail

echo.
echo verify_qt: ALL PASS (espview_virtual_display.exe)
exit /b 0

:fail
echo.
echo verify_qt: FAILED
exit /b 1
