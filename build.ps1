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

Write-Host "[1/2] Running make..." -ForegroundColor Yellow
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
    ./q3map.exe -v maps/modeltest.map | Select-String -Pattern "(Fan|Generated total|Simplified|wood_bridge|CSG Merge|WARNING)"

    Write-Host ""
    Write-Host "[3/3] Running light -v maps/modeltest.bsp..." -ForegroundColor Cyan
    Write-Host "----------------------------------------" -ForegroundColor Gray
    ./light.exe -v maps/modeltest.bsp | Select-String -Pattern "(Lighting|exact lightmap|seconds elapsed|WARNING)"
} else {
    Write-Host "----------------------------------------" -ForegroundColor Red
    Write-Host "        ERROR: BUILD FAILED!" -ForegroundColor Red
    Write-Host "----------------------------------------" -ForegroundColor Red
    Write-Host "Exit Code: $LASTEXITCODE" -ForegroundColor Red
}

Write-Host ""
Write-Host "Press any key to exit..."
$null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
