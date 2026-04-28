q3map.exe -samplesize 4 maps/modeltest.map
light.exe -antialiasing 14 -smooth 0 -smoothradius 0.25 -opencl 1 maps/modeltest.bsp
copy /Y maps\modeltest.bsp "C:\Users\German\Documents\My Games\Warfork 2.1\basewf\maps"
pause