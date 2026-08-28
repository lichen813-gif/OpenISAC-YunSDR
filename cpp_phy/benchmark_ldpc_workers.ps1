param(
    [string]$Binary = "$PSScriptRoot\build\ninja-vs2019\openisac_phy_video_bridge.exe",
    [string]$OutputDirectory = "$PSScriptRoot\..\out\ldpc-worker-sweep-windows",
    [int]$Packets = 500,
    [int]$Warmup = 50,
    [int]$Repeats = 3,
    [int[]]$Workers = @(1, 2, 4, 6, 8),
    [switch]$ProfileBackend
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Binary)) {
    throw "Video PHY bridge not found: $Binary"
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$metrics = Join-Path $OutputDirectory 'ldpc_worker_performance.csv'
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
if ($ProfileBackend) {
    $common += '--profile-backend'
}

function Invoke-LdpcWorkerBenchmark {
    param(
        [string]$Label,
        [int]$WorkerCount,
        [string[]]$ModeArguments
    )
    for ($repeat = 1; $repeat -le $Repeats; $repeat++) {
        $caseLabel = "${Label}_w${WorkerCount}"
        Write-Host "[$caseLabel] repeat $repeat/$Repeats"
        & $Binary @common '--workers' "$WorkerCount" '--benchmark-label' $caseLabel @ModeArguments
        if ($LASTEXITCODE -ne 0) {
            throw "Benchmark failed: $caseLabel repeat $repeat"
        }
    }
}

foreach ($workerCount in $Workers) {
    foreach ($pilot in @('fdm', 'nr-dmrs')) {
        Invoke-LdpcWorkerBenchmark "rank2_4port_$pilot" $workerCount @(
            '--rank', '2', '--tx-ports', '4', '--rx-ports', '4',
            '--mimo-mode', 'spatial', '--pilot-mode', $pilot)
        Invoke-LdpcWorkerBenchmark "rank4_$pilot" $workerCount @(
            '--rank', '4', '--tx-ports', '4', '--rx-ports', '4',
            '--mimo-mode', 'spatial', '--pilot-mode', $pilot)
    }
}

Write-Host "Wrote $metrics"
