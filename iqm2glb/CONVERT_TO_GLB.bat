@echo off
if "%~1"=="" (
    echo Usage: Drag and drop an .iqm file onto this script, or run:
    echo %~nx0 ^<input.iqm^>
    pause
    exit /b 1
)

set INPUT=%~1
set OUTPUT=%~dpn1.glb

echo Converting %INPUT% to %OUTPUT%...
"%~dp0iqm2glb.exe" "%INPUT%" "%OUTPUT%"

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Conversion FAILED!
) else (
    echo.
    echo Conversion SUCCESSFUL: %OUTPUT% generated.
)
pause
