q3map.exe -samplesize 4 maps/modeltest.map
q3map.exe -vis maps/modeltest.map
light.exe -radiosity 0.5 maps/modeltest.bsp
copy /Y maps\modeltest.bsp "E:\CODE\warsow_21\basewsw\maps"
copy /Y maps\modeltest.bsp "C:\Users\German\Documents\My Games\Warfork 2.1\basewf\maps
pause