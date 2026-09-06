[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^COM[0-9]+$')]
    [string]$Port,

    [Parameter(Mandatory = $true)]
    [ValidateSet('Status', 'Host', 'Local', 'Play')]
    [string]$Action,

    [ValidateRange(250, 10000)]
    [int]$TimeoutMilliseconds = 2000,

    [switch]$HostVolumeDismounted
)

if ($Action -eq 'Local' -and -not $HostVolumeDismounted) {
    throw 'MEDIA LOCAL requires -HostVolumeDismounted after Windows has flushed and dismounted the fixture volume.'
}

if ($Action -ne 'Local' -and $HostVolumeDismounted) {
    throw '-HostVolumeDismounted is valid only with -Action Local.'
}

# The P1.2 wire protocol remains the fixed four-command bring-up bridge.
# Host maps to MEDIA HOST; firmware may either confirm existing MSC ownership
# or start one idle media initialization attempt after an uninitialized/faulted
# card state.
$wireCommand = switch ($Action) {
    'Status' { 'STATUS' }
    'Host'   { 'MEDIA HOST' }
    'Local'  { 'MEDIA LOCAL' }
    'Play'   { 'PLAY' }
}

$serial = [System.IO.Ports.SerialPort]::new(
    $Port,
    115200,
    [System.IO.Ports.Parity]::None,
    8,
    [System.IO.Ports.StopBits]::One)
$serial.ReadTimeout = 100
$serial.WriteTimeout = $TimeoutMilliseconds
$serial.NewLine = "`n"
$serial.DtrEnable = $true

try {
    $serial.Open()
    Start-Sleep -Milliseconds 100
    $serial.DiscardInBuffer()
    $serial.WriteLine($wireCommand)

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMilliseconds)
    $response = [System.Text.StringBuilder]::new()
    while ([DateTime]::UtcNow -lt $deadline) {
        $chunk = $serial.ReadExisting()
        if ($chunk.Length -ne 0) {
            [void]$response.Append($chunk)
            $newline = $response.ToString().IndexOf("`n", [StringComparison]::Ordinal)
            if ($newline -ge 0) {
                $line = $response.ToString(0, $newline).TrimEnd("`r")
                Write-Output $line
                if ($line.StartsWith('ERR ', [StringComparison]::Ordinal)) {
                    exit 2
                }
                exit 0
            }
        }
        Start-Sleep -Milliseconds 20
    }
    throw "Timed out waiting for a fixture response on $Port"
}
finally {
    if ($serial.IsOpen) {
        $serial.Close()
    }
    $serial.Dispose()
}
