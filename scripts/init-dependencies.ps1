[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$McuxSdkSource,

    [Parameter(Mandatory = $true)]
    [string]$CmsisSource,

    [Parameter(Mandatory = $true)]
    [string]$FreeRtosSource,

    [Parameter(Mandatory = $true)]
    [string]$TinyUsbSource,

    [Parameter(Mandatory = $true)]
    [string]$SdmmcSource,

    [Parameter(Mandatory = $true)]
    [string]$FatFsSource
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$generatorRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$thirdPartyRoot = Join-Path $generatorRoot "third_party"
$lockPath = Join-Path $generatorRoot "dependencies.lock.json"
$manifestPath = Join-Path $thirdPartyRoot "materialized-files.json"
$lock = Get-Content -Raw -LiteralPath $lockPath | ConvertFrom-Json

function Get-LockedDependency {
    param([Parameter(Mandatory = $true)][string]$Name)

    $dependency = $lock.dependencies | Where-Object { $_.name -eq $Name }
    if ($null -eq $dependency) {
        throw "Dependency '$Name' is missing from dependencies.lock.json"
    }
    return $dependency
}

function Assert-VerifiedGitSource {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ExpectedCommit
    )

    $resolved = (Resolve-Path -LiteralPath $Path).Path
    $actualCommit = (& git -C $resolved rev-parse HEAD 2>$null).Trim()
    if ($LASTEXITCODE -ne 0 -or $actualCommit -ne $ExpectedCommit) {
        throw "$Name source commit '$actualCommit' does not match locked commit '$ExpectedCommit'"
    }

    $dirty = & git -C $resolved status --porcelain --untracked-files=all
    if ($LASTEXITCODE -ne 0 -or $dirty) {
        throw "$Name source checkout is dirty or could not be verified"
    }

    return $resolved
}

function Assert-EmptyDestination {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (Test-Path -LiteralPath $Path) {
        $entry = Get-ChildItem -LiteralPath $Path -Force | Select-Object -First 1
        if ($null -ne $entry) {
            throw "Refusing to overwrite non-empty dependency destination: $Path"
        }
    } else {
        New-Item -ItemType Directory -Path $Path | Out-Null
    }
}

function Copy-DependencySelection {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$DestinationName,
        [Parameter(Mandatory = $true)][string[]]$RelativePaths
    )

    $dependency = Get-LockedDependency -Name $Name
    if ($dependency.status -notin @("planned", "materialized")) {
        throw "$Name must have a verified locked commit before materialization"
    }
    $sourceRoot = Assert-VerifiedGitSource -Name $Name -Path $Source -ExpectedCommit $dependency.commit
    $destinationRoot = Join-Path $thirdPartyRoot $DestinationName

    # Only the declared source selection is part of the import. Reparse points
    # elsewhere in a donor checkout are irrelevant, while a selected link would
    # make the copied byte set depend on data outside the locked tree.
    foreach ($relativePath in $RelativePaths) {
        $sourcePath = Join-Path $sourceRoot $relativePath
        if (-not (Test-Path -LiteralPath $sourcePath)) {
            throw "$Name locked selection is missing: $relativePath"
        }
        $selectedItems = @((Get-Item -LiteralPath $sourcePath -Force))
        if ((Get-Item -LiteralPath $sourcePath -Force).PSIsContainer) {
            $selectedItems += @(Get-ChildItem -LiteralPath $sourcePath -Recurse -Force)
        }
        $reparse = $selectedItems |
            Where-Object { ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 } |
            Select-Object -First 1
        if ($null -ne $reparse) {
            throw "$Name selected source contains a reparse point: $($reparse.FullName)"
        }
    }

    Assert-EmptyDestination -Path $destinationRoot

    foreach ($relativePath in $RelativePaths) {
        $sourcePath = Join-Path $sourceRoot $relativePath
        $destinationPath = Join-Path $destinationRoot $relativePath
        $parent = Split-Path -Parent $destinationPath
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
        Copy-Item -LiteralPath $sourcePath -Destination $destinationPath -Recurse
    }
}

Copy-DependencySelection -Name "mcux-sdk" -Source $McuxSdkSource -DestinationName "mcux-sdk" -RelativePaths @(
    "COPYING-BSD-3",
    "boards/evkmimxrt1170",
    "components/codec",
    "components/gpio/component_igpio_adapter.cmake",
    "components/gpio/fsl_adapter_gpio.h",
    "components/gpio/fsl_adapter_igpio.c",
    "components/i2c",
    "components/lists",
    "components/osa/component_osa_free_rtos.cmake",
    "components/osa/fsl_os_abstraction.h",
    "components/osa/fsl_os_abstraction_config.h",
    "components/osa/fsl_os_abstraction_free_rtos.c",
    "components/osa/fsl_os_abstraction_free_rtos.h",
    "components/osa/set_component_osa.cmake",
    "components/uart",
    "devices/MIMXRT1176",
    "drivers/common",
    "drivers/dmamux",
    "drivers/edma",
    "drivers/igpio",
    "drivers/lpi2c",
    "drivers/lpuart",
    "drivers/mu",
    "drivers/sai",
    "drivers/usdhc/driver_usdhc.cmake",
    "drivers/usdhc/fsl_usdhc.c",
    "drivers/usdhc/fsl_usdhc.h",
    "utilities/assert",
    "utilities/debug_console_lite",
    "utilities/misc_utilities",
    "utilities/str"
)

Copy-DependencySelection -Name "CMSIS" -Source $CmsisSource -DestinationName "CMSIS" -RelativePaths @(
    "LICENSE.txt",
    "Core/Include"
)

Copy-DependencySelection -Name "FreeRTOS-Kernel" -Source $FreeRtosSource -DestinationName "FreeRTOS-Kernel" -RelativePaths @(
    "LICENSE.md",
    "include",
    "portable/GCC/ARM_CM7/r0p1",
    "list.c",
    "queue.c",
    "tasks.c"
)

Copy-DependencySelection -Name "tinyusb" -Source $TinyUsbSource -DestinationName "tinyusb" -RelativePaths @(
    "LICENSE",
    "src"
)

Copy-DependencySelection -Name "sdmmc" -Source $SdmmcSource -DestinationName "sdmmc" -RelativePaths @(
    "COPYING-BSD-3",
    "common/fsl_sdmmc_common.c",
    "common/fsl_sdmmc_common.h",
    "common/fsl_sdmmc_spec.h",
    "host/usdhc/fsl_sdmmc_host.h",
    "host/usdhc/non_blocking/fsl_sdmmc_host.c",
    "osa/fsl_sdmmc_osa.c",
    "osa/fsl_sdmmc_osa.h",
    "sd/fsl_sd.c",
    "sd/fsl_sd.h"
)

Copy-DependencySelection -Name "fatfs" -Source $FatFsSource -DestinationName "fatfs" -RelativePaths @(
    "LICENSE.txt",
    "SW-Content-Register.txt",
    "source/diskio.c",
    "source/diskio.h",
    "source/ff.c",
    "source/ff.h",
    "source/ffunicode.c",
    "source/fsl_sd_disk/fsl_sd_disk.c",
    "source/fsl_sd_disk/fsl_sd_disk.h"
)

$files = Get-ChildItem -LiteralPath $thirdPartyRoot -Recurse -File -Force |
    Where-Object {
        $_.FullName -ne $manifestPath -and
        $_.FullName -ne (Join-Path $thirdPartyRoot "README.md")
    } |
    ForEach-Object {
        $relative = [IO.Path]::GetRelativePath($generatorRoot, $_.FullName).Replace('\', '/')
        [ordered]@{
            path = $relative
            bytes = $_.Length
            sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLowerInvariant()
        }
    } |
    Sort-Object { $_.path }

$manifest = [ordered]@{
    schema_version = "wfg-materialized-files/1"
    generated_from = "explicit clean source checkouts matching dependencies.lock.json"
    files = @($files)
}
$json = $manifest | ConvertTo-Json -Depth 6
[IO.File]::WriteAllText($manifestPath, $json + [Environment]::NewLine,
    [Text.UTF8Encoding]::new($false))

foreach ($dependency in $lock.dependencies) {
    if ($dependency.status -notin @("planned", "materialized")) {
        continue
    }

    $prefix = $dependency.destination.TrimEnd('/') + '/'
    $dependencyFiles = @(
        $files |
            Where-Object { $_.path.StartsWith($prefix, [StringComparison]::Ordinal) } |
            ForEach-Object {
                [ordered]@{
                    path = $_.path.Substring($prefix.Length)
                    size_bytes = $_.bytes
                    sha256 = $_.sha256
                }
            }
    )
    if ($dependencyFiles.Count -eq 0) {
        throw "No materialized files were recorded for $($dependency.name)"
    }
    $dependency.status = "materialized"
    $dependency.files = $dependencyFiles
}

$lockJson = $lock | ConvertTo-Json -Depth 10
[IO.File]::WriteAllText($lockPath, $lockJson + [Environment]::NewLine,
    [Text.UTF8Encoding]::new($false))

Write-Host "Materialized $($files.Count) dependency files under $thirdPartyRoot"
Write-Host "Manifest: $manifestPath"
Write-Host "Updated lock: $lockPath"
