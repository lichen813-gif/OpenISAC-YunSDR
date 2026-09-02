[CmdletBinding()]
param(
    [string]$ArchivePath = '',
    [string]$Destination = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$importDirectory = Join-Path $repositoryRoot 'import'
if ([string]::IsNullOrWhiteSpace($Destination)) {
    $Destination = Join-Path $importDirectory 'vendor-y240-26-01-00.1\source'
}

if (Test-Path -LiteralPath (Join-Path $Destination 'CMakeLists.txt')) {
    Write-Host "Y240 vendor source is already prepared: $Destination"
    exit 0
}

if ([string]::IsNullOrWhiteSpace($ArchivePath)) {
    $archiveCandidates = @(
        (Join-Path $importDirectory 'libyunsdr-26-01-00.1.zip.zip'),
        (Join-Path $importDirectory 'libyunsdr-26-01-00.1.zip')
    )
    $ArchivePath = $archiveCandidates |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
}

if ([string]::IsNullOrWhiteSpace($ArchivePath) -or
    -not (Test-Path -LiteralPath $ArchivePath)) {
    throw @"
The YunSDR vendor archive is not part of the public Git repository.
Obtain libyunsdr 26-01-00.1 from YunSDR, then place one of these files here:
  $importDirectory\libyunsdr-26-01-00.1.zip.zip
  $importDirectory\libyunsdr-26-01-00.1.zip
The archive is not downloaded automatically because its redistribution and
download terms are controlled by the vendor.
"@
}

function Find-LibyunsdrSourceRoot([string]$Root) {
    $cmakeFiles = Get-ChildItem -LiteralPath $Root -Filter CMakeLists.txt -File -Recurse
    foreach ($cmake in $cmakeFiles) {
        $candidate = $cmake.Directory.FullName
        if (Test-Path -LiteralPath (Join-Path $candidate 'src\yunsdr_ss\CMakeLists.txt')) {
            return $candidate
        }
    }
    return $null
}

$vendorDirectory = Split-Path -Parent $Destination
$stagingDirectory = Join-Path $vendorDirectory ('.extract-' + [guid]::NewGuid().ToString('N'))
$nestedDirectory = Join-Path $stagingDirectory 'nested'

New-Item -ItemType Directory -Force -Path $vendorDirectory | Out-Null
New-Item -ItemType Directory -Force -Path $stagingDirectory | Out-Null

try {
    Expand-Archive -LiteralPath $ArchivePath -DestinationPath $stagingDirectory -Force
    $sourceRoot = Find-LibyunsdrSourceRoot $stagingDirectory

    if ($null -eq $sourceRoot) {
        $nestedArchives = @(Get-ChildItem -LiteralPath $stagingDirectory -Filter *.zip -File -Recurse)
        if ($nestedArchives.Count -ne 1) {
            throw "The archive does not contain one recognizable libyunsdr source tree."
        }
        New-Item -ItemType Directory -Force -Path $nestedDirectory | Out-Null
        Expand-Archive -LiteralPath $nestedArchives[0].FullName `
            -DestinationPath $nestedDirectory -Force
        $sourceRoot = Find-LibyunsdrSourceRoot $nestedDirectory
    }

    if ($null -eq $sourceRoot) {
        throw "The archive is missing src\yunsdr_ss\CMakeLists.txt."
    }

    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    Get-ChildItem -LiteralPath $sourceRoot -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $Destination -Recurse -Force
    }

    $required = @(
        'CMakeLists.txt',
        'src\yunsdr_ss\CMakeLists.txt',
        'src\yunsdr_ss\include\yunsdr_api_ss.h',
        'lib\libpcies.lib',
        'lib\libfirmware.lib'
    )
    foreach ($relativePath in $required) {
        if (-not (Test-Path -LiteralPath (Join-Path $Destination $relativePath))) {
            throw "The prepared SDK is missing $relativePath"
        }
    }

    Write-Host "Prepared YunSDR Y240 source: $Destination" -ForegroundColor Green
} finally {
    if (Test-Path -LiteralPath $stagingDirectory) {
        Remove-Item -LiteralPath $stagingDirectory -Recurse -Force
    }
}
