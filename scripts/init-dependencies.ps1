# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Paul R. Decker

[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$generatorRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$config = Get-Content -Raw -LiteralPath (Join-Path $generatorRoot 'dependencies.json') | ConvertFrom-Json
$paths = @($config.dependencies | ForEach-Object { $_.destination })
# Update only the six dependencies used here, not upstream example submodules.
# Git refuses conflicting local changes; never force/reset vendor work.
git -C $generatorRoot submodule sync -- @paths
if ($LASTEXITCODE -ne 0) { throw 'Could not synchronize submodule URLs' }
git -C $generatorRoot -c core.autocrlf=false submodule update --init --checkout -- @paths
if ($LASTEXITCODE -ne 0) { throw 'Could not initialize submodules' }
python (Join-Path $PSScriptRoot 'dependencies.py') --root $generatorRoot
if ($LASTEXITCODE -ne 0) { throw 'Dependency verification failed' }
