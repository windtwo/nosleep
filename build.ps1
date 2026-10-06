# Build script for NoSleep
# Requires: MinGW-w64 (gcc, windres) and make / mingw32-make.
# All compiler flags live in the Makefile; this script only invokes make.

$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    Write-Host "Building NoSleep..." -ForegroundColor Cyan

    $make = Get-Command make, mingw32-make -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $make) {
        Write-Host "make / mingw32-make not found. Install it (MSYS2: pacman -S make) and retry." -ForegroundColor Red
        exit 1
    }

    & $make.Source
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Build failed!" -ForegroundColor Red
        exit 1
    }

    $exe = Get-Item nosleep_pure.exe
    Write-Host "Build successful!" -ForegroundColor Green
    Write-Host "Output: $($exe.FullName)"
    Write-Host "Size: $([math]::Round($exe.Length / 1KB, 2)) KB"
}
finally {
    Pop-Location
}
