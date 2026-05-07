q3map.exe -samplesize 4 maps/modeltest.map
q3map.exe -vis maps/modeltest.map
light.exe -deluxe 0 -fast -antialiasing 0 -smooth 4 -falloff lambert -radiosity 4 -opencl 1 maps/modeltest.bsp
copy /Y maps\modeltest.bsp "C:\Users\German\Documents\My Games\Warfork 2.1\basewf\maps"
pause