# Build script for NoSleep
# Requires: MinGW-w64 (gcc, windres)

Write-Host "Building NoSleep..." -ForegroundColor Cyan

# Compile resource file
Write-Host "Compiling resources..."
windres -i nosleep.rc -o nosleep.res -O coff
if ($LASTEXITCODE -ne 0) {
    Write-Host "Resource compilation failed!" -ForegroundColor Red
    exit 1
}

# Compile executable
Write-Host "Compiling executable..."
gcc -static -o nosleep_pure.exe nosleep_win32.c nosleep.res -mwindows -municode -lcomctl32 -lshell32
if ($LASTEXITCODE -ne 0) {
    Write-Host "Compilation failed!" -ForegroundColor Red
    exit 1
}

# Show result
$exe = Get-Item nosleep_pure.exe -ErrorAction SilentlyContinue
if ($exe) {
    Write-Host "Build successful!" -ForegroundColor Green
    Write-Host "Output: $($exe.FullName)"
    Write-Host "Size: $([math]::Round($exe.Length / 1KB, 2)) KB"
} else {
    Write-Host "Build failed - executable not found" -ForegroundColor Red
    exit 1
}
