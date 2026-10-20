@echo off
setlocal

rem ============================================================
rem  topHeightmap.bat - offset a heightmap to the top of its
rem  range, in place.
rem ============================================================
rem  Drag and drop ONE file onto this script:
rem    - a standalone heightmap: _h _height _hmap _offset _disp
rem      -> greyscale is re-anchored in place AND, if a sibling
rem         normalmap exists next to it, embedded into its alpha
rem    - a normalmap with height in the alpha channel:
rem      _norm _nrm _local _ddn _n _nh -> alpha re-anchored in place
rem
rem  Optional arguments after the file select the transform:
rem    bottom  ,  center  ,  range anchor lo hi  ,  -channel c
rem  If none is given, the default anchor "top" is used.
rem
rem  NOTE: files are REWRITTEN IN PLACE. Back them up first.
rem  jpg files are skipped: degrading format, no writer.
rem ============================================================

if "%~1"=="" goto usage

set "INPUT=%~1"
set "MAKEBSP=E:\CODE\antigravity\makebsp\makebsp.exe"
if exist "%~dp0makebsp.exe" set "MAKEBSP=%~dp0makebsp.exe"

echo Processing heightmap: "%INPUT%"
echo.

"%MAKEBSP%" -fs_pakpath "C:/Users/German/Documents/My Games/Warfork 2.1/basewf/" -fs_basepath "C:\Program Files (x86)\Steam\steamapps\common\fvi" -fs_game basewf -game warfork -heightmap "%INPUT%" %2 %3 %4 %5 %6 -embed

echo.
pause
exit /b

:usage
echo =======================================================
echo Heightmap "offset to top" tool for MakeBSP
echo =======================================================
echo Drag and drop a heightmap or a height-in-alpha normalmap
echo onto this script.
echo.
echo Optional arguments after the file select the transform:
echo   bottom , center , range anchor lo hi , -channel c
echo Without them the default anchor "top" is used.
echo.
pause
endlocal
