@echo off
REM ============================================================
REM ESPView M7-D5: one-shot build entrypoint (host / Qt / ESP32).
REM
REM   Usage:  scripts\espview_build.bat [-host] [-qt] [-esp32]
REM                                    [-b <profile>] [--check] [-h|--help]
REM
REM   Default (no args): -host -qt -esp32  (all three targets)
REM   -host     : host suite (reuses scripts\verify_host.bat)
REM   -qt       : Qt 6 GUI build (reuses scripts\verify_qt.bat)
REM   -esp32    : ESP32 firmware build (idf.py -B build\<profile> build)
REM   -b <name> : ESP32 build sub-directory under esp32\build\ (default uart_hw)
REM               uart_hw = UART verification firmware (ESPVIEW_DEFAULT_MODE=2,
REM               OLED, LVGL, TEST hooks). Future profiles get their own dir.
REM               Each profile keeps its own sdkconfig inside its build dir
REM               (build\<profile>\sdkconfig), seeded once from esp32\sdkconfig;
REM               the shared esp32\sdkconfig is never overwritten by builds.
REM   --check   : preflight only (PATH / MSYS2 / ESP-IDF probe); no build.
REM
REM   Exit codes:
REM     0  all requested steps OK
REM     1  a build/test step failed
REM     2  usage error (unknown argument / invalid profile name)
REM     3  preflight error (MSYS2 MinGW64 or ESP-IDF not found)
REM
REM Requires: MSYS2 MinGW64 (g++, cmake, ctest); ESP-IDF v6.0.2
REM   PowerShell profile at
REM   C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1
REM Overrides (environment): MINGW64_BIN, ESPIDF_PROFILE.
REM Security: never reads or prints esp32\sdkconfig; never touches
REM Wi-Fi credentials.
REM ============================================================
setlocal

if "%MINGW64_BIN%"=="" set "MINGW64_BIN=C:\msys64\mingw64\bin"
if "%ESPIDF_PROFILE%"=="" set "ESPIDF_PROFILE=C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1"
set "ESP32_PROFILE=uart_hw"
set "ROOT=%~dp0.."

REM ---- argument parsing -------------------------------------
set "DO_HOST="
set "DO_QT="
set "DO_ESP32="
set "CHECK_ONLY="

:parse
if "%~1"=="" goto :parsed
if /i "%~1"=="-host"        ( set "DO_HOST=1"   & shift & goto :parse )
if /i "%~1"=="-qt"          ( set "DO_QT=1"     & shift & goto :parse )
if /i "%~1"=="-esp32"       ( set "DO_ESP32=1"  & shift & goto :parse )
if /i "%~1"=="-b"           ( if "%~2"=="" goto :usage
                              set "ESP32_PROFILE=%~2" & shift & shift & goto :parse )
if /i "%~1"=="--check"      ( set "CHECK_ONLY=1" & shift & goto :parse )
if /i "%~1"=="-h"   ( set "ERR=0" & goto :usage )
if /i "%~1"=="--help" ( set "ERR=0" & goto :usage )
if /i "%~1"=="/?"   ( set "ERR=0" & goto :usage )
echo [build] ERROR: unknown argument: %~1
set "ERR=2" & goto :usage

:parsed
if not defined DO_HOST if not defined DO_QT if not defined DO_ESP32 (
    set "DO_HOST=1" & set "DO_QT=1" & set "DO_ESP32=1"
)
echo %ESP32_PROFILE%|findstr /r /c:"[^a-zA-Z0-9_-]" >nul
if not errorlevel 1 (
    echo [build] ERROR: invalid profile name: %ESP32_PROFILE%
    echo [build] ERROR: profile name may contain only A-Z a-z 0-9 _ -
    set "ERR=2" & goto :usage
)

echo ============================================================
echo ESPView build: host=%DO_HOST% qt=%DO_QT% esp32=%DO_ESP32%
echo                 esp32 profile=%ESP32_PROFILE%  check-only=%CHECK_ONLY%
echo ============================================================

REM ---- preflight: MSYS2 (needed for host / qt) --------------
if not defined DO_HOST if not defined DO_QT goto :preflight_esp32
if not exist "%MINGW64_BIN%\g++.exe"  goto :no_msys
if not exist "%MINGW64_BIN%\cmake.exe" goto :no_msys
if not exist "%MINGW64_BIN%\ctest.exe" goto :no_msys
echo [preflight] MSYS2 MinGW64 OK: %MINGW64_BIN%

:preflight_esp32
if not defined DO_ESP32 goto :preflight_done
if not exist "%ESPIDF_PROFILE%" goto :no_idf
echo [preflight] probing ESP-IDF profile: %ESPIDF_PROFILE%
powershell -NoProfile -ExecutionPolicy Bypass -Command "& { . '%ESPIDF_PROFILE%'; if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) { Write-Host '[preflight] idf.py not available after profile load'; exit 3 }; idf.py --version; if ($LASTEXITCODE -ne 0) { exit 3 }; exit 0 }"
if errorlevel 1 goto :no_idf
:preflight_done

if defined CHECK_ONLY (
    echo.
    echo espview_build: PREFLIGHT OK
    exit /b 0
)

REM ---- host ---------------------------------------------------
if not defined DO_HOST goto :qt
echo.
echo [1/3] host suite (verify_host.bat)
call "%ROOT%\scripts\verify_host.bat"
if errorlevel 1 ( set "ERR=1" & goto :fail )

:qt
if not defined DO_QT goto :esp32
echo.
echo [2/3] Qt GUI build (verify_qt.bat)
call "%ROOT%\scripts\verify_qt.bat"
if errorlevel 1 ( set "ERR=1" & goto :fail )

:esp32
if not defined DO_ESP32 goto :done
echo.
echo [3/3] ESP32 firmware build (profile=%ESP32_PROFILE%, dir=esp32\build\%ESP32_PROFILE%)
REM M7-F F4: per-profile sdkconfig isolation. IDF's default SDKCONFIG is the
REM shared esp32\sdkconfig; building several profiles through it makes every
REM profile drift to the last-built config. Each profile gets its own sdkconfig
REM inside its build dir (bootstrap-copied from esp32\sdkconfig on first use so
REM machine-specific settings + Wi-Fi credentials are preserved, and it stays
REM inside the gitignored build tree).
set "PROFILE_SDKCONFIG=%ROOT%\esp32\build\%ESP32_PROFILE%\sdkconfig"
if not exist "%PROFILE_SDKCONFIG%" (
    if exist "%ROOT%\esp32\sdkconfig" (
        echo [esp32] bootstrap profile sdkconfig: esp32\sdkconfig -^> build\%ESP32_PROFILE%\sdkconfig
        copy /y "%ROOT%\esp32\sdkconfig" "%PROFILE_SDKCONFIG%" >nul
    )
)
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; try { & { . '%ESPIDF_PROFILE%' }; Set-Location '%ROOT%\esp32'; idf.py -B build/%ESP32_PROFILE% -D SDKCONFIG=%PROFILE_SDKCONFIG% build; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE } } catch { Write-Host ('[esp32] ERROR: ' + $_.Exception.Message); exit 1 }"
if errorlevel 1 ( set "ERR=1" & goto :fail )

:done
echo.
echo espview_build: ALL PASS
exit /b 0

:no_msys
echo [build] ERROR: MSYS2 MinGW64 not found in %MINGW64_BIN%
echo [build] ERROR: expected g++.exe, cmake.exe, ctest.exe (MSYS2 MinGW64 not found; install MSYS2 or set MINGW64_BIN)
set "ERR=3" & goto :fail

:no_idf
echo [build] ERROR: ESP-IDF unavailable (profile not found or idf.py probe failed)
echo [build] ERROR: expected profile at %ESPIDF_PROFILE% (ESP-IDF not found; run the Espressif installer or set ESPIDF_PROFILE)
set "ERR=3" & goto :fail

:usage
if not defined ERR set "ERR=2"
echo.
echo Usage: scripts\espview_build.bat [-host] [-qt] [-esp32] [-b ^<profile^>] [--check]
echo   default (no args): -host -qt -esp32
echo   -b ^<profile^>  ESP32 build dir under esp32\build\ (default uart_hw)
echo   --check     preflight only, no build
echo   -h/--help   show this help
exit /b %ERR%

:fail
echo.
echo espview_build: FAILED (exit code %ERR%)
exit /b %ERR%