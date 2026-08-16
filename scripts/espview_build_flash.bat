@echo off
REM ============================================================
REM ESPView M7-G: build + flash in one shot.
REM
REM   Usage:
REM     scripts\espview_build_flash.bat [build-flash|build|flash]
REM                                [-host] [-qt] [-esp32] [-b <profile>]
REM                                [-p <port>] [--no-reset] [--dry-run]
REM                                [--check] [--any-profile] [-h|--help]
REM     scripts\espview_build_flash.bat verify|profile ...
REM
REM   Subcommands:
REM     build-flash (default) : build then flash
REM     build                : espview_build.bat only
REM     flash                : espview_flash.bat only
REM     check                : build preflight + flash dry-run (never flashes)
REM     dry-run              : print the build+flash plan, run nothing
REM     verify               : delegate to scripts\espview_verify.bat
REM     profile              : delegate to scripts\espview_build.bat profile
REM
REM   Build args: -host -qt -esp32 -b <profile> --check
REM   Flash args: -p <port> --no-reset --dry-run --any-profile
REM   Defaults  : -host -qt -esp32 -p COM4 -b uart_hw (= uart alias, keeps legacy build dir)
REM
REM   Exit codes: see espview_build.bat (0/1/2/3) and
REM   espview_flash.bat (0/2/3/4/5/6); the first failure wins.
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
set "BUILD_ARGS="
set "FLASH_ARGS="
set "TARGET_GIVEN="
set "MODE=build_flash"

:parse
if "%~1"=="" goto :parsed
if /i "%~1"=="-host"   ( set "BUILD_ARGS=%BUILD_ARGS% -host"  & set "TARGET_GIVEN=1" & shift & goto :parse )
if /i "%~1"=="-qt"     ( set "BUILD_ARGS=%BUILD_ARGS% -qt"    & set "TARGET_GIVEN=1" & shift & goto :parse )
if /i "%~1"=="-esp32"  ( set "BUILD_ARGS=%BUILD_ARGS% -esp32" & set "TARGET_GIVEN=1" & shift & goto :parse )
if /i "%~1"=="-b"      ( if "%~2"=="" goto :usage
                          set "BUILD_ARGS=%BUILD_ARGS% -b %~2" & set "FLASH_ARGS=%FLASH_ARGS% -b %~2" & shift & shift & goto :parse )
if /i "%~1"=="--profile" ( if "%~2"=="" goto :usage
                          set "BUILD_ARGS=%BUILD_ARGS% -b %~2" & set "FLASH_ARGS=%FLASH_ARGS% -b %~2" & shift & shift & goto :parse )
if /i "%~1"=="--check" ( set "BUILD_ARGS=%BUILD_ARGS% --check" & set "FLASH_ARGS=%FLASH_ARGS% --dry-run" & shift & goto :parse )
if /i "%~1"=="-p"      ( if "%~2"=="" goto :usage
                          set "FLASH_ARGS=%FLASH_ARGS% -p %~2" & shift & shift & goto :parse )
if /i "%~1"=="--no-reset" ( set "FLASH_ARGS=%FLASH_ARGS% --no-reset" & shift & goto :parse )
if /i "%~1"=="--dry-run"  ( set "MODE=dry_run" & set "FLASH_ARGS=%FLASH_ARGS% --dry-run"  & shift & goto :parse )
if /i "%~1"=="--any-profile" ( set "FLASH_ARGS=%FLASH_ARGS% --any-profile" & shift & goto :parse )
if /i "%~1"=="build-flash" ( set "MODE=build_flash" & shift & goto :parse )
if /i "%~1"=="build"     ( set "MODE=build" & shift & goto :parse )
if /i "%~1"=="flash"     ( set "MODE=flash" & shift & goto :parse )
if /i "%~1"=="check"     ( set "MODE=check" & set "BUILD_ARGS=%BUILD_ARGS% --check" & set "FLASH_ARGS=%FLASH_ARGS% --dry-run" & shift & goto :parse )
if /i "%~1"=="dry-run"   ( set "MODE=dry_run" & shift & goto :parse )
if /i "%~1"=="verify"    goto :do_verify
if /i "%~1"=="profile"   goto :do_profile
if /i "%~1"=="-h"   ( set "ERR=0" & goto :usage )
if /i "%~1"=="--help" ( set "ERR=0" & goto :usage )
if /i "%~1"=="/?"   ( set "ERR=0" & goto :usage )
echo [build_flash] ERROR: unknown argument: %~1
set "ERR=2" & goto :usage

:parsed
if not defined TARGET_GIVEN set "BUILD_ARGS= -host -qt -esp32%BUILD_ARGS%"

if "%MODE%"=="dry_run" goto :plan_only

echo ============================================================
echo ESPView build+flash ^(mode=%MODE%^)
echo   build : espview_build.bat%BUILD_ARGS%
echo   flash : espview_flash.bat%FLASH_ARGS%
echo ============================================================

if "%MODE%"=="flash" goto :only_flash
call "%ROOT%\scripts\espview_build.bat"%BUILD_ARGS%
set "ERR=%ERRORLEVEL%"
if not "%ERR%"=="0" goto :fail

if "%MODE%"=="build" goto :done
:only_flash
call "%ROOT%\scripts\espview_flash.bat"%FLASH_ARGS%
set "ERR=%ERRORLEVEL%"
if not "%ERR%"=="0" goto :fail

:done
echo.
echo espview_build_flash: ALL PASS
exit /b 0

:plan_only
echo ============================================================
echo ESPView build+flash DRY-RUN ^(nothing executed^)
echo   build : espview_build.bat%BUILD_ARGS%
echo   flash : espview_flash.bat%FLASH_ARGS%
echo   profile list : scripts\espview_build.bat profile list
echo ============================================================
exit /b 0

:do_verify
call "%ROOT%\scripts\espview_verify.bat" %*
set "ERR=%ERRORLEVEL%"
exit /b %ERR%

:do_profile
call "%ROOT%\scripts\espview_build.bat" %*
set "ERR=%ERRORLEVEL%"
exit /b %ERR%

:usage
if not defined ERR set "ERR=2"
echo.
echo Usage: scripts\espview_build_flash.bat [build-flash^|build^|flash] [-host] [-qt] [-esp32]
echo        [-b ^<profile^>] [-p ^<port^>] [--no-reset] [--dry-run] [--check] [--any-profile]
echo   Build args : -host -qt -esp32 -b ^<profile^> --check
echo   Flash args : -p ^<port^> --no-reset --dry-run --any-profile
echo   Defaults   : -host -qt -esp32 -p COM4 -b uart_hw (= uart alias, keeps legacy build dir)
echo   --check    preflight only; implies flash --dry-run ^(never flashes^)
echo   --dry-run  print the plan ^(build+flash^), run nothing
echo   verify     delegate to scripts\espview_verify.bat
echo   profile    delegate to scripts\espview_build.bat profile
echo   -h/--help  show this help
exit /b %ERR%

:fail
echo.
echo espview_build_flash: FAILED (exit code %ERR%)
exit /b %ERR%
