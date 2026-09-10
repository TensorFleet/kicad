#!/usr/bin/env pwsh
# Check out vcpkg at the baseline pinned in vcpkg-configuration.json, bootstrap it and
# export VCPKG_ROOT plus the binary-cache settings for the following steps.
#
# Env in:  GITHUB_WORKSPACE (or the current directory), RUNNER_TEMP, GITHUB_ENV
# Env out: VCPKG_ROOT, VCPKG_DEFAULT_BINARY_CACHE, VCPKG_BINARY_SOURCES, NIGHTLY_TRIPLETS
$ErrorActionPreference = "Stop"

$root = if ($env:GITHUB_WORKSPACE) { $env:GITHUB_WORKSPACE } else { (Get-Location).Path }
$temp = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }
$vcpkgRoot = Join-Path $temp "vcpkg"
$cache = Join-Path $root ".vcpkg-cache"

$config = Get-Content (Join-Path $root "vcpkg-configuration.json") | ConvertFrom-Json
$baseline = $config."default-registry".baseline
Write-Host "vcpkg baseline: $baseline"

if (-not (Test-Path $vcpkgRoot)) {
    git clone --quiet --filter=tree:0 https://github.com/microsoft/vcpkg $vcpkgRoot
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
git -C $vcpkgRoot checkout --quiet $baseline
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& (Join-Path $vcpkgRoot "bootstrap-vcpkg.bat") -disableMetrics
if ($LASTEXITCODE) { exit $LASTEXITCODE }

New-Item -ItemType Directory -Force -Path $cache | Out-Null

$exports = @{
    VCPKG_ROOT                 = $vcpkgRoot
    VCPKG_DEFAULT_BINARY_CACHE = $cache
    VCPKG_BINARY_SOURCES       = "clear;files,$cache,readwrite"
    NIGHTLY_TRIPLETS           = (Join-Path $root "tools\nightly\windows\triplets")
}
foreach ($kv in $exports.GetEnumerator()) {
    Write-Host "$($kv.Key)=$($kv.Value)"
    if ($env:GITHUB_ENV) { "$($kv.Key)=$($kv.Value)" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8 }
    Set-Item -Path "env:$($kv.Key)" -Value $kv.Value
}
