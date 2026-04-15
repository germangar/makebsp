q3map.exe -samplesize 4 -forceuvgen maps/modeltest.map
light.exe -falloff lambert -smooth 0 -radiosity 4 -rad_bounce_scale 1.0 -rad_color_ratio 1.0 -rad_min_dist 32 -rad_scale 4 maps/modeltest.bsp
copy /Y maps\modeltest.bsp "C:\Users\German\Documents\My Games\Warfork 2.1\basewf\maps"
pause