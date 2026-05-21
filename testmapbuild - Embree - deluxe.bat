q3map.exe -samplesize 4 maps/modeltest.map
q3map.exe -vis maps/modeltest.map
light.exe -rad_interval 4 -radiosity 0 -supersample 0.0 -smoothradius 0.3 -mao_gather_radius 512 -mao_radius 800 maps/modeltest.bsp
copy /Y maps\modeltest.bsp "C:\Users\German\Documents\My Games\Warfork 2.1\basewf\maps"
pause