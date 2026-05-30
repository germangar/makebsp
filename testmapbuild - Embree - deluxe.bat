@echo off
set "USERDIR=C:/Users/German/Documents/My Games/Warfork 2.1"
set "GAMEDIR=basewf"
set "MAPNAME=modeltest"
set "MAPFILE=%USERDIR%/%GAMEDIR%/maps/%MAPNAME%.map"
set "BSPFILE=%USERDIR%/%GAMEDIR%/maps/%MAPNAME%.bsp"
set "DESTDIR=E:\CODE\warsow_21\basewsw\maps"

echo Compiling: %MAPNAME%
makebsp.exe -guessuvs -userdir "%USERDIR%" -gamedir "%GAMEDIR%" "%MAPFILE%"
makebsp.exe -vis -userdir "%USERDIR%" -gamedir "%GAMEDIR%" "%BSPFILE%"
light.exe -userdir "%USERDIR%" -gamedir "%GAMEDIR%" "%BSPFILE%"

echo.
echo Copying %MAPNAME%.bsp to %DESTDIR%...
copy /Y "%BSPFILE:/=\%" "%DESTDIR%"

pause