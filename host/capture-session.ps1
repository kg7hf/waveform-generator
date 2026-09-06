<#
.SYNOPSIS
Run waveform_capture.exe as a controlled session: start, log READY, wait for a stop file, send STOP.

.DESCRIPTION
Starts the recorder with --stop-stdin and stdin redirected, copies its JSON Lines to
-JsonlPath through polled asynchronous reads (no event handlers: PowerShell script
blocks cannot run on .NET thread-pool threads, and the recorder is silent between
READY and FINAL so a blocking read would never return), and writes STOP on stdin as
soon as -StopFile exists or -MaxSeconds elapses. The recorder then finalizes the WAV
header and exits. The recorder needs the WinLibs runtime DLLs on PATH.

.EXAMPLE
.\host\capture-session.ps1 -Output run\capture.wav -DeviceName "Microphone (Realtek(R) Audio)" -StopFile run\stop.flag -JsonlPath run\capture.jsonl
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Output,
    [Parameter(Mandatory = $true)] [string] $DeviceName,
    [Parameter(Mandatory = $true)] [string] $StopFile,
    [Parameter(Mandatory = $true)] [string] $JsonlPath,
    [string] $Backend = 'wasapi-raw',
    [string] $Channel = 'left',
    [int] $MaxSeconds = 4000,
    [string] $Recorder = (Join-Path $PSScriptRoot '..\build\host-release\host\capture\waveform_capture.exe')
)

$ErrorActionPreference = 'Stop'
$recorderPath = (Resolve-Path $Recorder).Path
if (Test-Path $StopFile) { Remove-Item $StopFile -Force }
Set-Content -LiteralPath $JsonlPath -Value '' -NoNewline

$info = [System.Diagnostics.ProcessStartInfo]::new()
$info.FileName = $recorderPath
foreach ($argument in @('--backend', $Backend, '--output', $Output, '--device-name', $DeviceName, '--sample-rate', '48000',
                        '--channel', $Channel, '--max-seconds', [string]$MaxSeconds, '--stop-stdin')) {
    $info.ArgumentList.Add($argument)
}
$info.UseShellExecute = $false
$info.RedirectStandardInput = $true
$info.RedirectStandardOutput = $true
$info.RedirectStandardError = $true

$process = [System.Diagnostics.Process]::Start($info)
$stdoutTask = $process.StandardOutput.ReadLineAsync()
$stderrTask = $process.StandardError.ReadLineAsync()
$stopSent = $false
$deadline = [DateTime]::UtcNow.AddSeconds($MaxSeconds + 30)

function Drain-Task([ref] $taskRef, [System.IO.StreamReader] $reader) {
    while ($taskRef.Value.IsCompleted) {
        $line = $taskRef.Value.Result
        if ($null -eq $line) { return $false }
        Add-Content -LiteralPath $JsonlPath -Value $line
        Write-Output $line
        $taskRef.Value = $reader.ReadLineAsync()
    }
    return $true
}

$stdoutOpen = $true
$stderrOpen = $true
while (-not $process.HasExited) {
    if ($stdoutOpen) { $stdoutOpen = Drain-Task ([ref]$stdoutTask) $process.StandardOutput }
    if ($stderrOpen) { $stderrOpen = Drain-Task ([ref]$stderrTask) $process.StandardError }
    if (-not $stopSent -and ((Test-Path $StopFile) -or ([DateTime]::UtcNow -gt $deadline))) {
        $process.StandardInput.WriteLine('STOP')
        $process.StandardInput.Flush()
        $stopSent = $true
        Write-Output "STOP sent at $([DateTime]::UtcNow.ToString('o'))"
    }
    Start-Sleep -Milliseconds 250
}
$process.WaitForExit()
if ($stdoutOpen) { [void](Drain-Task ([ref]$stdoutTask) $process.StandardOutput) }
if ($stderrOpen) { [void](Drain-Task ([ref]$stderrTask) $process.StandardError) }
Write-Output "recorder exit code: $($process.ExitCode)"
exit $process.ExitCode
