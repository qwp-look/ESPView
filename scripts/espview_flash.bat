@echo off
REM ============================================================
REM ESPView M7-D5: flash entrypoint (idf.py flash via ESP-IDF env).
REM
REM   Usage:  scripts\espview_flash.bat [-p COM4] [-b <profile>]
REM                                    [--no-reset] [--dry-run] [-h|--help]
REM
REM   Defaults: port COM4, profile uart_hw (esp32\build\uart_hw).
REM   -p <port>    serial port, e.g. COM4 (bare number allowed: 4 = COM4)
REM   -b <name>    ESP32 build sub-directory under esp32\build\ (default uart_hw)
REM   --no-reset   do not reset the chip after flashing. ESP-IDF v6.0.2
REM                idf.py flash has no native --no-reset, so this calls
REM                esptool directly (--before default-reset --after
REM                no-reset write-flash @flash_args) so the chip keeps
REM                running after the write completes.
REM   --dry-run    parse args + validate only; do NOT flash anything.
REM
REM   Exit codes:
REM     0  OK (or dry-run validation passed)
REM     2  usage error
REM     3  ESP-IDF profile missing / probe failed
REM     4  COM port not found
REM     5  firmware binary not found (run espview_build.bat -esp32 first)
REM     6  flash failed (idf.py / esptool)
REM
REM Security: prints only the firmware path + profile label. Never reads
REM or prints esp32\sdkconfig; never touches Wi-Fi credentials (SSID /
REM password stay in the untracked local sdkconfig, see docs/DESIGN.md).
REM ============================================================
setlocal

if "%ESPIDF_PROFILE%"=="" set "ESPIDF_PROFILE=C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1"
set "PORT=COM4"
set "ESP32_PROFILE=uart_hw"
set "NO_RESET="
set "DRY_RUN="
set "ROOT=%~dp0.."

:parse
if "%~1"=="" goto :parsed
if /i "%~1"=="-p" ( if "%~2"=="" goto :usage
                    set "PORT=%~2" & shift & shift & goto :parse )
if /i "%~1"=="-b" ( if "%~2"=="" goto :usage
                    set "ESP32_PROFILE=%~2" & shift & shift & goto :parse )
if /i "%~1"=="--no-reset" ( set "NO_RESET=1" & shift & goto :parse )
if /i "%~1"=="--dry-run"  ( set "DRY_RUN=1"  & shift & goto :parse )
if /i "%~1"=="-h"   ( set "ERR=0" & goto :usage )
if /i "%~1"=="--help" ( set "ERR=0" & goto :usage )
if /i "%~1"=="/?"   ( set "ERR=0" & goto :usage )
echo [flash] ERROR: unknown argument: %~1
set "ERR=2" & goto :usage

:parsed
REM normalize port: "4" -> COM4; case-insensitive validation
echo %PORT%|findstr /r /c:"^[0-9][0-9]*$" >nul
if not errorlevel 1 set "PORT=COM%PORT%"
set "PORT=%PORT: =%"
echo %PORT%|findstr /i /r /c:"^COM[0-9][0-9]*$" >nul
if errorlevel 1 (
    echo [flash] ERROR: invalid port: %PORT%  ^(expected COMx, e.g. COM4^)
    set "ERR=2" & goto :usage
)
echo %ESP32_PROFILE%|findstr /r /c:"[^a-zA-Z0-9_-]" >nul
if not errorlevel 1 (
    echo [flash] ERROR: invalid profile name: %ESP32_PROFILE%
    set "ERR=2" & goto :usage
)

REM ---- preflight: ESP-IDF --------------------------------------
if not exist "%ESPIDF_PROFILE%" goto :no_idf
echo [flash] probing ESP-IDF profile: %ESPIDF_PROFILE%
powershell -NoProfile -ExecutionPolicy Bypass -Command "& { . '%ESPIDF_PROFILE%'; if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) { Write-Host '[flash] idf.py not available after profile load'; exit 3 }; idf.py --version; if ($LASTEXITCODE -ne 0) { exit 3 }; exit 0 }"
if errorlevel 1 goto :no_idf

REM ---- COM port check ------------------------------------------
powershell -NoProfile -ExecutionPolicy Bypass -Command "if ([System.IO.Ports.SerialPort]::GetPortNames() -contains '%PORT%') { exit 0 } else { Write-Host ('[flash] COM port not found: ' + '%PORT%'); exit 1 }"
if errorlevel 1 goto :no_com

REM ---- firmware binary check ------------------------------------
set "BIN=%ROOT%\esp32\build\%ESP32_PROFILE%\espview_esp32.bin"
if not exist "%BIN%" goto :no_bin

REM M7-F F4: mirror the build script's per-profile sdkconfig isolation.
REM idf.py flash reconfigures the build dir, so it must use the same
REM build\<profile>\sdkconfig the profile was built with (bootstrap-copied
REM from esp32\sdkconfig on first use so machine settings are preserved).
set "PROFILE_SDKCONFIG=%ROOT%\esp32\build\%ESP32_PROFILE%\sdkconfig"
if not exist "%PROFILE_SDKCONFIG%" (
    if exist "%ROOT%\esp32\sdkconfig" (
        echo [flash] bootstrap profile sdkconfig: esp32\sdkconfig -^> build\%ESP32_PROFILE%\sdkconfig
        copy /y "%ROOT%\esp32\sdkconfig" "%PROFILE_SDKCONFIG%" >nul
    )
)

set "PROFILE_LABEL=%ESP32_PROFILE%"
if /i "%ESP32_PROFILE%"=="uart_hw" set "PROFILE_LABEL=uart_hw (UART verification firmware: ESPVIEW_DEFAULT_MODE=2, OLED, LVGL, TEST hooks)"

echo.
echo [flash] Will flash (esptool via ESP-IDF v6.0.2):
echo   Profile : %PROFILE_LABEL%
echo   Firmware: %BIN%
echo   Port    : %PORT%
if defined NO_RESET echo   Reset   : no-reset ^(esptool --after no-reset^)

if defined DRY_RUN (
    echo.
    echo [flash] DRY-RUN: validation passed, flash NOT performed.
    exit /b 0
)

if defined NO_RESET goto :flash_noreset
echo [flash] running: idf.py -B build\%ESP32_PROFILE% -p %PORT% flash
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; try { & { . '%ESPIDF_PROFILE%' }; Set-Location '%ROOT%\esp32'; idf.py -B build/%ESP32_PROFILE% -D SDKCONFIG=%PROFILE_SDKCONFIG% -p %PORT% flash; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE } } catch { Write-Host ('[flash] ERROR: ' + $_.Exception.Message); exit 6 }"
goto :flash_check

:flash_noreset
REM --no-reset: esptool --after must precede the write-flash subcommand
REM (esptool v5.3.1 rejects --after after write-flash). Call esptool
REM directly using the flash_args file generated by idf.py.
echo [flash] running: esptool --chip esp32 -p %PORT% -b 460800 --before default-reset --after no-reset write-flash @flash_args
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; try { & { . '%ESPIDF_PROFILE%' }; Set-Location '%ROOT%\esp32\build\%ESP32_PROFILE%'; & ($env:IDF_PYTHON_ENV_PATH + '\Scripts\python.exe') -m esptool --chip esp32 -p '%PORT%' -b 460800 --before default-reset --after no-reset write-flash '@flash_args'; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE } } catch { Write-Host ('[flash] ERROR: ' + $_.Exception.Message); exit 6 }"

:flash_check
if errorlevel 1 ( set "ERR=6" & goto :fail )

echo.
echo [flash] ALL PASS
exit /b 0

:no_idf
echo [flash] ERROR: ESP-IDF unavailable
echo [flash] ERROR: expected profile at %ESPIDF_PROFILE% (ESP-IDF not found; run the Espressif installer or set ESPIDF_PROFILE)
set "ERR=3" & goto :fail

:no_com
echo [flash] ERROR: COM port not found: %PORT%
echo [flash] ERROR: check cable/driver (CH340) and close any serial monitor holding the port
set "ERR=4" & goto :fail

:no_bin
echo [flash] ERROR: firmware binary not found: %BIN%
echo [flash] ERROR: build the firmware first: scripts\espview_build.bat -esp32 -b %ESP32_PROFILE%
set "ERR=5" & goto :fail

:usage
echo.
echo Usage: scripts\espview_flash.bat [-p COM4] [-b ^<profile^>] [--no-reset] [--dry-run]
echo   -p ^<port^>      serial port, default COM4 (bare number allowed: 4 = COM4)
echo   -b ^<profile^>   ESP32 build dir under esp32\build\ (default uart_hw)
echo   --no-reset   skip chip reset after flashing (esptool --after no-reset)
echo   --dry-run    validate only, do not flash
echo   -h/--help    show this help
exit /b %ERR%

:fail
echo.
echo [flash] FAILED (exit code %ERR%)
exit /b %ERR%