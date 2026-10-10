<#
    Acceptance checklist for SlaveTatsMenu.

    Reads the plugin log and reports whether the things the rewrite was supposed
    to prove actually happened, one line per claim:

      - the plugin loaded and both partner interfaces came up
      - the menu framework calls us on the game's main thread
      - the catalogue came from SlaveTatsNG itself, not from the JSON fallback
      - .SlaveTatsMenu.known_targets was actually written to
      - a tattoo was applied, and an already-applied one was restyled
      - a preset was saved and re-applied
      - nothing crashed while the log was being produced

    Usage:
        .\tools\check-acceptance.ps1
        .\tools\check-acceptance.ps1 -Log <path-to-SlaveTatsMenu.log>

    Exit code = number of unsatisfied checks.
#>
[CmdletBinding()]
param(
    [string]$Log = (Join-Path $env:USERPROFILE 'Documents\My Games\Skyrim Special Edition\SKSE\SlaveTatsMenu.log'),
    [string]$CrashDir = (Join-Path $env:USERPROFILE 'Documents\My Games\Skyrim Special Edition\SKSE')
)

$script:failed = 0

function Check {
    param([bool]$Ok, [string]$What, [string]$Detail = '')

    $tag = if ($Ok) { '[ ok ]' } else { '[FAIL]' }
    if (-not $Ok) { $script:failed++ }

    if ($Detail) {
        '{0} {1}  ({2})' -f $tag, $What, $Detail
    } else {
        '{0} {1}' -f $tag, $What
    }
}

if (-not (Test-Path $Log)) {
    Write-Output "log not found: $Log"
    exit 2
}

$lines = @(Get-Content $Log)

Write-Output '=== SlaveTatsMenu acceptance ==='
Write-Output "log   : $Log"
Write-Output "lines : $($lines.Count)"
Write-Output ''

Write-Output '--- boot ---'
Check ([bool]($lines -match 'SlaveTatsMenu 2\.0\.0 loaded'))             'plugin 2.0.0 loaded'
Check ([bool]($lines -match 'SlaveTatsNG: C\+\+ interface acquired'))   'SlaveTatsNG interface acquired'
Check ([bool]($lines -match 'JContainers: reflection functions bound')) 'JContainers reflection bound'
Check ([bool]($lines -match 'slot limits from'))                        'skee64.ini slot limits read'

$thread = $lines | Select-String -Pattern 'rendering on thread (\d+) \(plugin load thread (\d+)\)' | Select-Object -Last 1
if ($thread) {
    $render = $thread.Matches[0].Groups[1].Value
    $load = $thread.Matches[0].Groups[2].Value
    Check ($render -eq $load) 'render thread is the load thread' "$render vs $load"
} else {
    Check $false 'render thread reported' 'open the window once so the framework renders'
}
Write-Output ''

Write-Output '--- catalogue source ---'
$source = $lines | Select-String -Pattern '\(source: ([^)]+)\)' | Select-Object -Last 1
if ($source) {
    $name = $source.Matches[0].Groups[1].Value.Trim()
    Check ($name -eq 'SlaveTatsNG API') 'authoritative source won (not the JSON fallback)' "used: $name"
    $count = $lines | Select-String -Pattern '(\d+) tattoos indexed' | Select-Object -Last 1
    if ($count) { Write-Output "         library size: $($count.Matches[0].Groups[1].Value)" }
} else {
    Check $false 'catalogue was built'
}
Write-Output ''

Write-Output '--- operations actually performed through this menu ---'
$registered = @($lines | Where-Object { $_ -match 'KnownTargets: registered' }).Count
$applied = @($lines | Where-Object { $_ -match 'Op Apply ok' }).Count
$restyled = @($lines | Where-Object { $_ -match 'Op Recolor ok' }).Count
$saved = @($lines | Where-Object { $_ -match 'Op PresetSave ok' }).Count
$reapplied = @($lines | Where-Object { $_ -match 'Op PresetApply ok' }).Count
$failedLines = @($lines | Where-Object { $_ -match 'Op \S+ FAILED' })
$deferredLines = @($lines | Where-Object { $_ -match 'Op \S+ DEFERRED' })

Check ($registered -gt 0) 'known_targets contract exercised' "registered $registered time(s)"
Check ($applied -gt 0) 'a tattoo was applied' "$applied time(s)"
Check ($restyled -gt 0) 'an applied tattoo was restyled' "$restyled time(s)"
Check ($saved -gt 0) 'a preset was saved' "$saved time(s)"
Check ($reapplied -gt 0) 'a preset was re-applied' "$reapplied time(s)"

Write-Output "         failed operations: $($failedLines.Count), deferred (3D not loaded): $($deferredLines.Count)"
if ($failedLines.Count -gt 0) {
    Write-Output '         last failures:'
    $failedLines | Select-Object -Last 3 | ForEach-Object { Write-Output "           $_" }
}
Write-Output ''

Write-Output '--- crashes ---'
$since = (Get-Item $Log).LastWriteTime.AddMinutes(-30)
$crashes = @(Get-ChildItem $CrashDir -Filter 'crash-*.log' -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $since })
Check ($crashes.Count -eq 0) 'no crash around this run' "$($crashes.Count) found"
Write-Output ''

if ($script:failed -eq 0) {
    Write-Output 'ALL ACCEPTANCE CHECKS PASSED'
} else {
    Write-Output "$($script:failed) CHECK(S) NOT YET SATISFIED"
}

exit $script:failed
