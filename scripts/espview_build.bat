@echo off
REM ============================================================
REM ESPView M7-G: one-shot build entrypoint (host / Qt / ESP32).
REM
REM   Usage:
REM     scripts\espview_build.bat [build] [-host] [-qt] [-esp32]
REM                                [-b <profile>|--profile <profile>]
REM                                [-t <target>|--target <target>]
REM                                [--check] [--dry-run] [-h|--help]
REM     scripts\espview_build.bat verify [verify args]  -> espview_verify.bat
REM     scripts\espview_build.bat profile list|show <name>|check <name>
REM
REM   Subcommands: build (default) / verify / profile / check / dry-run
REM   Default (no args): -host -qt -esp32  (all three targets)
REM   -host     : host suite (reuses scripts\verify_host.bat)
REM   -qt       : Qt 6 GUI build (reuses scripts\verify_qt.bat)
REM   -esp32    : ESP32 firmware build (idf.py -B build\<profile> build)
REM   -b <name> : ESP32 build profile (whitelist below; default uart)
REM   --check   : preflight only (PATH / MSYS2 / ESP-IDF probe); no build
REM   --dry-run : print the exact build plan; run nothing
REM   profile   : profile whitelist / label table queries
REM
REM   Profile system (M7-G):
REM     Whitelist: uart tcp oled oled-off diagnostic g1_a g1_b g1_c g1_d
REM                (uart_hw = legacy alias of uart, keeps its old build dir)
REM     Each profile keeps its OWN isolated sdkconfig at
REM     esp32\build\<profile>\sdkconfig, seeded once from esp32\sdkconfig
REM     (whole-file copy, never inspected) then force-applied with the
REM     profile's whitelisted Kconfig keys on every build, so profiles
REM     can never drift (no more mode_c with OLED=y / TCP=y). The shared
REM     esp32\sdkconfig is never overwritten.
REM
REM   Exit codes:
REM     0  all requested steps OK
REM     1  a build/test step failed
REM     2  usage error (unknown argument / unknown profile name)
REM     3  preflight error (MSYS2 MinGW64 / ESP-IDF / python missing)
REM
REM Requires: MSYS2 MinGW64 (g++, cmake, ctest); ESP-IDF v6.0.2
REM   PowerShell profile at
REM   C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1
REM Overrides (environment): MINGW64_BIN, ESPIDF_PROFILE, ESPVIEW_PYTHON.
REM Security: never prints esp32\sdkconfig content; never reads, prints or
REM hardcodes Wi-Fi credentials (seed copy only, credentials stay inside
REM the gitignored per-profile sdkconfig).
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
set "ESP32_PROFILE=uart_hw"
set "ESP32_TARGET=esp32"
for %%i in ("%~dp0..") do set "ROOT=%%~fi"

REM ---- argument parsing -------------------------------------
set "DO_HOST="
set "DO_QT="
set "DO_ESP32="
set "CHECK_ONLY="
set "DRY_RUN="

:parse
if "%~1"=="" goto :parsed
if /i "%~1"=="-host"        ( set "DO_HOST=1"  & shift & goto :parse )
if /i "%~1"=="-qt"          ( set "DO_QT=1"    & shift & goto :parse )
if /i "%~1"=="-esp32"       ( set "DO_ESP32=1" & shift & goto :parse )
if /i "%~1"=="-t"           ( if "%~2"=="" goto :usage
                              set "ESP32_TARGET=%~2" & shift & shift & goto :parse )
if /i "%~1"=="--target"     ( if "%~2"=="" goto :usage
                              set "ESP32_TARGET=%~2" & shift & shift & goto :parse )
if /i "%~1"=="-b"           ( if "%~2"=="" goto :usage
                              set "ESP32_PROFILE=%~2" & shift & shift & goto :parse )
if /i "%~1"=="--profile"    ( if "%~2"=="" goto :usage
                              set "ESP32_PROFILE=%~2" & shift & shift & goto :parse )
if /i "%~1"=="--check"      ( set "CHECK_ONLY=1" & shift & goto :parse )
if /i "%~1"=="--dry-run"    ( set "DRY_RUN=1"    & shift & goto :parse )
if /i "%~1"=="verify"       goto :do_verify
if /i "%~1"=="profile"      ( shift & goto :profile_parse )
if /i "%~1"=="build"        ( shift & goto :parse )
if /i "%~1"=="check"        ( set "CHECK_ONLY=1" & shift & goto :parse )
if /i "%~1"=="dry-run"      ( set "DRY_RUN=1"    & shift & goto :parse )
if /i "%~1"=="-h"   ( set "ERR=0" & goto :usage )
if /i "%~1"=="--help" ( set "ERR=0" & goto :usage )
if /i "%~1"=="/?"   ( set "ERR=0" & goto :usage )
echo [build] ERROR: unknown argument: %~1
set "ERR=2" & goto :usage

:parsed
if not defined DO_HOST if not defined DO_QT if not defined DO_ESP32 (
    set "DO_HOST=1" & set "DO_QT=1" & set "DO_ESP32=1"
)

REM ---- profile validation (whitelist) -----------------------
if not defined DO_ESP32 goto :prof_ok
echo %ESP32_PROFILE%|findstr /r /c:"[^a-zA-Z0-9_-]" >nul
if not errorlevel 1 (
    echo [build] ERROR: invalid profile name: %ESP32_PROFILE%
    echo [build] ERROR: profile name may contain only A-Z a-z 0-9 _ -
    set "ERR=2" & goto :usage
)
"%ESPVIEW_PYTHON%" "%ROOT%\scripts\espview_profile_sdkconfig.py" --check %ESP32_PROFILE% >nul
if errorlevel 2 goto :bad_profile
if errorlevel 1 (
    echo [build] ERROR: profile tool failed - python missing? set ESPVIEW_PYTHON to a working interpreter
    set "ERR=3" & goto :fail
)
:prof_ok

echo ============================================================
echo ESPView build: host=%DO_HOST% qt=%DO_QT% esp32=%DO_ESP32%
echo                 esp32 profile=%ESP32_PROFILE%  check-only=%CHECK_ONLY% dry-run=%DRY_RUN%
echo ============================================================

if defined DO_ESP32 (
    echo [esp32] target=%ESP32_TARGET%  profile summary:
    "%ESPVIEW_PYTHON%" "%ROOT%\scripts\espview_profile_sdkconfig.py" --show %ESP32_PROFILE%
    if errorlevel 1 goto :prof_tool_fail
)
REM M8-C C4: per-target build dir (esp32\build\<target>\<profile>) - the same
REM profile name never shares a build dir / sdkconfig across IDF targets,
REM so target drift is impossible (build dir is fully traceable).
set "ESP32_BUILD_DIR=%ROOT%\esp32\build\%ESP32_TARGET%\%ESP32_PROFILE%"
set "PROFILE_SDKCONFIG=%ESP32_BUILD_DIR%\sdkconfig"
if defined DO_ESP32 (
    echo [esp32] traceability: target=%ESP32_TARGET% profile=%ESP32_PROFILE%
    echo [esp32] build dir  : %ESP32_BUILD_DIR%
    echo [esp32] sdkconfig  : %PROFILE_SDKCONFIG%  ^(isolated; shared esp32\sdkconfig untouched^)
)

REM ---- dry-run: print plan only ------------------------------
if not defined DRY_RUN goto :not_dry
echo.
echo [build] DRY-RUN plan:
if defined DO_HOST echo   host   : scripts\verify_host.bat
if defined DO_QT   echo   qt     : scripts\verify_qt.bat
if defined DO_ESP32 (
    echo   esp32  : "%ESPVIEW_PYTHON%" scripts\espview_profile_sdkconfig.py --apply %ESP32_PROFILE% --sdkconfig "%PROFILE_SDKCONFIG%" --target %ESP32_TARGET%
    echo   esp32  : idf.py -B build\%ESP32_TARGET%\%ESP32_PROFILE% -D SDKCONFIG=%PROFILE_SDKCONFIG% build
)
echo.
echo [build] DRY-RUN: plan printed, nothing executed.
exit /b 0

:not_dry
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
echo [3/3] ESP32 firmware build (target=%ESP32_TARGET%, profile=%ESP32_PROFILE%, dir=esp32\build\%ESP32_TARGET%\%ESP32_PROFILE%)
REM M7-G: prepare the profile's own isolated sdkconfig (seed once from
REM esp32\sdkconfig without inspecting it, then force-apply the profile's
REM whitelisted Kconfig keys so no drift is possible). Credentials stay
REM inside the untracked per-profile sdkconfig; nothing is ever printed.
REM M8-A7（A7-7）：--target 使 seed/sdkconfig 的 CONFIG_IDF_TARGET 不匹配时
REM 显式失败（防 esp32s3 漂移静默污染 esp32 构建）。
"%ESPVIEW_PYTHON%" "%ROOT%\scripts\espview_profile_sdkconfig.py" --apply %ESP32_PROFILE% --sdkconfig "%PROFILE_SDKCONFIG%" --seed "%ROOT%\esp32\sdkconfig" --seed-defaults "%ROOT%\esp32\sdkconfig.defaults" --target %ESP32_TARGET%
if errorlevel 1 goto :prof_tool_fail
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; try { & { . '%ESPIDF_PROFILE%' }; Set-Location '%ROOT%\esp32'; idf.py -B build/%ESP32_TARGET%/%ESP32_PROFILE% -D SDKCONFIG=%PROFILE_SDKCONFIG% build; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE } } catch { Write-Host ('[esp32] ERROR: ' + $_.Exception.Message); exit 1 }"
if errorlevel 1 ( set "ERR=1" & goto :fail )

:done
echo.
echo espview_build: ALL PASS
exit /b 0

:do_verify
call "%ROOT%\scripts\espview_verify.bat" %*
set "ERR=%ERRORLEVEL%"
exit /b %ERR%

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
echo   profile check ^<name^>      validate a profile name (exit 0/2)
exit /b 0

:profile_usage
echo [profile] ERROR: profile requires a subcommand (list / show ^<name^> / check ^<name^>)
set "ERR=2" & goto :usage

:bad_profile
echo [build] ERROR: unknown profile name: %ESP32_PROFILE%
echo [build] ERROR: whitelist: uart tcp oled oled-off diagnostic g1_a g1_b g1_c g1_d ^(uart_hw alias^)
echo [build] ERROR: list all profiles: scripts\espview_build.bat profile list
set "ERR=2" & goto :usage

:prof_tool_fail
echo [build] ERROR: profile sdkconfig tool failed
echo [build] ERROR: run with ESPVIEW_PYTHON=<path-to-python> to override
set "ERR=3" & goto :fail

:no_msys
echo [build] ERROR: MSYS2 MinGW64 not found in %MINGW64_BIN%
echo [build] ERROR: expected g++.exe, cmake.exe, ctest.exe (install MSYS2 or set MINGW64_BIN)
set "ERR=3" & goto :fail

:no_idf
echo [build] ERROR: ESP-IDF unavailable (profile not found or idf.py probe failed)
echo [build] ERROR: expected profile at %ESPIDF_PROFILE% (run the Espressif installer or set ESPIDF_PROFILE)
set "ERR=3" & goto :fail

:usage
if not defined ERR set "ERR=2"
echo.
echo Usage: scripts\espview_build.bat [build] [-host] [-qt] [-esp32] [-b ^<profile^>] [-t ^<target^>] [--check] [--dry-run]
echo   default (no args): -host -qt -esp32
echo   -b ^<profile^>   ESP32 build profile (whitelist; default uart)
echo   -t ^<target^>   IDF target ^(esp32 default; esp32s3^)
echo   --check     preflight only, no build
echo   --dry-run   print the plan, run nothing
echo   verify      delegate to scripts\espview_verify.bat
echo   profile     profile list / show ^<name^> / check ^<name^>
echo   -h/--help   show this help
exit /b %ERR%

:fail
echo.
echo espview_build: FAILED (exit code %ERR%)
exit /b %ERR%
