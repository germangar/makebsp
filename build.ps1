$msysPath = "E:\CODE\msys64\mingw64\bin"
$env:PATH = "$msysPath;$env:PATH"

Clear-Host
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "      Q3MAP.EXE POWERSHELL BUILD" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "MSYS2 Path: $msysPath"
Write-Host ""

# Kill running instance
Get-Process q3map -ErrorAction SilentlyContinue | Stop-Process -Force

Write-Host "[1/2] Running make..." -ForegroundColor Yellow
& make

Write-Host ""
if ($LASTEXITCODE -eq 0) {
    Write-Host "----------------------------------------" -ForegroundColor Green
    Write-Host "        SUCCESS: BUILD COMPLETE!" -ForegroundColor Green
    Write-Host "----------------------------------------" -ForegroundColor Green
    
    Write-Host ""
    Write-Host "[2/2] Running modeltest.map..." -ForegroundColor Cyan
    Write-Host "----------------------------------------" -ForegroundColor Gray
    # Run the map and show relevant optimization stats
    ./q3map.exe -v maps/modeltest.map | Select-String -Pattern "(Fan|Generated total|Simplified|wood_bridge|CSG Merge|WARNING)"
} else {
    Write-Host "----------------------------------------" -ForegroundColor Red
    Write-Host "        ERROR: BUILD FAILED!" -ForegroundColor Red
    Write-Host "----------------------------------------" -ForegroundColor Red
    Write-Host "Exit Code: $LASTEXITCODE" -ForegroundColor Red
}

Write-Host ""
Write-Host "Press any key to exit..."
$null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
