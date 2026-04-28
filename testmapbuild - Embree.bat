q3map.exe -samplesize 4 maps/modeltest.map
light.exe -radiosity 4 -rad_bounce_scale 1.0 -rad_color_ratio 1.0 -opencl 1 maps/modeltest.bsp
copy /Y maps\modeltest.bsp "C:\Users\German\Documents\My Games\Warfork 2.1\basewf\maps"
pause