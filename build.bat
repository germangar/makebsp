@echo off
:: This batch file launches the PowerShell script so you can just double-click it.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" -release

ECHO [3/3] Running makelight -rad_passes 0 -v C:/Users/German/Documents/My Games/Warfork 2.1/basewf/maps/modeltest.bsp...
ECHO ----------------------------------------
set "DESTDIR=E:\CODE\netradiant-custom-2026-0114"
set "FILE1=makebsp.exe"
set "FILE2=makelight.exe"
echo Copying to %DESTDIR%...
copy /Y "%FILE1:/=\%" "%DESTDIR%"
copy /Y "%FILE2:/=\%" "%DESTDIR%"
pause