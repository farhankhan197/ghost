$ErrorActionPreference = "Stop"

# npx-style one-liner init for ghost
# Usage: irm https://raw.githubusercontent.com/farhankhan197/ghost/main/init.ps1 | iex

$GHOST_REPO = "farhankhan197/ghost"
$GITHUB_API = "https://api.github.com/repos/$GHOST_REPO"

$TMP_DIR = [System.IO.Path]::GetTempPath() + [System.Guid]::NewGuid().ToString()
New-Item -ItemType Directory -Force -Path $TMP_DIR | Out-Null

Write-Host "▖ initializing ghost..." -ForegroundColor Cyan

# Detect architecture
$ARCH = [System.Environment]::Is64BitOperatingSystem ? "x86_64" : "x86_64"
$OS_NAME = "windows"

# Get latest release
Write-Host "  fetching latest release..." -ForegroundColor Gray
$Response = Invoke-RestMethod -Uri "$GITHUB_API/releases/latest" -Method Get
$Latest = $Response.tag_name

if (-not $Latest) {
    Write-Host "  ERROR: could not find latest release" -ForegroundColor Red
    exit 1
}

Write-Host "  latest: $Latest" -ForegroundColor Gray

# Determine binary names
$GHOST_BINARY = "ghost-${OS_NAME}-${ARCH}.exe"
$CHECKPOINT_BINARY = "ghost-checkpoint-${OS_NAME}-${ARCH}.exe"

$DOWNLOAD_URL = "https://github.com/$GHOST_REPO/releases/download/$Latest"

# Download to temp
Write-Host "  downloading ghost to temp directory..." -ForegroundColor Gray
Invoke-WebRequest -Uri "$DOWNLOAD_URL/$GHOST_BINARY" -OutFile "$TMP_DIR\ghost.exe" -UseBasicParsing
Invoke-WebRequest -Uri "$DOWNLOAD_URL/$CHECKPOINT_BINARY" -OutFile "$TMP_DIR\ghost-checkpoint.exe" -UseBasicParsing

if (-not (Test-Path "$TMP_DIR\ghost.exe")) {
    Write-Host "  ERROR: failed to download ghost binary" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "  Running ghost init --yes..." -ForegroundColor Cyan
Write-Host ""

# Run init from temp
$env:GHOST_BIN = $TMP_DIR
& "$TMP_DIR\ghost.exe" init --yes

Write-Host ""
Write-Host "  Ghost is initialized!" -ForegroundColor Green
Write-Host ""
Write-Host "  To keep ghost permanently installed:" -ForegroundColor Cyan
Write-Host "    irm https://raw.githubusercontent.com/farhankhan197/ghost/main/install.ps1 | iex"
Write-Host ""
Write-Host "  Or add this directory to your PATH for this session only:" -ForegroundColor Gray
Write-Host "    $env:Path += `";$TMP_DIR`""
Write-Host ""
