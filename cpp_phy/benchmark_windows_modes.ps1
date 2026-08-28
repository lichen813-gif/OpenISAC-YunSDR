param(
    [string]$Binary = "$PSScriptRoot\build\ninja-vs2019\openisac_phy_video_bridge.exe",
    [string]$OutputDirectory = "$PSScriptRoot\..\out\platform-benchmark\windows",
    [int]$Packets = 200,
    [int]$Warmup = 20,
    [int]$Repeats = 5
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Binary)) {
    throw "Video PHY bridge not found: $Binary"
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$metrics = Join-Path $OutputDirectory 'mode_performance.csv'
if (Test-Path -LiteralPath $metrics) {
    throw "Refusing to append to existing benchmark file: $metrics"
}

$common = @(
    '--self-test', "$Packets",
    '--benchmark-warmup', "$Warmup",
    '--metrics-csv', $metrics,
    '--modulation', '64QAM',
    '--snr', '45',
    '--cfo', '300',
    '--sfo', '20',
    '--timing', '20',
    '--tx-correlation', '0.02',
    '--rx-correlation', '0.02',
    '--seed', '49239',
    '--spatial-seed', '49239',
    '--sensing-coherent', '0',
    '--backend', 'cpu'
)

function Invoke-ModeBenchmark {
    param([string]$Label, [string[]]$ModeArguments)
    for ($repeat = 1; $repeat -le $Repeats; $repeat++) {
        Write-Host "[$Label] repeat $repeat/$Repeats"
        & $Binary @common '--benchmark-label' $Label @ModeArguments
        if ($LASTEXITCODE -ne 0) {
            throw "Benchmark failed: $Label repeat $repeat"
        }
    }
}

foreach ($pilot in @('fdm', 'nr-dmrs')) {
    Invoke-ModeBenchmark "siso_$pilot" @(
        '--rank', '1', '--tx-ports', '1', '--rx-ports', '1',
        '--mimo-mode', 'spatial', '--pilot-mode', $pilot)
    Invoke-ModeBenchmark "mimo2_$pilot" @(
        '--rank', '2', '--tx-ports', '2', '--rx-ports', '2',
        '--mimo-mode', 'spatial', '--pilot-mode', $pilot)
    Invoke-ModeBenchmark "stbc_$pilot" @(
        '--rank', '1', '--tx-ports', '2', '--rx-ports', '2',
        '--mimo-mode', 'stbc', '--pilot-mode', $pilot)
    Invoke-ModeBenchmark "rank2_4port_$pilot" @(
        '--rank', '2', '--tx-ports', '4', '--rx-ports', '4',
        '--mimo-mode', 'spatial', '--pilot-mode', $pilot)
    Invoke-ModeBenchmark "rank4_$pilot" @(
        '--rank', '4', '--tx-ports', '4', '--rx-ports', '4',
        '--mimo-mode', 'spatial', '--pilot-mode', $pilot)
}

Write-Host "Wrote $metrics"
