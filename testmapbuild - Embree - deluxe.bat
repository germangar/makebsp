q3map.exe -samplesize 4 maps/modeltest.map
q3map.exe -vis maps/modeltest.map
light.exe -radiosity 0.0 maps/modeltest.bsp
copy /Y maps\modeltest.bsp "E:\CODE\warsow_21\basewsw\maps"
pause