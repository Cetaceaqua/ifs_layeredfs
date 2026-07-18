#!/usr/bin/env pwsh
# NOTE: file is slop-ported from the .sh. Consider the .sh the source of truth.
# AI nonsense follows:

$ErrorActionPreference = 'Stop'

# mon note: I use a Dev Drive for speed, you probably want to change these
$Build32 = 'X:\layeredfs_build32'
$Build64 = 'X:\layeredfs_build64'

# leading 32/64 args pick the arch(es); the rest is passed to meson test
$Archs = @()
$MesonArgs = @($args)
while ($MesonArgs.Count -gt 0 -and ($MesonArgs[0] -eq '32' -or $MesonArgs[0] -eq '64')) {
    $Archs += $MesonArgs[0]
    $MesonArgs = @($MesonArgs | Select-Object -Skip 1)
}
if ($Archs.Count -eq 0) { $Archs = @('32', '64') }

if ($Archs -contains '32') {
    meson test -C $Build32 --print-errorlogs @MesonArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if ($Archs -contains '64') {
    meson test -C $Build64 --print-errorlogs @MesonArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
