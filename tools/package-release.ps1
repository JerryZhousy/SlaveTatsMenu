<#
    Assemble the release archive for SlaveTatsMenu.

    The archive holds exactly what a user installs, plus the notices that have to travel with the
    binary because the DLL statically links GPL/LGPL/MIT/BSD code:

        SKSE/Plugins/SlaveTatsMenu.dll
        SKSE/Plugins/SlaveTatsMenu/SlaveTatsMenu.ini
        LICENSE
        THIRD_PARTY.md
        LICENSES/*

    Never included: build/ (CommonLibSSE-NG objects, the MinHook sources, the precompiled header),
    SlaveTatsMenu.dll.bak-*, meta.ini, src/, tools/.

    Usage:
        .\tools\package-release.ps1
        .\tools\package-release.ps1 -Version 2.0.1 -OutDir D:\releases
#>
[CmdletBinding()]
param(
    [string]$Version = '2.0.0',
    [string]$OutDir = (Join-Path $PSScriptRoot '..\dist')
)

$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$outDir = [System.IO.Path]::GetFullPath($OutDir)

Write-Output "source : $root"
Write-Output "output : $outDir"
Write-Output ''

# --- what goes in -----------------------------------------------------------------------------

$payload = [System.Collections.Generic.List[string]]::new()

$dll = Join-Path $root 'SKSE\Plugins\SlaveTatsMenu.dll'
$ini = Join-Path $root 'SKSE\Plugins\SlaveTatsMenu\SlaveTatsMenu.ini'
$payload.Add($dll)
$payload.Add($ini)

foreach ($doc in 'LICENSE', 'THIRD_PARTY.md') {
    $payload.Add((Join-Path $root $doc))
}

$licenceDir = Join-Path $root 'LICENSES'
$licenceFiles = @(Get-ChildItem $licenceDir -File -ErrorAction SilentlyContinue | Sort-Object Name)
if ($licenceFiles.Count -eq 0) {
    throw "LICENSES/ is empty or missing - the binary cannot be shipped without the notices."
}
foreach ($file in $licenceFiles) {
    $payload.Add($file.FullName)
}

# The notices must be at least as new as the code they describe.
$stale = @(Get-ChildItem (Join-Path $root 'src'), (Join-Path $root 'include') -Recurse -File |
    Where-Object { $_.LastWriteTime -gt (Get-Item $dll).LastWriteTime })
if ($stale.Count -gt 0) {
    Write-Warning "$($stale.Count) source file(s) are newer than the DLL - rebuild before shipping."
}

# --- verify, then stage -----------------------------------------------------------------------

$missing = @($payload | Where-Object { -not (Test-Path $_) })
if ($missing.Count -gt 0) {
    throw "missing payload file(s):`n  $($missing -join "`n  ")"
}
if ($payload -match '\\build\\|\.bak-') {
    throw 'build output or a backup DLL slipped into the payload list.'
}

$staging = Join-Path ([System.IO.Path]::GetTempPath()) 'slave-tats-menu-release'
if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }
New-Item -ItemType Directory -Path $staging | Out-Null

foreach ($file in $payload) {
    $relative = $file.Substring($root.Length).TrimStart('\')
    $target = Join-Path $staging $relative
    New-Item -ItemType Directory -Path (Split-Path $target) -Force | Out-Null
    Copy-Item $file $target -Force
}

New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$archive = Join-Path $outDir "SlaveTatsMenu-$Version.zip"
if (Test-Path $archive) { Remove-Item $archive -Force }
Compress-Archive -Path (Join-Path $staging '*') -DestinationPath $archive -CompressionLevel Optimal

# --- report -----------------------------------------------------------------------------------

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($archive)
try {
    Write-Output '=== archive contents ==='
    foreach ($entry in $zip.Entries | Sort-Object FullName) {
        '{0,9}  {1}' -f $entry.Length, $entry.FullName
    }
    '{0,9}  {1}' -f (Get-Item $archive).Length, "(archive: $archive)"
    Write-Output ''
    Write-Output "$($zip.Entries.Count) entries."
}
finally {
    $zip.Dispose()
    Remove-Item $staging -Recurse -Force
}
