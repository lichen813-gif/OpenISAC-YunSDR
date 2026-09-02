param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$VideoFile,
    [ValidateSet('siso', 'mimo2', 'stbc')]
    [string]$Mode = 'siso',
    [ValidateSet('qpsk', '16qam', '64qam', '256qam')]
    [string]$Modulation = '64qam',
    [ValidateSet('fdm', 'dmrs')]
    [string]$Pilot = 'fdm',
    [double]$FrequencyMHz = 1500,
    [double]$TxGain = 60,
    [double]$RxGain = 20,
    [int]$VideoBitrateKbps = 1000,
    [int]$BatchPackets = 8,
    [int]$LeadBlocks = 48,
    [int]$Retries = 8,
    [int]$DurationSeconds = 0,
    [double]$RefreshSeconds = 0.8,
    [ValidateSet(16, 32, 64, 128)]
    [int]$SensingCoherentFrames = 16,
    [switch]$NoMonitor
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$bridge = Join-Path $root 'build\ninja-vs2019-hardware\yunsdr_video_bridge.exe'
$vlc = Join-Path $env:ProgramFiles 'VideoLAN\VLC\vlc.exe'
if (-not (Test-Path -LiteralPath $VideoFile -PathType Leaf)) {
    throw "Video file not found: $VideoFile"
}
if (-not (Test-Path -LiteralPath $bridge -PathType Leaf)) {
    throw "Hardware video bridge not found. Run build_vendor_y240_pcies_vs2019.cmd and build_hardware_vs2019.cmd first: $bridge"
}
if (-not (Test-Path -LiteralPath $vlc -PathType Leaf)) {
    throw "VLC was not found: $vlc"
}

$monitorScript = Join-Path $root '..\cpp_phy\live_phy_monitor.py'
$pythonExecutable = $null
$pythonArgumentPrefix = @()
if (-not $NoMonitor) {
    if (-not (Test-Path -LiteralPath $monitorScript -PathType Leaf)) {
        throw "OpenISAC live monitor was not found: $monitorScript"
    }
    $venvPythonw = Join-Path $root '..\.venv\Scripts\pythonw.exe'
    $venvPython = Join-Path $root '..\.venv\Scripts\python.exe'
    if (Test-Path -LiteralPath $venvPythonw -PathType Leaf) {
        $pythonExecutable = $venvPythonw
    } elseif (Test-Path -LiteralPath $venvPython -PathType Leaf) {
        $pythonExecutable = $venvPython
    }
    $pythonCommand = $null
    if ($null -eq $pythonExecutable) {
        $pythonCommand = Get-Command pythonw.exe -ErrorAction SilentlyContinue
    }
    if ($null -eq $pythonCommand) {
        $pythonCommand = Get-Command python.exe -ErrorAction SilentlyContinue
    }
    if ($null -ne $pythonCommand) { $pythonExecutable = $pythonCommand.Source }
    if ($null -eq $pythonExecutable) {
        $pythonCommand = Get-Command py.exe -ErrorAction SilentlyContinue
        if ($null -ne $pythonCommand) {
            $pythonExecutable = $pythonCommand.Source
            $pythonArgumentPrefix = @('-3')
        }
    }
    if ($null -eq $pythonExecutable -or
        -not (Test-Path -LiteralPath $pythonExecutable -PathType Leaf)) {
        throw 'Python with Tkinter and NumPy was not found. See docs\Y240_WINDOWS_QUICKSTART_zh.md.'
    }
}

function Quote-Argument([string]$Value) {
    return '"' + $Value.Replace('"', '\"') + '"'
}

function Stop-TestProcess(
    [System.Diagnostics.Process]$Process,
    [string]$Name
) {
    if ($null -eq $Process) { return }
    try {
        $Process.Refresh()
        if ($Process.HasExited) { return }
        if ($Process.MainWindowHandle -ne [IntPtr]::Zero) {
            $Process.CloseMainWindow() | Out-Null
        }
        if (-not $Process.WaitForExit(1500)) {
            Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
        }
        Write-Host "Closed $Name." -ForegroundColor DarkGray
    } catch {
        Write-Host "Cleanup warning for $Name`: $($_.Exception.Message)" -ForegroundColor Yellow
    }
}

function Test-CleanBridgeLog([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    return [bool](Select-String -LiteralPath $Path `
        -Pattern 'UDP input idle; bridge exiting cleanly\.' -Quiet)
}

$bridgeProcess = $null
$monitorProcess = $null
$receiverProcess = $null
$senderProcess = $null
$logDirectory = Join-Path $root 'out\hardware-video'
New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
$telemetry = Join-Path $logDirectory 'live_phy_monitor'
New-Item -ItemType Directory -Path $telemetry -Force | Out-Null
$logStamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$bridgeLog = $null
$bridgeErrorLog = $null
try {
    Write-Host 'Y240 hardware video test' -ForegroundColor Cyan
    Write-Host "Mode=$Mode, modulation=$Modulation, pilot=$Pilot"
    Write-Host "Frequency=$FrequencyMHz MHz, TX gain=$TxGain, RX gain=$RxGain"
    Write-Host "Video=$VideoFile, bitrate=$VideoBitrateKbps kbit/s"

    $bridgeArguments = @(
        '--mode', $Mode,
        '--modulation', $Modulation,
        '--pilot', $Pilot,
        '--frequency-mhz', $FrequencyMHz,
        '--tx-gain', $TxGain,
        '--rx-gain', $RxGain,
        '--batch-packets', $BatchPackets,
        '--lead-blocks', $LeadBlocks,
        '--idle-exit-seconds', '3',
        '--retries', $Retries,
        '--telemetry-dir', (Quote-Argument $telemetry),
        '--telemetry-interval', $RefreshSeconds,
        '--telemetry-points', '4096',
        '--sensing-coherent', $SensingCoherentFrames,
        '--sensing-range-bins', '128'
    ) -join ' '
    $bridgeReady = $false
    for ($startupAttempt = 1; $startupAttempt -le 3; $startupAttempt++) {
        $bridgeLog = Join-Path $logDirectory "bridge-$logStamp-attempt$startupAttempt.log"
        $bridgeErrorLog = Join-Path $logDirectory "bridge-$logStamp-attempt$startupAttempt.err.log"
        Write-Host "Starting PHY bridge (attempt $startupAttempt/3)..."
        $bridgeProcess = Start-Process -FilePath $bridge `
            -ArgumentList $bridgeArguments -PassThru `
            -RedirectStandardOutput $bridgeLog `
            -RedirectStandardError $bridgeErrorLog
        $startupDeadline = [DateTime]::UtcNow.AddSeconds(20)
        while ([DateTime]::UtcNow -lt $startupDeadline) {
            Start-Sleep -Milliseconds 250
            $bridgeProcess.Refresh()
            if ((Test-Path -LiteralPath $bridgeLog -PathType Leaf) -and
                (Select-String -LiteralPath $bridgeLog -Pattern 'Hardware warmup:' -Quiet)) {
                $bridgeReady = $true
                break
            }
            if ($bridgeProcess.HasExited) { break }
        }
        if ($bridgeReady) { break }
        Stop-TestProcess $bridgeProcess "Y240 PHY bridge startup attempt $startupAttempt"
        if ((Test-Path -LiteralPath $bridgeErrorLog -PathType Leaf) -and
            (Get-Item -LiteralPath $bridgeErrorLog).Length -gt 0) {
            Get-Content -LiteralPath $bridgeErrorLog -Tail 3
        }
        Start-Sleep -Seconds 2
    }
    if (-not $bridgeReady) {
        throw 'PHY bridge did not pass hardware warmup after 3 startup attempts.'
    }
    Write-Host 'PHY bridge hardware warmup passed.' -ForegroundColor Green

    if (-not $NoMonitor) {
        $monitorArguments = $pythonArgumentPrefix + @(
            (Quote-Argument $monitorScript),
            (Quote-Argument $telemetry),
            '--refresh', [string]$RefreshSeconds
        )
        $monitorProcess = Start-Process -FilePath $pythonExecutable `
            -ArgumentList $monitorArguments -PassThru
        Start-Sleep -Milliseconds 800
        Write-Host 'Live PHY and sensing monitor started.' -ForegroundColor Green
    }

    $receiverProcess = Start-Process -FilePath $vlc -ArgumentList @(
        '--no-one-instance', 'udp://@:50001', '--network-caching=800'
    ) -PassThru
    Start-Sleep -Seconds 1

    $streamOutput = "#transcode{vcodec=h264,vb=$VideoBitrateKbps,acodec=mp4a,ab=96,channels=2}:standard{access=udp,mux=ts,dst=127.0.0.1:50000}"
    $senderArguments = @(
        '--intf', 'dummy', '--no-one-instance',
        (Quote-Argument $VideoFile),
        '--sout', (Quote-Argument $streamOutput),
        '--play-and-exit'
    )
    if ($DurationSeconds -gt 0) {
        $senderArguments += @('--run-time', $DurationSeconds)
    }
    $senderProcess = Start-Process -FilePath $vlc -WindowStyle Hidden `
        -ArgumentList ($senderArguments -join ' ') -PassThru

    Write-Host 'VLC receiver is visible. Waiting for sender to complete; Ctrl+C stops early.' -ForegroundColor Green
    while ($true) {
        Start-Sleep -Milliseconds 500
        $senderProcess.Refresh()
        $receiverProcess.Refresh()
        $bridgeProcess.Refresh()
        if ($bridgeProcess.HasExited) {
            if ((Test-CleanBridgeLog $bridgeLog) -and $senderProcess.HasExited) {
                Write-Host 'PHY bridge completed cleanly with the sender.'
                break
            }
            $bridgeProcess.WaitForExit()
            throw "PHY bridge exited unexpectedly with code $($bridgeProcess.ExitCode)."
        }
        if ($senderProcess.HasExited) {
            Write-Host 'Sender completed; draining queued UDP packets and waiting for the bridge...'
            if (-not $bridgeProcess.WaitForExit(60000)) {
                Write-Host 'Bridge drain exceeded 60 seconds; stopping it.' -ForegroundColor Yellow
            } elseif (($null -ne $bridgeProcess.ExitCode) -and
                      ($bridgeProcess.ExitCode -ne 0)) {
                throw "PHY bridge exited with code $($bridgeProcess.ExitCode)."
            } elseif (-not (Test-CleanBridgeLog $bridgeLog)) {
                throw 'PHY bridge exited without a clean completion marker.'
            }
            break
        }
        if ($receiverProcess.HasExited) {
            Write-Host 'Receiver VLC was closed.'
            break
        }
    }
} finally {
    Write-Host 'Closing VLC and PHY bridge processes...' -ForegroundColor Yellow
    Stop-TestProcess $senderProcess 'VLC sender'
    Stop-TestProcess $receiverProcess 'VLC receiver'
    Stop-TestProcess $monitorProcess 'live PHY monitor'
    Stop-TestProcess $bridgeProcess 'Y240 PHY bridge'
    Write-Host "Bridge log: $bridgeLog" -ForegroundColor Cyan
    if (Test-Path -LiteralPath $bridgeLog -PathType Leaf) {
        $summary = Select-String -LiteralPath $bridgeLog `
            -Pattern 'Hardware warmup:|Forwarded UDP packets:|UDP input idle|Packets='
        if ($null -ne $summary) {
            $summary | Select-Object -Last 6 | ForEach-Object { $_.Line }
        } else {
            Get-Content -LiteralPath $bridgeLog -Tail 12
        }
    }
    if ((Test-Path -LiteralPath $bridgeErrorLog -PathType Leaf) -and
        (Get-Item -LiteralPath $bridgeErrorLog).Length -gt 0) {
        Write-Host 'Bridge error log:' -ForegroundColor Yellow
        Get-Content -LiteralPath $bridgeErrorLog -Tail 12
    }
    Write-Host 'Test processes are closed.' -ForegroundColor Green
}
