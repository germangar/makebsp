@echo off
if "%~1"=="" (
    echo =======================================================
    echo Decal Font generator for MakeBSP
    echo =======================================================
    echo Please drag and drop a font file file onto this script.
    echo.
    pause
    exit /b
)

echo Generating font image and descriptor file: "%~1"
"%~dp0makebsp.exe" -fontatlas "%~1" 2048
pause
