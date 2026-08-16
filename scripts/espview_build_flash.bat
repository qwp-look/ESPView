@echo off
REM ============================================================
REM ESPView M7-D5: build + flash in one shot.
REM
REM   Runs espview_build.bat then espview_flash.bat and exits
REM   with the first failing step's exit code.
REM
REM   Build args: -host -qt -esp32 -b <profile> --check
REM   Flash args: -p <port> --no-reset --dry-run
REM   --check also implies flash --dry-run: preflight only, never flashes.
REM   Defaults : -host -qt -esp32 -p COM4 -b uart_hw
REM
REM   Exit codes: see espview_build.bat (0/1/2/3) and
REM   espview_flash.bat (0/2/3/4/5/6); the first failure wins.
REM ============================================================
setlocal

set "ROOT=%~dp0.."
set "BUILD_ARGS="
set "FLASH_ARGS="
set "TARGET_GIVEN="

:parse
if "%~1"=="" goto :parsed
if /i "%~1"=="-host"   ( set "BUILD_ARGS=%BUILD_ARGS% -host"  & set "TARGET_GIVEN=1" & shift & goto :parse )
if /i "%~1"=="-qt"     ( set "BUILD_ARGS=%BUILD_ARGS% -qt"    & set "TARGET_GIVEN=1" & shift & goto :parse )
if /i "%~1"=="-esp32"  ( set "BUILD_ARGS=%BUILD_ARGS% -esp32" & set "TARGET_GIVEN=1" & shift & goto :parse )
if /i "%~1"=="-b"      ( if "%~2"=="" goto :usage
                          set "BUILD_ARGS=%BUILD_ARGS% -b %~2" & set "FLASH_ARGS=%FLASH_ARGS% -b %~2" & shift & shift & goto :parse )
if /i "%~1"=="--check" ( set "BUILD_ARGS=%BUILD_ARGS% --check" & set "FLASH_ARGS=%FLASH_ARGS% --dry-run" & shift & goto :parse )
if /i "%~1"=="-p"      ( if "%~2"=="" goto :usage
                          set "FLASH_ARGS=%FLASH_ARGS% -p %~2" & shift & shift & goto :parse )
if /i "%~1"=="--no-reset" ( set "FLASH_ARGS=%FLASH_ARGS% --no-reset" & shift & goto :parse )
if /i "%~1"=="--dry-run"  ( set "FLASH_ARGS=%FLASH_ARGS% --dry-run"  & shift & goto :parse )
if /i "%~1"=="-h"   ( set "ERR=0" & goto :usage )
if /i "%~1"=="--help" ( set "ERR=0" & goto :usage )
if /i "%~1"=="/?"   ( set "ERR=0" & goto :usage )
echo [build_flash] ERROR: unknown argument: %~1
set "ERR=2" & goto :usage

:parsed
if not defined TARGET_GIVEN set "BUILD_ARGS= -host -qt -esp32%BUILD_ARGS%"

echo ============================================================
echo ESPView build+flash
echo   build : espview_build.bat%BUILD_ARGS%
echo   flash : espview_flash.bat%FLASH_ARGS%
echo ============================================================

call "%ROOT%\scripts\espview_build.bat"%BUILD_ARGS%
set "ERR=%ERRORLEVEL%"
if not "%ERR%"=="0" goto :fail

call "%ROOT%\scripts\espview_flash.bat"%FLASH_ARGS%
set "ERR=%ERRORLEVEL%"
if not "%ERR%"=="0" goto :fail

echo.
echo espview_build_flash: ALL PASS
exit /b 0

:usage
echo.
echo Usage: scripts\espview_build_flash.bat [-host] [-qt] [-esp32]
echo        [-b ^<profile^>] [-p ^<port^>] [--no-reset] [--dry-run] [--check]
echo   Build args : -host -qt -esp32 -b ^<profile^> --check
echo   Flash args : -p ^<port^> --no-reset --dry-run
echo   Defaults   : -host -qt -esp32 -p COM4 -b uart_hw
echo   --check    preflight only; implies flash --dry-run (never flashes)
echo   -h/--help   show this help
exit /b %ERR%

:fail
echo.
echo espview_build_flash: FAILED (exit code %ERR%)
exit /b %ERR%