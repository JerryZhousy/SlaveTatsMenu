<#
    Pipeline helper: Build -> Package -> Deploy for SlaveTatsMenu
    Usage:
        .\tools\deploy-all.ps1
        .\tools\deploy-all.ps1 -SkipBuild
#>
[CmdletBinding()]
param(
    [string]$MoPath = 'E:\MO_Skyrim',
    [string]$DownloadPath = 'D:\download',
    [switch]$SkipBuild,
    [switch]$SkipPackage
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

Write-Host "=== SlaveTatsMenu Pipeline ===" -ForegroundColor Cyan

# 1. Build
if (-not $SkipBuild) {
    Write-Host "[1/3] Building SlaveTatsMenu..." -ForegroundColor Yellow
    Push-Location $root
    try {
        & cmd.exe /c build.cmd
        if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }
    } finally {
        Pop-Location
    }
}

# 2. Package
if (-not $SkipPackage) {
    Write-Host "[2/3] Packaging SlaveTatsMenu.zip..." -ForegroundColor Yellow
    & "D:\Program Files\PowerShell\7\pwsh.exe" -File (Join-Path $root 'tools\package-release.ps1')
    if ($LASTEXITCODE -ne 0) { throw "Packaging failed with exit code $LASTEXITCODE" }
}

# 3. Deploy
Write-Host "[3/3] Deploying artifacts..." -ForegroundColor Yellow
$zip = Join-Path $root 'dist\SlaveTatsMenu.zip'
$dll = Join-Path $root 'SKSE\Plugins\SlaveTatsMenu.dll'

if (-not (Test-Path $zip)) { throw "Archive not found: $zip" }
if (-not (Test-Path $dll)) { throw "DLL not found: $dll" }

# Copy to user download folder
if ($DownloadPath) {
    New-Item -ItemType Directory -Path $DownloadPath -Force | Out-Null
    Copy-Item $zip (Join-Path $DownloadPath 'SlaveTatsMenu.zip') -Force
    Write-Host "  -> Copied to $DownloadPath\SlaveTatsMenu.zip" -ForegroundColor Green
}

# Copy to system Downloads folder
$sysDownload = 'D:\Downloads'
if (Test-Path $sysDownload) {
    Copy-Item $zip (Join-Path $sysDownload 'SlaveTatsMenu.zip') -Force
    Write-Host "  -> Copied to $sysDownload\SlaveTatsMenu.zip" -ForegroundColor Green
}

# Deploy to Mod Organizer 2
if ($MoPath -and (Test-Path $MoPath)) {
    $moModDir = Join-Path $MoPath 'mods\SlaveTatsMenu\SKSE\Plugins'
    New-Item -ItemType Directory -Path $moModDir -Force | Out-Null
    Copy-Item $dll (Join-Path $moModDir 'SlaveTatsMenu.dll') -Force
    Write-Host "  -> Deployed live DLL to $moModDir\SlaveTatsMenu.dll" -ForegroundColor Green

    $moDownloads = Join-Path $MoPath 'downloads'
    if (Test-Path $moDownloads) {
        Copy-Item $zip (Join-Path $moDownloads 'SlaveTatsMenu.zip') -Force
        Write-Host "  -> Deployed release zip to $moDownloads\SlaveTatsMenu.zip" -ForegroundColor Green
    }
}

Write-Host "=== Pipeline Finished Successfully! ===" -ForegroundColor Cyan
