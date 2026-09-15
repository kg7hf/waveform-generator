# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Paul R. Decker

<#[
Generate a calibration WAV (or use an existing PCM24 source), render CW and a mixed
interference example, and optionally play one result on an exact WASAPI output.
]#>
[CmdletBinding()]
param(
    [string]$OutputDirectory = 'build/demo-generated',
    [string]$SourceWav = '',
    [ValidateSet('none', 'clean', 'cw', 'field')][string]$Play = 'none',
    [string]$Device = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($Play -ne 'none' -and [string]::IsNullOrWhiteSpace($Device)) {
    throw 'Playback requires -Device with an exact name from host/play_wav.py --list-devices.'
}
if ([IO.Path]::IsPathRooted($OutputDirectory)) { $out = [IO.Path]::GetFullPath($OutputDirectory) }
else { $out = [IO.Path]::GetFullPath((Join-Path $root $OutputDirectory)) }
$renderer = Join-Path $root 'build/host-release/host/render/signal_lab_render.exe'
if (!(Test-Path -LiteralPath $renderer)) { throw 'Build host-release first; see docs/how-to-use.md.' }
if (Test-Path -LiteralPath $out) { throw "Use a new output directory: $out" }
New-Item -ItemType Directory -Path $out | Out-Null
$clean = Join-Path $out 'clean.wav'
if ($SourceWav) {
    $source = $SourceWav
    if (![IO.Path]::IsPathRooted($source)) { $source = Join-Path $root $source }
    & python (Join-Path $root 'host/stage_wav.py') inspect $source
    if ($LASTEXITCODE -ne 0) { throw 'Source must be a valid 48 kHz mono PCM24 WAV.' }
    Copy-Item -LiteralPath $source -Destination $clean
} else {
    & python (Join-Path $root 'host/stage_wav.py') make-tone --output $clean
    if ($LASTEXITCODE -ne 0) { throw 'Clean waveform generation failed.' }
}
foreach ($case in @(@('cw','cw-900hz'), @('field','field-light'))) {
    $name, $recipe = $case
    & $renderer --source $clean --scenario (Join-Path $root "examples/$recipe.json") `
        --output (Join-Path $out "$name.wav") --sidecar (Join-Path $out "$name.json")
    if ($LASTEXITCODE -ne 0) { throw "Rendering $name failed; inspect diagnostics." }
}
Write-Output "Created clean.wav, cw.wav, field.wav and render sidecars in $out"
if ($Play -ne 'none') {
    & python (Join-Path $root 'host/play_wav.py') (Join-Path $out "$Play.wav") --device $Device --gain-db -12
    if ($LASTEXITCODE -ne 0) { throw 'Playback did not complete cleanly.' }
}
