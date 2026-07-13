[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = $PSScriptRoot
$publishDirectory = Join-Path $repoRoot 'publish'
$cmake = (Get-Command cmake -CommandType Application -ErrorAction Stop).Source

$buildMatrix = @(
    [pscustomobject]@{
        Architecture = 'x86'
        Platform = 'Win32'
        WindowsTarget = 'WIN8'
        BuildDirectory = 'build-x86'
        PackageSuffix = ''
    }
    [pscustomobject]@{
        Architecture = 'x64'
        Platform = 'x64'
        WindowsTarget = 'WIN8'
        BuildDirectory = 'build-x64'
        PackageSuffix = ''
    }
    [pscustomobject]@{
        Architecture = 'x86'
        Platform = 'Win32'
        WindowsTarget = 'WIN7'
        BuildDirectory = 'build-win7-x86'
        PackageSuffix = '_Win7'
    }
    [pscustomobject]@{
        Architecture = 'x64'
        Platform = 'x64'
        WindowsTarget = 'WIN7'
        BuildDirectory = 'build-win7-x64'
        PackageSuffix = '_Win7'
    }
)

function Invoke-CMake {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    & $cmake @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "cmake failed with exit code ${LASTEXITCODE}: cmake $($Arguments -join ' ')"
    }
}

function Get-CMakeProjectVersion {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BuildDirectory
    )

    $cachePath = Join-Path $BuildDirectory 'CMakeCache.txt'
    $match = Select-String -LiteralPath $cachePath -Pattern '^CMAKE_PROJECT_VERSION:STATIC=(.+)$'
    if ($null -eq $match -or $match.Matches.Count -ne 1) {
        throw "Could not read CMAKE_PROJECT_VERSION from $cachePath"
    }

    return $match.Matches[0].Groups[1].Value.Trim()
}

$outputs = foreach ($build in $buildMatrix) {
    $buildDirectory = Join-Path $repoRoot $build.BuildDirectory
    Write-Host "Configuring $($build.Architecture) / $($build.WindowsTarget)..." -ForegroundColor Cyan
    Invoke-CMake -Arguments @(
        '-S', $repoRoot,
        '-B', $buildDirectory,
        '-A', $build.Platform,
        "-DFLASHIE_WINDOWS_TARGET=$($build.WindowsTarget)"
    )

    Write-Host "Building $($build.Architecture) / $($build.WindowsTarget) (Release)..." -ForegroundColor Cyan
    Invoke-CMake -Arguments @(
        '--build', $buildDirectory,
        '--config', 'Release',
        '--parallel'
    )

    $releaseDirectory = Join-Path $buildDirectory 'Release'
    $exePath = Join-Path $releaseDirectory 'FlashIE.exe'
    $ocxPath = Join-Path $releaseDirectory 'Flash.ocx'

    foreach ($artifact in @($exePath, $ocxPath)) {
        if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
            throw "Expected build artifact was not found: $artifact"
        }
    }

    [pscustomobject]@{
        Architecture = $build.Architecture
        PackageSuffix = $build.PackageSuffix
        Version = Get-CMakeProjectVersion -BuildDirectory $buildDirectory
        ExePath = $exePath
        OcxPath = $ocxPath
    }
}

$versions = @($outputs.Version | Select-Object -Unique)
if ($versions.Count -ne 1 -or [string]::IsNullOrWhiteSpace($versions[0])) {
    throw "Build configurations produced inconsistent project versions: $($versions -join ', ')"
}

New-Item -ItemType Directory -Path $publishDirectory -Force | Out-Null

foreach ($output in $outputs) {
    $archiveName = "FlashIE_$($output.Architecture)_$($output.Version)$($output.PackageSuffix).zip"
    $archivePath = Join-Path $publishDirectory $archiveName

    if (Test-Path -LiteralPath $archivePath) {
        Remove-Item -LiteralPath $archivePath -Force
    }

    Compress-Archive `
        -LiteralPath @($output.ExePath, $output.OcxPath) `
        -DestinationPath $archivePath `
        -CompressionLevel Optimal

    Write-Host "Created $archivePath" -ForegroundColor Green
}

Write-Host "Published $($outputs.Count) archives for FlashIE $($versions[0])." -ForegroundColor Green
