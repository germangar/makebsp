q3map.exe -game quake3 -samplesize 8 maps/modeltest.map
light.exe -smooth 1 -radiosity 4 -rad_bounce_scale 1.0 -rad_color_ratio 1.0 -rad_min_dist 32 -rad_interval 4 -upscale maps/modeltest.bsp
copy /Y maps\modeltest.bsp "C:\Users\German\Documents\My Games\Warfork 2.1\basewf\maps"
pause