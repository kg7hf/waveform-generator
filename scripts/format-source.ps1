# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

<#[
Format owned C++ sources using the project's clang-format rules. Third-party
submodules and generated build overlays are excluded by construction.
]#>
[CmdletBinding()]
param(
    [string]$ClangFormatPath = 'clang-format',
    [switch]$Check
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$formatter = (Get-Command $ClangFormatPath -ErrorAction Stop).Source
$version = (& $formatter --version) -join "`n"
if ($LASTEXITCODE -ne 0 -or $version -notmatch 'clang-format version 19\.') {
    throw "Use clang-format 19 to match CI. Found: $version"
}
$files = @('channel-sim', 'common', 'examples', 'signal-lab', 'waveform-source', 'host', 'rt1170', 'rt1064', 'tests') |
    ForEach-Object { Get-ChildItem -LiteralPath (Join-Path $root $_) -Recurse -File } |
    Where-Object { $_.Extension -in '.cpp', '.hpp' } |
    Sort-Object FullName
$arguments = @('--style=file', '--fallback-style=none')
if ($Check) { $arguments += '--dry-run', '--Werror' } else { $arguments += '-i' }
for ($offset = 0; $offset -lt $files.Count; $offset += 50) {
    $last = [Math]::Min($offset + 49, $files.Count - 1)
    $output = & $formatter @arguments @($files[$offset..$last].FullName) 2>&1
    if ($LASTEXITCODE -ne 0) { throw ($output -join "`n") }
}
Write-Output "Checked $($files.Count) owned C++ paths; .clang-format-ignore preserves generated files."
