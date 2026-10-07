# ─────────────────────────────────────────────────────────────────────
#  build.ps1 — PowerShell build script for Windows (no Make required)
# ─────────────────────────────────────────────────────────────────────

Write-Host "=== Building Binary HTTP Protocol ===" -ForegroundColor Cyan

Write-Host "  Compiling bserve.exe ..." -ForegroundColor Yellow
gcc -Wall -Wextra -Wpedantic -std=c99 -O2 -o bserve.exe bserve.c protocol.c -lws2_32
if ($LASTEXITCODE -ne 0) { Write-Host "  FAILED" -ForegroundColor Red; exit 1 }

Write-Host "  Compiling bcurl.exe ..." -ForegroundColor Yellow
gcc -Wall -Wextra -Wpedantic -std=c99 -O2 -o bcurl.exe bcurl.c protocol.c -lws2_32
if ($LASTEXITCODE -ne 0) { Write-Host "  FAILED" -ForegroundColor Red; exit 1 }

Write-Host ""
Write-Host "=== Build successful! ===" -ForegroundColor Green
Write-Host ""
Write-Host "  Start the server:"
Write-Host "    .\bserve.exe .\www 9000" -ForegroundColor White
Write-Host ""
Write-Host "  Test with the client:"
Write-Host "    .\bcurl.exe -v localhost:9000/index.html" -ForegroundColor White
Write-Host "    .\bcurl.exe -v localhost:9000/hello.txt"  -ForegroundColor White
Write-Host ""
