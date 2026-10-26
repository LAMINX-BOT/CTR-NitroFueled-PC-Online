@echo off
rem CTR Nitro-Fueled PC without the launcher (offline; CTR-NitroFueled-PC.exe does the same with
rem its window, and online play needs it). The merged game goes in .\CUSA14876-1.21 or
rem .\CUSA13795-1.21 (or set BB_GAME_DIR). In-game port menu: Insert, or L3+R3 on the controller.
setlocal
cd /d "%~dp0"
if "%BB_GAME_DIR%"=="" if exist "%~dp0CUSA14876-1.21" set "BB_GAME_DIR=%~dp0CUSA14876-1.21"
if "%BB_GAME_DIR%"=="" set "BB_GAME_DIR=%~dp0CUSA13795-1.21"
set "PYEXE=python"
set "PYARGS="
if exist "%~dp0python\python.exe" (
    set "PYEXE=%~dp0python\python.exe"
) else (
    where py >nul 2>nul && set "PYEXE=py" && set "PYARGS=-3"
)
"%PYEXE%" %PYARGS% -X utf8 run.py %*
if %errorlevel% neq 0 (
    echo.
    echo The game ended with an error. See last_run.log and README-AR.txt / ONLINE.md.
    pause
)
