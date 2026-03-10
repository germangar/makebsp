@echo off
setlocal enabledelayedexpansion

if "%~1" == "" (
    echo Usage: Drag and drop an .iqm file onto this script, or run:
    echo %~nx0 ^<input_file.iqm^>
    pause
    exit /b 1
)

set "INPUT=%~1"
set "EXT=%~x1"

if /i "%EXT%" == ".iqm" (
    set "OUTPUT_GLB=%~dpn1.glb"
    set "OUTPUT_FBX=%~dpn1.fbx"
) else (
    echo Error: For this test environment, please use .iqm files.
    pause
    exit /b 1
)

echo Converting: %INPUT%
echo To GLB:     %OUTPUT_GLB%
echo To FBX:     %OUTPUT_FBX%
echo.

"%~dp0iqm2glb.exe" "%INPUT%" "%OUTPUT_GLB%"
set GLB_ERR=!ERRORLEVEL!

"%~dp0iqm2glb.exe" "%INPUT%" "%OUTPUT_FBX%"
set FBX_ERR=!ERRORLEVEL!

echo.
if !GLB_ERR! NEQ 0 (
    echo GLB Conversion FAILED with error code !GLB_ERR!.
) else (
    echo GLB Conversion SUCCESSFUL: %OUTPUT_GLB% generated.
)

if !FBX_ERR! NEQ 0 (
    echo FBX Conversion FAILED with error code !FBX_ERR!.
) else (
    echo FBX Conversion SUCCESSFUL: %OUTPUT_FBX% generated.
)

pause
