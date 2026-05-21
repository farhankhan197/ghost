$ErrorActionPreference = "Stop"

$GHOST_REPO = "farhankhan197/ghost"
$GHOST_BIN_DIR = "$env:USERPROFILE\.ghost\bin"
$GITHUB_API = "https://api.github.com/repos/$GHOST_REPO"

Write-Host "▖ installing ghost-ai..." -ForegroundColor Cyan

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

# Create bin directory
if (-not (Test-Path $GHOST_BIN_DIR)) {
    New-Item -ItemType Directory -Force -Path $GHOST_BIN_DIR | Out-Null
}

# Download binaries
Write-Host "  downloading ghost..." -ForegroundColor Gray
Invoke-WebRequest -Uri "$DOWNLOAD_URL/$GHOST_BINARY" -OutFile "$GHOST_BIN_DIR\ghost.exe" -UseBasicParsing

Write-Host "  downloading ghost-checkpoint..." -ForegroundColor Gray
Invoke-WebRequest -Uri "$DOWNLOAD_URL/$CHECKPOINT_BINARY" -OutFile "$GHOST_BIN_DIR\ghost-checkpoint.exe" -UseBasicParsing

# Verify download
if (-not (Test-Path "$GHOST_BIN_DIR\ghost.exe")) {
    Write-Host "  ERROR: failed to download ghost binary" -ForegroundColor Red
    exit 1
}

# Add to PATH if not already present
$CurrentPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ($CurrentPath -notlike "*$GHOST_BIN_DIR*") {
    Write-Host ""
    Write-Host "  Adding $GHOST_BIN_DIR to your PATH..." -ForegroundColor Yellow
    [Environment]::SetEnvironmentVariable("Path", "$CurrentPath;$GHOST_BIN_DIR", "User")
    $env:Path = "$env:Path;$GHOST_BIN_DIR"
}

Write-Host ""
Write-Host "  installed: $GHOST_BIN_DIR\ghost.exe ($Latest)" -ForegroundColor Green
Write-Host ""
Write-Host "  Next: cd to your repo and run 'ghost install'" -ForegroundColor Cyan
