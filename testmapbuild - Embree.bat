q3map.exe -samplesize 4 maps/modeltest.map
light.exe -falloff halflambert -antialiasing 0 -smooth 4 -smoothradius 0.25 -opencl 1 maps/modeltest.bsp
copy /Y maps\modeltest.bsp "C:\Users\German\Documents\My Games\Warfork 2.1\basewf\maps"
pause