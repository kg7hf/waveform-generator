# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

<#
Run clang-tidy with the repository .clang-tidy over the owned translation units
of a build's compile database (docs/coding-standard.md, "Compliance ratchet").
Only sources under the owned top-level directories and outside explicitly
excluded nested trees are analysed. Header findings use the same ownership and
exclusion rules; the script passes those filters anchored at the repository
root, so a checkout below a directory named build or third_party is checked
the same way.
Each source runs once, a finding in a shared header is reported once, and the
findings are sorted by location, so two runs over the same tree print the same
text. A diagnostic without a location (a driver, flag or configuration error)
is a finding against its unit, and any other clang-tidy output is printed per
unit after the findings, so a failure always shows its cause. The script fails
when clang-tidy exits with a failure or reports an error: a compiler, driver or
configuration error, or a WarningsAsErrors check.

-BuildDir names the build whose compile_commands.json is read: a host build,
by default build/host-release, which `cmake --preset host-release` writes,
channel-sim's own build (build/main/pathsim-host), whose owned units are
channel-sim's and common/status.cpp (pathsim's sources there are a build tree),
or a firmware build, given the Arm GNU toolchain's include directories through
-ExtraArgs (docs/coding-standard.md, the firmware hand run). -Files restricts
the run to the named sources (relative to the repository root); findings in
the headers they include are still reported. -ExtraArgs passes each of its
values to clang-tidy as an --extra-arg, after the unit's own flags. -Summary
appends the finding counts per check and per top-level directory.

A firmware database carries the post-link stack check's GCC flags
(scripts/check-stack-usage.py): -fcallgraph-info=su, which clang rejects as an
unknown argument (an error in every unit), and -fstack-usage, which only asks
for a .su file. A database that carries either, in a compile command's string or
its arguments list, is read through a copy without them in the temporary
directory, removed afterwards; the database itself is left as it is, and the
output says which flags were dropped from how many compile commands.
#>
#Requires -Version 7.0
[CmdletBinding()]
param(
    [string]$BuildDir = 'build/host-release',
    [string[]]$Files = @(),
    [string[]]$ExtraArgs = @(),
    [switch]$Summary,
    [string]$ClangTidyPath = 'clang-tidy',
    [ValidateRange(1, 256)][int]$Jobs = [Environment]::ProcessorCount
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ownedDirectories = @('channel-sim', 'common', 'examples', 'host', 'rt1170', 'rt1064', 'signal-lab', 'tests', 'waveform-source')
$excludedDirectories = @('third_party', 'build', 'external', 'externals', 'vendor', 'vendors', '_deps', 'tinyusb', 'nxp', 'stm', 'HFSimulator', 'pathsim', 'Catch2')
$sourceExtensions = @('.c', '.cc', '.cpp', '.cxx')
# Diagnostics and status lines are told apart by case ('error:' against 'Error:'),
# so they are matched with -cmatch. A note without a location (about a fix-it)
# stays a plain line of the finding it follows.
$checksPattern = '(?: \[(?<checks>[^\]\s]+)\])?$'
$locatedPattern = '^(?<path>.+?):(?<line>\d+):(?<column>\d+): (?<severity>warning|error|note): (?<message>.*?)' + $checksPattern
$unlocatedPattern = '^(?<severity>warning|error): (?<message>.*?)' + $checksPattern
# clang-tidy's own status lines; they go to stderr, so their place among the findings varies.
$statusPattern = '^(Error while processing |Error parsing |Error: )'
$noisePattern = '^(\d+ warnings?( and \d+ errors?)? generated\.|\d+ errors? generated\.|Suppressed \d+ warnings.*|Use -header-filter=.*)$'
# The GCC flags the post-link stack check adds, which clang does not take (see above).
$gccOnlyFlagPattern = '-fcallgraph-info(?:=\S*)?|-fstack-usage'

# Repository-relative path with forward slashes; '..' first when outside the repository.
function ConvertTo-RepoPath([string]$Path, [string]$BasePath = $projectRoot) {
    $full = [IO.Path]::GetFullPath($Path, $BasePath)
    return [IO.Path]::GetRelativePath($projectRoot, $full).Replace('\', '/')
}

function Test-OwnedSource([string]$RepoPath) {
    $segments = $RepoPath.Split('/')
    foreach ($segment in $segments) {
        if ($excludedDirectories -contains $segment) { return $false }
    }
    $top = $segments[0]
    return $ownedDirectories -contains $top -and $sourceExtensions -contains [IO.Path]::GetExtension($RepoPath).ToLowerInvariant()
}

# One path character as a POSIX extended regular expression (the syntax of
# clang-tidy's header filters): either separator, and an ASCII letter in either
# case, because Windows ignores case and the compile database may spell the
# root differently from this script's location.
function ConvertTo-CharacterRegex([char]$Character) {
    if ($Character -eq '/' -or $Character -eq '\') { return '[/\\]' }
    if ("$Character" -cmatch '^[A-Za-z]$') { return "[$("$Character".ToLowerInvariant())$("$Character".ToUpperInvariant())]" }
    if ('.[]{}()*+?|^$'.Contains($Character)) { return "\$Character" }
    return "$Character"
}

# A regular expression matching the names that start with the directory $Path.
function ConvertTo-RootRegex([string]$Path) {
    $pattern = [Text.StringBuilder]::new('^')
    foreach ($character in $Path.TrimEnd('/', '\').ToCharArray()) {
        [void]$pattern.Append((ConvertTo-CharacterRegex $character))
    }
    return $pattern.ToString()
}

function ConvertTo-CaseInsensitiveRegex([string]$Text) {
    $pattern = [Text.StringBuilder]::new()
    foreach ($character in $Text.ToCharArray()) {
        [void]$pattern.Append((ConvertTo-CharacterRegex $character))
    }
    return $pattern.ToString()
}

# One output line as the head of a finding, or $null when it is no diagnostic.
# A diagnostic without a location belongs to $Unit, at line 0.
function ConvertFrom-Diagnostic([string]$Line, [string]$Unit) {
    if ($Line -cmatch $locatedPattern) {
        $path = ConvertTo-RepoPath $Matches.path
        $lineNumber = [int]$Matches.line
        $column = [int]$Matches.column
        $text = "${path}:${lineNumber}:${column}: $($Matches.severity): $($Matches.message)"
    }
    elseif ($Line -cmatch $unlocatedPattern) {
        $path = $Unit
        $lineNumber = 0
        $column = 0
        $text = "${Unit}: $($Matches.severity): $($Matches.message)"
    }
    else {
        return $null
    }
    $checks = if ($Matches.ContainsKey('checks')) { $Matches.checks } else { 'clang-diagnostic-error' }
    if ($Matches.ContainsKey('checks')) { $text += " [$checks]" }
    $finding = [pscustomobject]@{
        Path = $path; Line = $lineNumber; Column = $column
        Severity = $Matches.severity; Checks = $checks; Message = $Matches.message
        Text = [Collections.Generic.List[string]]::new()
    }
    $finding.Text.Add($text)
    return $finding
}

# Splits one translation unit's output into Findings (a warning or error line
# followed by its source excerpt and notes) and Other: the lines that belong to
# no finding, which are clang-tidy's status lines and anything printed before
# the first diagnostic. Compiler summary lines are dropped.
function Split-Output([string]$Unit, [string[]]$Lines) {
    $findings = [Collections.Generic.List[object]]::new()
    $other = [Collections.Generic.List[string]]::new()
    $current = $null
    foreach ($line in $Lines) {
        if ($line -match $noisePattern) { continue }
        if ($line -cmatch $statusPattern) {
            $other.Add($line)
            continue
        }
        $diagnostic = ConvertFrom-Diagnostic $line $Unit
        if ($null -eq $diagnostic) {
            if ($null -ne $current) { $current.Text.Add($line) } else { $other.Add($line) }
            continue
        }
        if ($diagnostic.Severity -eq 'note' -and $null -ne $current) {
            $current.Text.Add($diagnostic.Text[0])
            continue
        }
        $current = $diagnostic
        $findings.Add($current)
    }
    return [pscustomobject]@{ Findings = $findings; Other = $other }
}

# Drops the GCC-only flags from one compile database entry, in the form it
# writes its command: a "command" string (CMake's) or an "arguments" list.
# Returns the flags it dropped.
function Remove-GccOnlyFlags([object]$Entry) {
    $dropped = [Collections.Generic.List[string]]::new()
    if ($null -ne $Entry.PSObject.Properties['arguments']) {
        $kept = [Collections.Generic.List[string]]::new()
        foreach ($argument in @($Entry.arguments)) {
            if ("$argument" -cmatch "^(?:$gccOnlyFlagPattern)$") { $dropped.Add("$argument") } else { $kept.Add("$argument") }
        }
        $Entry.arguments = $kept.ToArray()
    }
    if ($null -ne $Entry.PSObject.Properties['command']) {
        $flag = "(?:$gccOnlyFlagPattern)(?=\s|$)"
        foreach ($match in [regex]::Matches($Entry.command, "(?<=\s)$flag")) { $dropped.Add($match.Value) }
        $Entry.command = [regex]::Replace($Entry.command, "\s+$flag", '')
    }
    return $dropped.ToArray()
}

function Write-CountTable([string]$Title, [object[]]$Rows) {
    Write-Output ''
    Write-Output $Title
    $Rows | Sort-Object @{ Expression = 'Count'; Descending = $true }, @{ Expression = 'Name'; Descending = $false } |
        ForEach-Object { Write-Output ('{0,7}  {1}' -f $_.Count, $_.Name) }
}

$tidy = (Get-Command $ClangTidyPath -ErrorAction Stop).Source
$buildRoot = [IO.Path]::GetFullPath($BuildDir, $projectRoot)
$database = Join-Path $buildRoot 'compile_commands.json'
if (-not (Test-Path -LiteralPath $database)) { throw "No compile database in $buildRoot; configure that build first." }
$entries = @(Get-Content -LiteralPath $database -Raw | ConvertFrom-Json)
$units = @($entries |
    ForEach-Object { ConvertTo-RepoPath $_.file $_.directory } |
    Where-Object { Test-OwnedSource $_ } |
    Sort-Object -Unique)
if ($Files.Count -gt 0) {
    $requested = @($Files | ForEach-Object { ConvertTo-RepoPath $_ } | Sort-Object -Unique)
    $missing = @($requested | Where-Object { $units -notcontains $_ })
    if ($missing.Count -gt 0) {
        throw "Not an owned translation unit of $BuildDir (for a header, name a source that includes it): $($missing -join ', ')"
    }
    $units = $requested
}
if ($units.Count -eq 0) { throw "No owned translation units in $database." }

# The database clang-tidy reads: this one, or a copy without the GCC-only flags.
$droppedFlags = [Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
$filteredCommands = 0
foreach ($entry in $entries) {
    $dropped = @(Remove-GccOnlyFlags $entry)
    if ($dropped.Count -gt 0) {
        $filteredCommands++
        foreach ($flag in $dropped) { [void]$droppedFlags.Add($flag) }
    }
}
$extraArguments = @($ExtraArgs | ForEach-Object { "--extra-arg=$_" })

# The .clang-tidy header filters, anchored at this checkout. clang-tidy matches
# them against absolute paths, so unanchored they would also match a directory
# above the repository that has an owned or excluded name.
$rootRegex = ConvertTo-RootRegex $projectRoot
$ownedDirectoryPattern = ($ownedDirectories | ForEach-Object { ConvertTo-CaseInsensitiveRegex $_ }) -join '|'
$excludedDirectoryPattern = ($excludedDirectories | ForEach-Object { ConvertTo-CaseInsensitiveRegex $_ }) -join '|'
$filterArguments = @(
    "--header-filter=${rootRegex}[/\\]($ownedDirectoryPattern)[/\\]",
    "--exclude-header-filter=${rootRegex}[/\\](.*[/\\])?($excludedDirectoryPattern)[/\\]"
)

$readRoot = $buildRoot
Push-Location $projectRoot
try {
    if ($filteredCommands -gt 0) {
        $readRoot = Join-Path ([IO.Path]::GetTempPath()) "wfg-clang-tidy-$([guid]::NewGuid().ToString('N'))"
        New-Item -ItemType Directory -Path $readRoot | Out-Null
        ConvertTo-Json -InputObject $entries -Depth 8 |
            Set-Content -LiteralPath (Join-Path $readRoot 'compile_commands.json') -Encoding utf8NoBOM
    }
    $results = @($units | ForEach-Object -ThrottleLimit $Jobs -Parallel {
        # clang-tidy writes its summary to stderr; read it as text, not as errors.
        $ErrorActionPreference = 'Continue'
        $filters = $using:filterArguments
        $extra = $using:extraArguments
        $output = @(& $using:tidy -p $using:readRoot --quiet @filters @extra $_ 2>&1 | ForEach-Object { "$_" })
        [pscustomobject]@{ Unit = $_; ExitCode = $LASTEXITCODE; Output = $output }
    } | Sort-Object Unit)
}
finally {
    Pop-Location
    if ($readRoot -ne $buildRoot -and (Test-Path -LiteralPath $readRoot)) {
        Remove-Item -LiteralPath $readRoot -Recurse -Force -Confirm:$false
    }
}

# clang-tidy exits 0 after some errors (a .clang-tidy it cannot parse), so an
# error diagnostic fails its unit as well.
$parsed = @($results | ForEach-Object {
    $split = Split-Output $_.Unit $_.Output
    $errors = @($split.Findings | Where-Object Severity -eq 'error')
    [pscustomobject]@{
        Unit = $_.Unit; ExitCode = $_.ExitCode; Findings = $split.Findings; Other = $split.Other
        Failed = $_.ExitCode -ne 0 -or $errors.Count -gt 0
    }
})

# A header included by several units yields the same finding in each; keep one.
$seen = [Collections.Generic.HashSet[string]]::new()
$findings = @($parsed | ForEach-Object { $_.Findings } |
    Where-Object { $seen.Add("$($_.Path):$($_.Line):$($_.Column):$($_.Severity):$($_.Checks):$($_.Message)") } |
    Sort-Object Path, Line, Column, Checks, Message)
$findings | ForEach-Object { $_.Text | Write-Output }
$parsed | Where-Object { $_.Other.Count -gt 0 } | ForEach-Object {
    Write-Output ''
    Write-Output "clang-tidy output for $($_.Unit) (exit code $($_.ExitCode)):"
    $_.Other | Write-Output
}
$failed = @($parsed | Where-Object Failed)

if ($Summary) {
    # A finding reported under aliased checks counts once under each of them.
    $perCheck = @($findings | ForEach-Object { $_.Checks.Split(',') } | Group-Object -NoElement)
    $perDirectory = @($findings | ForEach-Object { $_.Path.Split('/')[0] } | Group-Object -NoElement)
    Write-CountTable 'Findings per check:' $perCheck
    Write-CountTable 'Findings per top-level directory:' $perDirectory
}
Write-Output ''
if ($filteredCommands -gt 0) {
    Write-Output "clang-tidy read $database without the GCC-only flags $($droppedFlags -join ', ') ($filteredCommands compile commands)."
}
Write-Output "clang-tidy: $($findings.Count) findings in $($units.Count) translation units ($BuildDir)."
if ($failed.Count -gt 0) { throw "clang-tidy reported errors in: $(($failed | ForEach-Object Unit) -join ', ')" }
