<#
    Assemble the release archive for SlaveTatsMenu.

    The archive is a FOMOD package: Mod Organizer 2 and Vortex open it with a
    one-step wizard that picks the interface language and writes it into the INI,
    which is friendlier than making the user hand-edit a config file before the
    first launch.

        fomod/info.xml
        fomod/ModuleConfig.xml
        Options/English/SlaveTatsMenu.ini     generated from the canonical INI
        Options/Chinese/SlaveTatsMenu.ini     generated from the canonical INI
        SKSE/Plugins/SlaveTatsMenu.dll
        SKSE/Plugins/SlaveTatsMenu/SlaveTatsMenu.ini
        LICENSE
        THIRD_PARTY.md
        LICENSES/*

    The two Options INIs are generated here instead of being kept in the repo:
    each is the canonical INI with only its "Language =" line rewritten, so a
    config key added later can never go missing from one of the language
    variants. The version in fomod/info.xml is stamped from CMakeLists.txt for
    the same reason.

    Never included: build/ (CommonLibSSE-NG objects, the MinHook sources, the
    precompiled header), src/, tools/, meta.ini, SlaveTatsMenu.dll.bak-*.

    Staging happens in a fresh %TEMP% directory per run and is deliberately not
    deleted afterwards - nothing here removes files except the previous archive,
    which is moved to D:\待删除.

    Usage:
        .\tools\package-release.ps1
        .\tools\package-release.ps1 -OutDir D:\releases
        .\tools\package-release.ps1 -NoFomod        # flat archive, no wizard
#>
[CmdletBinding()]
param(
    [string]$ArchiveName = 'SlaveTatsMenu.zip',
    [string]$OutDir = (Join-Path $PSScriptRoot '..\dist'),
    [switch]$NoFomod
)

$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$outDir = [System.IO.Path]::GetFullPath($OutDir)
$utf8 = [System.Text.UTF8Encoding]::new($false)

Write-Output "source : $root"
Write-Output "output : $outDir"
Write-Output ''

# --- what goes in -----------------------------------------------------------------------------

$payload = [System.Collections.Generic.List[string]]::new()

$dll = Join-Path $root 'SKSE\Plugins\SlaveTatsMenu.dll'
$ini = Join-Path $root 'SKSE\Plugins\SlaveTatsMenu\SlaveTatsMenu.ini'
$payload.Add($dll)
$payload.Add($ini)

$transDir = Join-Path $root 'Interface\Translations'
$transFiles = @(Get-ChildItem $transDir -File -ErrorAction SilentlyContinue | Sort-Object Name)
if ($transFiles.Count -eq 0) {
    throw "Interface/Translations/ is empty or missing."
}
foreach ($file in $transFiles) {
    $payload.Add($file.FullName)
}

# --- translation coverage ---------------------------------------------------------------------
#
# Every $STM_ key referenced in src/ must exist in *every* shipped translation file, and the
# files must agree with each other. A missing key does not fail at runtime: Translation::Tr()
# falls back to returning the key verbatim, so the UI silently renders the literal string
# "$STM_Area_Hands". That is exactly how the first batch of missing keys went unnoticed, so the
# check belongs here, where it is impossible to ship without passing it.

$usedKeys = [System.Collections.Generic.HashSet[string]]::new()
foreach ($srcFile in Get-ChildItem (Join-Path $root 'src') -Recurse -File -Include '*.cpp', '*.h') {
    $text = [System.IO.File]::ReadAllText($srcFile.FullName)
    foreach ($match in [regex]::Matches($text, '\$STM_[A-Za-z0-9_]+')) {
        [void]$usedKeys.Add($match.Value)
    }
}
if ($usedKeys.Count -eq 0) {
    throw 'no $STM_ keys found under src/ - the scan itself is broken.'
}

$keySets = [ordered]@{}
foreach ($file in $transFiles) {
    $set = [System.Collections.Generic.HashSet[string]]::new()
    foreach ($line in [System.IO.File]::ReadAllLines($file.FullName, [System.Text.Encoding]::Unicode)) {
        if ($line -match '^\s*;' -or $line -notmatch "`t") { continue }
        [void]$set.Add(($line -split "`t", 2)[0].Trim())
    }
    $keySets[$file.Name] = $set
}

$referenceName = @($keySets.Keys)[0]
foreach ($name in $keySets.Keys) {
    $missingKeys = @($usedKeys | Where-Object { -not $keySets[$name].Contains($_) } | Sort-Object)
    if ($missingKeys.Count -gt 0) {
        throw "$name is missing $($missingKeys.Count) key(s) used in src/:`n  $($missingKeys -join "`n  ")"
    }

    $unusedKeys = @($keySets[$name] | Where-Object { -not $usedKeys.Contains($_) } | Sort-Object)
    if ($unusedKeys.Count -gt 0) {
        Write-Warning "$name has $($unusedKeys.Count) unused key(s): $($unusedKeys -join ', ')"
    }

    $asymmetric = @($keySets[$referenceName] | Where-Object { -not $keySets[$name].Contains($_) } | Sort-Object)
    if ($asymmetric.Count -gt 0) {
        throw "$name is missing key(s) that $referenceName has:`n  $($asymmetric -join "`n  ")"
    }
}
Write-Output "translations: $($usedKeys.Count) key(s) referenced in src/, present in all $($keySets.Count) file(s)"
Write-Output ''

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

# --- version and FOMOD sources ----------------------------------------------------------------

$versionLine = Select-String -LiteralPath (Join-Path $root 'CMakeLists.txt') `
    -Pattern 'project\s*\(\s*SlaveTatsMenu\s+VERSION\s+([0-9]+(?:\.[0-9]+)*)' | Select-Object -First 1
if (-not $versionLine) { throw 'cannot read the project version from CMakeLists.txt.' }
$version = $versionLine.Matches[0].Groups[1].Value
Write-Output "version: $version"

$fomodSource = @('info.xml', 'ModuleConfig.xml') | ForEach-Object { Join-Path $root "fomod\$_" }
if (-not $NoFomod) {
    $missingFomod = @($fomodSource | Where-Object { -not (Test-Path $_) })
    if ($missingFomod.Count -gt 0) {
        throw "missing FOMOD source(s):`n  $($missingFomod -join "`n  ")"
    }
}

# --- verify, then stage -----------------------------------------------------------------------

$missing = @($payload | Where-Object { -not (Test-Path $_) })
if ($missing.Count -gt 0) {
    throw "missing payload file(s):`n  $($missing -join "`n  ")"
}
if ($payload -match '\\build\\|\.bak-') {
    throw 'build output or a backup DLL slipped into the payload list.'
}

$staging = Join-Path ([System.IO.Path]::GetTempPath()) ('slave-tats-menu-release-{0}-{1}' -f
    (Get-Date -Format 'yyyyMMdd-HHmmss'), ([guid]::NewGuid().ToString('N').Substring(0, 6)))
New-Item -ItemType Directory -Path $staging -Force | Out-Null
Write-Output "staging: $staging"
Write-Output ''

foreach ($file in $payload) {
    $relative = $file.Substring($root.Length).TrimStart('\')
    $target = Join-Path $staging $relative
    New-Item -ItemType Directory -Path (Split-Path $target) -Force | Out-Null
    Copy-Item $file $target -Force
}

if (-not $NoFomod) {
    # fomod/: both XMLs, with the version stamped into info.xml from CMakeLists.txt
    $fomodStage = Join-Path $staging 'fomod'
    New-Item -ItemType Directory -Path $fomodStage -Force | Out-Null
    foreach ($file in $fomodSource) {
        Copy-Item $file (Join-Path $fomodStage (Split-Path $file -Leaf)) -Force
    }

    $infoPath = Join-Path $fomodStage 'info.xml'
    $info = [System.IO.File]::ReadAllText($infoPath, $utf8)
    $stamped = [regex]::Replace($info, '<Version\s+MachineVersion="[^"]*"\s*>[^<]*</Version>',
        "<Version MachineVersion=`"$version`">$version</Version>")
    if ($stamped -notmatch [regex]::Escape("MachineVersion=`"$version`"")) {
        throw 'could not stamp the version into fomod/info.xml.'
    }
    [System.IO.File]::WriteAllText($infoPath, $stamped, $utf8)

    # Options/<language>/SlaveTatsMenu.ini, generated from the canonical INI
    $canonical = [System.IO.File]::ReadAllText($ini, $utf8)
    if ($canonical -notmatch '(?m)^\s*Language\s*=') {
        throw "the canonical INI has no 'Language =' line to switch."
    }
    foreach ($language in 'English', 'Chinese') {
        $code = if ($language -eq 'English') { 'en' } else { 'zh' }
        $variant = [regex]::Replace($canonical, '(?m)^[ \t]*Language[ \t]*=.*$', "Language = $code")
        $variantDir = Join-Path $staging "Options\$language"
        New-Item -ItemType Directory -Path $variantDir -Force | Out-Null
        [System.IO.File]::WriteAllText((Join-Path $variantDir 'SlaveTatsMenu.ini'), $variant, $utf8)
    }

    # every source the wizard references has to exist in the staged tree
    $moduleConfig = [xml][System.IO.File]::ReadAllText((Join-Path $fomodStage 'ModuleConfig.xml'), $utf8)
    $references = @($moduleConfig.SelectNodes('//*[@source]') | ForEach-Object { $_.source } | Sort-Object -Unique)
    foreach ($reference in $references) {
        if (-not (Test-Path -LiteralPath (Join-Path $staging $reference))) {
            throw "fomod/ModuleConfig.xml references a path that is not in the archive: $reference"
        }
    }
    Write-Output "FOMOD references verified: $($references.Count) source(s)"
    Write-Output ''
}

# --- archive -----------------------------------------------------------------------------------

New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$archive = Join-Path $outDir $ArchiveName
if (Test-Path $archive) {
    $trashDir = 'D:\待删除'
    if (-not (Test-Path $trashDir)) { New-Item -ItemType Directory -Path $trashDir -Force | Out-Null }
    $trashTarget = Join-Path $trashDir ("SlaveTatsMenu_" + (Get-Date -Format 'yyyyMMdd_HHmmss') + ".zip")
    Move-Item $archive $trashTarget -Force
    Write-Host "  -> Moved existing archive to $trashTarget" -ForegroundColor Gray
}
Compress-Archive -Path (Join-Path $staging '*') -DestinationPath $archive -CompressionLevel Optimal -Force

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

    if (-not $NoFomod) {
        $names = @($zip.Entries | ForEach-Object FullName)
        foreach ($required in 'fomod/info.xml', 'fomod/ModuleConfig.xml',
                               'Options/English/SlaveTatsMenu.ini', 'Options/Chinese/SlaveTatsMenu.ini') {
            if ($names -notcontains $required) { throw "archive is missing $required" }
        }
        Write-Output 'FOMOD present: fomod/info.xml + fomod/ModuleConfig.xml (UI language step).'
    }
}
finally {
    $zip.Dispose()
}
