@echo off
REM ============================================================
REM ESPView M7-G: verification entrypoint (host / Qt / LVGL / ESP32).
REM
REM   Usage:
REM     scripts\espview_verify.bat [verify] [-host] [-qt] [-lvgl]
REM                                [-esp32] [-b <profile>]
REM                                [--check] [--dry-run] [-h|--help]
REM     scripts\espview_verify.bat profile list|show <name>|check <name>
REM
REM   Subcommands: verify (default) / check / dry-run / profile
REM   Default (no args): -host -qt
REM   -host    : host suite (scripts\verify_host.bat; MSYS2 only)
REM   -qt      : Qt 6 GUI build check (scripts\verify_qt.bat; MSYS2+Qt)
REM   -lvgl    : scripts\verify_lvgl.bat (host tests + ESP32 idf.py build
REM              + optional COM3 LVGL sanity) -- OPT-IN because it builds
REM              the ESP32 firmware.
REM   -esp32   : ESP32 preflight only (ESP-IDF probe; no build). Real
REM              firmware builds belong to scripts\espview_build.bat.
REM   --check  : preflight toolchain probes only, run nothing heavy
REM   --dry-run: print the exact verification plan, run nothing
REM
REM   Exit codes:
REM     0  all requested steps OK
REM     1  a verification step failed
REM     2  usage error (unknown argument / unknown profile name)
REM     3  preflight error (MSYS2 / ESP-IDF / python missing)
REM
REM Security: never prints esp32\sdkconfig content; never touches Wi-Fi
REM credentials.
REM ============================================================
setlocal

if "%MINGW64_BIN%"=="" set "MINGW64_BIN=C:\msys64\mingw64\bin"
if "%ESPIDF_PROFILE%"=="" set "ESPIDF_PROFILE=C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1"
if "%ESPVIEW_PYTHON%"=="" set "ESPVIEW_PYTHON=py"
where "%ESPVIEW_PYTHON%" >nul 2>nul
if errorlevel 1 (
    set "ESPVIEW_PYTHON=python"
    where python >nul 2>nul
    if errorlevel 1 set "ESPVIEW_PYTHON=py"
)
set "DO_HOST="
set "DO_QT="
set "DO_LVGL="
set "DO_ESP32="
set "CHECK_ONLY="
set "DRY_RUN="
set "VERIFY_PROFILE="
for %%i in ("%~dp0..") do set "ROOT=%%~fi"

:parse
if "%~1"=="" goto :parsed
if /i "%~1"=="-host"   ( set "DO_HOST=1"  & shift & goto :parse )
if /i "%~1"=="-qt"     ( set "DO_QT=1"    & shift & goto :parse )
if /i "%~1"=="-lvgl"   ( set "DO_LVGL=1"  & shift & goto :parse )
if /i "%~1"=="-esp32"  ( set "DO_ESP32=1" & shift & goto :parse )
if /i "%~1"=="-b" ( if "%~2"=="" goto :usage
                    set "VERIFY_PROFILE=%~2" & shift & shift & goto :parse )
if /i "%~1"=="--profile" ( if "%~2"=="" goto :usage
                    set "VERIFY_PROFILE=%~2" & shift & shift & goto :parse )
if /i "%~1"=="--check"   ( set "CHECK_ONLY=1" & shift & goto :parse )
if /i "%~1"=="--dry-run" ( set "DRY_RUN=1"    & shift & goto :parse )
if /i "%~1"=="profile"   ( shift & goto :profile_parse )
if /i "%~1"=="verify"    ( shift & goto :parse )
if /i "%~1"=="check"     ( set "CHECK_ONLY=1" & shift & goto :parse )
if /i "%~1"=="dry-run"   ( set "DRY_RUN=1"    & shift & goto :parse )
if /i "%~1"=="-h"   ( set "ERR=0" & goto :usage )
if /i "%~1"=="--help" ( set "ERR=0" & goto :usage )
if /i "%~1"=="/?"   ( set "ERR=0" & goto :usage )
echo [verify] ERROR: unknown argument: %~1
set "ERR=2" & goto :usage

:parsed
if not defined DO_HOST if not defined DO_QT if not defined DO_LVGL if not defined DO_ESP32 (
    set "DO_HOST=1" & set "DO_QT=1"
)

if not defined VERIFY_PROFILE set "VERIFY_PROFILE=uart"
echo %VERIFY_PROFILE%|findstr /r /c:"[^a-zA-Z0-9_-]" >nul
if not errorlevel 1 (
    echo [verify] ERROR: invalid profile name: %VERIFY_PROFILE%
    set "ERR=2" & goto :usage
)
"%ESPVIEW_PYTHON%" "%ROOT%\scripts\espview_profile_sdkconfig.py" --check %VERIFY_PROFILE% >nul
if errorlevel 2 goto :bad_profile

echo ============================================================
echo ESPView verify: host=%DO_HOST% qt=%DO_QT% lvgl=%DO_LVGL% esp32=%DO_ESP32%
echo                 check-only=%CHECK_ONLY% dry-run=%DRY_RUN%
echo                 profile=%VERIFY_PROFILE%
echo ============================================================

REM ---- dry-run: print plan only ------------------------------
if not defined DRY_RUN goto :not_dry
echo.
echo [verify] DRY-RUN plan:
if defined DO_HOST echo   host  : scripts\verify_host.bat
if defined DO_QT   echo   qt    : scripts\verify_qt.bat
if defined DO_LVGL echo   lvgl  : scripts\verify_lvgl.bat  ^(includes ESP32 idf.py build; profile %VERIFY_PROFILE% informational^)
if defined DO_ESP32 echo   esp32 : profile=%VERIFY_PROFILE% preflight only ^(use espview_build.bat for builds^)
echo.
echo [verify] DRY-RUN: plan printed, nothing executed.
exit /b 0

:not_dry
REM ---- preflight ----------------------------------------------
if not defined DO_HOST if not defined DO_QT goto :preflight_esp32
if not exist "%MINGW64_BIN%\g++.exe"  goto :no_msys
if not exist "%MINGW64_BIN%\cmake.exe" goto :no_msys
if not exist "%MINGW64_BIN%\ctest.exe" goto :no_msys
echo [preflight] MSYS2 MinGW64 OK: %MINGW64_BIN%

:preflight_esp32
if not defined DO_ESP32 if not defined DO_LVGL goto :preflight_done
if not exist "%ESPIDF_PROFILE%" goto :no_idf
echo [preflight] probing ESP-IDF profile: %ESPIDF_PROFILE%
powershell -NoProfile -ExecutionPolicy Bypass -Command "& { . '%ESPIDF_PROFILE%'; if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) { Write-Host '[preflight] idf.py not available after profile load'; exit 3 }; idf.py --version; if ($LASTEXITCODE -ne 0) { exit 3 }; exit 0 }"
if errorlevel 1 goto :no_idf
:preflight_done

if defined CHECK_ONLY (
    echo.
    echo espview_verify: PREFLIGHT OK
    exit /b 0
)

REM ---- host ---------------------------------------------------
if not defined DO_HOST goto :qt
echo.
echo [1/3] host suite (verify_host.bat)
call "%ROOT%\scripts\verify_host.bat"
if errorlevel 1 ( set "ERR=1" & goto :fail )

:qt
if not defined DO_QT goto :lvgl
echo.
echo [2/3] Qt GUI build (verify_qt.bat)
call "%ROOT%\scripts\verify_qt.bat"
if errorlevel 1 ( set "ERR=1" & goto :fail )

:lvgl
if not defined DO_LVGL goto :esp32
echo.
echo [3/3] LVGL verification (verify_lvgl.bat: host tests + ESP32 build + optional COM3 sanity)
call "%ROOT%\scripts\verify_lvgl.bat"
if errorlevel 1 ( set "ERR=1" & goto :fail )

:esp32
if not defined DO_ESP32 goto :done
echo.
echo [esp32] preflight only (no build). Use scripts\espview_build.bat -esp32 for a real build.
echo [esp32] ESP-IDF probe passed (see preflight above).
echo [esp32] profile=%VERIFY_PROFILE%
:done
echo.
echo espview_verify: ALL PASS
exit /b 0

REM ---- profile queries ------------------------------------------
:profile_parse
if "%~1"=="" goto :profile_usage
if /i "%~1"=="list"   goto :profile_list
if /i "%~1"=="show"   ( if "%~2"=="" goto :profile_usage
                         set "PNAME=%~2" & goto :profile_show )
if /i "%~1"=="check"  ( if "%~2"=="" goto :profile_usage
                         set "PNAME=%~2" & goto :profile_check )
if /i "%~1"=="-h"     goto :profile_help
if /i "%~1"=="--help" goto :profile_help
echo [profile] ERROR: unknown profile subcommand: %~1
set "ERR=2" & goto :usage

:profile_list
"%ESPVIEW_PYTHON%" "%ROOT%\scripts\espview_profile_sdkconfig.py" --list
set "ERR=%ERRORLEVEL%"
exit /b %ERR%

:profile_show
"%ESPVIEW_PYTHON%" "%ROOT%\scripts\espview_profile_sdkconfig.py" --show %PNAME%
set "ERR=%ERRORLEVEL%"
exit /b %ERR%

:profile_check
"%ESPVIEW_PYTHON%" "%ROOT%\scripts\espview_profile_sdkconfig.py" --check %PNAME%
set "ERR=%ERRORLEVEL%"
exit /b %ERR%

:profile_help
echo profile subcommands:
echo   profile list               list whitelisted profiles + 6 attributes
echo   profile show ^<name^>       show one profile's attribute table
echo   profile check ^<name^>      validate a profile name ^(exit 0/2^)
exit /b 0

:profile_usage
echo [profile] ERROR: profile requires a subcommand ^(list / show ^<name^> / check ^<name^>^)
set "ERR=2" & goto :usage

:no_msys
echo [verify] ERROR: MSYS2 MinGW64 not found in %MINGW64_BIN%
echo [verify] ERROR: expected g++.exe, cmake.exe, ctest.exe ^(install MSYS2 or set MINGW64_BIN^)
set "ERR=3" & goto :fail

:no_idf
echo [verify] ERROR: ESP-IDF unavailable ^(profile not found or idf.py probe failed^)
echo [verify] ERROR: expected profile at %ESPIDF_PROFILE% ^(run the Espressif installer or set ESPIDF_PROFILE^)
set "ERR=3" & goto :fail

:bad_profile
echo [verify] ERROR: unknown profile name: %VERIFY_PROFILE%
echo [verify] ERROR: whitelist: uart tcp oled oled-off diagnostic g1_a g1_b g1_c g1_d ^(uart_hw alias^)
set "ERR=2" & goto :usage

:usage
if not defined ERR set "ERR=2"
echo.
echo Usage: scripts\espview_verify.bat [verify] [-host] [-qt] [-lvgl] [-esp32] [-b ^<profile^>] [--check] [--dry-run]
echo   -b ^<profile^>  ESP32 profile for preflight context ^(whitelist; default uart^)
echo   default (no args): -host -qt
echo   -host    host suite ^(verify_host.bat^)
echo   -qt      Qt 6 GUI build check ^(verify_qt.bat^)
echo   -lvgl    LVGL verification ^(verify_lvgl.bat; includes ESP32 build^) -- opt-in
echo   -esp32   ESP32 preflight only ^(no build; use espview_build.bat^)
echo   --check  preflight toolchain probes only
echo   --dry-run print the plan, run nothing
echo   profile  profile list / show ^<name^> / check ^<name^>
echo   -h/--help show this help
exit /b %ERR%

:fail
echo.
echo espview_verify: FAILED (exit code %ERR%)
exit /b %ERR%
