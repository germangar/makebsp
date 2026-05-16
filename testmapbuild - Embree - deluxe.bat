q3map.exe -samplesize 4 maps/modeltest.map
q3map.exe -vis maps/modeltest.map
light.exe -radiosity 4 -smooth 4 -smoothradius 0.25 -falloff_softbias 0.0 -falloff_sun_softbias 0.0 maps/modeltest.bsp
copy /Y maps\modeltest.bsp "C:\Users\German\Documents\My Games\Warfork 2.1\basewf\maps"
pause