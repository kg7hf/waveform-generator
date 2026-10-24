# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

# Runs a build's CTest suite: a root host preset (build/<preset>), or with
# -Preset pathsim-host channel-sim's own build (build/main/pathsim-host, its
# chain CLI and gate script tests included), which the root project and the
# Linux host CI do not build.
[CmdletBinding()]
param([ValidateSet('host-debug', 'host-release', 'pathsim-host')][string]$Preset = 'host-release')

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$channelSim = $Preset -eq 'pathsim-host'
$buildDirectory = if ($channelSim) { 'build\main\pathsim-host' } else { "build\$Preset" }
$buildRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot $buildDirectory))
if (-not (Test-Path -LiteralPath (Join-Path $buildRoot 'CTestTestfile.cmake'))) {
    throw "Configure and build $Preset before running tests."
}
Push-Location $projectRoot
try {
    # channel-sim's presets define no test preset; its build directory is the preset's.
    if ($channelSim) { ctest --test-dir $buildRoot --output-on-failure }
    else { ctest --preset $Preset --output-on-failure }
    $testExit = $LASTEXITCODE
}
finally {
    Pop-Location
}
if ($testExit -ne 0) { throw "CTest failed with exit code $testExit" }
# CTest writes logs even on success. Failed-run diagnostics stay in the build
# tree; remove only this preset's successful generated results.
$results = Join-Path $buildRoot 'Testing'
if (Test-Path -LiteralPath $results) {
    $resolved = (Resolve-Path -LiteralPath $results).Path
    if ($resolved -ne [IO.Path]::GetFullPath($results) -or
        -not $resolved.StartsWith($projectRoot + '\', [StringComparison]::OrdinalIgnoreCase) -or
        (Get-Item -LiteralPath $results).Attributes.HasFlag([IO.FileAttributes]::ReparsePoint)) {
        throw "Unexpected test-results cleanup path: $resolved"
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
