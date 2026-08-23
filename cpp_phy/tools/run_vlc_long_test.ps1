param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,
    [int]$DurationSeconds = 1800,
    [int]$VideoBitrateKbps = 1500,
    [double]$SnrDb = 45
)

$ErrorActionPreference = 'Stop'
$vlc = 'C:\Program Files\VideoLAN\VLC\vlc.exe'
$bridgeExe = Join-Path $RepositoryRoot 'cpp_phy\build\ninja-vs2019\openisac_phy_video_bridge.exe'
$inputFile = Join-Path $RepositoryRoot 'measurement\vlc_video_long_run\noise_pattern.yuv'
if (-not (Test-Path -LiteralPath $vlc)) { throw "VLC not found: $vlc" }
if (-not (Test-Path -LiteralPath $bridgeExe)) { throw "Bridge not found: $bridgeExe" }
if (-not (Test-Path -LiteralPath $inputFile)) { throw "Test video not found: $inputFile" }
if ($DurationSeconds -lt 10) { throw 'DurationSeconds must be at least 10' }

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$statusPath = Join-Path $OutputDirectory 'status.txt'
$bridgeOut = Join-Path $OutputDirectory 'bridge_stdout.log'
$bridgeErr = Join-Path $OutputDirectory 'bridge_stderr.log'
$senderOut = Join-Path $OutputDirectory 'sender_stdout.log'
$senderErr = Join-Path $OutputDirectory 'sender_stderr.log'
$started = Get-Date
$bridge = $null
$receiver = $null
$sender = $null

try {
    @(
        'state=RUNNING'
        "start_time=$($started.ToString('o'))"
        "duration_seconds=$DurationSeconds"
        "video_bitrate_kbps=$VideoBitrateKbps"
        "snr_db=$SnrDb"
        'cfo_hz=300'
        'sfo_ppm=20'
        'tdl=0:0:0+3:-4:45+9:-8:-80'
    ) | Set-Content -LiteralPath $statusPath

    $bridgeArgs = @('--snr', "$SnrDb", '--cfo', '300', '--sfo', '20',
        '--tdl', '0:0:0+3:-4:45+9:-8:-80')
    $bridge = Start-Process -FilePath $bridgeExe -ArgumentList $bridgeArgs `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput $bridgeOut `
        -RedirectStandardError $bridgeErr
    Start-Sleep -Seconds 1

    $receiverLine = '--no-one-instance "udp://@:50001" --network-caching=300 --verbose=1'
    $receiver = Start-Process -FilePath $vlc -ArgumentList $receiverLine -PassThru
    Start-Sleep -Seconds 1

    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class OpenIsacWindow {
    [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
}
'@
    $receiver.Refresh()
    [void][OpenIsacWindow]::ShowWindowAsync($receiver.MainWindowHandle, 9)
    [void][OpenIsacWindow]::SetForegroundWindow($receiver.MainWindowHandle)

    $clipSeconds = 10
    $clipCount = [Math]::Max(1, [Math]::Ceiling($DurationSeconds / $clipSeconds))
    $repeat = $clipCount - 1
    $senderLine = '--no-one-instance -I dummy --demux=rawvid ' +
        '--rawvid-width=320 --rawvid-height=180 --rawvid-fps=15 ' +
        '--rawvid-chroma=I420 --input-repeat=' + $repeat + ' "' + $inputFile + '" ' +
        '--sout "#transcode{vcodec=h264,vb=' + $VideoBitrateKbps +
        ',fps=15}:standard{access=udp,mux=ts,dst=127.0.0.1:50000}" ' +
        '--play-and-exit --verbose=1'
    $sender = Start-Process -FilePath $vlc -ArgumentList $senderLine `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput $senderOut `
        -RedirectStandardError $senderErr
    $sender.WaitForExit()
    $senderExitCode = $sender.ExitCode
    Start-Sleep -Seconds 3

    if ($bridge -and -not $bridge.HasExited) {
        Stop-Process -Id $bridge.Id
        $bridge.WaitForExit()
    }
    if ($receiver -and -not $receiver.HasExited) {
        Stop-Process -Id $receiver.Id
        $receiver.WaitForExit()
    }

    $lastStats = Get-Content -LiteralPath $bridgeOut -ErrorAction SilentlyContinue |
        Where-Object { $_ -like 'UDP in/out/drop*' } | Select-Object -Last 1
    $encoderRate = Get-Content -LiteralPath $senderErr -ErrorAction SilentlyContinue |
        Where-Object { $_ -like '*x264 encoder: kb/s:*' } | Select-Object -Last 1
    $finished = Get-Date
    @(
        'state=COMPLETE'
        "start_time=$($started.ToString('o'))"
        "finish_time=$($finished.ToString('o'))"
        "elapsed_seconds=$([Math]::Round(($finished - $started).TotalSeconds, 3))"
        "sender_exit_code=$senderExitCode"
        "last_bridge_stats=$lastStats"
        "encoder_rate=$encoderRate"
    ) | Set-Content -LiteralPath $statusPath
}
catch {
    @(
        'state=FAILED'
        "error=$($_.Exception.Message)"
        "time=$((Get-Date).ToString('o'))"
    ) | Add-Content -LiteralPath $statusPath
    throw
}
finally {
    foreach ($process in @($sender, $receiver, $bridge)) {
        if ($process -and -not $process.HasExited) {
            Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
        }
    }
}
