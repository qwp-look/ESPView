@echo off
REM ============================================================
REM ESPView M8-A6: local benchmark runner (host only, no hardware).
REM
REM   scripts\run_bench.bat [--quick]
REM
REM Builds both bench executables under build\bench, runs
REM espview_protocol_bench (full, or --quick smoke) and
REM espview_m8a4_bench, writes CSVs to build\bench\results\, then
REM compares against the committed baselines (full mode only) and
REM enforces the stream_encode alloc_count=0 gate.
REM Requires: MSYS2 MinGW64 (g++, cmake).
REM Override compiler bin dir with MINGW64_BIN if needed.
REM ============================================================
setlocal

if "%MINGW64_BIN%"=="" set "MINGW64_BIN=C:\msys64\mingw64\bin"
set "PATH=%MINGW64_BIN%;%PATH%"

set "ROOT=%~dp0.."
set "BUILD=%ROOT%\build\bench"
set "QUICK="
if "%1"=="--quick" set "QUICK=--quick"

echo [1/4] Configure + build bench targets
cmake -S "%ROOT%\shared\protocol" -B "%BUILD%\protocol" -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 goto :fail
cmake --build "%BUILD%\protocol" --target espview_protocol_bench espview_m8a4_bench -j 8
if errorlevel 1 goto :fail

if not exist "%BUILD%\results" mkdir "%BUILD%\results"

echo [2/4] Run protocol benchmark (%QUICK%)
"%BUILD%\protocol\espview_protocol_bench.exe" %QUICK% > "%BUILD%\results\protocol-bench.csv" 2> "%BUILD%\results\protocol-bench-guards.log"
if errorlevel 1 goto :fail

echo [3/4] Run display/input/OLED benchmark
"%BUILD%\protocol\espview_m8a4_bench.exe" > "%BUILD%\results\display-bench.csv" 2> "%BUILD%\results\display-bench-guards.log"
if errorlevel 1 goto :fail

echo [4/4] Compare against committed baselines
if "%QUICK%"=="" (
  py "%ROOT%\scripts\bench_compare.py" "%ROOT%\shared\protocol\bench\results\m8a1_baseline.csv" "%BUILD%\results\protocol-bench.csv"
  if errorlevel 1 goto :fail
  py "%ROOT%\scripts\bench_compare.py" "%ROOT%\shared\bench\results\m8a4_baseline.csv" "%BUILD%\results\display-bench.csv"
  if errorlevel 1 goto :fail
) else (
  py "%ROOT%\scripts\bench_compare.py" --warn-only "%ROOT%\shared\protocol\bench\results\m8a1_baseline.csv" "%BUILD%\results\protocol-bench.csv"
)

echo.
echo run_bench: ALL PASS (results in %BUILD%\results\)
exit /b 0

:fail
echo.
echo run_bench: FAILED
exit /b 1
