param(
    [switch]$release
)

$msysPath = "D:\CODE\msys64\mingw64\bin"
$env:PATH = "$msysPath;D:\CODE\msys64\usr\bin;$env:PATH"

Clear-Host
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "      Q3MAP & LIGHT POWERSHELL BUILD" -ForegroundColor Cyan
if ($release) {
    Write-Host "             (RELEASE BUILD)" -ForegroundColor Yellow
}
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "MSYS2 Path: $msysPath"
Write-Host ""

# Kill running instances
Get-Process makebsp -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process makelight -ErrorAction SilentlyContinue | Stop-Process -Force

# Note: 'make clean' now preserves Assimp and MeshLib binaries on Windows for speed.
# Use 'make clean-all' manually if a full library rebuild is needed.
Write-Host "[1/2] Running make clean && make..." -ForegroundColor Yellow
& make clean

if ($release) {
    & make RELEASE=1
} else {
    & make
}

Write-Host ""
if ($LASTEXITCODE -eq 0) {
    Write-Host "----------------------------------------" -ForegroundColor Green
    Write-Host "        SUCCESS: BUILD COMPLETE!" -ForegroundColor Green
    Write-Host "----------------------------------------" -ForegroundColor Green
} else {
    Write-Host "----------------------------------------" -ForegroundColor Red
    Write-Host "        ERROR: BUILD FAILED!" -ForegroundColor Red
    Write-Host "----------------------------------------" -ForegroundColor Red
    Write-Host "Exit Code: $LASTEXITCODE" -ForegroundColor Red
}

Write-Host ""
#Write-Host "Press any key to exit..."
#$null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
