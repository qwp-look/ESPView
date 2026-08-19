@echo off
REM ============================================================
REM ESPView M2 / M8-C C5: Qt 6 Virtual Display host build check + runtime deploy.
REM
REM Configures pc/ with ESPVIEW_BUILD_QT_GUI=ON and builds
REM espview_virtual_display (Qt 6 Widgets + SerialPort, C++17).
REM M8-C C5: deploys Qt6 DLLs + MinGW runtime + platforms/imageformats/
REM iconengines plugins into build/verify_qt/, then does a clean-PATH smoke
REM (PATH=C:\Windows\System32 only) to prove the exe runs standalone.
REM Does NOT require COM3 / ESP32 (GUI app runs only when the
REM user starts it against a real port).
REM Requires: MSYS2 MinGW64 with Qt 6 (C:\msys64\mingw64).
REM Override the compiler bin dir with MINGW64_BIN if needed.
REM ============================================================
setlocal

if "%MINGW64_BIN%"=="" set "MINGW64_BIN=C:\msys64\mingw64\bin"
set "PATH=%MINGW64_BIN%;%PATH%"

set "ROOT=%~dp0.."
set "BUILD=%ROOT%\build\verify_qt"

echo [1/3] Configure pc/ with Qt GUI target
cmake -S "%ROOT%\pc" -B "%BUILD%" -G "MinGW Makefiles" -DESPVIEW_BUILD_QT_GUI=ON
if errorlevel 1 goto :fail

echo [2/5] Build espview_virtual_display
cmake --build "%BUILD%" --target espview_virtual_display -j 8
if errorlevel 1 goto :fail

echo [3/5] Qt GUI target exists
if not exist "%BUILD%\espview_virtual_display.exe" goto :fail

REM ---- M8-C C5: deploy Qt runtime (standalone, no user PATH needed) ----
set "QT_PREFIX=%MINGW64_BIN%\..\share\qt6"
set "QT_PLUGINS=%QT_PREFIX%\plugins"
echo [4/5] Deploy Qt6 + MinGW runtime + plugins to %BUILD%

REM Qt6 modules imported by espview_virtual_display (objdump -p): Core/Gui/Widgets.
REM Copy them plus SerialPort/Network defensively (tiny; future-proof).
for %%M in (Core Gui Widgets SerialPort Network) do (
    if exist "%MINGW64_BIN%\Qt6%%M.dll" copy /y "%MINGW64_BIN%\Qt6%%M.dll" "%BUILD%\" >nul
)

REM MinGW runtime DLLs (libgcc/libstdc++/libwinpthread).
for %%D in (libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll) do (
    if exist "%MINGW64_BIN%\%%D" copy /y "%MINGW64_BIN%\%%D" "%BUILD%\" >nul
)

REM Qt platform + image + icon plugins (qwindows.dll is mandatory; qoffscreen.dll
REM keeps headless CI smoke possible; imageformats for .ico/.jpg/.png resources).
if not exist "%QT_PLUGINS%\platforms\qwindows.dll" goto :fail
if not exist "%BUILD%\platforms" mkdir "%BUILD%\platforms"
copy /y "%QT_PLUGINS%\platforms\qwindows.dll" "%BUILD%\platforms\" >nul
if exist "%QT_PLUGINS%\platforms\qoffscreen.dll" copy /y "%QT_PLUGINS%\platforms\qoffscreen.dll" "%BUILD%\platforms\" >nul
if not exist "%BUILD%\imageformats" mkdir "%BUILD%\imageformats"
copy /y "%QT_PLUGINS%\imageformats\*.dll" "%BUILD%\imageformats\" >nul 2>nul
if exist "%QT_PLUGINS%\iconengines" (
    if not exist "%BUILD%\iconengines" mkdir "%BUILD%\iconengines"
    copy /y "%QT_PLUGINS%\iconengines\*.dll" "%BUILD%\iconengines\" >nul 2>nul
)

REM ---- M8-C C5: clean-PATH smoke (no MSYS2 / Qt on PATH) -----------
echo [5/5] Clean-PATH smoke: launch exe with PATH=C:\Windows\System32 only
powershell -NoProfile -ExecutionPolicy Bypass -Command "$env:PATH='C:\Windows\System32'; $exe='%BUILD%\espview_virtual_display.exe'; $p = Start-Process -FilePath $exe -PassThru; Start-Sleep -Seconds 3; if ($p.HasExited) { Write-Host ('[qt] clean-PATH smoke FAILED: exited code ' + $p.ExitCode); exit 1 } else { Write-Host ('[qt] clean-PATH smoke OK (pid ' + $p.Id + ' alive, Qt runtime deployed)'); Stop-Process -Id $p.Id -Force; exit 0 }"
if errorlevel 1 goto :fail

echo.
echo verify_qt: ALL PASS (espview_virtual_display.exe + standalone Qt runtime)
exit /b 0

:fail
echo.
echo verify_qt: FAILED
exit /b 1
