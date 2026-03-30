q3map.exe -samplesize 2 maps/modeltest.map
light.exe -falloff lambert -smooth 1 maps/modeltest.bsp
copy /Y maps\modeltest.bsp "C:\Users\German\Documents\My Games\Warfork 2.1\basewf\maps"
pause