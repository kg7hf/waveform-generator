# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$generatorRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$config = Get-Content -Raw -LiteralPath (Join-Path $generatorRoot 'dependencies.json') | ConvertFrom-Json
# Update only the dependencies this repository builds, not upstream example
# submodules: the dependencies.json entries (the six firmware inputs and Catch2, the
# host unit-test framework) and pathsim, the channel-sim build's reference
# simulator, so a fresh clone has everything any build needs before the first
# configure. pathsim is in no dependencies.json entry: channel-sim's configure
# applies its patches to a build-tree copy (docs/modules/pathsim.md).
$pathsim = 'third_party/pathsim'
$paths = @($config.dependencies | ForEach-Object { $_.destination }) + $pathsim
# Git refuses conflicting local changes; never force/reset vendor work.
# dependencies.py then checks every dependencies.json entry's local files.
# Git records each submodule revision.
git -C $generatorRoot submodule sync -- @paths
if ($LASTEXITCODE -ne 0) { throw 'Could not synchronize submodule URLs' }
git -C $generatorRoot -c core.autocrlf=false submodule update --init --checkout -- @paths
if ($LASTEXITCODE -ne 0) { throw 'Could not initialize submodules' }
python (Join-Path $PSScriptRoot 'dependencies.py') --root $generatorRoot
if ($LASTEXITCODE -ne 0) { throw 'Dependency verification failed' }
