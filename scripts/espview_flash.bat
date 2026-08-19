@echo off
REM ============================================================
REM ESPView M7-G: flash entrypoint (idf.py flash via ESP-IDF env).
REM
REM   Usage:
REM     scripts\espview_flash.bat [flash] [-p COM4] [-b <profile>] [-t <target>]
REM                               [--no-reset] [--dry-run] [--check]
REM                               [--any-profile] [-h|--help]
REM     scripts\espview_flash.bat probe [-p COM4] [--baud 115200]
REM                               [--pulse-reset] [--timeout-ms N]
REM     scripts\espview_flash.bat profile list|show <name>|check <name>
REM
REM   Subcommands: flash (default) / probe / profile / check / dry-run
REM   Defaults: port COM4, profile uart (whitelist, see profile list).
REM   -p <port>     serial port, e.g. COM4 (bare number allowed: 4 = COM4)
REM   -b <name>     ESP32 build profile (whitelisted; default uart)
REM   -t <target>   IDF target (esp32 default / esp32s3); drives esptool --chip
REM   --no-reset    do not reset the chip after flashing. ESP-IDF v6.0.2
REM                 idf.py flash has no native --no-reset, so this calls
REM                 esptool directly (--before default-reset --after
REM                 no-reset write-flash @flash_args) so the chip keeps
REM                 running after the write completes. flash_args is
REM                 preflighted for existence before invoking esptool.
REM   --dry-run     parse args + validate only; do NOT flash anything.
REM   --check       same as --dry-run (preflight only).
REM   M8-C C4: flash is artifact-only - it NEVER reconfigures, never applies           the profile and never rewrites sdkconfig. Build dir is per-target:           esp32\build\<target>\<profile>\  (from -t and -b). A read-only           CONFIG_IDF_TARGET cross-check blocks flashing a build dir that           was built for a different target.
REM   --any-profile skip the whitelist (harness-internal build dirs such
REM                 as the M7-E mode_a/b/c dirs; charset check only).
REM   probe         run build\win32_probe\win32_com_probe.exe against the
REM                 port (CH340 link check; --pulse-reset pulses EN).
REM
REM   Exit codes:
REM     0  OK (or dry-run validation passed)
REM     2  usage error (unknown argument / unknown profile / bad port)
REM     3  ESP-IDF profile missing / probe failed / win32_com_probe missing
REM     4  COM port not found (probe open failure also maps here)
REM     5  firmware artifact missing (bin or flash_args; run build first)
REM     6  flash failed (idf.py / esptool) or probe run failed
REM
REM Security: prints only the firmware path + profile label. Never prints
REM esp32\sdkconfig content; never touches Wi-Fi credentials (SSID /
REM password stay in the untracked per-profile sdkconfig).
REM ============================================================
setlocal

if "%ESPIDF_PROFILE%"=="" set "ESPIDF_PROFILE=C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1"
if "%ESPVIEW_PYTHON%"=="" set "ESPVIEW_PYTHON=py"
where "%ESPVIEW_PYTHON%" >nul 2>nul
if errorlevel 1 (
    set "ESPVIEW_PYTHON=python"
    where python >nul 2>nul
    if errorlevel 1 set "ESPVIEW_PYTHON=py"
)
set "PORT=COM4"
set "BAUD=115200"
set "ESP32_PROFILE=uart_hw"
set "ESP32_TARGET=esp32"
set "NO_RESET="
set "DRY_RUN="
set "ANY_PROFILE="
set "PROBE="
set "PROBE_PULSE="
set "PROBE_TIMEOUT="
for %%i in ("%~dp0..") do set "ROOT=%%~fi"

:parse
if "%~1"=="" goto :parsed
if /i "%~1"=="-p" ( if "%~2"=="" goto :usage
                    set "PORT=%~2" & shift & shift & goto :parse )
if /i "%~1"=="--baud" ( if "%~2"=="" goto :usage
                    set "BAUD=%~2" & shift & shift & goto :parse )
if /i "%~1"=="-b" ( if "%~2"=="" goto :usage
                    set "ESP32_PROFILE=%~2" & shift & shift & goto :parse )
if /i "%~1"=="--profile" ( if "%~2"=="" goto :usage
                    set "ESP32_PROFILE=%~2" & shift & shift & goto :parse )
if /i "%~1"=="-t" ( if "%~2"=="" goto :usage
                    set "ESP32_TARGET=%~2" & shift & shift & goto :parse )
if /i "%~1"=="--target" ( if "%~2"=="" goto :usage
                    set "ESP32_TARGET=%~2" & shift & shift & goto :parse )
if /i "%~1"=="--no-reset" ( set "NO_RESET=1" & shift & goto :parse )
if /i "%~1"=="--dry-run"  ( set "DRY_RUN=1"  & shift & goto :parse )
if /i "%~1"=="--check"    ( set "DRY_RUN=1"  & shift & goto :parse )
if /i "%~1"=="--any-profile" ( set "ANY_PROFILE=1" & shift & goto :parse )
if /i "%~1"=="probe"      ( set "PROBE=1" & shift & goto :parse )
if /i "%~1"=="verify"     ( set "PROBE=1" & shift & goto :parse )
if /i "%~1"=="profile"    ( shift & goto :profile_parse )
if /i "%~1"=="flash"      ( shift & goto :parse )
if /i "%~1"=="check"      ( set "DRY_RUN=1"  & shift & goto :parse )
if /i "%~1"=="dry-run"    ( set "DRY_RUN=1"  & shift & goto :parse )
if /i "%~1"=="--pulse-reset" ( set "PROBE_PULSE=1" & shift & goto :parse )
if /i "%~1"=="--timeout-ms" ( if "%~2"=="" goto :usage
                    set "PROBE_TIMEOUT=%~2" & shift & shift & goto :parse )
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
echo %BAUD%|findstr /r /c:"^[0-9][0-9]*$" >nul
if errorlevel 1 (
    echo [flash] ERROR: invalid baud: %BAUD%  ^(expected digits^)
    set "ERR=2" & goto :usage
)
echo %ESP32_PROFILE%|findstr /r /c:"[^a-zA-Z0-9_-]" >nul
if not errorlevel 1 (
    echo [flash] ERROR: invalid profile name: %ESP32_PROFILE%
    set "ERR=2" & goto :usage
)
echo %ESP32_TARGET%|findstr /i /r /c:"^esp32$" >nul
if not errorlevel 1 goto :target_ok
echo %ESP32_TARGET%|findstr /i /r /c:"^esp32s3$" >nul
if not errorlevel 1 goto :target_ok
echo [flash] ERROR: invalid target: %ESP32_TARGET%  ^(expected esp32 or esp32s3^)
set "ERR=2" & goto :usage
:target_ok


REM ---- probe mode: no whitelist / ESP-IDF / bin needed ---------
if defined PROBE goto :probe_entry
REM ---- profile whitelist --------------------------------------
if defined ANY_PROFILE goto :prof_ok
"%ESPVIEW_PYTHON%" "%ROOT%\scripts\espview_profile_sdkconfig.py" --check %ESP32_PROFILE% >nul
if errorlevel 2 goto :bad_profile
if errorlevel 1 goto :prof_tool_fail
:prof_ok

echo ============================================================
echo ESPView flash: port=%PORT% baud=%BAUD% profile=%ESP32_PROFILE% target=%ESP32_TARGET%
echo                no-reset=%NO_RESET% dry-run=%DRY_RUN%
echo ============================================================
if not defined ANY_PROFILE (
    echo [flash] profile summary:
    "%ESPVIEW_PYTHON%" "%ROOT%\scripts\espview_profile_sdkconfig.py" --show %ESP32_PROFILE%
    if errorlevel 1 goto :prof_tool_fail
)

REM ---- preflight: ESP-IDF --------------------------------------
if not exist "%ESPIDF_PROFILE%" goto :no_idf
echo [flash] probing ESP-IDF profile: %ESPIDF_PROFILE%
powershell -NoProfile -ExecutionPolicy Bypass -Command "& { . '%ESPIDF_PROFILE%'; if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) { Write-Host '[flash] idf.py not available after profile load'; exit 3 }; idf.py --version; if ($LASTEXITCODE -ne 0) { exit 3 }; exit 0 }"
if errorlevel 1 goto :no_idf

REM ---- COM port check ------------------------------------------
powershell -NoProfile -ExecutionPolicy Bypass -Command "if ([System.IO.Ports.SerialPort]::GetPortNames() -contains '%PORT%') { exit 0 } else { Write-Host ('[flash] COM port not found: ' + '%PORT%'); exit 1 }"
if errorlevel 1 goto :no_com

REM ---- per-target build dir (M8-C C4) ----------------------------
REM Flash is artifact-only: it never reconfigures, never applies the
REM profile and never rewrites sdkconfig. It only flashes artifacts
REM produced by a previous build in esp32\build\<target>\<profile>.
if defined ANY_PROFILE (
    set "ESP32_BUILD_DIR=%ROOT%\esp32\build\%ESP32_PROFILE%"
) else (
    set "ESP32_BUILD_DIR=%ROOT%\esp32\build\%ESP32_TARGET%\%ESP32_PROFILE%"
)

REM ---- firmware binary check ------------------------------------
set "BIN=%ESP32_BUILD_DIR%\espview_esp32.bin"
if not exist "%BIN%" goto :no_bin

REM ---- flash_args preflight (only used by --no-reset) -----------
set "FLASH_ARGS_FILE=%ESP32_BUILD_DIR%\flash_args"
if defined NO_RESET if not exist "%FLASH_ARGS_FILE%" goto :no_flash_args

REM ---- traceability: target / profile / sdkconfig / build dir ----
set "PROFILE_SDKCONFIG=%ESP32_BUILD_DIR%\sdkconfig"
if defined ANY_PROFILE goto :trace_done
if not exist "%PROFILE_SDKCONFIG%" goto :no_sdkconfig
REM read-only CONFIG_IDF_TARGET cross-check (never prints sdkconfig content)
powershell -NoProfile -ExecutionPolicy Bypass -Command "$line = Select-String -Path '%PROFILE_SDKCONFIG%' -Pattern '^CONFIG_IDF_TARGET=' | Select-Object -First 1; if ($null -eq $line) { Write-Host ('[flash] ERROR: CONFIG_IDF_TARGET missing in ' + '%PROFILE_SDKCONFIG%'); exit 1 }; $value = (($line.Line -split '=')[1]).Trim().Trim([char]34); if ($value -ne '%ESP32_TARGET%') { Write-Host ('[flash] ERROR: build dir target ' + $value + ' != requested target %ESP32_TARGET%'); exit 1 }; Write-Host ('[flash] target check: ' + $value + ' OK'); exit 0"
if errorlevel 1 goto :target_mismatch
:trace_done

set "PROFILE_LABEL=%ESP32_PROFILE%"
if /i "%ESP32_PROFILE%"=="uart" set "PROFILE_LABEL=uart (UART baseline: OLED, LVGL, F12 hooks)"
if /i "%ESP32_PROFILE%"=="tcp" set "PROFILE_LABEL=tcp (TCP production: Wi-Fi STA + TCP, OLED, LVGL)"

echo.
echo [flash] Will flash (esptool via ESP-IDF v6.0.2):
echo   Profile : %PROFILE_LABEL%
echo   Target  : %ESP32_TARGET%
echo   BuildDir: %ESP32_BUILD_DIR%
echo   Sdkconfig: %PROFILE_SDKCONFIG%  ^(read-only; not modified by flash^)
echo   Firmware: %BIN%
echo   Port    : %PORT%
if defined NO_RESET echo   Reset   : no-reset ^(esptool --after no-reset^)

if defined DRY_RUN (
    echo.
    echo [flash] DRY-RUN: validation passed, flash NOT performed.
    exit /b 0
)

if defined NO_RESET goto :flash_noreset
echo [flash] running: idf.py -B build\%ESP32_TARGET%\%ESP32_PROFILE% -p %PORT% flash (artifact-only; no reconfigure)
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; try { & { . '%ESPIDF_PROFILE%' }; Set-Location '%ROOT%\esp32'; idf.py -B build/%ESP32_TARGET%/%ESP32_PROFILE% -p %PORT% flash; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE } } catch { Write-Host ('[flash] ERROR: ' + $_.Exception.Message); exit 6 }"
goto :flash_check

:flash_noreset
REM --no-reset: esptool --after must precede the write-flash subcommand
REM (esptool v5.3.1 rejects --after after write-flash). Call esptool
REM directly using the flash_args file generated by idf.py.
REM M8-A7（A7-7）：--chip 由 --target 确定（esp32 / esp32s3），不再硬编码 esp32。
echo [flash] running: esptool --chip %ESP32_TARGET% -p %PORT% -b 460800 --before default-reset --after no-reset write-flash @flash_args
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; try { & { . '%ESPIDF_PROFILE%' }; Set-Location '%ESP32_BUILD_DIR%'; & ($env:IDF_PYTHON_ENV_PATH + '\Scripts\python.exe') -m esptool --chip %ESP32_TARGET% -p '%PORT%' -b 460800 --before default-reset --after no-reset write-flash '@flash_args'; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE } } catch { Write-Host ('[flash] ERROR: ' + $_.Exception.Message); exit 6 }"

:flash_check
if errorlevel 1 ( set "ERR=6" & goto :fail )

echo.
echo [flash] ALL PASS
exit /b 0

REM ---- probe (win32_com_probe) ---------------------------------
:probe_entry
set "PROBE_EXE=%ROOT%\build\win32_probe\win32_com_probe.exe"
if not exist "%PROBE_EXE%" (
    if defined WIN32_COM_PROBE if exist "%WIN32_COM_PROBE%" set "PROBE_EXE=%WIN32_COM_PROBE%"
)
if not exist "%PROBE_EXE%" goto :no_probe_exe
REM MinGW-built win32_com_probe.exe needs the MSYS2 runtime DLLs on PATH.
if exist "C:\msys64\mingw64\bin" set "PATH=C:\msys64\mingw64\bin;C:\msys64\usr\bin;%PATH%"
echo ============================================================
echo ESPView probe: %PROBE_EXE%
echo                port=%PORT% baud=%BAUD% pulse-reset=%PROBE_PULSE%
echo ============================================================
set "PROBE_CMD=%PROBE_EXE% --port %PORT% --baud %BAUD%"
if defined PROBE_PULSE set "PROBE_CMD=%PROBE_CMD% --pulse-reset"
if defined PROBE_TIMEOUT set "PROBE_CMD=%PROBE_CMD% --timeout-ms %PROBE_TIMEOUT%"
if defined DRY_RUN (
    echo.
    echo [probe] DRY-RUN: would run: %PROBE_CMD%
    exit /b 0
)
echo [probe] running: %PROBE_CMD%
%PROBE_CMD%
set "PROBE_EL=%ERRORLEVEL%"
if "%PROBE_EL%"=="0" ( echo. & echo [probe] ALL PASS & exit /b 0 )
if "%PROBE_EL%"=="3" ( echo [probe] ERROR: COM port open failed: %PORT% & set "ERR=4" & goto :fail )
echo [probe] ERROR: probe failed ^(exit %PROBE_EL%^)
set "ERR=6" & goto :fail

:no_probe_exe
echo [probe] ERROR: win32_com_probe.exe not found
echo [probe] ERROR: expected at build\win32_probe\win32_com_probe.exe ^(or set WIN32_COM_PROBE^)
echo [probe] ERROR: build it from pc\src\win32_com_probe.cpp ^(see pc\CMakeLists.txt^)
set "ERR=3" & goto :fail

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

REM ---- error handlers -------------------------------------------
:bad_profile
echo [flash] ERROR: unknown profile name: %ESP32_PROFILE%
echo [flash] ERROR: whitelist: uart tcp oled oled-off diagnostic g1_a g1_b g1_c g1_d ^(uart_hw alias^)
echo [flash] ERROR: list all profiles: scripts\espview_build.bat profile list
echo [flash] ERROR: harness-internal dirs need --any-profile ^(e.g. M7-E mode_a/b/c^)
set "ERR=2" & goto :usage

:prof_tool_fail
echo [flash] ERROR: profile tool failed - python missing? set ESPVIEW_PYTHON to a working interpreter
set "ERR=3" & goto :fail

:no_idf
echo [flash] ERROR: ESP-IDF unavailable
echo [flash] ERROR: expected profile at %ESPIDF_PROFILE% ^(run the Espressif installer or set ESPIDF_PROFILE^)
set "ERR=3" & goto :fail

:no_com
echo [flash] ERROR: COM port not found: %PORT%
echo [flash] ERROR: check cable/driver ^(CH340^) and close any serial monitor holding the port
set "ERR=4" & goto :fail

:no_bin
echo [flash] ERROR: firmware binary not found: %BIN%
echo [flash] ERROR: build the firmware first: scripts\espview_build.bat -esp32 -b %ESP32_PROFILE% -t %ESP32_TARGET%
set "ERR=5" & goto :fail

:no_sdkconfig
echo [flash] ERROR: sdkconfig not found: %PROFILE_SDKCONFIG%
echo [flash] ERROR: build the firmware first (flash never reconfigures)
set "ERR=5" & goto :fail

:target_mismatch
echo [flash] ERROR: build dir target does not match -t %ESP32_TARGET%
echo [flash] ERROR: build with: scripts\espview_build.bat -esp32 -b %ESP32_PROFILE% -t %ESP32_TARGET%
set "ERR=5" & goto :fail

:no_flash_args
echo [flash] ERROR: flash_args not found: %FLASH_ARGS_FILE%
echo [flash] ERROR: --no-reset needs idf.py-generated flash_args; build the firmware first
set "ERR=5" & goto :fail

:usage
if not defined ERR set "ERR=2"
echo.
echo Usage: scripts\espview_flash.bat [flash] [-p COM4] [-b ^<profile^>] [-t ^<target^>] [--no-reset] [--dry-run]
echo   -p ^<port^>      serial port, default COM4 ^(bare number allowed: 4 = COM4^)
echo   -b ^<profile^>   ESP32 build profile ^(whitelist; default uart^)
echo   -t ^<target^>   IDF target ^(esp32 default; esp32s3^); used for esptool --chip
echo   --no-reset   skip chip reset after flashing ^(esptool --after no-reset^)
echo   --dry-run    validate only, do not flash
echo   --any-profile skip the whitelist ^(harness-internal dirs only^)
echo   probe        run win32_com_probe against the port ^(--pulse-reset optional^)
echo   profile      profile list / show ^<name^> / check ^<name^>
echo   -h/--help    show this help
exit /b %ERR%

:fail
echo.
echo espview_flash: FAILED (exit code %ERR%)
exit /b %ERR%
