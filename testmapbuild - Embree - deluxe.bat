q3map.exe -samplesize 2 maps/modeltest.map
q3map.exe -vis maps/modeltest.map
light.exe -radiosity 4 -smooth 4 -smoothradius 0.35 -rad_intensity 0.5 -rad_depthintensity 1.0 -deluxe_minangle 30 -rad_depthmin 0.0 -rad_depthmax 32.0 maps/modeltest.bsp
copy /Y maps\modeltest.bsp "C:\Users\German\Documents\My Games\Warfork 2.1\basewf\maps"
pause