@echo off
REM ============================================================
REM ESPView M5-A: LVGL display backend verification entrypoint.
REM
REM   [1/3] host tests (shared/protocol + shared/display + shared/wifi)
REM         via MSYS2 MinGW64 (g++, cmake, ctest);
REM   [2/3] ESP32 build via ESP-IDF v6.0.2 (esp32/, LVGL app);
REM   [3/3] optional COM3 LVGL sanity (only when ESPVIEW_COM3 is set,
REM         e.g. `set ESPVIEW_COM3=COM3`). Requires the LVGL firmware
REM         to be flashed first (idf.py -p COM3 flash).
REM
REM ctest never requires COM3 / ESP32 / Qt GUI.
REM Requires: MSYS2 MinGW64; ESP-IDF v6.0.2 PowerShell profile
REM   (default C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1;
REM    override with ESPIDF_PROFILE)
REM Override the compiler bin dir with MINGW64_BIN if needed.
REM ============================================================
setlocal

if "%ESPIDF_PROFILE%"=="" set "ESPIDF_PROFILE=C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1"

if "%MINGW64_BIN%"=="" set "MINGW64_BIN=C:\msys64\mingw64\bin"
set "PATH=%MINGW64_BIN%;%PATH%"

REM COM3 sanity 需要 pyserial：默认用 ESP-IDF venv python（系统 python 通常没有）。
REM 可覆盖：set ESPVIEW_PYTHON=C:\path\python.exe
if "%ESPVIEW_PYTHON%"=="" set "ESPVIEW_PYTHON=C:\Espressif\tools\python\v6.0.2\venv\Scripts\python.exe"

set "ROOT=%~dp0.."
set "BUILD=%ROOT%\build\verify_lvgl"

echo [1/3] Configure + build host tests (protocol + display + wifi)
cmake -S "%ROOT%\shared\protocol" -B "%BUILD%\protocol" -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 goto :fail
cmake --build "%BUILD%\protocol" --target espview_protocol_tests scan_transaction_test -j 8
if errorlevel 1 goto :fail
ctest --test-dir "%BUILD%\protocol" --output-on-failure
if errorlevel 1 goto :fail

echo [2/3] ESP32 build (idf.py, LVGL app)
powershell -NoProfile -ExecutionPolicy Bypass -Command "& {. '%ESPIDF_PROFILE%'; Set-Location '%ROOT%\esp32'; idf.py build }"
if errorlevel 1 goto :fail

if "%ESPVIEW_COM3%"=="" goto :done
echo [3/3] COM3 LVGL sanity on %ESPVIEW_COM3%
"%ESPVIEW_PYTHON%" "%ROOT%\scripts\pc_com3_lvgl_sanity.py" --port %ESPVIEW_COM3% --baud 115200
if errorlevel 1 goto :fail

:done
echo.
echo verify_lvgl: ALL PASS
exit /b 0

:fail
echo.
echo verify_lvgl: FAILED
exit /b 1
