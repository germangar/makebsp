@echo off
if "%~1"=="" (
    echo =======================================================
    echo KTX Image Converter for MakeBSP
    echo =======================================================
    echo Please drag and drop a .ktx file onto this script.
    echo.
    pause
    exit /b
)

echo Converting KTX file: "%~1"
"%~dp0makebsp.exe" -convertKTX "%~1"
//pause
