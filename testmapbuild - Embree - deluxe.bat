q3map.exe -samplesize 4 maps/modeltest.map
q3map.exe -vis maps/modeltest.map
light.exe -mao_gather_radius 256 -mao_radius 512 -radiosity 1.0 -deluxe_ambient_exaggerate 1.0 -deluxe_radiosity_exaggerate 1.0 maps/modeltest.bsp
copy /Y maps\modeltest.bsp "C:\Users\German\Documents\My Games\Warfork 2.1\basewf\maps"
pause