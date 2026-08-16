@echo off
REM ============================================================
REM ESPView M7-G G10: static documentation checker.
REM
REM   Usage: scripts\check_docs.bat [--help]
REM
REM   Runs scripts\check_docs.py over the working tree (README.md,
REM   docs/**, scripts/**, esp32/**, pc/**, shared/**) and reports:
REM     - README references to missing files / commands / profiles
REM     - README messages/capabilities not found in code or DESIGN.md
REM     - forbidden patterns: FRAME_FULL, "16 KiB MAX_PACKET_PAYLOAD",
REM       "undefined frameSeq", real Wi-Fi credential patterns
REM       (password=, WIFI_PASSWORD, CONFIG_ESPVIEW_WIFI_*),
REM       real private IPs in scripts
REM     - "verified" claims without findable evidence
REM     - tracked esp32/sdkconfig* files
REM     - docs/**/*.md + README.md markdown cross-links resolving to
REM       existing files/dirs
REM     - example-command scripts referenced in docs code fences
REM     - README CI badge workflow references
REM     - README Documentation index links (docs/ci.md, docs/README.md)
REM
REM   Exit codes: 0 clean / 1 issues found / 2 usage or IO error.
REM   No build, no hardware, no network: safe to run any time.
REM   Requires: python (stdlib only).
REM ============================================================
setlocal

if "%ESPVIEW_PYTHON%"=="" set "ESPVIEW_PYTHON=py"
where "%ESPVIEW_PYTHON%" >nul 2>nul
if errorlevel 1 (
    set "ESPVIEW_PYTHON=python"
    where python >nul 2>nul
    if errorlevel 1 set "ESPVIEW_PYTHON=py"
)
for %%i in ("%~dp0..") do set "ROOT=%%~fi"

if /i "%~1"=="-h"   ( set "ERR=0" & goto :usage )
if /i "%~1"=="--help" ( set "ERR=0" & goto :usage )
if /i "%~1"=="/?"   ( set "ERR=0" & goto :usage )
if not "%~1"==""    ( echo [check_docs] ERROR: unknown argument: %~1 & set "ERR=2" & goto :usage )

"%ESPVIEW_PYTHON%" "%ROOT%\scripts\check_docs.py"
set "ERR=%ERRORLEVEL%"
if "%ERR%"=="0" exit /b 0
if "%ERR%"=="1" exit /b 1
echo [check_docs] ERROR: checker failed to run ^(exit %ERR%^) - python missing? set ESPVIEW_PYTHON
exit /b 2

:usage
echo.
echo Usage: scripts\check_docs.bat
echo   Runs scripts\check_docs.py (README/doc/script reference check,
echo   docs cross-link check, example-command script check, CI badge
echo   check, documentation index check, capability evidence check,
echo   forbidden-pattern check).
echo   Exit codes: 0 clean / 1 issues found / 2 usage or IO error.
exit /b %ERR%
