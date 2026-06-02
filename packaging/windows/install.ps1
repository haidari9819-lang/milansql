# ============================================================
# install.ps1 — MilanSQL Windows Silent Installer
# Usage (as Administrator):
#   Set-ExecutionPolicy Bypass -Scope Process -Force
#   .\install.ps1
#   .\install.ps1 -Port 4406 -DataDir "C:\MilanSQL\data" -InstallService
# ============================================================
param(
    [string]$Version    = "5.7.0",
    [string]$Port       = "4406",
    [string]$HttpPort   = "8080",
    [string]$InstallDir = "$env:ProgramFiles\MilanSQL",
    [string]$DataDir    = "$env:ProgramData\MilanSQL\data",
    [switch]$InstallService,
    [switch]$StartNow
)

$ErrorActionPreference = "Stop"
$ProgressPreference    = "SilentlyContinue"

Write-Host ""
Write-Host "=== MilanSQL v$Version Windows Installer ===" -ForegroundColor Cyan
Write-Host ""

# ── Require Administrator ──────────────────────────────────────
if (-not ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error "This installer must be run as Administrator."
    exit 1
}

# ── Create directories ────────────────────────────────────────
Write-Host "[1/5] Creating directories..."
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
New-Item -ItemType Directory -Force -Path $DataDir    | Out-Null

# ── Download binary ───────────────────────────────────────────
Write-Host "[2/5] Downloading MilanSQL v$Version..."
$zipUrl  = "https://github.com/haidari9819-lang/milansql/releases/download/v$Version/milansql-windows-x64.zip"
$zipPath = "$env:TEMP\milansql-$Version.zip"

try {
    Invoke-WebRequest -Uri $zipUrl -OutFile $zipPath -UseBasicParsing
    Write-Host "     Downloaded to $zipPath"
} catch {
    Write-Warning "Download failed: $_"
    Write-Host "     Please manually place milansql.exe in $InstallDir"
}

# ── Extract ────────────────────────────────────────────────────
if (Test-Path $zipPath) {
    Write-Host "[3/5] Extracting..."
    Expand-Archive -Path $zipPath -DestinationPath $InstallDir -Force
    Remove-Item $zipPath -Force
} else {
    Write-Host "[3/5] Skipping extraction (no zip found)."
}

# ── Add to PATH ───────────────────────────────────────────────
Write-Host "[4/5] Updating system PATH..."
$machinePath = [System.Environment]::GetEnvironmentVariable("PATH", "Machine")
if ($machinePath -notlike "*$InstallDir*") {
    [System.Environment]::SetEnvironmentVariable(
        "PATH", "$machinePath;$InstallDir", "Machine")
    Write-Host "     Added $InstallDir to system PATH."
} else {
    Write-Host "     PATH already contains $InstallDir."
}

# ── Optional: Windows Service ─────────────────────────────────
Write-Host "[5/5] Service setup..."
$exePath = Join-Path $InstallDir "milansql.exe"

if ($InstallService -and (Test-Path $exePath)) {
    $svcName = "MilanSQL"
    $existing = Get-Service -Name $svcName -ErrorAction SilentlyContinue
    if ($existing) {
        Write-Host "     Service '$svcName' already exists — skipping."
    } else {
        $binPath = "`"$exePath`" --server --port $Port --http --http-port $HttpPort --data-dir `"$DataDir`""
        New-Service -Name $svcName `
                    -DisplayName "MilanSQL Database Engine" `
                    -Description "Production-grade SQL database engine (v$Version)" `
                    -BinaryPathName $binPath `
                    -StartupType Automatic | Out-Null
        Write-Host "     Windows Service '$svcName' installed."
    }

    if ($StartNow) {
        Start-Service -Name $svcName
        Write-Host "     Service started."
    }
} elseif ($InstallService) {
    Write-Warning "milansql.exe not found at $exePath — service not installed."
} else {
    Write-Host "     (Use -InstallService to install as a Windows Service)"
}

# ── Summary ───────────────────────────────────────────────────
Write-Host ""
Write-Host "=== Installation complete ===" -ForegroundColor Green
Write-Host "  Install dir : $InstallDir"
Write-Host "  Data dir    : $DataDir"
Write-Host "  TCP port    : $Port"
Write-Host "  HTTP port   : $HttpPort"
Write-Host ""
Write-Host "Start MilanSQL:" -ForegroundColor Yellow
Write-Host "  milansql --server --port $Port --http --http-port $HttpPort"
Write-Host ""
Write-Host "Connect:"
Write-Host "  milansql --client --port $Port"
Write-Host ""
