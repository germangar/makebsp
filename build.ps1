$msysPath = "E:\CODE\msys64\mingw64\bin"
$env:PATH = "$msysPath;$env:PATH"

Clear-Host
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "      Q3MAP & LIGHT POWERSHELL BUILD" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "MSYS2 Path: $msysPath"
Write-Host ""

# Kill running instances
Get-Process makebsp -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process light -ErrorAction SilentlyContinue | Stop-Process -Force

Write-Host "[1/2] Running make clean && make..." -ForegroundColor Yellow
& make clean
& make

Write-Host ""
if ($LASTEXITCODE -eq 0) {
    Write-Host "----------------------------------------" -ForegroundColor Green
    Write-Host "        SUCCESS: BUILD COMPLETE!" -ForegroundColor Green
    Write-Host "----------------------------------------" -ForegroundColor Green
    
    Write-Host ""
    $UserDir = "C:/Users/German/Documents/My Games/Warfork 2.1"
    $GameDir = "basewf"
    $MapName = "modeltest"
    $MapFile = "$UserDir/$GameDir/maps/$MapName.map"
    $BspFile = "$UserDir/$GameDir/maps/$MapName.bsp"

    Write-Host "[2/3] Running makebsp -v $MapFile..." -ForegroundColor Cyan
    Write-Host "----------------------------------------" -ForegroundColor Gray
    # Run the map and show relevant optimization stats
    ./makebsp.exe -v -samplesize 16 -userdir "$UserDir" -gamedir "$GameDir" "$MapFile"
    Write-Host ""
    Write-Host "[3/3] Running light -rad_passes 0 -v $BspFile..." -ForegroundColor Cyan
    Write-Host "----------------------------------------" -ForegroundColor Gray
    ./light.exe -smoothpasses 1 -rad_passes 0 -v -userdir "$UserDir" -gamedir "$GameDir" "$BspFile"
} else {
    Write-Host "----------------------------------------" -ForegroundColor Red
    Write-Host "        ERROR: BUILD FAILED!" -ForegroundColor Red
    Write-Host "----------------------------------------" -ForegroundColor Red
    Write-Host "Exit Code: $LASTEXITCODE" -ForegroundColor Red
}

Write-Host ""
#Write-Host "Press any key to exit..."
#$null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
