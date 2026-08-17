@echo off
REM ============================================================
REM ESPView M1-3D/M6-A: host-only verification entrypoint.
REM
REM Builds shared/protocol host unit tests, runs ctest,
REM builds pc tools (com3_frame_test ByteQueue self-test +
REM tcp_transport_test loopback; 127.0.0.1, no real Wi-Fi).
REM
REM Does NOT require: COM3, ESP32, Qt, ESP-IDF.
REM (Qt GUI target is skipped via -DESPVIEW_BUILD_QT_GUI=OFF;
REM  use verify_qt.bat for the Qt build check.)
REM Requires: MSYS2 MinGW64 (g++, cmake, ctest).
REM Override the compiler bin dir with MINGW64_BIN if needed:
REM   set MINGW64_BIN=C:\msys64\mingw64\bin
REM ============================================================
setlocal

if "%MINGW64_BIN%"=="" set "MINGW64_BIN=C:\msys64\mingw64\bin"
set "PATH=%MINGW64_BIN%;%PATH%"

set "ROOT=%~dp0.."
set "BUILD=%ROOT%\build\verify_host"

echo [1/5] Configure + build shared/protocol host tests
cmake -S "%ROOT%\shared\protocol" -B "%BUILD%\protocol" -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 goto :fail
cmake --build "%BUILD%\protocol" --target espview_protocol_tests scan_transaction_test -j 8
if errorlevel 1 goto :fail

echo [2/5] ctest (host protocol suite)
ctest --test-dir "%BUILD%\protocol" --output-on-failure
if errorlevel 1 goto :fail

echo [3/5] Configure + build pc tools (com3_frame_test + tcp_transport_test + transport_config_test; no COM3 needed)
cmake -S "%ROOT%\pc" -B "%BUILD%\pc" -G "MinGW Makefiles" -DESPVIEW_BUILD_QT_GUI=OFF
if errorlevel 1 goto :fail
cmake --build "%BUILD%\pc" --target com3_frame_test tcp_transport_test transport_config_test -j 8
if errorlevel 1 goto :fail

echo [3b/5] TransportConfig host tests (M6-D; invalid config / defaults)
"%BUILD%\pc\transport_config_test.exe"
if errorlevel 1 goto :fail

echo [4/5] ByteQueue self-test (--selftest-queue)
"%BUILD%\pc\com3_frame_test.exe" --selftest-queue
if errorlevel 1 goto :fail

echo [5/5] TCP loopback host tests (M6-A; 127.0.0.1, no real Wi-Fi)
"%BUILD%\pc\tcp_transport_test.exe"
if errorlevel 1 goto :fail

echo.
echo verify_host: ALL PASS
exit /b 0

:fail
echo.
echo verify_host: FAILED
exit /b 1
