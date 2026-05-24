q3map.exe -samplesize 4 -snapuvs 0 -guessuvs maps/modeltest.map
q3map.exe -vis maps/modeltest.map
light.exe -mao_gather_radius 256 -mao_radius 512 -nodirect -radiosity 0.0 maps/modeltest.bsp
copy /Y maps\modeltest.bsp "C:\Users\German\Documents\My Games\Warfork 2.1\basewf\maps"
pause