q3map.exe -samplesize 8 -forceuvgen maps/modeltest.map
light.exe -surface -smooth 0 -radiosity 2 -rad_bounce_scale 1.0 -rad_color_ratio 1.0 maps/modeltest.bsp
copy /Y maps\modeltest.bsp "C:\Users\German\Documents\My Games\Warfork 2.1\basewf\maps"

copy /Y maps\modeltest.bsp "C:\Users\German\Documents\My Games\Warfork 2.1\basewf\maps"
pause