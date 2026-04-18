q3map.exe -samplesize 4 -forceuvgen -snapuvs maps/modeltest.map
light.exe -smooth 1 -upscale -radiosity 4 -rad_bounce_scale 1.0 -rad_color_ratio 1.0 maps/modeltest.bsp
copy /Y maps\modeltest.bsp "C:\Users\German\Documents\My Games\Warfork 2.1\basewf\maps"
pause