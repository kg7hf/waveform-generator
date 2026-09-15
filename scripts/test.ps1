# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Paul R. Decker

[CmdletBinding()]
param([ValidateSet('host-debug', 'host-release')][string]$Preset = 'host-release')

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$buildRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot "build\$Preset"))
if (-not (Test-Path -LiteralPath (Join-Path $buildRoot 'CTestTestfile.cmake'))) {
    throw "Configure and build $Preset before running tests."
}
Push-Location $projectRoot
try {
    ctest --preset $Preset --output-on-failure
    $testExit = $LASTEXITCODE
}
finally {
    Pop-Location
    # CTest writes logs even on success. Keep the result in the console, then
    # remove only this preset's generated test results.
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
}
if ($testExit -ne 0) { throw "CTest failed with exit code $testExit" }
