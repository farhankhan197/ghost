$ErrorActionPreference = "Stop"

$REPO = "farhankhan197/ghost"
$GHOST_BIN_DIR = "$env:USERPROFILE\.ghost\bin"

Write-Host "▖ building ghost-ai from source..." -ForegroundColor Cyan

# ── Check prerequisites ───────────────────────────────────────────
$hasGit = Get-Command git -ErrorAction SilentlyContinue
$hasCMake = Get-Command cmake -ErrorAction SilentlyContinue

if (-not $hasGit) {
    Write-Host "✖ git is required" -ForegroundColor Red
    exit 1
}
if (-not $hasCMake) {
    Write-Host "✖ cmake is required" -ForegroundColor Red
    exit 1
}

# ── Detect generator ──────────────────────────────────────────────
$generator = ""
$extraArgs = @()

# Try Visual Studio first
$vsRoot = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath 2>$null
if (-not $vsRoot) {
    $vsRoot = & "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath 2>$null
}

if ($vsRoot) {
    # Find MSVC version
    $vcVersion = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property catalog_productLineVersion 2>$null
    if (-not $vcVersion) {
        $vcVersion = & "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property catalog_productLineVersion 2>$null
    }
    Write-Host "  detected Visual Studio $vcVersion" -ForegroundColor Gray
    $generator = "Visual Studio $vcVersion"
    $extraArgs += "-A"
    $extraArgs += "x64"
}
else {
    # Try Ninja
    $hasNinja = Get-Command ninja -ErrorAction SilentlyContinue
    if ($hasNinja) {
        Write-Host "  detected Ninja generator" -ForegroundColor Gray
        $generator = "Ninja"
    }
    else {
        Write-Host "  using default CMake generator" -ForegroundColor Gray
    }
}

# ── Detect architecture ───────────────────────────────────────────
$arch = if ([System.Environment]::Is64BitOperatingSystem) { "x86_64" } else { "x86" }
Write-Host "  platform: windows/${arch}" -ForegroundColor Gray

# ── Build in a temp dir ───────────────────────────────────────────
$tmpDir = [System.IO.Path]::GetTempPath() + [System.Guid]::NewGuid().ToString()
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null

try {
    Write-Host "  cloning $REPO..." -ForegroundColor Gray
    git clone --depth 1 "https://github.com/$REPO.git" "$tmpDir\ghost"

    $buildDir = "$tmpDir\ghost\build"
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

    Write-Host "  configuring..." -ForegroundColor Gray
    $configArgs = @("-S", "$tmpDir\ghost", "-B", "$buildDir", "-DCMAKE_BUILD_TYPE=Release")
    if ($generator) {
        $configArgs += "-G"
        $configArgs += $generator
    }
    if ($extraArgs) {
        $configArgs += $extraArgs
    }
    & cmake @configArgs
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

    Write-Host "  building..." -ForegroundColor Gray
    & cmake --build $buildDir --config Release
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

    # ── Locate built binaries ─────────────────────────────────────
    $ghostBin = "$buildDir\Release\ghost.exe"
    $checkpointBin = "$buildDir\Release\ghost-checkpoint.exe"

    if (-not (Test-Path $ghostBin)) {
        $ghostBin = "$buildDir\ghost.exe"
        $checkpointBin = "$buildDir\ghost-checkpoint.exe"
    }
    if (-not (Test-Path $ghostBin)) {
        $ghostBin = "$buildDir\bin\Release\ghost.exe"
        $checkpointBin = "$buildDir\bin\Release\ghost-checkpoint.exe"
    }
    if (-not (Test-Path $ghostBin)) {
        # Search for the binary
        $found = Get-ChildItem -Recurse -Filter "ghost.exe" -Path "$buildDir" | Select-Object -First 1
        if ($found) {
            $ghostBin = $found.FullName
            $checkpointBin = $found.Directory.FullName + "\ghost-checkpoint.exe"
        }
        else {
            throw "Build succeeded but ghost.exe not found"
        }
    }

    if (-not (Test-Path $checkpointBin)) {
        Write-Host "  (ghost-checkpoint.exe not found, skipping)" -ForegroundColor Yellow
        $checkpointBin = $null
    }

    # ── Install to ~\.ghost\bin ────────────────────────────────────
    Write-Host "  installing to $GHOST_BIN_DIR..." -ForegroundColor Gray
    New-Item -ItemType Directory -Force -Path $GHOST_BIN_DIR | Out-Null

    Copy-Item -Path $ghostBin -Destination "$GHOST_BIN_DIR\ghost.exe" -Force
    if ($checkpointBin) {
        Copy-Item -Path $checkpointBin -Destination "$GHOST_BIN_DIR\ghost-checkpoint.exe" -Force
    }

    Write-Host "  installed: $GHOST_BIN_DIR\ghost.exe" -ForegroundColor Green

    # ── Add to PATH ──────────────────────────────────────────────
    $currentPath = [Environment]::GetEnvironmentVariable("Path", "User")
    if ($currentPath -notlike "*$GHOST_BIN_DIR*") {
        Write-Host ""
        Write-Host "  Adding $GHOST_BIN_DIR to your PATH..." -ForegroundColor Yellow
        [Environment]::SetEnvironmentVariable("Path", "$currentPath;$GHOST_BIN_DIR", "User")
        $env:Path = "$env:Path;$GHOST_BIN_DIR"
    }
}
finally {
    # Clean up
    Remove-Item -Recurse -Force -Path $tmpDir -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "  Next steps:" -ForegroundColor Cyan
Write-Host "    cd your-repo" -ForegroundColor Gray
Write-Host "    ghost install" -ForegroundColor Gray
