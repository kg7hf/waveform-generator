# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Paul R. Decker

<#[
Download a CPython audio test sample, retain it, explicitly convert
to a ten-second mono 48 kHz PCM24 demo, render effects, and optionally play.
]#>
[CmdletBinding()]
param(
    [string]$OutputDirectory = 'build/demo-download',
    [ValidateSet('none', 'clean', 'cw', 'field')][string]$Play = 'none',
    [string]$Device = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($Play -ne 'none' -and [string]::IsNullOrWhiteSpace($Device)) { throw 'Playback requires an exact -Device name.' }
if ([IO.Path]::IsPathRooted($OutputDirectory)) { $out = [IO.Path]::GetFullPath($OutputDirectory) }
else { $out = [IO.Path]::GetFullPath((Join-Path $root $OutputDirectory)) }
if (Test-Path -LiteralPath $out) { throw "Use a new output directory: $out" }
New-Item -ItemType Directory -Path $out | Out-Null
$url = 'https://raw.githubusercontent.com/python/cpython/v3.13.0/Lib/test/audiodata/pluck-pcm16.wav'
$original = Join-Path $out 'downloaded-original.wav'
Invoke-WebRequest -Uri $url -OutFile $original
@{ url = $url; license = 'https://github.com/python/cpython/blob/v3.13.0/LICENSE' } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $out 'download.json') -Encoding utf8
$canonical = Join-Path $out 'canonical.wav'
& python (Join-Path $root 'host/prepare_demo_wav.py') $original $canonical --seconds 10
if ($LASTEXITCODE -ne 0) { throw 'Explicit demo conversion failed.' }
& (Join-Path $PSScriptRoot 'demo-waveform.ps1') -OutputDirectory (Join-Path $out 'mixes') `
    -SourceWav $canonical -Play $Play -Device $Device
