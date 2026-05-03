$msysPath = "E:\CODE\msys64\mingw64\bin"
$env:PATH = "$msysPath;$env:PATH"

Clear-Host
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "      Q3MAP & LIGHT POWERSHELL BUILD" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "MSYS2 Path: $msysPath"
Write-Host ""

# Kill running instances
Get-Process q3map -ErrorAction SilentlyContinue | Stop-Process -Force
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
    Write-Host "[2/3] Running q3map -v maps/modeltest.map..." -ForegroundColor Cyan
    Write-Host "----------------------------------------" -ForegroundColor Gray
    # Run the map and show relevant optimization stats
    ./q3map.exe -v -samplesize 16 maps/modeltest.map
    Write-Host ""
    Write-Host "[3/3] Running light -radiosity 1 -v maps/modeltest.bsp..." -ForegroundColor Cyan
    Write-Host "----------------------------------------" -ForegroundColor Gray
    ./light.exe -smooth 1 -radiosity 0 -v maps/modeltest.bsp
} else {
    Write-Host "----------------------------------------" -ForegroundColor Red
    Write-Host "        ERROR: BUILD FAILED!" -ForegroundColor Red
    Write-Host "----------------------------------------" -ForegroundColor Red
    Write-Host "Exit Code: $LASTEXITCODE" -ForegroundColor Red
}

Write-Host ""
#Write-Host "Press any key to exit..."
#$null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
