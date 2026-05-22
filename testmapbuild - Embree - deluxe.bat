q3map.exe -samplesize 4 maps/modeltest.map
q3map.exe -vis maps/modeltest.map
light.exe -rad_interval 4 -radiosity 4 -deluxe_minangle 15 -smooth 0.35 -mao_gather_radius 256 -mao_radius 512 maps/modeltest.bsp
copy /Y maps\modeltest.bsp "C:\Users\German\Documents\My Games\Warfork 2.1\basewf\maps"
pause