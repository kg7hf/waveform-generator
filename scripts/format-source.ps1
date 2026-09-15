# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Paul R. Decker

<#[
Format owned C++ sources using the project's AStyle rules. Third-party
submodules and generated build overlays are excluded by construction.
]#>
[CmdletBinding()]
param(
    [string]$AStylePath = 'astyle',
    [switch]$Check
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$formatter = (Get-Command $AStylePath -ErrorAction Stop).Source
$options = Join-Path $PSScriptRoot 'cgm_options.txt'
$files = @('common', 'examples/encoder-plugin', 'signal-lab', 'waveform-source', 'host', 'rt1170', 'tests') |
    ForEach-Object { Get-ChildItem -LiteralPath (Join-Path $root $_) -Recurse -File } |
    Where-Object { $_.Extension -in '.cpp', '.hpp' } |
    Sort-Object FullName
$arguments = @("--options=$options", '--suffix=none', '--formatted')
if ($Check) { $arguments += '--dry-run' }
$changes = @()
for ($offset = 0; $offset -lt $files.Count; $offset += 50) {
    $last = [Math]::Min($offset + 49, $files.Count - 1)
    $output = & $formatter @arguments @($files[$offset..$last].FullName) 2>&1
    if ($LASTEXITCODE -ne 0) { throw ($output -join "`n") }
    $changes += @($output | Where-Object { $_ -match '^Formatted\s' })
}
$changes | Write-Output
if ($Check -and $changes.Count -gt 0) { throw "$($changes.Count) C++ files need formatting." }
Write-Output "Checked $($files.Count) owned C++ files."
