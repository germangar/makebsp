@echo off
setlocal enabledelayedexpansion

if "%~1" == "" (
    echo Usage: Drag and drop an .iqm or .glb/.gltf file onto this script, or run:
    echo %~nx0 ^<input_file^>
    pause
    exit /b 1
)

set "INPUT=%~1"
set "EXT=%~x1"

if /i "%EXT%" == ".iqm" (
    set "OUTPUT=%~dpn1.glb"
) else if /i "%EXT%" == ".glb" (
    set "OUTPUT=%~dpn1.iqm"
) else if /i "%EXT%" == ".gltf" (
    set "OUTPUT=%~dpn1.iqm"
) else (
    echo Error: Unsupported file extension "%EXT%".
    echo Please use .iqm, .glb, or .gltf
    pause
    exit /b 1
)

echo Converting: %INPUT%
echo To:         %OUTPUT%
echo.

"%~dp0iqm2glb.exe" "%INPUT%" "%OUTPUT%"

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Conversion FAILED with error code %ERRORLEVEL%.
) else (
    echo.
    echo Conversion SUCCESSFUL: %OUTPUT% generated.
)

pause
