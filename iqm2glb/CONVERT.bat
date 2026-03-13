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
set "BASENAME=%~dpn1"

if /i "%EXT%" == ".iqm" (
    set "OUT1=%BASENAME%.glb"
    set "OUT2=%BASENAME%.fbx"
    set "TYPE1=GLB"
    set "TYPE2=FBX"
) else if /i "%EXT%" == ".glb" (
    set "OUT1=%BASENAME%.iqm"
    set "OUT2=%BASENAME%.fbx"
    set "TYPE1=IQM"
    set "TYPE2=FBX"
) else if /i "%EXT%" == ".fbx" (
    set "OUT1=%BASENAME%.iqm"
    set "OUT2=%BASENAME%.glb"
    set "TYPE1=IQM"
    set "TYPE2=GLB"
) else (
    echo Error: Unsupported file extension: %EXT%
    echo Please use .iqm, .glb, or .fbx files.
    pause
    exit /b 1
)

echo Converting: %INPUT%
echo To %TYPE1%:     %OUT1%
echo To %TYPE2%:     %OUT2%
echo.

"%~dp0iqm2glb.exe" "%INPUT%" "%OUT1%"
set ERR1=!ERRORLEVEL!

"%~dp0iqm2glb.exe" "%INPUT%" "%OUT2%"
set ERR2=!ERRORLEVEL!

echo.
if !ERR1! NEQ 0 (
    echo %TYPE1% Conversion FAILED with error code !ERR1!.
) else (
    echo %TYPE1% Conversion SUCCESSFUL: %OUT1% generated.
)

if !ERR2! NEQ 0 (
    echo %TYPE2% Conversion FAILED with error code !ERR2!.
) else (
    echo %TYPE2% Conversion SUCCESSFUL: %OUT2% generated.
)

pause
