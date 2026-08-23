param(
    [Parameter(Position = 0)][string]$VideoFile,
    [Parameter(Position = 1)][ValidateSet(1, 2, 4)][int]$Rank = 2,
    [Parameter(Position = 2)][ValidateSet('QPSK', '16QAM', '64QAM', '256QAM')][string]$Modulation = '64QAM',
    [Parameter(Position = 3)][double]$SnrDb = 45,
    [Parameter(Position = 4)][double]$CfoHz = 300,
    [Parameter(Position = 5)][double]$SfoPpm = 20,
    [Parameter(Position = 6)][ValidateRange(0, 128)][int]$TimingSamples = 20,
    [Parameter(Position = 7)][string]$Tdl = '0:0:0:0+3:-14:45:0+9:-8:-80:69.444444',
    [Parameter(Position = 8)][ValidateRange(0.2, 3600)][double]$RefreshSeconds = 2,
    [Parameter(Position = 9)][ValidateRange(0, 512)][int]$SensingCoherentFrames = 128,
    [Parameter(Position = 10)][ValidateRange(-0.95, 0.95)][double]$TxCorrelation = 0.2,
    [Parameter(Position = 11)][ValidateRange(-0.95, 0.95)][double]$RxCorrelation = 0.2,
    [Parameter(Position = 12)][ValidateRange(250, 5000)][int]$VideoBitrateKbps = 1000,
    [Parameter(Position = 13)][ValidateSet('fdm', 'nr-dmrs')][string]$PilotMode = 'fdm'
)

$ErrorActionPreference = 'Stop'
$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$repositoryRoot = Split-Path -Parent $scriptDirectory
$interactive = [string]::IsNullOrWhiteSpace($VideoFile)
if ($interactive) {
    $defaultVideo = Get-ChildItem -LiteralPath $repositoryRoot -Filter '1920x1080_2Mbps_2.mp4' -File -Recurse |
        Select-Object -First 1
    if ($null -ne $defaultVideo) { $VideoFile = $defaultVideo.FullName }
    if ($null -eq $defaultVideo) {
        # Keep this Windows PowerShell 5.1 script ASCII-only. UTF-8 files
        # without a BOM are otherwise decoded using the legacy ANSI code page.
        $videoSourceDirectoryName = [string][char]0x89C6 + [char]0x9891 + [char]0x6E90
        $defaultVideo = Get-ChildItem -LiteralPath (Join-Path $repositoryRoot $videoSourceDirectoryName) -Filter '*.mp4' -File -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($null -ne $defaultVideo) { $VideoFile = $defaultVideo.FullName }
    }
}

function Read-DefaultValue {
    param([string]$Prompt, [string]$DefaultValue)
    $entered = Read-Host "$Prompt [$DefaultValue]"
    if ([string]::IsNullOrWhiteSpace($entered)) { return $DefaultValue }
    return $entered
}

function Quote-ProcessArgument {
    param([string]$Value)
    return [string][char]34 + $Value + [char]34
}

if ($interactive) {
    Write-Host ''
    Write-Host 'OpenISAC Windows manual video PHY test' -ForegroundColor Cyan
    Write-Host 'Press Enter to keep each default value.'
    Write-Host ''
    $VideoFile = Read-DefaultValue 'Video file' $VideoFile
    $Rank = [int](Read-DefaultValue 'Spatial Rank, 1, 2 or 4' ([string]$Rank))
    $Modulation = Read-DefaultValue 'Modulation QPSK/16QAM/64QAM/256QAM' $Modulation
    $SnrDb = [double](Read-DefaultValue 'SNR dB' ([string]$SnrDb))
    $CfoHz = [double](Read-DefaultValue 'Carrier frequency offset Hz' ([string]$CfoHz))
    $SfoPpm = [double](Read-DefaultValue 'Sample-rate offset ppm' ([string]$SfoPpm))
    $TimingSamples = [int](Read-DefaultValue 'Timing offset samples' ([string]$TimingSamples))
    $Tdl = Read-DefaultValue 'TDL delay:power_dB:phase_deg:doppler_Hz joined by +' $Tdl
    $RefreshSeconds = [double](Read-DefaultValue 'Plot snapshot interval seconds' ([string]$RefreshSeconds))
    $SensingCoherentFrames = [int](Read-DefaultValue 'Sensing coherent frames, 0/16/32/64/128/256/512' ([string]$SensingCoherentFrames))
    $TxCorrelation = [double](Read-DefaultValue 'Transmit spatial correlation, -0.95 to 0.95' ([string]$TxCorrelation))
    $RxCorrelation = [double](Read-DefaultValue 'Receive spatial correlation, -0.95 to 0.95' ([string]$RxCorrelation))
    $VideoBitrateKbps = [int](Read-DefaultValue 'Rank-4 VLC transcoding bitrate kbit/s' ([string]$VideoBitrateKbps))
    $PilotMode = Read-DefaultValue 'Pilot mode fdm/nr-dmrs' $PilotMode
}

$allowedModulations = @('QPSK', '16QAM', '64QAM', '256QAM')
if ($Rank -notin @(1, 2, 4)) { throw 'Rank must be 1, 2 or 4.' }
if ($Modulation.ToUpperInvariant() -notin $allowedModulations) {
    throw 'Modulation must be QPSK, 16QAM, 64QAM or 256QAM.'
}
if ($PilotMode.ToLowerInvariant() -notin @('fdm', 'nr-dmrs')) {
    throw 'Pilot mode must be fdm or nr-dmrs.'
}
$PilotMode = $PilotMode.ToLowerInvariant()
if ($TimingSamples -lt 0 -or $TimingSamples -gt 128) {
    throw 'Timing offset must be between 0 and 128 samples.'
}
if ($RefreshSeconds -lt 0.2) { throw 'Refresh interval must be at least 0.2 seconds.' }
if ($SensingCoherentFrames -notin @(0, 16, 32, 64, 128, 256, 512)) {
    throw 'Sensing coherent frames must be 0, 16, 32, 64, 128, 256 or 512.'
}
if ([Math]::Abs($TxCorrelation) -ge 1 -or [Math]::Abs($RxCorrelation) -ge 1) {
    throw 'Tx/Rx spatial correlation magnitude must be below 1.'
}
$VideoFile = [System.IO.Path]::GetFullPath($VideoFile)
if (-not (Test-Path -LiteralPath $VideoFile -PathType Leaf)) {
    throw "Video file was not found: $VideoFile"
}

$bridge = Join-Path $scriptDirectory 'build\ninja-vs2019\openisac_phy_video_bridge.exe'
if (-not (Test-Path -LiteralPath $bridge -PathType Leaf)) {
    Write-Host 'Building the PHY with VS2019...' -ForegroundColor Yellow
    & (Join-Path $scriptDirectory 'build_windows_vs2019.cmd')
    if ($LASTEXITCODE -ne 0) { throw 'VS2019 build failed.' }
}

$vlc = Join-Path $env:ProgramFiles 'VideoLAN\VLC\vlc.exe'
if (-not (Test-Path -LiteralPath $vlc -PathType Leaf)) {
    $programFilesX86 = [Environment]::GetFolderPath('ProgramFilesX86')
    $vlc = Join-Path $programFilesX86 'VideoLAN\VLC\vlc.exe'
}
if (-not (Test-Path -LiteralPath $vlc -PathType Leaf)) { throw 'VLC was not found.' }

$pythonw = Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies\python\pythonw.exe'
if (-not (Test-Path -LiteralPath $pythonw -PathType Leaf)) {
    $pythonCommand = Get-Command pythonw.exe -ErrorAction SilentlyContinue
    if ($null -ne $pythonCommand) { $pythonw = $pythonCommand.Source }
}
if (-not (Test-Path -LiteralPath $pythonw -PathType Leaf)) {
    throw 'Python with Tkinter and NumPy was not found.'
}

$telemetry = Join-Path $repositoryRoot 'measurement\live_phy_monitor'
New-Item -ItemType Directory -Force -Path $telemetry | Out-Null
$bridgeArguments = @(
    '--rank', [string]$Rank,
    '--modulation', $Modulation,
    '--pilot-mode', $PilotMode,
    '--fft', '1024',
    '--cp', '128',
    '--subcarrier-spacing', '15000',
    '--snr', [string]$SnrDb,
    '--cfo', [string]$CfoHz,
    '--sfo', [string]$SfoPpm,
    '--timing', [string]$TimingSamples,
    '--tdl', $Tdl,
    '--queue-packets', '8192',
    '--workers', $(if ($Rank -eq 4) { '12' } else { '8' }),
    '--tx-correlation', [string]$TxCorrelation,
    '--rx-correlation', [string]$RxCorrelation,
    '--mmse-scale', '0.5',
    '--spatial-seed', '49239'
)
$bridgeArguments += @(
    '--telemetry-dir', (Quote-ProcessArgument $telemetry),
    '--telemetry-interval', [string]$RefreshSeconds,
    '--telemetry-points', '4096',
    '--sensing-coherent', [string]$SensingCoherentFrames,
    '--sensing-range-bins', '128'
)

Write-Host ''
Write-Host "Starting Rank-$Rank / $Modulation, SNR $SnrDb dB, CFO $CfoHz Hz, SFO $SfoPpm ppm" -ForegroundColor Cyan
Write-Host 'Formal frame: FFT 1024, CP 128, 15 kHz spacing, LDPC + CRC16'
Write-Host "Pilot mode: $PilotMode; frame symbols: $(if ($PilotMode -eq 'nr-dmrs') { 5 } else { 3 })"
if ($Rank -le 2) {
    Write-Host "Dynamic sensing: $SensingCoherentFrames coherent frames; TDL fourth field is Doppler Hz"
} else {
    Write-Host "Rank-4 sensing: 16-link power combining, $SensingCoherentFrames coherent frames." -ForegroundColor Cyan
    Write-Host "VLC input will be transcoded to H.264 at $VideoBitrateKbps kbit/s."
}
Write-Host "MIMO spatial correlation: Tx=$TxCorrelation, Rx=$RxCorrelation, seed=49239"

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class OpenIsacForegroundWindow {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr window, int command);
}
'@

function Stop-ManagedProcess {
    param(
        [System.Diagnostics.Process]$Process,
        [string]$Name
    )
    if ($null -eq $Process) { return }
    try {
        $Process.Refresh()
        if ($Process.HasExited) { return }
        if ($Process.MainWindowHandle -ne [IntPtr]::Zero) {
            $Process.CloseMainWindow() | Out-Null
        }
        if (-not $Process.WaitForExit(1500)) {
            Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
            $Process.WaitForExit(1500) | Out-Null
        }
        Write-Host "Closed $Name." -ForegroundColor DarkGray
    } catch {
        Write-Host "Cleanup warning for $Name`: $($_.Exception.Message)" -ForegroundColor Yellow
    }
}

function Wait-ForBridgeDrain {
    param(
        [System.Diagnostics.Process]$BridgeProcess,
        [string]$StatusPath,
        [double]$TelemetryIntervalSeconds
    )
    $quietSeconds = [Math]::Max(3.0, $TelemetryIntervalSeconds + 1.0)
    $deadline = [DateTime]::UtcNow.AddSeconds(60)
    $quietSince = [DateTime]::UtcNow
    $lastWriteTicks = 0L
    if (Test-Path -LiteralPath $StatusPath -PathType Leaf) {
        $lastWriteTicks = (Get-Item -LiteralPath $StatusPath).LastWriteTimeUtc.Ticks
    }
    Write-Host "Sender finished; draining PHY and VLC buffers (quiet period $quietSeconds s)..." -ForegroundColor Yellow
    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 250
        $BridgeProcess.Refresh()
        if ($BridgeProcess.HasExited) { break }
        $currentWriteTicks = $lastWriteTicks
        if (Test-Path -LiteralPath $StatusPath -PathType Leaf) {
            $currentWriteTicks = (Get-Item -LiteralPath $StatusPath).LastWriteTimeUtc.Ticks
        }
        if ($currentWriteTicks -ne $lastWriteTicks) {
            $lastWriteTicks = $currentWriteTicks
            $quietSince = [DateTime]::UtcNow
        }
        if (([DateTime]::UtcNow - $quietSince).TotalSeconds -ge $quietSeconds) {
            break
        }
    }
}

$bridgeProcess = $null
$monitorProcess = $null
$receiverProcess = $null
$senderProcess = $null
try {
    $bridgeProcess = Start-Process -FilePath $bridge -ArgumentList $bridgeArguments -PassThru
    Start-Sleep -Seconds 1
    $monitorProcess = Start-Process -FilePath $pythonw -ArgumentList @(
        (Quote-ProcessArgument (Join-Path $scriptDirectory 'live_phy_monitor.py')),
        (Quote-ProcessArgument $telemetry), '--refresh', '0.8'
    ) -PassThru
    Start-Sleep -Seconds 1
    $receiverProcess = Start-Process -FilePath $vlc -ArgumentList @(
        '--no-one-instance', 'udp://@:50001', '--network-caching=500'
    ) -PassThru
    Start-Sleep -Seconds 1
    $streamOutput = '#standard{access=udp,mux=ts,dst=127.0.0.1:50000}'
    if ($Rank -eq 4) {
        $streamOutput = "#transcode{vcodec=h264,vb=$VideoBitrateKbps,acodec=mpga,ab=96,channels=2}:standard{access=udp,mux=ts,dst=127.0.0.1:50000}"
    }
    $senderProcess = Start-Process -FilePath $vlc -WindowStyle Hidden -ArgumentList @(
        '--intf', 'dummy', '--no-one-instance', (Quote-ProcessArgument $VideoFile),
        '--sout', $streamOutput,
        '--play-and-exit'
    ) -PassThru

    Start-Sleep -Seconds 1
    $receiverProcess.Refresh()
    [OpenIsacForegroundWindow]::ShowWindowAsync($receiverProcess.MainWindowHandle, 9) | Out-Null
    [OpenIsacForegroundWindow]::SetForegroundWindow($receiverProcess.MainWindowHandle) | Out-Null

    Write-Host 'Started VLC receiver, live PHY monitor and PHY log window.' -ForegroundColor Green
    Write-Host "PID: bridge=$($bridgeProcess.Id), monitor=$($monitorProcess.Id), receiver=$($receiverProcess.Id), sender=$($senderProcess.Id)"
    Write-Host 'Waiting for the video to finish. Press Ctrl+C here to stop early.'

    $exitReason = 'video sender completed'
    while ($true) {
        Start-Sleep -Milliseconds 500
        $senderProcess.Refresh()
        $receiverProcess.Refresh()
        $bridgeProcess.Refresh()
        if ($senderProcess.HasExited) {
            $exitReason = 'video sender completed'
            Wait-ForBridgeDrain $bridgeProcess (Join-Path $telemetry 'status.csv') $RefreshSeconds
            break
        }
        if ($receiverProcess.HasExited) {
            $exitReason = 'receiver VLC was closed'
            break
        }
        if ($bridgeProcess.HasExited) {
            $exitReason = 'PHY bridge exited'
            break
        }
    }
    Write-Host "Test ending: $exitReason." -ForegroundColor Cyan
} finally {
    Write-Host 'Closing OpenISAC test processes...' -ForegroundColor Yellow
    Stop-ManagedProcess $senderProcess 'VLC sender'
    Stop-ManagedProcess $receiverProcess 'VLC receiver'
    Stop-ManagedProcess $monitorProcess 'Python live monitor'
    Stop-ManagedProcess $bridgeProcess 'PHY bridge'
    Write-Host 'All VLC, monitor and PHY processes started by this test are closed.' -ForegroundColor Green
}
